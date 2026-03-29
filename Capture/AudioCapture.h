#pragma once
#include "RecordingEngine.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <ksmedia.h>  // FIX: Required for KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#include <queue>
#include <mutex>

struct AudioPacket {
    std::vector<BYTE> data;
    uint64_t timestamp;
    uint32_t sampleCount;
    uint32_t bytesPerSample;
};

class AudioCapture {
public:
    AudioCapture() : m_running(false), m_sampleRate(0), m_bitDepth(0), m_channels(0), m_bytesPerSample(0) {}

    ~AudioCapture() {
        StopCapture();
    }

    bool Initialize(uint32_t requestedSampleRate, uint16_t requestedBitDepth) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // FIX: Don't fail if COM already initialized (S_FALSE or RPC_E_CHANGED_MODE)
        if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            spdlog::error("COM initialization failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_deviceEnumerator));
        if (FAILED(hr)) {
            spdlog::error("Failed to create device enumerator: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // Get default audio endpoint
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        if (FAILED(hr)) {
            spdlog::error("Failed to get default audio endpoint: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // Activate audio client
        hr = m_device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
        if (FAILED(hr)) {
            spdlog::error("Failed to activate audio client: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // --- GET NATIVE WINDOWS FORMAT ---
        // Instead of forcing 48000Hz, we politely ask Windows what it is currently using
        WAVEFORMATEX* mixFormat = nullptr;

        hr = m_audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            spdlog::error("Failed to get audio mix format");
            return false;
        }

        // Ensure we are using 32-bit Float
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* wfex = (WAVEFORMATEXTENSIBLE*)mixFormat;
            if (IsEqualGUID(wfex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                spdlog::info("WASAPI: Capturing in Perfect Quality (32-bit Float)");
            }
        }

        // Save the real settings so the WAV file writes at the correct speed
        m_sampleRate = mixFormat->nSamplesPerSec;
        m_bitDepth = mixFormat->wBitsPerSample;
        m_channels = mixFormat->nChannels;
        m_bytesPerSample = m_bitDepth / 8;

        REFERENCE_TIME bufferDuration = 10 * 10000; // 10ms

        // Initialize using Windows' Native Format
        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            mixFormat,
            nullptr);

        if (FAILED(hr)) {
            uint32_t errCode = static_cast<uint32_t>(hr);
            spdlog::error("Failed to initialize audio client: 0x{:08X}", errCode);
            CoTaskMemFree(mixFormat);
            return false;
        }

        CoTaskMemFree(mixFormat);
        mixFormat = nullptr;  // FIX: Avoid dangling pointer

        // Get capture client
        hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
        if (FAILED(hr)) {
            spdlog::error("Failed to get capture client: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        spdlog::info("AudioCapture initialized: {}Hz / {}-bit / {} channels", m_sampleRate, m_bitDepth, m_channels);
        return true;
    }

    // Getters for the WAV Header builder
    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint16_t GetBitDepth() const { return m_bitDepth; }
    uint16_t GetChannels() const { return m_channels; }

    bool StartCapture() {
        if (!m_audioClient) {
            spdlog::error("Audio client not initialized");
            return false;
        }

        HRESULT hr = m_audioClient->Start();
        if (FAILED(hr)) {
            spdlog::error("Failed to start audio client: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        m_running = true;
        m_captureThread = std::thread(&AudioCapture::CaptureLoop, this);
        return true;
    }

    void StopCapture() {
        m_running = false;
        
        // FIX: Wake up any waiting threads
        m_packetAvailable.notify_all();
        
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        if (m_audioClient) {
            m_audioClient->Stop();
        }
        
        // FIX: Clear queue to free memory
        {
            std::lock_guard<std::mutex> lock(m_packetMutex);
            std::queue<AudioPacket> empty;
            std::swap(m_packetQueue, empty);
        }
    }

    bool GetNextPacket(AudioPacket& packet, uint32_t timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(m_packetMutex);

        if (m_packetQueue.empty()) {
            if (!m_packetAvailable.wait_for(lock,
                std::chrono::milliseconds(timeoutMs),
                [this] { return !m_packetQueue.empty() || !m_running; })) {
                return false;
            }
        }

        if (m_packetQueue.empty()) return false;

        packet = std::move(m_packetQueue.front());  // FIX: Use move to avoid copy
        m_packetQueue.pop();
        return true;
    }

    uint64_t GetTimestamp() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

private:
    void CaptureLoop() {
        spdlog::info("Audio capture thread started ({}Hz / {}-bit)", m_sampleRate, m_bitDepth);

        // FIX: Pre-calculate frame size
        const uint32_t frameSize = m_channels * m_bytesPerSample;

        while (m_running) {
            UINT32 nextPacketSize = 0;
            HRESULT hr = m_captureClient->GetNextPacketSize(&nextPacketSize);
            if (FAILED(hr)) {
                spdlog::warn("GetNextPacketSize failed: 0x{:08X}", static_cast<uint32_t>(hr));
                break;
            }

            while (nextPacketSize > 0 && m_running) {
                BYTE* data = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;

                UINT64 devPos = 0, qpcPos = 0;
                hr = m_captureClient->GetBuffer(&data, &numFramesAvailable, &flags, &devPos, &qpcPos);

                if (SUCCEEDED(hr)) {
                    AudioPacket packet;

                    // Use QPC timestamp for high-precision A/V sync
                    LARGE_INTEGER qpcFreq;
                    QueryPerformanceFrequency(&qpcFreq);
                    packet.timestamp = (qpcPos * 1000000) / qpcFreq.QuadPart;

                    packet.sampleCount = numFramesAvailable;
                    packet.bytesPerSample = m_bytesPerSample;

                    size_t dataSize = static_cast<size_t>(numFramesAvailable) * frameSize;
                    packet.data.resize(dataSize);

                    // Handle silent frames correctly (like when nothing is playing)
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        memset(packet.data.data(), 0, dataSize);
                    }
                    else if (data) {  // FIX: Check data pointer before copying
                        memcpy(packet.data.data(), data, dataSize);
                    }

                    {
                        std::lock_guard<std::mutex> lock(m_packetMutex);
                        // FIX: Increased queue limit for high sample rates
                        if (m_packetQueue.size() > 200) {
                            m_packetQueue.pop(); // Keep max ~2 seconds of audio in queue
                        }
                        m_packetQueue.push(std::move(packet));  // FIX: Use move
                    }

                    m_packetAvailable.notify_one();
                    hr = m_captureClient->ReleaseBuffer(numFramesAvailable);
                }

                if (FAILED(hr)) break;
                hr = m_captureClient->GetNextPacketSize(&nextPacketSize);
                if (FAILED(hr)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        spdlog::info("Audio capture thread stopped");
    }

    ComPtr<IMMDeviceEnumerator> m_deviceEnumerator;
    ComPtr<IMMDevice> m_device;
    ComPtr<IAudioClient3> m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;

    std::atomic<bool> m_running;
    std::thread m_captureThread;

    uint32_t m_sampleRate;
    uint16_t m_bitDepth;
    uint16_t m_bytesPerSample;
    uint16_t m_channels;

    std::queue<AudioPacket> m_packetQueue;
    std::mutex m_packetMutex;
    std::condition_variable m_packetAvailable;
};

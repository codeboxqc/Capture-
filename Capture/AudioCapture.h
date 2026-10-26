#pragma once
#include "RecordingEngine.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cmath>
#include <algorithm>

struct AudioPacket {
    std::vector<BYTE> data;
    uint64_t timestamp = 0;
    uint32_t sampleCount = 0;
    uint32_t bytesPerSample = 0;
};

class AudioCapture {
public:
    AudioCapture()
        : m_running(false)
        , m_sampleRate(48000)
        , m_bitDepth(32)
        , m_channels(2)
        , m_bytesPerSample(4)
        , m_nativeChannels(2)
        , m_isFloat(true)
    {
    }

    ~AudioCapture() {
        StopCapture();
    }

    bool Initialize(uint32_t requestedSampleRate, uint16_t requestedBitDepth) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            spdlog::error("COM initialization failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_deviceEnumerator));
        if (FAILED(hr)) {
            spdlog::error("Failed to create device enumerator: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_device);
        if (FAILED(hr)) {
            hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
            if (FAILED(hr)) {
                spdlog::error("Failed to get default audio endpoint: 0x{:08X}", static_cast<uint32_t>(hr));
                return false;
            }
        }

        // Activate Audio Client
        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
        if (FAILED(hr)) {
            spdlog::error("Failed to activate audio client: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        hr = m_audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            spdlog::error("Failed to get audio mix format");
            return false;
        }

        m_nativeChannels = mixFormat->nChannels;
        m_sampleRate = mixFormat->nSamplesPerSec;
        m_bitDepth = mixFormat->wBitsPerSample;
        m_bytesPerSample = m_bitDepth / 8;

        // Check if format is IEEE float
        m_isFloat = false;
        if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            m_isFloat = true;
        }
        else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
            if (IsEqualGUID(wfex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                m_isFloat = true;
            }
        }

        // Always provide 2-channel stereo to the output pipeline
        m_channels = 2;
        REFERENCE_TIME bufferDuration = 20 * 10000; // 20ms buffer

        // Primary: standard shared loopback
        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            mixFormat,
            nullptr
        );

        // Fallback: If initial call fails, re-activate client before retrying
        if (FAILED(hr)) {
            m_audioClient.Reset();
            if (SUCCEEDED(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient))) {
                hr = m_audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                    bufferDuration,
                    0,
                    mixFormat,
                    nullptr
                );
            }
        }

        if (FAILED(hr)) {
            spdlog::error("Failed to initialize audio client: 0x{:08X}", static_cast<uint32_t>(hr));
            CoTaskMemFree(mixFormat);
            return false;
        }

        CoTaskMemFree(mixFormat);

        hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
        if (FAILED(hr)) {
            spdlog::error("Failed to get capture client: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        spdlog::info("AudioCapture initialized: {}Hz / {}-bit / {}ch (Native {}ch, {})",
            m_sampleRate, m_bitDepth, m_channels, m_nativeChannels, m_isFloat ? "Float" : "Int PCM");
        return true;
    }

    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint16_t GetBitDepth() const { return m_bitDepth; }
    uint16_t GetChannels() const { return m_channels; }
    bool IsFloat() const { return m_isFloat; }

    bool StartCapture() {
        if (!m_audioClient) return false;
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
        m_packetAvailable.notify_all();
        if (m_captureThread.joinable()) m_captureThread.join();
        if (m_audioClient) m_audioClient->Stop();

        std::lock_guard<std::mutex> lock(m_packetMutex);
        std::queue<AudioPacket> empty;
        std::swap(m_packetQueue, empty);
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
        packet = std::move(m_packetQueue.front());
        m_packetQueue.pop();
        return true;
    }

private:
    void DownmixToStereo(const BYTE* src, BYTE* dst, UINT32 numFrames) {
        if (m_isFloat && m_bytesPerSample == 4) {
            const float* in = reinterpret_cast<const float*>(src);
            float* out = reinterpret_cast<float*>(dst);

            if (m_nativeChannels == 2) {
                memcpy(dst, src, numFrames * 2 * sizeof(float));
            }
            else if (m_nativeChannels == 6) { // 5.1 Surround
                for (UINT32 i = 0; i < numFrames; ++i) {
                    float fl = in[i * 6 + 0];
                    float fr = in[i * 6 + 1];
                    float fc = in[i * 6 + 2];
                    float lfe = in[i * 6 + 3];
                    float bl = in[i * 6 + 4];
                    float br = in[i * 6 + 5];

                    out[i * 2 + 0] = std::clamp(fl + 0.707f * fc + 0.707f * bl + 0.5f * lfe, -1.0f, 1.0f);
                    out[i * 2 + 1] = std::clamp(fr + 0.707f * fc + 0.707f * br + 0.5f * lfe, -1.0f, 1.0f);
                }
            }
            else if (m_nativeChannels == 8) { // 7.1 Surround
                for (UINT32 i = 0; i < numFrames; ++i) {
                    float fl = in[i * 8 + 0];
                    float fr = in[i * 8 + 1];
                    float fc = in[i * 8 + 2];
                    float lfe = in[i * 8 + 3];
                    float bl = in[i * 8 + 4];
                    float br = in[i * 8 + 5];
                    float sl = in[i * 8 + 6];
                    float sr = in[i * 8 + 7];

                    out[i * 2 + 0] = std::clamp(fl + 0.707f * fc + 0.5f * (bl + sl) + 0.5f * lfe, -1.0f, 1.0f);
                    out[i * 2 + 1] = std::clamp(fr + 0.707f * fc + 0.5f * (br + sr) + 0.5f * lfe, -1.0f, 1.0f);
                }
            }
            else {
                for (UINT32 i = 0; i < numFrames; ++i) {
                    out[i * 2 + 0] = in[i * m_nativeChannels + 0];
                    out[i * 2 + 1] = in[i * m_nativeChannels + 1];
                }
            }
        }
        else if (m_bytesPerSample == 2) { // 16-bit Integer PCM
            const int16_t* in = reinterpret_cast<const int16_t*>(src);
            int16_t* out = reinterpret_cast<int16_t*>(dst);

            for (UINT32 i = 0; i < numFrames; ++i) {
                out[i * 2 + 0] = in[i * m_nativeChannels + 0];
                out[i * 2 + 1] = in[i * m_nativeChannels + 1];
            }
        }
        else {
            // Generic fallback
            for (UINT32 i = 0; i < numFrames; ++i) {
                memcpy(dst + i * 2 * m_bytesPerSample, src + i * m_nativeChannels * m_bytesPerSample, 2 * m_bytesPerSample);
            }
        }
    }

    void CaptureLoop() {
        spdlog::info("Audio capture thread started ({}Hz / {}-bit)", m_sampleRate, m_bitDepth);

        const uint32_t targetFrameSize = m_channels * m_bytesPerSample;
        LARGE_INTEGER qpcFreq;
        QueryPerformanceFrequency(&qpcFreq);
        uint64_t lastPacketEndUs = 0;

        while (m_running) {
            UINT32 nextPacketSize = 0;
            HRESULT hr = m_captureClient->GetNextPacketSize(&nextPacketSize);
            if (FAILED(hr)) break;

            if (nextPacketSize == 0) {
                // Windows 11 idle state: sleep briefly and emit continuous silent frames if endpoint is idle >= 20ms
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                uint64_t nowUs = (qpcNow.QuadPart * 1000000ULL) / qpcFreq.QuadPart;
                if (lastPacketEndUs == 0) {
                    lastPacketEndUs = nowUs;
                } else if (nowUs >= lastPacketEndUs + 20000ULL) {
                    UINT32 silentFrames = m_sampleRate / 50; // 20ms of silence
                    AudioPacket silentPacket;
                    silentPacket.timestamp = lastPacketEndUs;
                    silentPacket.sampleCount = silentFrames;
                    silentPacket.bytesPerSample = m_bytesPerSample;
                    silentPacket.data.assign(static_cast<size_t>(silentFrames) * targetFrameSize, 0);
                    lastPacketEndUs += (static_cast<uint64_t>(silentFrames) * 1000000ULL) / m_sampleRate;
                    {
                        std::lock_guard<std::mutex> lock(m_packetMutex);
                        if (m_packetQueue.size() > 300) m_packetQueue.pop();
                        m_packetQueue.push(std::move(silentPacket));
                    }
                    m_packetAvailable.notify_one();
                }
                continue;
            }

            while (nextPacketSize > 0 && m_running) {
                BYTE* data = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;

                hr = m_captureClient->GetBuffer(&data, &numFramesAvailable, &flags, &devPos, &qpcPos);

                if (SUCCEEDED(hr)) {
                    AudioPacket packet;

                    LARGE_INTEGER qpcNow;
                    QueryPerformanceCounter(&qpcNow);
                    uint64_t nowUs = (qpcNow.QuadPart * 1000000ULL) / qpcFreq.QuadPart;
                    uint64_t durationUs = (static_cast<uint64_t>(numFramesAvailable) * 1000000ULL) / m_sampleRate;
                    if (lastPacketEndUs > 0 && nowUs < lastPacketEndUs + durationUs && lastPacketEndUs <= nowUs + 50000ULL) {
                        packet.timestamp = lastPacketEndUs;
                    } else {
                        packet.timestamp = (nowUs > durationUs) ? (nowUs - durationUs) : nowUs;
                    }
                    lastPacketEndUs = packet.timestamp + durationUs;

                    packet.sampleCount = numFramesAvailable;
                    packet.bytesPerSample = m_bytesPerSample;

                    size_t outputDataSize = static_cast<size_t>(numFramesAvailable) * targetFrameSize;
                    packet.data.resize(outputDataSize);

                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        memset(packet.data.data(), 0, outputDataSize);
                    }
                    else if (data) {
                        DownmixToStereo(data, packet.data.data(), numFramesAvailable);
                    }

                    {
                        std::lock_guard<std::mutex> lock(m_packetMutex);
                        if (m_packetQueue.size() > 300) m_packetQueue.pop();
                        m_packetQueue.push(std::move(packet));
                    }

                    m_packetAvailable.notify_one();
                    m_captureClient->ReleaseBuffer(numFramesAvailable);
                }

                if (FAILED(hr)) break;
                hr = m_captureClient->GetNextPacketSize(&nextPacketSize);
                if (FAILED(hr)) break;
            }
        }
        spdlog::info("Audio capture thread stopped");
    }

    ComPtr<IMMDeviceEnumerator> m_deviceEnumerator;
    ComPtr<IMMDevice> m_device;
    ComPtr<IAudioClient> m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;

    std::atomic<bool> m_running;
    std::thread m_captureThread;

    uint32_t m_sampleRate;
    uint16_t m_bitDepth;
    uint16_t m_bytesPerSample;
    uint16_t m_channels;
    uint16_t m_nativeChannels;
    bool m_isFloat;

    std::queue<AudioPacket> m_packetQueue;
    std::mutex m_packetMutex;
    std::condition_variable m_packetAvailable;
};
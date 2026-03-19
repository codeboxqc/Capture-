#pragma once
#include "RecordingEngine.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <queue>
#include <mutex>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// USB Audio device info
struct USBAudioDeviceInfo {
    std::string name;
    std::string symbolicLink;
    int index;
    bool isCapture;
};

// Audio packet from USB capture
struct USBAudioPacket {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
};

class USBAudioCapture {
public:
    USBAudioCapture()
        : m_running(false)
        , m_sampleRate(48000)
        , m_channels(2)
        , m_bitsPerSample(16)
        , m_comInitialized(false)
        , m_mfInitialized(false)
    {
    }

    ~USBAudioCapture() {
        Stop();
        Shutdown();
    }

    // Enumerate all USB audio capture devices (inputs)
    static std::vector<USBAudioDeviceInfo> EnumerateAudioDevices() {
        std::vector<USBAudioDeviceInfo> devices;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool comInit = SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (comInit) CoUninitialize();
            return devices;
        }

        ComPtr<IMFAttributes> pAttributes;
        hr = MFCreateAttributes(&pAttributes, 1);
        if (FAILED(hr)) {
            MFShutdown();
            if (comInit) CoUninitialize();
            return devices;
        }

        // Get audio capture devices (microphones, line-in, USB audio inputs)
        hr = pAttributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID
        );

        IMFActivate** ppDevices = nullptr;
        UINT32 count = 0;

        hr = MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &count);
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count; i++) {
                USBAudioDeviceInfo info;
                info.index = i;
                info.isCapture = true;

                // Get friendly name
                WCHAR* friendlyName = nullptr;
                UINT32 nameLen = 0;
                hr = ppDevices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                    &friendlyName,
                    &nameLen
                );

                if (SUCCEEDED(hr) && friendlyName) {
                    int size = WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nullptr, 0, nullptr, nullptr);
                    if (size > 0) {
                        info.name.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, &info.name[0], size, nullptr, nullptr);
                    }
                    CoTaskMemFree(friendlyName);
                }

                // Get symbolic link (device path)
                WCHAR* symbolicLink = nullptr;
                UINT32 linkLen = 0;
                hr = ppDevices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_SYMBOLIC_LINK,
                    &symbolicLink,
                    &linkLen
                );

                if (SUCCEEDED(hr) && symbolicLink) {
                    int size = WideCharToMultiByte(CP_UTF8, 0, symbolicLink, -1, nullptr, 0, nullptr, nullptr);
                    if (size > 0) {
                        info.symbolicLink.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, symbolicLink, -1, &info.symbolicLink[0], size, nullptr, nullptr);
                    }
                    CoTaskMemFree(symbolicLink);
                }

                if (!info.name.empty()) {
                    spdlog::info("Found audio capture device [{}]: {}", i, info.name);
                    devices.push_back(info);
                }

                ppDevices[i]->Release();
            }
            CoTaskMemFree(ppDevices);
        }

        MFShutdown();
        if (comInit) CoUninitialize();

        return devices;
    }

    // Find matching audio device for a video capture device
    // Magewell cards typically have matching names like:
    //   Video: "USB3.0 Capture"
    //   Audio: "USB3.0 Capture Audio" or "Digital Audio Interface (USB3.0 Capture)"
    static int FindMatchingAudioDevice(const std::string& videoDeviceName) {
        auto audioDevices = EnumerateAudioDevices();

        // Extract base name from video device (remove suffixes)
        std::string videoNameLower = videoDeviceName;
        std::transform(videoNameLower.begin(), videoNameLower.end(), videoNameLower.begin(), ::tolower);

        // Remove common suffixes for matching
        std::vector<std::string> suffixesToRemove = { " video", " [capture card]", " [webcam]", " [video capture]" };
        for (const auto& suffix : suffixesToRemove) {
            size_t pos = videoNameLower.find(suffix);
            if (pos != std::string::npos) {
                videoNameLower = videoNameLower.substr(0, pos);
            }
        }

        spdlog::info("Looking for audio device matching video device base name: '{}'", videoNameLower);

        // First pass: look for exact match with "audio" suffix
        for (const auto& audioDevice : audioDevices) {
            std::string audioNameLower = audioDevice.name;
            std::transform(audioNameLower.begin(), audioNameLower.end(), audioNameLower.begin(), ::tolower);

            // Check for "USB3.0 Capture Audio" pattern
            if (audioNameLower.find(videoNameLower) != std::string::npos &&
                audioNameLower.find("audio") != std::string::npos) {
                spdlog::info("Found matching audio device (exact match): '{}' (index {})", audioDevice.name, audioDevice.index);
                return audioDevice.index;
            }
        }

        // Second pass: look for any device containing the base name
        for (const auto& audioDevice : audioDevices) {
            std::string audioNameLower = audioDevice.name;
            std::transform(audioNameLower.begin(), audioNameLower.end(), audioNameLower.begin(), ::tolower);

            if (audioNameLower.find(videoNameLower) != std::string::npos) {
                spdlog::info("Found matching audio device (partial match): '{}' (index {})", audioDevice.name, audioDevice.index);
                return audioDevice.index;
            }

            // Check for "Digital Audio Interface (DeviceName)" pattern
            size_t parenStart = audioNameLower.find('(');
            size_t parenEnd = audioNameLower.find(')');
            if (parenStart != std::string::npos && parenEnd != std::string::npos && parenEnd > parenStart) {
                std::string inParens = audioNameLower.substr(parenStart + 1, parenEnd - parenStart - 1);
                if (inParens.find(videoNameLower) != std::string::npos ||
                    videoNameLower.find(inParens) != std::string::npos) {
                    spdlog::info("Found matching audio device (parentheses match): '{}' (index {})", audioDevice.name, audioDevice.index);
                    return audioDevice.index;
                }
            }
        }

        spdlog::warn("No matching audio device found for video device: {}", videoDeviceName);
        return -1;
    }

    // Initialize with device index
    bool Initialize(int deviceIndex) {
        spdlog::info("Initializing USB audio capture for device index {}", deviceIndex);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInitialized = SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            spdlog::error("MFStartup failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
        m_mfInitialized = true;

        // Get audio capture devices
        ComPtr<IMFAttributes> pAttributes;
        hr = MFCreateAttributes(&pAttributes, 1);
        if (FAILED(hr)) {
            spdlog::error("MFCreateAttributes failed");
            Shutdown();
            return false;
        }

        hr = pAttributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID
        );

        IMFActivate** ppDevices = nullptr;
        UINT32 count = 0;

        hr = MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &count);
        if (FAILED(hr) || count == 0) {
            spdlog::error("No audio capture devices found");
            Shutdown();
            return false;
        }

        if (deviceIndex < 0 || deviceIndex >= (int)count) {
            spdlog::error("Invalid device index: {} (available: {})", deviceIndex, count);
            for (UINT32 i = 0; i < count; i++) ppDevices[i]->Release();
            CoTaskMemFree(ppDevices);
            Shutdown();
            return false;
        }

        // Get device name for logging
        WCHAR* friendlyName = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(ppDevices[deviceIndex]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLen))) {
            int size = WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (size > 0) {
                std::string name(size - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, &name[0], size, nullptr, nullptr);
                spdlog::info("Opening USB audio device: {}", name);
            }
            CoTaskMemFree(friendlyName);
        }

        // Activate the selected device
        hr = ppDevices[deviceIndex]->ActivateObject(
            IID_PPV_ARGS(&m_mediaSource)
        );

        for (UINT32 i = 0; i < count; i++) ppDevices[i]->Release();
        CoTaskMemFree(ppDevices);

        if (FAILED(hr) || !m_mediaSource) {
            spdlog::error("Failed to activate audio source: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }

        // Create source reader
        ComPtr<IMFAttributes> readerAttrs;
        MFCreateAttributes(&readerAttrs, 1);

        hr = MFCreateSourceReaderFromMediaSource(
            m_mediaSource.Get(),
            readerAttrs.Get(),
            &m_sourceReader
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create audio source reader: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }





         


        // Configure output format (PCM 48kHz 16-bit stereo)
        ComPtr<IMFMediaType> mediaType;
        hr = MFCreateMediaType(&mediaType);
        if (SUCCEEDED(hr)) {
            mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            

            mediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);

            hr = m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mediaType.Get());
           

            if (FAILED(hr)) {
                spdlog::warn("PCM 48kHz/16bit/stereo not supported, trying default format");
            }
        }

        // Get actual format
        ComPtr<IMFMediaType> currentType;
        hr = m_sourceReader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            &currentType
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to get audio media type");
            Shutdown();
            return false;
        }

        // Extract format info
        UINT32 sampleRate = 0, channels = 0, bitsPerSample = 0;
        currentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
        currentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        currentType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);

        m_sampleRate = sampleRate > 0 ? sampleRate : 48000;
        m_channels = channels > 0 ? (uint16_t)channels : 2;
        m_bitsPerSample = bitsPerSample > 0 ? (uint16_t)bitsPerSample : 16;

        spdlog::info("USB audio initialized: {}Hz, {} channels, {} bits", m_sampleRate, m_channels, m_bitsPerSample);
        return true;
    }

    bool Start(uint32_t bufferSize = 64) {
        if (!m_sourceReader) {
            spdlog::error("USB audio not initialized");
            return false;
        }

        m_bufferSize = bufferSize;
        m_running = true;
        m_captureThread = std::thread(&USBAudioCapture::CaptureThreadFunc, this);

        return true;
    }

    void Stop() {
        if (!m_running) return;

        m_running = false;
        m_packetAvailable.notify_all();

        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_packetQueue.empty()) m_packetQueue.pop();
    }

    bool GetNextPacket(USBAudioPacket& packet, uint32_t timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_packetQueue.empty()) {
            if (!m_packetAvailable.wait_for(
                lock,
                std::chrono::milliseconds(timeoutMs),
                [this] { return !m_packetQueue.empty() || !m_running; }
            )) {
                return false;
            }
        }

        if (m_packetQueue.empty()) return false;

        packet = std::move(m_packetQueue.front());
        m_packetQueue.pop();
        return true;
    }

    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint16_t GetChannels() const { return m_channels; }
    uint16_t GetBitDepth() const { return m_bitsPerSample; }
    bool IsRunning() const { return m_running; }

private:
    void Shutdown() {
        if (m_sourceReader) {
            m_sourceReader->Flush(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
            m_sourceReader.Reset();
        }

        if (m_mediaSource) {
            m_mediaSource->Shutdown();
            m_mediaSource.Reset();
        }

        if (m_mfInitialized) {
            MFShutdown();
            m_mfInitialized = false;
        }

        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    void CaptureThreadFunc() {
        spdlog::info("USB audio capture thread started ({}Hz, {} ch, {} bit)", m_sampleRate, m_channels, m_bitsPerSample);

        while (m_running) {
            IMFSample* sample = nullptr;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;

            HRESULT hr = m_sourceReader->ReadSample(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                0,
                nullptr,
                &streamFlags,
                &timestamp,
                &sample
            );

            if (FAILED(hr)) {
                spdlog::warn("Audio ReadSample failed: 0x{:08X}", static_cast<uint32_t>(hr));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (streamFlags & MF_SOURCE_READERF_ERROR) {
                spdlog::error("Audio stream error");
                break;
            }

            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                spdlog::info("Audio end of stream");
                break;
            }

            if (!sample) continue;

            // Get buffer
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer(&buffer);
            if (FAILED(hr)) {
                sample->Release();
                continue;
            }

            BYTE* data = nullptr;
            DWORD length = 0;
            hr = buffer->Lock(&data, nullptr, &length);
            if (FAILED(hr) || !data || length == 0) {
                sample->Release();
                continue;
            }

            // Create packet
            USBAudioPacket packet;
            packet.data.assign(data, data + length);
            packet.timestamp = GetCurrentTimestamp();
            packet.sampleRate = m_sampleRate;
            packet.channels = m_channels;
            packet.bitsPerSample = m_bitsPerSample;

            buffer->Unlock();
            sample->Release();

            // Add to queue
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (m_packetQueue.size() >= m_bufferSize) {
                    m_packetQueue.pop();  // Drop oldest
                }

                m_packetQueue.push(std::move(packet));
            }

            m_packetAvailable.notify_one();
        }

        spdlog::info("USB audio capture thread stopped");
    }

    uint64_t GetCurrentTimestamp() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    ComPtr<IMFMediaSource> m_mediaSource;
    ComPtr<IMFSourceReader> m_sourceReader;

    std::atomic<bool> m_running;
    std::thread m_captureThread;

    uint32_t m_sampleRate;
    uint16_t m_channels;
    uint16_t m_bitsPerSample;
    uint32_t m_bufferSize;

    bool m_comInitialized;
    bool m_mfInitialized;

    std::queue<USBAudioPacket> m_packetQueue;
    std::mutex m_mutex;
    std::condition_variable m_packetAvailable;
};
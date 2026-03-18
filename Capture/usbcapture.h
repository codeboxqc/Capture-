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

// Simple USB Frame structure
struct USBFrame {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t timestamp;
    uint32_t frameIndex;
};

// Simplified USB Capture Device Info
struct USBCaptureDevice {
    std::string name;
    int index;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

class SimpleUSBCapture {
public:
    SimpleUSBCapture()
        : m_running(false)
        , m_frameCount(0)
        , m_width(0)
        , m_height(0)
        , m_comInitialized(false)
        , m_mfInitialized(false)
    {
    }

    ~SimpleUSBCapture() {
        Stop();
        Shutdown();
    }

    // ========== PUBLIC API ==========

    // List all available USB capture devices
    static std::vector<USBCaptureDevice> EnumerateDevices() {
        std::vector<USBCaptureDevice> devices;

        // Initialize COM for enumeration
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needsUninit = SUCCEEDED(hr);

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (needsUninit) CoUninitialize();
            return devices;
        }

        // Create enumeration attributes
        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr)) {
            attributes->SetGUID(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
            );

            // Enumerate
            IMFActivate** activateArray = nullptr;
            UINT32 count = 0;
            hr = MFEnumDeviceSources(attributes.Get(), &activateArray, &count);

            if (SUCCEEDED(hr)) {
                for (UINT32 i = 0; i < count; i++) {
                    USBCaptureDevice device;
                    device.index = i;

                    // Get friendly name
                    WCHAR* name = nullptr;
                    UINT32 nameLen = 0;
                    if (SUCCEEDED(activateArray[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen))) {

                        int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
                        device.name.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, name, -1, &device.name[0], size, nullptr, nullptr);
                        CoTaskMemFree(name);
                    }

                    // Try to get resolution (this requires activating the device)
                    ComPtr<IMFMediaSource> source;
                    if (SUCCEEDED(activateArray[i]->ActivateObject(IID_PPV_ARGS(&source)))) {
                        ComPtr<IMFSourceReader> reader;
                        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
                            ComPtr<IMFMediaType> mediaType;
                            if (SUCCEEDED(reader->GetNativeMediaType(
                                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &mediaType))) {

                                UINT32 width = 0, height = 0;
                                MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);
                                device.width = width;
                                device.height = height;

                                // Get framerate
                                UINT32 fpsNum = 0, fpsDen = 1;
                                MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
                                device.fps = fpsDen > 0 ? fpsNum / fpsDen : 30;
                            }
                        }
                        source->Shutdown();
                    }

                    devices.push_back(device);
                    activateArray[i]->Release();
                }
                CoTaskMemFree(activateArray);
            }
        }

        MFShutdown();
        if (needsUninit) CoUninitialize();

        return devices;
    }

    // Initialize with specific device
    bool Initialize(int deviceIndex, ComPtr<ID3D11Device> d3d11Device) {
        if (!d3d11Device) {
            spdlog::error("SimpleUSBCapture: NULL D3D11 device");
            return false;
        }

        m_d3d11Device = d3d11Device;
        m_d3d11Device->GetImmediateContext(&m_d3d11Context);

        // Initialize COM
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            m_comInitialized = true;
        }
        else if (hr != RPC_E_CHANGED_MODE) {
            spdlog::error("COM init failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // Initialize Media Foundation
        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            spdlog::error("MFStartup failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }
        m_mfInitialized = true;

        // Create attributes
        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 1);
        if (FAILED(hr)) {
            spdlog::error("MFCreateAttributes failed");
            Shutdown();
            return false;
        }

        attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
        );

        // Enumerate devices
        IMFActivate** activateArray = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attributes.Get(), &activateArray, &count);
        if (FAILED(hr) || count == 0) {
            spdlog::error("No USB capture devices found");
            Shutdown();
            return false;
        }

        spdlog::info("Found {} USB capture device(s)", count);

        // Validate device index
        if (deviceIndex < 0 || deviceIndex >= (int)count) {
            spdlog::error("Invalid device index: {}", deviceIndex);
            for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
            CoTaskMemFree(activateArray);
            Shutdown();
            return false;
        }

        // Activate selected device
        hr = activateArray[deviceIndex]->ActivateObject(IID_PPV_ARGS(&m_mediaSource));

        // Log device name
        WCHAR* deviceName = nullptr;
        if (SUCCEEDED(activateArray[deviceIndex]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &deviceName, nullptr))) {
            int size = WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, nullptr, 0, nullptr, nullptr);
            std::string name(size - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, &name[0], size, nullptr, nullptr);
            spdlog::info("Opening USB device: {}", name);
            CoTaskMemFree(deviceName);
        }

        // Cleanup activate array
        for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
        CoTaskMemFree(activateArray);

        if (FAILED(hr)) {
            spdlog::error("Failed to activate device: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }

        // Create source reader
        ComPtr<IMFAttributes> readerAttrs;
        MFCreateAttributes(&readerAttrs, 2);
        if (readerAttrs) {
            readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }

        hr = MFCreateSourceReaderFromMediaSource(
            m_mediaSource.Get(),
            readerAttrs.Get(),
            &m_sourceReader
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create source reader: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }

        // Configure output format (RGB32 for easy D3D11 interop)
        ComPtr<IMFMediaType> mediaType;
        hr = MFCreateMediaType(&mediaType);
        if (SUCCEEDED(hr)) {
            mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

            hr = m_sourceReader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                nullptr,
                mediaType.Get()
            );

            if (FAILED(hr)) {
                spdlog::warn("RGB32 not supported, using default format");
            }
        }

        // Get actual format
        ComPtr<IMFMediaType> currentType;
        hr = m_sourceReader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            &currentType
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to get media type");
            Shutdown();
            return false;
        }

        // Get resolution
        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr) || width == 0 || height == 0) {
            spdlog::error("Invalid resolution");
            Shutdown();
            return false;
        }

        m_width = width;
        m_height = height;

        spdlog::info("USB capture initialized: {}x{}", m_width, m_height);
        return true;
    }

    // Start capturing frames
    bool Start(uint32_t bufferSize = 16) {
        if (!m_sourceReader) {
            spdlog::error("Not initialized");
            return false;
        }

        m_bufferSize = bufferSize;
        m_running = true;
        m_captureThread = std::thread(&SimpleUSBCapture::CaptureThreadFunc, this);

        return true;
    }

    // Stop capturing
    void Stop() {
        if (!m_running) return;

        m_running = false;
        m_frameAvailable.notify_all();

        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }

        // Clear buffer
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_frameQueue.empty()) m_frameQueue.pop();
        }

        // Clear texture pool
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            while (!m_texturePool.empty()) m_texturePool.pop();
        }
    }

    // Get next captured frame (blocking with timeout)
    bool GetFrame(USBFrame& frame, uint32_t timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_frameQueue.empty()) {
            if (!m_frameAvailable.wait_for(
                lock,
                std::chrono::milliseconds(timeoutMs),
                [this] { return !m_frameQueue.empty() || !m_running; }
            )) {
                return false;
            }
        }

        if (m_frameQueue.empty()) return false;

        frame = m_frameQueue.front();
        m_frameQueue.pop();
        return true;
    }

    // Return texture to pool for reuse (Fixes memory leak)
    void ReturnTexture(ComPtr<ID3D11Texture2D> texture) {
        if (!texture) return;
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_texturePool.push(texture);
    }

    // Get current resolution
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool IsRunning() const { return m_running; }

private:
    // Cleanup all resources
    void Shutdown() {
        if (m_sourceReader) {
            m_sourceReader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
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

        m_d3d11Context.Reset();
        m_d3d11Device.Reset();
    }

    // Capture thread
    void CaptureThreadFunc() {
        spdlog::info("USB capture thread started");

        while (m_running) {
            IMFSample* sample = nullptr;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;

            HRESULT hr = m_sourceReader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                nullptr,
                &streamFlags,
                &timestamp,
                &sample
            );

            // Handle errors
            if (FAILED(hr)) {
                spdlog::warn("ReadSample failed: 0x{:08X}", static_cast<uint32_t>(hr));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Handle stream flags
            if (streamFlags & MF_SOURCE_READERF_ERROR) {
                spdlog::error("Stream error");
                break;
            }

            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                spdlog::info("End of stream");
                break;
            }

            if (streamFlags & MF_SOURCE_READERF_STREAMTICK) {
                continue; // No data
            }

            if (!sample) continue;

            // Convert to D3D11 texture
            ComPtr<ID3D11Texture2D> texture = ConvertToTexture(sample);
            sample->Release();

            if (texture) {
                USBFrame frame;
                frame.texture = texture;
                frame.timestamp = GetCurrentTimestamp();
                frame.frameIndex = m_frameCount++;

                // Add to queue
                {
                    std::lock_guard<std::mutex> lock(m_mutex);

                    // Drop oldest if full
                    if (m_frameQueue.size() >= m_bufferSize) {
                        ReturnTexture(m_frameQueue.front().texture);
                        m_frameQueue.pop();
                    }

                    m_frameQueue.push(frame);
                }

                m_frameAvailable.notify_one();
            }
        }

        spdlog::info("USB capture thread stopped");
    }

    // Convert Media Foundation sample to D3D11 texture
    ComPtr<ID3D11Texture2D> ConvertToTexture(IMFSample* sample) {
        if (!sample || !m_d3d11Device) return nullptr;

        // Get buffer
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) return nullptr;

        // Lock buffer
        BYTE* data = nullptr;
        DWORD length = 0;
        hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr) || !data) return nullptr;

        // Validate size (RGB32 = 4 bytes per pixel)
        DWORD expectedSize = m_width * m_height * 4;
        if (length < expectedSize) {
            spdlog::warn("Buffer size mismatch: {} < {}", length, expectedSize);
            buffer->Unlock();
            return nullptr;
        }

        // Get or Create D3D11 texture
        ComPtr<ID3D11Texture2D> texture;
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (!m_texturePool.empty()) {
                texture = m_texturePool.front();
                m_texturePool.pop();

                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc(&desc);
                if (desc.Width != m_width || desc.Height != m_height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                    texture.Reset(); // Wrong size/format, create new
                }
            }
        }

        if (!texture) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = m_width;
            desc.Height = m_height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            hr = m_d3d11Device->CreateTexture2D(&desc, nullptr, &texture);
            if (FAILED(hr)) {
                buffer->Unlock();
                return nullptr;
            }
        }

        // Update texture data
        m_d3d11Context->UpdateSubresource(texture.Get(), 0, nullptr, data, m_width * 4, 0);

        buffer->Unlock();
        return texture;
    }

    uint64_t GetCurrentTimestamp() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    // Member variables
    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    ComPtr<IMFMediaSource> m_mediaSource;
    ComPtr<IMFSourceReader> m_sourceReader;

    std::atomic<bool> m_running;
    std::thread m_captureThread;

    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_bufferSize;
    uint64_t m_frameCount;

    bool m_comInitialized;
    bool m_mfInitialized;

    std::queue<USBFrame> m_frameQueue;
    std::mutex m_mutex;
    std::condition_variable m_frameAvailable;

    // Texture Pool (Fixes memory leak)
    std::queue<ComPtr<ID3D11Texture2D>> m_texturePool;
    std::mutex m_poolMutex;
};
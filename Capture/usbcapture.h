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
    bool isKeyframe;
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
        , m_fps(30)
        , m_comInitialized(false)
        , m_mfInitialized(false)
        , m_bufferSize(8)
        , m_bytesPerPixel(4)
        , m_isYUY2(false)
        , m_isNV12(false)
        , m_noSignalCount(0)
        , m_hasSignal(false)
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

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needsUninit = SUCCEEDED(hr);

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (needsUninit) CoUninitialize();
            return devices;
        }

        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr)) {
            attributes->SetGUID(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
            );

            IMFActivate** activateArray = nullptr;
            UINT32 count = 0;
            hr = MFEnumDeviceSources(attributes.Get(), &activateArray, &count);

            if (SUCCEEDED(hr)) {
                for (UINT32 i = 0; i < count; i++) {
                    USBCaptureDevice device;
                    device.index = i;

                    WCHAR* name = nullptr;
                    UINT32 nameLen = 0;
                    if (SUCCEEDED(activateArray[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen))) {

                        int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
                        device.name.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, name, -1, &device.name[0], size, nullptr, nullptr);
                        CoTaskMemFree(name);
                    }

                    ComPtr<IMFMediaSource> source;
                    if (SUCCEEDED(activateArray[i]->ActivateObject(IID_PPV_ARGS(&source)))) {
                        ComPtr<IMFSourceReader> reader;
                        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
                            ComPtr<IMFMediaType> mediaType;
                            if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &mediaType))) {
                                UINT32 width = 0, height = 0;
                                MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);

                                UINT32 fpsNum = 0, fpsDen = 1;
                                MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);

                                device.width = width;
                                device.height = height;
                                device.fps = (fpsDen > 0) ? (fpsNum / fpsDen) : 30;
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

    // Initialize with device index and D3D11 device
    bool Initialize(int deviceIndex, ComPtr<ID3D11Device> d3d11Device) {
        if (!d3d11Device) {
            spdlog::error("D3D11 device is null");
            return false;
        }

        m_d3d11Device = d3d11Device;
        m_d3d11Device->GetImmediateContext(&m_d3d11Context);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            m_comInitialized = true;
        }
        else if (hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
            spdlog::error("COM init failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            spdlog::error("MFStartup failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }
        m_mfInitialized = true;

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

        IMFActivate** activateArray = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attributes.Get(), &activateArray, &count);
        if (FAILED(hr) || count == 0) {
            spdlog::error("No USB capture devices found");
            Shutdown();
            return false;
        }

        spdlog::info("Found {} USB capture device(s)", count);

        if (deviceIndex < 0 || deviceIndex >= (int)count) {
            spdlog::error("Invalid device index: {}", deviceIndex);
            for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
            CoTaskMemFree(activateArray);
            Shutdown();
            return false;
        }

        hr = activateArray[deviceIndex]->ActivateObject(IID_PPV_ARGS(&m_mediaSource));

        WCHAR* deviceName = nullptr;
        if (SUCCEEDED(activateArray[deviceIndex]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &deviceName, nullptr))) {
            int size = WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, nullptr, 0, nullptr, nullptr);
            std::string name(size - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, &name[0], size, nullptr, nullptr);
            spdlog::info("Opening USB device: {}", name);
            CoTaskMemFree(deviceName);
        }

        for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
        CoTaskMemFree(activateArray);

        if (FAILED(hr)) {
            spdlog::error("Failed to activate device: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }

        // Create source reader with LOW LATENCY and VIDEO PROCESSING (for format conversion)
        ComPtr<IMFAttributes> readerAttrs;
        hr = MFCreateAttributes(&readerAttrs, 3);
        if (SUCCEEDED(hr)) {
            readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            // Enable video processing to allow format conversion
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

        // Get native format info first
        ComPtr<IMFMediaType> nativeType;
        hr = m_sourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType);
        if (SUCCEEDED(hr)) {
            GUID subtype;
            nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);

            if (subtype == MFVideoFormat_YUY2) {
                spdlog::info("Native format: YUY2");
            }
            else if (subtype == MFVideoFormat_NV12) {
                spdlog::info("Native format: NV12");
            }
            else if (subtype == MFVideoFormat_RGB32) {
                spdlog::info("Native format: RGB32");
            }
            else {
                spdlog::info("Native format: Other");
            }
        }

        // Try to set RGB32 output format (MF will convert if possible)
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
                spdlog::warn("RGB32 not supported, trying NV12...");

                // Try NV12
                mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                hr = m_sourceReader->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                    nullptr,
                    mediaType.Get()
                );

                if (FAILED(hr)) {
                    spdlog::warn("NV12 not supported, using native format");
                }
            }
        }

        // Get actual output format
        ComPtr<IMFMediaType> currentType;
        hr = m_sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType);
        if (FAILED(hr)) {
            spdlog::error("Failed to get current media type");
            Shutdown();
            return false;
        }

        GUID actualSubtype;
        currentType->GetGUID(MF_MT_SUBTYPE, &actualSubtype);

        if (actualSubtype == MFVideoFormat_RGB32) {
            m_bytesPerPixel = 4;
            m_isYUY2 = false;
            m_isNV12 = false;
            spdlog::info("Output format: RGB32 (4 bytes/pixel)");
        }
        else if (actualSubtype == MFVideoFormat_YUY2) {
            m_bytesPerPixel = 2;
            m_isYUY2 = true;
            m_isNV12 = false;
            spdlog::info("Output format: YUY2 (2 bytes/pixel) - will convert to BGRA");
        }
        else if (actualSubtype == MFVideoFormat_NV12) {
            m_bytesPerPixel = 1;  // NV12 is 1.5 bytes per pixel on average
            m_isYUY2 = false;
            m_isNV12 = true;
            spdlog::info("Output format: NV12 (1.5 bytes/pixel) - will convert to BGRA");
        }
        else {
            // Unknown format, assume RGB32
            m_bytesPerPixel = 4;
            m_isYUY2 = false;
            m_isNV12 = false;
            spdlog::warn("Unknown format, assuming RGB32");
        }

        // Get resolution and frame rate
        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr) || width == 0 || height == 0) {
            spdlog::error("Invalid resolution");
            Shutdown();
            return false;
        }

        m_width = width;
        m_height = height;

        UINT32 fpsNum = 0, fpsDen = 1;
        MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        m_fps = (fpsDen > 0) ? (fpsNum / fpsDen) : 30;

        spdlog::info("USB capture initialized: {}x{} @ {}fps", m_width, m_height, m_fps);
        return true;
    }

    // Start capturing frames
    bool Start(uint32_t bufferSize = 16) {
        if (!m_sourceReader) {
            spdlog::error("Not initialized");
            return false;
        }

        // Flush any old buffered frames before starting
        m_sourceReader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

        m_bufferSize = bufferSize;
        m_running = true;
        m_noSignalCount = 0;
        m_hasSignal = false;
        m_captureThread = std::thread(&SimpleUSBCapture::CaptureThreadFunc, this);
        SetThreadPriority(m_captureThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);

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

        spdlog::info("USB capture stopped. Total frames: {}", m_frameCount.load());
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

    // Return texture to pool for reuse
    void ReturnTexture(ComPtr<ID3D11Texture2D> texture) {
        if (!texture) return;

        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (m_texturePool.size() < m_bufferSize * 2) {
            m_texturePool.push(texture);
        }
    }

    // Get current resolution
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetFPS() const { return m_fps; }
    bool IsRunning() const { return m_running; }
    uint64_t GetFrameCount() const { return m_frameCount; }
    bool HasSignal() const { return m_hasSignal; }

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

    // Capture thread with NO-SIGNAL TIMEOUT handling
    void CaptureThreadFunc() {
        spdlog::info("USB capture thread started");

        // Track consecutive failures for no-signal detection
        const int NO_SIGNAL_THRESHOLD = 60;  // ~1 second at 60fps
        const int NO_SIGNAL_LOG_INTERVAL = 180;  // Log every ~3 seconds
        int consecutiveEmpty = 0;
        bool loggedNoSignal = false;

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

            // Check if we should stop
            if (!m_running) break;

            // Handle specific error codes
            if (FAILED(hr)) {
                // MF_E_HW_MFT_FAILED_START_STREAMING = 0xc00d4e85
                if (hr == static_cast<HRESULT>(0xc00d4e85)) {
                    spdlog::error("Hardware capture failed to start - device may be in use");
                    break;
                }

                consecutiveEmpty++;
                if (consecutiveEmpty >= NO_SIGNAL_THRESHOLD && !loggedNoSignal) {
                    spdlog::warn("No signal detected from capture device (waiting for input...)");
                    loggedNoSignal = true;
                    m_hasSignal = false;
                }

                // Don't spam logs, but check periodically
                if (consecutiveEmpty % NO_SIGNAL_LOG_INTERVAL == 0) {
                    spdlog::debug("Still waiting for signal... ({}ms)", consecutiveEmpty * 16);
                }

                // Short sleep to avoid CPU spinning
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            // Handle stream flags
            if (streamFlags & MF_SOURCE_READERF_ERROR) {
                spdlog::error("Stream error detected");
                consecutiveEmpty++;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                spdlog::info("End of stream");
                break;
            }

            // STREAMTICK means no data this frame (common with no signal)
            if (streamFlags & MF_SOURCE_READERF_STREAMTICK) {
                consecutiveEmpty++;
                if (consecutiveEmpty >= NO_SIGNAL_THRESHOLD && !loggedNoSignal) {
                    spdlog::warn("No signal - capture device not receiving input");
                    loggedNoSignal = true;
                    m_hasSignal = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            // No sample returned
            if (!sample) {
                consecutiveEmpty++;
                if (consecutiveEmpty >= NO_SIGNAL_THRESHOLD && !loggedNoSignal) {
                    spdlog::warn("No signal - no frames from capture device");
                    loggedNoSignal = true;
                    m_hasSignal = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            // We got a valid sample!
            if (!m_hasSignal || loggedNoSignal) {
                spdlog::info("Signal detected - capture started");
                m_hasSignal = true;
                loggedNoSignal = false;
            }
            consecutiveEmpty = 0;

            // Convert to D3D11 texture
            ComPtr<ID3D11Texture2D> texture = ConvertToTexture(sample);
            sample->Release();

            if (texture) {
                USBFrame frame;
                frame.texture = texture;
                frame.timestamp = GetCurrentTimestamp();
                frame.frameIndex = static_cast<uint32_t>(m_frameCount++);
                frame.isKeyframe = (frame.frameIndex % (m_fps * 2) == 0);

                // DEADLOCK FIX - save texture to return OUTSIDE the lock
                ComPtr<ID3D11Texture2D> textureToReturn = nullptr;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);

                    // Drop oldest if full
                    if (m_frameQueue.size() >= m_bufferSize) {
                        textureToReturn = m_frameQueue.front().texture;
                        m_frameQueue.pop();
                    }

                    m_frameQueue.push(frame);
                }

                // Return texture OUTSIDE the lock to avoid deadlock
                if (textureToReturn) {
                    ReturnTexture(textureToReturn);
                }

                m_frameAvailable.notify_one();
            }
        }

        spdlog::info("USB capture thread stopped. Frames captured: {}", m_frameCount.load());
    }

    // Convert YUY2 to BGRA
    void ConvertYUY2ToBGRA(const BYTE* yuy2Data, BYTE* bgraData, uint32_t width, uint32_t height) {
        const uint32_t yuy2Stride = width * 2;
        const uint32_t bgraStride = width * 4;

        for (uint32_t y = 0; y < height; y++) {
            const BYTE* yuy2Row = yuy2Data + (y * yuy2Stride);
            BYTE* bgraRow = bgraData + (y * bgraStride);

            for (uint32_t x = 0; x < width; x += 2) {
                int Y0 = yuy2Row[0];
                int U = yuy2Row[1];
                int Y1 = yuy2Row[2];
                int V = yuy2Row[3];
                yuy2Row += 4;

                int C0 = Y0 - 16;
                int C1 = Y1 - 16;
                int D = U - 128;
                int E = V - 128;

                int R0 = (298 * C0 + 409 * E + 128) >> 8;
                int G0 = (298 * C0 - 100 * D - 208 * E + 128) >> 8;
                int B0 = (298 * C0 + 516 * D + 128) >> 8;

                int R1 = (298 * C1 + 409 * E + 128) >> 8;
                int G1 = (298 * C1 - 100 * D - 208 * E + 128) >> 8;
                int B1 = (298 * C1 + 516 * D + 128) >> 8;

                bgraRow[0] = (BYTE)(B0 < 0 ? 0 : (B0 > 255 ? 255 : B0));
                bgraRow[1] = (BYTE)(G0 < 0 ? 0 : (G0 > 255 ? 255 : G0));
                bgraRow[2] = (BYTE)(R0 < 0 ? 0 : (R0 > 255 ? 255 : R0));
                bgraRow[3] = 255;

                bgraRow[4] = (BYTE)(B1 < 0 ? 0 : (B1 > 255 ? 255 : B1));
                bgraRow[5] = (BYTE)(G1 < 0 ? 0 : (G1 > 255 ? 255 : G1));
                bgraRow[6] = (BYTE)(R1 < 0 ? 0 : (R1 > 255 ? 255 : R1));
                bgraRow[7] = 255;

                bgraRow += 8;
            }
        }
    }

    // Convert NV12 to BGRA
    void ConvertNV12ToBGRA(const BYTE* nv12Data, BYTE* bgraData, uint32_t width, uint32_t height) {
        const BYTE* yPlane = nv12Data;
        const BYTE* uvPlane = nv12Data + (width * height);
        const uint32_t bgraStride = width * 4;

        for (uint32_t y = 0; y < height; y++) {
            const BYTE* yRow = yPlane + (y * width);
            const BYTE* uvRow = uvPlane + ((y / 2) * width);
            BYTE* bgraRow = bgraData + (y * bgraStride);

            for (uint32_t x = 0; x < width; x += 2) {
                int Y0 = yRow[0];
                int U = uvRow[0];
                int V = uvRow[1];
                int Y1 = yRow[1];

                int C0 = Y0 - 16;
                int C1 = Y1 - 16;
                int D = U - 128;
                int E = V - 128;

                int R0 = (298 * C0 + 409 * E + 128) >> 8;
                int G0 = (298 * C0 - 100 * D - 208 * E + 128) >> 8;
                int B0 = (298 * C0 + 516 * D + 128) >> 8;

                int R1 = (298 * C1 + 409 * E + 128) >> 8;
                int G1 = (298 * C1 - 100 * D - 208 * E + 128) >> 8;
                int B1 = (298 * C1 + 516 * D + 128) >> 8;

                bgraRow[0] = (BYTE)(B0 < 0 ? 0 : (B0 > 255 ? 255 : B0));
                bgraRow[1] = (BYTE)(G0 < 0 ? 0 : (G0 > 255 ? 255 : G0));
                bgraRow[2] = (BYTE)(R0 < 0 ? 0 : (R0 > 255 ? 255 : R0));
                bgraRow[3] = 255;

                bgraRow[4] = (BYTE)(B1 < 0 ? 0 : (B1 > 255 ? 255 : B1));
                bgraRow[5] = (BYTE)(G1 < 0 ? 0 : (G1 > 255 ? 255 : G1));
                bgraRow[6] = (BYTE)(R1 < 0 ? 0 : (R1 > 255 ? 255 : R1));
                bgraRow[7] = 255;

                yRow += 2;
                uvRow += 2;
                bgraRow += 8;
            }
        }
    }

    // Convert Media Foundation sample to D3D11 texture
    ComPtr<ID3D11Texture2D> ConvertToTexture(IMFSample* sample) {
        if (!sample || !m_d3d11Device) return nullptr;

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) return nullptr;

        BYTE* data = nullptr;
        DWORD length = 0;
        hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr) || !data) return nullptr;

        // Calculate expected size based on actual format
        DWORD expectedSize = m_width * m_height * m_bytesPerPixel;
        if (m_isNV12) {
            expectedSize = m_width * m_height * 3 / 2;
        }

        if (length < expectedSize) {
            spdlog::warn("Buffer size mismatch: {} < {} (format bpp={})", length, expectedSize, m_bytesPerPixel);
            buffer->Unlock();
            return nullptr;
        }

        // Get or create D3D11 texture
        ComPtr<ID3D11Texture2D> texture;
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (!m_texturePool.empty()) {
                texture = m_texturePool.front();
                m_texturePool.pop();

                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc(&desc);
                if (desc.Width != m_width || desc.Height != m_height) {
                    texture = nullptr;
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
                spdlog::warn("CreateTexture2D failed: 0x{:08X}", static_cast<uint32_t>(hr));
                buffer->Unlock();
                return nullptr;
            }
        }

        // Convert and upload
        if (m_isYUY2) {
            std::vector<BYTE> bgraBuffer(m_width * m_height * 4);
            ConvertYUY2ToBGRA(data, bgraBuffer.data(), m_width, m_height);
            m_d3d11Context->UpdateSubresource(texture.Get(), 0, nullptr, bgraBuffer.data(), m_width * 4, 0);
        }
        else if (m_isNV12) {
            std::vector<BYTE> bgraBuffer(m_width * m_height * 4);
            ConvertNV12ToBGRA(data, bgraBuffer.data(), m_width, m_height);
            m_d3d11Context->UpdateSubresource(texture.Get(), 0, nullptr, bgraBuffer.data(), m_width * 4, 0);
        }
        else {
            m_d3d11Context->UpdateSubresource(texture.Get(), 0, nullptr, data, m_width * 4, 0);
        }

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
    uint32_t m_fps;
    uint32_t m_bufferSize;
    std::atomic<uint64_t> m_frameCount;

    bool m_comInitialized;
    bool m_mfInitialized;

    uint32_t m_bytesPerPixel;
    bool m_isYUY2;
    bool m_isNV12;

    // No-signal detection
    std::atomic<int> m_noSignalCount;
    std::atomic<bool> m_hasSignal;

    std::queue<USBFrame> m_frameQueue;
    std::mutex m_mutex;
    std::condition_variable m_frameAvailable;

    std::queue<ComPtr<ID3D11Texture2D>> m_texturePool;
    std::mutex m_poolMutex;
};
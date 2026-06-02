#pragma once
#include "RecordingEngine.h"
#include "GPUDetector.h"
#include "VirtualDisplayManager.h"
#include "FrameCapture.h"
#include "usbcapture.h"
#include "camera.h"
#include "USBAudioCapture.h"
#include "AudioCapture.h"
#include "HardwareEncoder.h"
#include "DiskWriter.h"
#include "Mouse.h"
#include <d3d11_4.h>
#include <d3d10.h> 
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <memory>

class RecordingPipeline : public IRecordingEngine {
public:
    RecordingPipeline() : m_recording(false), m_droppedFrames(0), m_systemMemory(0),
        m_isSameAdapter(true), m_isUSBCapture(false), m_recordingStartTime(0), m_firstVideoTimestamp(0) {
    }

    ~RecordingPipeline() {
        Shutdown();
    }

    bool Initialize() override {
        SetupLogging();
        spdlog::info("Initializing Recording Engine");

        m_gpuDetector = std::make_unique<GPUDetector>();
        m_displayManager = std::make_unique<VirtualDisplayManager>();

        if (!m_displayManager->Initialize()) {
            spdlog::warn("Virtual display driver not installed");
        }

        m_systemMemory = m_gpuDetector->GetSystemMemory();
        return true;
    }

    void Shutdown() override {
        StopRecording();

        m_frameCapture.reset();
        m_usbCapture.reset();
        m_cameraCapture.reset();
        m_usbAudioCapture.reset();
        m_audioCapture.reset();
        m_encoder.reset();
        m_diskWriter.reset();

        m_sharedD3D11Context.Reset();
        m_sharedD3D11Device.Reset();
        m_d3d12Device.Reset();

        m_captureD3D11Device.Reset();
        m_captureD3D11Context.Reset();
        m_crossAdapterEncodeTexture.Reset();
        m_captureStagingTexture.Reset();
    }

    bool StartRecording(const RecordingSettings& settings) override {
        if (m_recording) StopRecording();

        m_settings = settings;
        m_droppedFrames = 0;

        m_isUSBCapture = (settings.usbDeviceIndex >= 0);
        m_isCameraCapture = (!settings.cameraUrl.empty());

        if (m_isCameraCapture) {
            spdlog::info("=== CAMERA CAPTURE MODE SELECTED ===");
            return StartCameraRecording(settings);
        }
        else if (m_isUSBCapture) {
            spdlog::info("=== USB CAPTURE MODE SELECTED ===");
            return StartUSBRecording(settings);
        }
        else {
            spdlog::info("=== DISPLAY CAPTURE MODE SELECTED ===");
            return StartDisplayRecording(settings);
        }
    }

    void StopRecording() override {
        if (!m_recording) return;

        spdlog::info("Stopping recording...");

        // FIX: Set flag first to stop all loops
        m_recording = false;

        // FIX: Stop audio capture IMMEDIATELY to prevent more audio being queued
        // This must happen BEFORE waiting for threads
        spdlog::info("Stopping audio capture immediately...");
        if (m_audioCapture) m_audioCapture->StopCapture();
        if (m_usbAudioCapture) m_usbAudioCapture->Stop();

        // FIX: Wait for threads BEFORE stopping video captures
        spdlog::info("Waiting for processing thread...");
        if (m_processThread.joinable()) m_processThread.join();

        spdlog::info("Waiting for sync thread...");
        if (m_syncThread.joinable()) m_syncThread.join();

        spdlog::info("Waiting for audio thread...");
        if (m_audioThread.joinable()) m_audioThread.join();

        spdlog::info("Waiting for USB audio thread...");
        if (m_usbAudioThread.joinable()) m_usbAudioThread.join();

        // Now stop video captures
        spdlog::info("Stopping video captures...");
        if (m_frameCapture) m_frameCapture->StopCapture();
        if (m_usbCapture) m_usbCapture->Stop();
        if (m_cameraCapture) m_cameraCapture->Stop();

        // Flush Encoder to ensure final frames are saved
        spdlog::info("Flushing encoder...");
        if (m_encoder && m_diskWriter) {
            std::vector<EncodedPacket> finalPackets;
            m_encoder->Flush(finalPackets);
            for (auto& packet : finalPackets) {
                WriteTask task;
                task.data = std::move(packet.data);
                task.timestamp = static_cast<uint64_t>(packet.pts);
                task.isVideo = true;
                task.pts = packet.pts;
                task.keyframe = packet.keyframe;
                m_diskWriter->QueueWriteTask(std::move(task));
            }
        }

        spdlog::info("Stopping disk writer...");
        if (m_diskWriter) m_diskWriter->StopWriter();

        // Reset sync variables for next recording
        m_recordingStartTime = 0;
        m_firstVideoTimestamp = 0;

        spdlog::info("Recording stopped");
        if (m_statusCallback) m_statusCallback("Recording stopped");
    }

    bool IsRecording() const override { return m_recording; }

    PerformanceMetrics GetMetrics() const override {
        PerformanceMetrics metrics = {};
        metrics.droppedFrames = m_droppedFrames;
        if (m_diskWriter) metrics.totalBytesWritten = m_diskWriter->GetBytesWritten();
        return metrics;
    }

    void UpdateSettings(const RecordingSettings& settings) override {
        m_settings = settings;
    }

    std::vector<ExtendedGPUInfo> GetAvailableGPUs() const override {
        if (!m_gpuDetector) return {};
        return m_gpuDetector->DetectGPUs();
    }

    void SetStatusCallback(std::function<void(const std::string&)> callback) override { m_statusCallback = callback; }
    void SetErrorCallback(std::function<void(const std::string&)> callback) override { m_errorCallback = callback; }

    bool CaptureScreenshot(const std::string& outputPath) override {
        spdlog::info("Capturing screenshot to: {}", outputPath);

        if (m_recording) {
            std::unique_lock<std::mutex> lock(m_screenshotMutex);
            m_screenshotRequested = true;
            m_screenshotPath = outputPath;

            // Wait for screenshot to be taken (timeout after 1s) using a condition variable
            bool success = m_screenshotCv.wait_for(lock, std::chrono::seconds(1), [this] {
                return !m_screenshotRequested;
                });

            if (!success) {
                spdlog::error("Screenshot timeout");
                m_screenshotRequested = false;
                return false;
            }
            return true;
        }
        else {
            // Not recording, need to temporarily start capture
            return CaptureSingleFrame(outputPath);
        }
    }

private:
    std::atomic<bool> m_screenshotRequested{ false };
    std::mutex m_screenshotMutex;
    std::condition_variable m_screenshotCv;
    std::string m_screenshotPath;

    bool CaptureSingleFrame(const std::string& outputPath) {
        spdlog::info("Performing single-frame capture (not recording)");

        // FIX: Ensure detector and display manager are initialized if this is called early
        if (!m_gpuDetector) {
            m_gpuDetector = std::make_unique<GPUDetector>();
        }
        if (!m_displayManager) {
            m_displayManager = std::make_unique<VirtualDisplayManager>();
            m_displayManager->Initialize();
        }

        try {
            m_gpuInfo = m_gpuDetector->GetGPUByIndex(m_settings.gpuIndex);
        }
        catch (...) {
            spdlog::error("Screenshot: Failed to get GPU info by index {}, trying optimal", m_settings.gpuIndex);
            try {
                m_gpuInfo = m_gpuDetector->GetOptimalGPU();
            } catch (...) {
                spdlog::error("Screenshot: Failed to get optimal GPU");
                return false;
            }
        }

        m_displayManager->SetActiveDisplay(m_settings.displayIndex);
        ComPtr<IDXGIOutput> output = m_displayManager->GetActiveDisplayOutput();

        ComPtr<IDXGIAdapter> displayAdapter;
        if (output) {
            output->GetParent(IID_PPV_ARGS(&displayAdapter));
        }

        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> encodeAdapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &encodeAdapter) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 desc;
            encodeAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == m_gpuInfo.adapterLuid.LowPart &&
                desc.AdapterLuid.HighPart == m_gpuInfo.adapterLuid.HighPart) {
                break;
            }
            encodeAdapter.Reset();
        }

        if (!encodeAdapter) {
            spdlog::error("Screenshot: Could not match encoding adapter");
            return false;
        }

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(encodeAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);

        if (FAILED(hr) || !device) {
            spdlog::error("Screenshot: Failed to create D3D11 device: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        bool success = false;
        if (m_settings.usbDeviceIndex >= 0) {
            auto usb = std::make_unique<SimpleUSBCapture>();
            if (usb->Initialize(m_settings.usbDeviceIndex, device)) {
                // Use a decent buffer for idle capture
                if (usb->Start(20)) {
                    USBFrame frame;
                    USBFrame lastValidFrame = {};
                    bool gotAnyFrame = false;

                    // Skip up to 30 frames (~1 sec at 30fps) to allow sensor to stabilize
                    // Some capture cards output black frames initially.
                    for (int i = 0; i < 45; ++i) {
                        if (usb->GetFrame(frame, 200)) {
                            if (gotAnyFrame && lastValidFrame.texture) {
                                usb->ReturnTexture(lastValidFrame.texture);
                            }
                            lastValidFrame = frame;
                            gotAnyFrame = true;
                            // Wait a tiny bit between frames to allow for exposure adjustments
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        if (!usb->IsRunning()) break;
                    }

                    if (gotAnyFrame && lastValidFrame.texture) {
                        SaveTextureAsPngManual(device, context, lastValidFrame.texture, outputPath);
                        usb->ReturnTexture(lastValidFrame.texture);
                        success = true;
                    }
                    usb->Stop();
                }
            }
        }
        else {
            if (output) {
                // If the display is on a different adapter, we MUST create a device on that adapter for capture
                ComPtr<ID3D11Device> captureDevice = device;
                ComPtr<ID3D11DeviceContext> captureContext = context;

                DXGI_ADAPTER_DESC displayDesc;
                displayAdapter->GetDesc(&displayDesc);
                DXGI_ADAPTER_DESC1 encodeDesc;
                encodeAdapter->GetDesc1(&encodeDesc);

                bool isSameAdapter = (displayDesc.AdapterLuid.LowPart == encodeDesc.AdapterLuid.LowPart &&
                    displayDesc.AdapterLuid.HighPart == encodeDesc.AdapterLuid.HighPart);

                if (!isSameAdapter) {
                    spdlog::info("Screenshot: Cross-adapter capture needed");
                    D3D11CreateDevice(displayAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &captureDevice, nullptr, &captureContext);
                }

                auto cap = std::make_unique<FrameCapture>();
                // Use a larger ring buffer for warmup
                if (cap->Initialize(!isSameAdapter, nullptr, m_gpuInfo, captureDevice, captureContext, output)) {
                    if (cap->StartCapture(20, 60)) {
                        CapturedFrame frame;
                        CapturedFrame lastValidFrame = {};
                        bool gotAnyFrame = false;

                        // Desktop duplication often needs a few frames to "warm up" and avoid returning 
                        // black/stale frames on the very first acquisition.
                        // We take up to 20 frames, but keep the last valid one if it goes idle.
                        for (int i = 0; i < 20; ++i) {
                            if (cap->GetNextFrame(frame, 500)) {
                                if (gotAnyFrame && lastValidFrame.texture) {
                                    cap->ReturnTexture(lastValidFrame.texture);
                                }
                                lastValidFrame = frame;
                                gotAnyFrame = true;
                                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            }
                            // If we already have a frame and it's taking too long (static screen), just use what we have
                            if (gotAnyFrame && i > 5) break;
                        }

                        if (gotAnyFrame && lastValidFrame.texture) {
                            SaveTextureAsPngManual(captureDevice, captureContext, lastValidFrame.texture, outputPath);
                            cap->ReturnTexture(lastValidFrame.texture);
                            success = true;
                        }
                        cap->StopCapture();
                    }
                }
            }
        }

        return success;
    }

    // Helper for CaptureSingleFrame to avoid dependency on pipeline state
    void SaveTextureAsPngManual(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context,
        ComPtr<ID3D11Texture2D> texture, const std::string& path) {
        if (!texture) return;
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        if (desc.Usage == D3D11_USAGE_STAGING && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) {
            // Already a staging texture, map directly
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(texture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                SaveRawRgbaToPng(reinterpret_cast<uint8_t*>(mapped.pData), desc.Width, desc.Height, mapped.RowPitch, path);
                context->Unmap(texture.Get(), 0);
            }
            return;
        }

        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC sDesc = desc;
        sDesc.Usage = D3D11_USAGE_STAGING;
        sDesc.BindFlags = 0;
        sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sDesc.MipLevels = 1;
        sDesc.ArraySize = 1;

        if (SUCCEEDED(device->CreateTexture2D(&sDesc, nullptr, &staging))) {
            context->CopyResource(staging.Get(), texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                SaveRawRgbaToPng(reinterpret_cast<uint8_t*>(mapped.pData), desc.Width, desc.Height, mapped.RowPitch, path);
                context->Unmap(staging.Get(), 0);
            }
        }
    }

    void SaveTextureAsPngAsync(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context, ComPtr<ID3D11Texture2D> texture, const std::string& path) {
        if (!texture || !device || !context) return;

        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        if (desc.Usage == D3D11_USAGE_STAGING && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) {
            // Already a staging texture, map directly
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(texture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                size_t dataSize = mapped.RowPitch * desc.Height;
                auto buffer = std::make_shared<std::vector<uint8_t>>(dataSize);
                memcpy(buffer->data(), mapped.pData, dataSize);
                context->Unmap(texture.Get(), 0);

                std::thread([buffer, width = desc.Width, height = desc.Height, stride = mapped.RowPitch, path]() {
                    RecordingPipeline::SaveRawRgbaToPngStatic(buffer->data(), width, height, stride, path);
                    }).detach();
            }
            return;
        }

        // Create staging texture to read back to CPU
        ComPtr<ID3D11Texture2D> stagingTexture;
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;

        HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
        if (FAILED(hr)) {
            spdlog::error("Failed to create staging texture for screenshot: 0x{:08X}", static_cast<uint32_t>(hr));
            return;
        }

        context->CopyResource(stagingTexture.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            spdlog::error("Failed to map staging texture for screenshot: 0x{:08X}", static_cast<uint32_t>(hr));
            return;
        }

        // Copy raw data for async processing
        size_t dataSize = mapped.RowPitch * desc.Height;
        auto buffer = std::make_shared<std::vector<uint8_t>>(dataSize);
        memcpy(buffer->data(), mapped.pData, dataSize);

        context->Unmap(stagingTexture.Get(), 0);

        // Run PNG encoding in a separate thread
        std::thread([buffer, width = desc.Width, height = desc.Height, stride = mapped.RowPitch, path]() {
            RecordingPipeline::SaveRawRgbaToPngStatic(buffer->data(), width, height, stride, path);
            }).detach();
    }

    void SaveRawRgbaToPng(const uint8_t* bgraData, uint32_t width, uint32_t height, uint32_t stride, const std::string& path) {
        SaveRawRgbaToPngStatic(bgraData, width, height, stride, path);
    }

    static void SaveRawBgraToBmp(const uint8_t* bgraData, uint32_t width, uint32_t height, uint32_t stride, const std::string& path) {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) return;

#pragma pack(push, 1)
        struct {
            uint16_t type{ 0x4D42 };
            uint32_t size;
            uint16_t res1{ 0 }, res2{ 0 };
            uint32_t offBits{ 54 };
            uint32_t biSize{ 40 };
            int32_t width;
            int32_t height;
            uint16_t planes{ 1 };
            uint16_t bitCount{ 24 };
            uint32_t compression{ 0 };
            uint32_t sizeImage{ 0 };
            int32_t xPels{ 0 }, yPels{ 0 };
            uint32_t clrUsed{ 0 }, clrImp{ 0 };
        } bmp;
#pragma pack(pop)

        uint32_t rowSize = (width * 3 + 3) & ~3;
        bmp.width = width;
        bmp.height = -static_cast<int32_t>(height);
        bmp.size = 54 + rowSize * height;

        ofs.write(reinterpret_cast<const char*>(&bmp), 54);
        std::vector<uint8_t> row(rowSize, 0);
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* src = bgraData + y * stride;
            for (uint32_t x = 0; x < width; x++) {
                row[x * 3 + 0] = src[x * 4 + 0]; // B
                row[x * 3 + 1] = src[x * 4 + 1]; // G
                row[x * 3 + 2] = src[x * 4 + 2]; // R
            }
            ofs.write(reinterpret_cast<const char*>(row.data()), rowSize);
        }
        spdlog::info("Screenshot saved as BMP (Perfect Quality): {}", path);
    }

    static void SaveRawRgbaToPngStatic(const uint8_t* bgraData, uint32_t width, uint32_t height, uint32_t stride, std::string path) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
        if (!codec) {
            spdlog::warn("PNG encoder not found, falling back to BMP for perfect quality");
            if (path.size() > 4) path.replace(path.size() - 3, 3, "bmp");
            SaveRawBgraToBmp(bgraData, width, height, stride, path);
            return;
        }

        AVCodecContext* c = avcodec_alloc_context3(codec);
        if (!c) return;

        c->width = width;
        c->height = height;
        c->pix_fmt = AV_PIX_FMT_RGB24;
        c->time_base = { 1, 1 };

        if (avcodec_open2(c, codec, nullptr) < 0) {
            avcodec_free_context(&c);
            return;
        }

        AVFrame* frame = av_frame_alloc();
        frame->format = c->pix_fmt;
        frame->width = c->width;
        frame->height = c->height;

        if (av_image_alloc(frame->data, frame->linesize, width, height, c->pix_fmt, 32) < 0) {
            av_frame_free(&frame);
            avcodec_free_context(&c);
            return;
        }

        SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_BGRA,
            width, height, AV_PIX_FMT_RGB24,
            SWS_LANCZOS, nullptr, nullptr, nullptr);

        if (sws) {
            const uint8_t* srcData[1] = { bgraData };
            int srcLinesize[1] = { (int)stride };
            sws_scale(sws, srcData, srcLinesize, 0, height, frame->data, frame->linesize);
            sws_freeContext(sws);
        }

        AVPacket* pkt = av_packet_alloc();
        if (avcodec_send_frame(c, frame) >= 0) {
            std::ofstream ofs(path, std::ios::binary);
            if (ofs) {
                while (avcodec_receive_packet(c, pkt) >= 0) {
                    ofs.write(reinterpret_cast<char*>(pkt->data), pkt->size);
                    av_packet_unref(pkt);
                }
                ofs.close();
                spdlog::info("Screenshot saved as PNG: {}", path);
            }
        }

        av_packet_free(&pkt);
        av_freep(&frame->data[0]);
        av_frame_free(&frame);
        avcodec_free_context(&c);
    }
    bool StartCameraRecording(const RecordingSettings& settings) {
        try {
            m_gpuInfo = m_gpuDetector->GetGPUByIndex(settings.gpuIndex);
        } catch (...) { return false; }

        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> encodeAdapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &encodeAdapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc; encodeAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == m_gpuInfo.adapterLuid.LowPart &&
                desc.AdapterLuid.HighPart == m_gpuInfo.adapterLuid.HighPart) break;
        }
        if (!encodeAdapter) return false;

        HRESULT hr = D3D11CreateDevice(encodeAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &m_sharedD3D11Device, nullptr, &m_sharedD3D11Context);
        if (FAILED(hr)) return false;

        m_cameraCapture = std::make_unique<CameraCapture>();
        std::string url = settings.cameraUrl;
        if (!m_cameraCapture->Initialize(url, m_sharedD3D11Device)) {
            CleanupOnFailure(); return false;
        }

        m_captureD3D11Device = m_sharedD3D11Device;
        m_captureD3D11Context = m_sharedD3D11Context;
        
        m_settings.width = m_cameraCapture->GetWidth();
        m_settings.height = m_cameraCapture->GetHeight();

        m_encoder = std::make_unique<HardwareEncoder>();
        if (!m_encoder->Initialize(m_gpuInfo, m_settings, m_sharedD3D11Device, m_sharedD3D11Context)) {
            CleanupOnFailure(); return false;
        }

        m_diskWriter = std::make_unique<DiskWriter>();
        if (!m_diskWriter->Initialize(m_settings, m_encoder->GetExtradata(), m_encoder->GetSoftwarePixelFormat())) {
            CleanupOnFailure(); return false;
        }

        if (!m_diskWriter->StartWriter()) { CleanupOnFailure(); return false; }

        m_recording = true;
        m_processThread = std::thread(&RecordingPipeline::ProcessLoop, this);
        m_syncThread = std::thread(&RecordingPipeline::SyncLoop, this);

        if (!m_cameraCapture->Start()) { CleanupOnFailure(); return false; }
        
        spdlog::info("Camera Recording started successfully");
        if (m_statusCallback) m_statusCallback("Camera Recording started");
        return true;
    }

    bool StartUSBRecording(const RecordingSettings& settings) {
        try {
            m_gpuInfo = m_gpuDetector->GetGPUByIndex(settings.gpuIndex);
            spdlog::info("Using GPU: {}", std::string(m_gpuInfo.name.begin(), m_gpuInfo.name.end()));
        }
        catch (const std::exception& e) {
            spdlog::error("Failed to get GPU: {}", e.what());
            return false;
        }

        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> encodeAdapter;

        for (UINT i = 0; factory->EnumAdapters1(i, &encodeAdapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            encodeAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == m_gpuInfo.adapterLuid.LowPart &&
                desc.AdapterLuid.HighPart == m_gpuInfo.adapterLuid.HighPart) {
                break;
            }
        }

        if (!encodeAdapter) {
            spdlog::error("Could not match GPU to adapter");
            return false;
        }

        HRESULT hr = D3D11CreateDevice(
            encodeAdapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_sharedD3D11Device, nullptr, &m_sharedD3D11Context
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create D3D11 device: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        ComPtr<ID3D10Multithread> multiThread;
        if (SUCCEEDED(m_sharedD3D11Device.As(&multiThread))) {
            multiThread->SetMultithreadProtected(TRUE);
        }

        m_usbCapture = std::make_unique<SimpleUSBCapture>();
        if (!m_usbCapture->Initialize(settings.usbDeviceIndex, m_sharedD3D11Device)) {
            spdlog::error("Failed to initialize USB capture");
            CleanupOnFailure();
            return false;
        }

        m_settings.width = m_usbCapture->GetWidth();
        m_settings.height = m_usbCapture->GetHeight();
        spdlog::info("USB Capture Resolution: {}x{}", m_settings.width, m_settings.height);

        m_captureD3D11Device = m_sharedD3D11Device;
        m_captureD3D11Context = m_sharedD3D11Context;

        // Initialize USB Audio
        if (settings.captureAudio) {
            auto usbVideoDevices = SimpleUSBCapture::EnumerateDevices();
            std::string videoDeviceName;

            if (settings.usbDeviceIndex >= 0 && settings.usbDeviceIndex < (int)usbVideoDevices.size()) {
                videoDeviceName = usbVideoDevices[settings.usbDeviceIndex].name;
            }

            int audioDeviceIndex = USBAudioCapture::FindMatchingAudioDevice(videoDeviceName);

            if (audioDeviceIndex >= 0) {
                m_usbAudioCapture = std::make_unique<USBAudioCapture>();
                if (!m_usbAudioCapture->Initialize(audioDeviceIndex)) {
                    spdlog::warn("Failed to initialize USB audio capture");
                    m_usbAudioCapture.reset();
                }
            }
        }

        m_encoder = std::make_unique<HardwareEncoder>();
        if (!m_encoder->Initialize(m_gpuInfo, m_settings, m_sharedD3D11Device, m_sharedD3D11Context)) {
            spdlog::error("Failed to initialize hardware encoder");
            CleanupOnFailure();
            return false;
        }

        m_diskWriter = std::make_unique<DiskWriter>();
        if (!m_diskWriter->Initialize(m_settings, m_encoder->GetExtradata(), m_encoder->GetSoftwarePixelFormat())) {
            spdlog::error("Failed to initialize disk writer");
            CleanupOnFailure();
            return false;
        }

        if (m_usbAudioCapture) {
            m_diskWriter->SetAudioFormat(
                m_usbAudioCapture->GetSampleRate(),
                m_usbAudioCapture->GetChannels(),
                m_usbAudioCapture->GetBitDepth()
            );
        }

        if (!m_diskWriter->StartWriter()) {
            spdlog::error("Failed to start disk writer");
            CleanupOnFailure();
            return false;
        }

        m_recording = true;

        m_processThread = std::thread(&RecordingPipeline::ProcessLoop, this);
        SetThreadPriority(m_processThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);

        m_syncThread = std::thread(&RecordingPipeline::SyncLoop, this);

        // FIX: Use separate m_usbAudioThread for USB audio
        if (m_usbAudioCapture) {
            if (m_usbAudioCapture->Start(128)) {
                m_usbAudioThread = std::thread(&RecordingPipeline::USBAudioLoop, this);
            }
            else {
                spdlog::warn("Failed to start USB audio capture");
                m_usbAudioCapture.reset();
            }
        }

        // === CALCULATE RING BUFFER SIZE FROM RAM SETTING ===
        // USB is typically 1920x1080 BGRA = 8.3 MB per frame
        uint64_t frameSize = static_cast<uint64_t>(m_settings.width) * m_settings.height * 4;
        uint32_t ringBufferSize = 64;

        if (settings.ramBufferSize > 0 && frameSize > 0) {
            uint64_t framesFromRam = settings.ramBufferSize / frameSize;
            ringBufferSize = static_cast<uint32_t>(framesFromRam);
            ringBufferSize = std::max(ringBufferSize, 32u);

            spdlog::info("USB RAM Buffer: {:.1f} GB = {} frames",
                settings.ramBufferSize / (1024.0 * 1024.0 * 1024.0), ringBufferSize);
        }
        else {
            ringBufferSize = std::max(settings.ringBufferSize, 64u);
        }

        if (!m_usbCapture->Start(ringBufferSize)) {
            spdlog::error("Failed to start USB capture");
            CleanupOnFailure();
            return false;
        }

        spdlog::info("USB Recording started successfully");
        if (m_statusCallback) m_statusCallback("USB Recording started");
        return true;
    }

    bool StartDisplayRecording(const RecordingSettings& settings) {
        try {
            m_gpuInfo = m_gpuDetector->GetGPUByIndex(settings.gpuIndex);
            spdlog::info("Using User-Selected GPU: {}", std::string(m_gpuInfo.name.begin(), m_gpuInfo.name.end()));
        }
        catch (const std::exception& e) {
            spdlog::error("Failed to get GPU: {}", e.what());
            return false;
        }


        m_displayManager->SetActiveDisplay(settings.displayIndex);
        ComPtr<IDXGIOutput> targetDisplayOutput = m_displayManager->GetActiveDisplayOutput();

        if (!targetDisplayOutput) {
            spdlog::error("Failed to acquire IDXGIOutput for Display {}", settings.displayIndex);
            return false;
        }

        // --- NEW: Calculate Region Cropping ---
        RecordingSettings activeSettings = settings;

        size_t displayIdx = (settings.displayIndex < m_displayManager->GetDisplayCount()) ? settings.displayIndex : 0;
        const auto& displayInfo = m_displayManager->GetDisplayInfo(displayIdx);
        activeSettings.width = displayInfo.width;
        activeSettings.height = displayInfo.height;

        if (activeSettings.captureRegion && activeSettings.regionWidth > 0 && activeSettings.regionHeight > 0) {
            // Map global screen coordinates to monitor-local coordinates
            int localX = activeSettings.regionX - displayInfo.positionX;
            int localY = activeSettings.regionY - displayInfo.positionY;

            localX = std::max(0, localX);
            localY = std::max(0, localY);

            int cropWidth = std::min((int)displayInfo.width - localX, activeSettings.regionWidth);
            int cropHeight = std::min((int)displayInfo.height - localY, activeSettings.regionHeight);

            if (cropWidth > 0 && cropHeight > 0) {
                // Dimensions should be even
                activeSettings.width = cropWidth & ~1;
                activeSettings.height = cropHeight & ~1;
                activeSettings.regionX = localX;
                activeSettings.regionY = localY;
                activeSettings.regionWidth = activeSettings.width;
                activeSettings.regionHeight = activeSettings.height;
                spdlog::info("Region capture enabled: {}x{} at {},{}", activeSettings.width, activeSettings.height, localX, localY);
            } else {
                spdlog::warn("Region outside selected display, falling back to full screen.");
                activeSettings.captureRegion = false;
            }
        }




        const auto& activeDisplay = m_displayManager->GetActiveDisplayInfo();
        if (activeSettings.width > 0 && activeSettings.height > 0) {
            m_settings.width = activeSettings.width;
            m_settings.height = activeSettings.height;
            m_settings.captureRegion = activeSettings.captureRegion;
            m_settings.regionX = activeSettings.regionX;
            m_settings.regionY = activeSettings.regionY;
            m_settings.regionWidth = activeSettings.regionWidth;
            m_settings.regionHeight = activeSettings.regionHeight;



            // FIX: Keep user's FPS setting, don't override with display refresh rate
            // Just log the display refresh rate for reference
            if (activeDisplay.refreshRate > 0) {
                spdlog::info("Display refresh rate: {}Hz, Recording at: {} FPS",
                    activeDisplay.refreshRate, m_settings.fps);

                // FIX: Auto-limit FPS to display refresh rate
                // Recording faster than display refresh just duplicates frames
                if (m_settings.fps > activeDisplay.refreshRate && activeDisplay.refreshRate > 0) {
                    spdlog::warn("Limiting FPS from {} to {} (display refresh rate)",
                        m_settings.fps, activeDisplay.refreshRate);
                    m_settings.fps = activeDisplay.refreshRate;
                }
            }
        }

        if (!CreateD3D12Device(m_d3d12Device)) {
            spdlog::warn("Failed to create D3D12 device, continuing with D3D11 only");
        }

        ComPtr<IDXGIAdapter> displayAdapter;
        targetDisplayOutput->GetParent(IID_PPV_ARGS(&displayAdapter));

        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> encodeAdapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &encodeAdapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            encodeAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == m_gpuInfo.adapterLuid.LowPart && desc.AdapterLuid.HighPart == m_gpuInfo.adapterLuid.HighPart) {
                break;
            }
        }
        if (!encodeAdapter) {
            spdlog::error("Could not match the selected GPU to hardware adapter");
            return false;
        }

        DXGI_ADAPTER_DESC displayDesc;
        displayAdapter->GetDesc(&displayDesc);
        m_isSameAdapter = (displayDesc.AdapterLuid.LowPart == m_gpuInfo.adapterLuid.LowPart && displayDesc.AdapterLuid.HighPart == m_gpuInfo.adapterLuid.HighPart);

        HRESULT hr = D3D11CreateDevice(
            encodeAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_sharedD3D11Device, nullptr, &m_sharedD3D11Context
        );

        if (FAILED(hr)) return false;

        ComPtr<ID3D10Multithread> multiThread;
        if (SUCCEEDED(m_sharedD3D11Device.As(&multiThread))) {
            multiThread->SetMultithreadProtected(TRUE);
        }

        if (m_isSameAdapter) {
            m_captureD3D11Device = m_sharedD3D11Device;
            m_captureD3D11Context = m_sharedD3D11Context;
        }
        else {
            spdlog::warn("Cross-Adapter Capture Detected. Enabling Dual GPU split workload.");
            spdlog::warn("PERFORMANCE WARNING: Cross-adapter copy is slow for high resolutions!");
            spdlog::warn("For best performance, connect this monitor to the NVIDIA GPU.");

            // Allow maximum FPS for cross-adapter capture
            spdlog::info("Cross-adapter capture: Allowing full requested FPS ({})", m_settings.fps);

            hr = D3D11CreateDevice(displayAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_captureD3D11Device, nullptr, &m_captureD3D11Context);
            if (FAILED(hr)) return false;

            ComPtr<ID3D10Multithread> capMultiThread;
            if (SUCCEEDED(m_captureD3D11Device.As(&capMultiThread))) {
                capMultiThread->SetMultithreadProtected(TRUE);
            }
        }

        m_frameCapture = std::make_unique<FrameCapture>();
        if (!m_frameCapture->Initialize(!m_isSameAdapter, m_d3d12Device, m_gpuInfo, m_captureD3D11Device, m_captureD3D11Context, targetDisplayOutput)) {
            spdlog::error("Failed to initialize frame capture");
            CleanupOnFailure();
            return false;
        }

        m_encoder = std::make_unique<HardwareEncoder>();
        if (!m_encoder->Initialize(m_gpuInfo, m_settings, m_sharedD3D11Device, m_sharedD3D11Context)) {
            spdlog::error("Failed to initialize hardware encoder");
            CleanupOnFailure();
            return false;
        }

        m_diskWriter = std::make_unique<DiskWriter>();
        if (!m_diskWriter->Initialize(m_settings, m_encoder->GetExtradata(), m_encoder->GetSoftwarePixelFormat())) {
            spdlog::error("Failed to initialize disk writer");
            CleanupOnFailure();
            return false;
        }

        // === MOUSE CAPTURE DISABLED ===
        // Cursor drawing is too slow for high-res recording (uses CPU staging)
        // TODO: Implement GPU compute shader cursor compositing
        /*
        if (settings.showMouseCursor) {
            m_mouseCapture = std::make_unique<MouseCapture>();
            if (!m_mouseCapture->Initialize(m_captureD3D11Device, m_captureD3D11Context)) {
                spdlog::warn("MouseCapture init failed, cursor will not be recorded");
                m_mouseCapture.reset();
            } else {
                spdlog::info("MouseCapture initialized - cursor will be recorded");
            }
        }
        */

        if (settings.captureAudio) {
            m_audioCapture = std::make_unique<AudioCapture>();
            if (m_audioCapture->Initialize(settings.audioSampleRate, settings.audioBitDepth)) {
                m_diskWriter->SetAudioFormat(
                    m_audioCapture->GetSampleRate(),
                    m_audioCapture->GetChannels(),
                    m_audioCapture->GetBitDepth()
                );

                if (!m_audioCapture->StartCapture()) {
                    spdlog::warn("Audio capture start failed");
                    m_audioCapture.reset();
                }
            }
            else {
                spdlog::warn("Audio capture init failed");
                m_audioCapture.reset();
            }
        }

        if (!m_diskWriter->StartWriter()) {
            spdlog::error("Failed to start disk writer");
            CleanupOnFailure();
            return false;
        }

        m_recording = true;

        m_processThread = std::thread(&RecordingPipeline::ProcessLoop, this);
        SetThreadPriority(m_processThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);

        m_syncThread = std::thread(&RecordingPipeline::SyncLoop, this);

        if (m_audioCapture) {
            m_audioThread = std::thread(&RecordingPipeline::AudioLoop, this);
        }

        // === CALCULATE RING BUFFER SIZE FROM RAM SETTING ===
        // Each frame is width * height * 4 bytes (BGRA)
        uint64_t frameSize = static_cast<uint64_t>(m_settings.width) * m_settings.height * 4;
        uint32_t ringBufferSize = 64;  // Minimum default

        if (settings.ramBufferSize > 0 && frameSize > 0) {
            // Calculate how many frames fit in the RAM buffer
            uint64_t framesFromRam = settings.ramBufferSize / frameSize;
            ringBufferSize = static_cast<uint32_t>(framesFromRam);  // Cap at 512 frames
            ringBufferSize = std::max(ringBufferSize, 32u);  // Minimum 32 frames

            spdlog::info("RAM Buffer: {:.1f} GB = {} frames ({} x {} @ 4 bytes/pixel)",
                settings.ramBufferSize / (1024.0 * 1024.0 * 1024.0),
                ringBufferSize, m_settings.width, m_settings.height);
        }
        else {
            ringBufferSize = std::max(settings.ringBufferSize, 64u);
        }

        // === SET CURSOR VISIBILITY ===
        m_frameCapture->SetShowCursor(settings.showMouseCursor);

        if (!m_frameCapture->StartCapture(ringBufferSize, m_settings.fps)) {
            spdlog::error("Failed to start frame capture");
            CleanupOnFailure();
            return false;
        }

        spdlog::info("Display Recording started successfully");
        if (m_statusCallback) m_statusCallback("Display Recording started");
        return true;
    }

    void CleanupOnFailure() {
        m_recording = false;
        if (m_frameCapture) m_frameCapture->StopCapture();
        if (m_usbCapture) m_usbCapture->Stop();
        if (m_cameraCapture) m_cameraCapture->Stop();
        if (m_usbAudioCapture) m_usbAudioCapture->Stop();
        if (m_audioCapture) m_audioCapture->StopCapture();
        if (m_diskWriter) m_diskWriter->StopWriter();
        if (m_mouseCapture) m_mouseCapture->Shutdown();
    }

    bool CreateD3D12Device(ComPtr<ID3D12Device>& device) {
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        return SUCCEEDED(hr);
    }

    void SetupLogging() {
        try {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("recording.log", 1024 * 1024 * 10, 3);
            std::vector<spdlog::sink_ptr> sinks{ console_sink, rotating_sink };
            auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());
            spdlog::set_default_logger(logger);
            spdlog::set_level(spdlog::level::info);
        }
        catch (...) {}
    }

    void ProcessLoop() {
        spdlog::info("Processing thread started");
        CapturedFrame frame;
        USBFrame usbFrame;
        std::vector<EncodedPacket> encodedPackets;

        // FIX: Track if we've sent the first frame - it MUST be a keyframe
        bool firstFrameSent = false;

        // FIX: For Desktop capture, clear stale frames and wait for fresh ones
        // This fixes the 2-3 second scramble at the start
        if (!m_isUSBCapture && m_frameCapture) {
            spdlog::info("Desktop capture: clearing stale frames from buffer...");
            m_frameCapture->ClearBuffer();

            // Wait a moment for fresh frames to arrive
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Discard any frames that arrived during the wait (they may have stale timestamps)
            CapturedFrame discardFrame;
            int discardedCount = 0;
            while (m_frameCapture->GetNextFrame(discardFrame, 10)) {
                if (discardFrame.texture) {
                    m_frameCapture->ReturnTexture(discardFrame.texture);
                }
                discardedCount++;
                if (discardedCount >= 10) break;  // Don't discard too many
            }
            spdlog::info("Desktop capture: discarded {} stale frames, ready for fresh capture", discardedCount);
        }

        while (m_recording) {
            bool gotFrame = false;
            CameraFrame camFrame;

            if (m_isCameraCapture) {
                if (!m_cameraCapture) break;
                gotFrame = m_cameraCapture->GetNextFrame(camFrame, 50);
                if (gotFrame && camFrame.texture) {
                    frame.texture = camFrame.texture;
                    frame.timestamp = camFrame.timestamp;
                    frame.frameIndex = static_cast<uint32_t>(m_cameraFrameCount.fetch_add(1));
                    frame.isKeyframe = (frame.frameIndex % (m_settings.fps > 0 ? m_settings.fps : 60) == 0);
                }
            }
            else if (m_isUSBCapture) {
                if (!m_usbCapture) break;

                gotFrame = m_usbCapture->GetFrame(usbFrame, 50);
                if (gotFrame && usbFrame.texture) {
                    frame.texture = usbFrame.texture;
                    frame.timestamp = usbFrame.timestamp;
                    frame.frameIndex = usbFrame.frameIndex;
                    frame.isKeyframe = usbFrame.isKeyframe;
                }
            }
            else {
                if (!m_frameCapture) break;

                gotFrame = m_frameCapture->GetNextFrame(frame, 50);
                if (gotFrame) {
                    frame.isKeyframe = (frame.frameIndex % (m_settings.fps > 0 ? m_settings.fps : 60) == 0);
                }
            }

            // FIX: Force first frame to be a keyframe to avoid scrambled start
            if (gotFrame && !firstFrameSent) {
                frame.isKeyframe = true;
                spdlog::info("Forcing first frame as keyframe (frameIndex: {}, timestamp: {})",
                    frame.frameIndex, frame.timestamp);
            }

            // Handle screenshot request - MUST be after getting a frame to have texture available
            // but BEFORE potential 'continue' or errors to ensure responsiveness.
            if (m_screenshotRequested && gotFrame && frame.texture) {
                {
                    std::lock_guard<std::mutex> lock(m_screenshotMutex);
                    // Double check inside lock
                    if (m_screenshotRequested) {
                        // FIX: Use capture device/context for screenshot since frame texture is on that adapter
                        SaveTextureAsPngAsync(m_captureD3D11Device, m_captureD3D11Context, frame.texture, m_screenshotPath);
                        m_screenshotRequested = false;
                    }
                }
                m_screenshotCv.notify_all();
            }

            if (!gotFrame || !frame.texture) continue;

            // === DRAW MOUSE CURSOR ON FRAME ===
            // NOTE: Cursor drawing is currently disabled due to performance issues
            // The staging texture approach is too slow for high-res/high-fps recording
            // TODO: Implement GPU-based cursor compositing using compute shaders
            /*
            if (!m_isUSBCapture && m_settings.showMouseCursor && m_mouseCapture) {
                // Get display offset for multi-monitor
                int offsetX = 0, offsetY = 0;
                if (m_displayManager) {
                    const auto& display = m_displayManager->GetActiveDisplayInfo();
                    offsetX = display.positionX;
                    offsetY = display.positionY;
                }

                // Draw cursor highlight if enabled (use colors from settings)
                if (m_settings.highlightCursor) {
                    m_mouseCapture->DrawCursorHighlight(
                        frame.texture.Get(), 0, 0, offsetX, offsetY,
                        m_settings.cursorHighlightRadius,
                        m_settings.highlightColorR,
                        m_settings.highlightColorG,
                        m_settings.highlightColorB,
                        m_settings.highlightColorA
                    );
                }

                // Draw cursor
                m_mouseCapture->DrawCursorOnTexture(frame.texture.Get(), 0, 0, offsetX, offsetY);

                // Draw click animation if enabled (use colors from settings)
                if (m_settings.showClickAnimation) {
                    MouseState state = m_mouseCapture->GetMouseState();
                    auto now = std::chrono::steady_clock::now();

                    // Detect new click
                    if (state.leftButton && !m_lastLeftButtonState) {
                        m_clickAnimationStart = now;
                        m_clickX = state.x;
                        m_clickY = state.y;
                        m_clickAnimationActive = true;
                    }
                    m_lastLeftButtonState = state.leftButton;

                    // Draw animation if active
                    if (m_clickAnimationActive) {
                        float elapsed = std::chrono::duration<float>(now - m_clickAnimationStart).count();
                        float progress = elapsed / 0.3f;  // 300ms animation

                        if (progress < 1.0f) {
                            m_mouseCapture->DrawClickAnimation(
                                frame.texture.Get(), m_clickX, m_clickY, offsetX, offsetY,
                                progress,
                                m_settings.clickColorR,
                                m_settings.clickColorG,
                                m_settings.clickColorB
                            );
                        } else {
                            m_clickAnimationActive = false;
                        }
                    }
                }
            }
            */

            // Cross-adapter texture copy if needed
            ComPtr<ID3D11Texture2D> originalTexture = frame.texture;
            if (!m_isSameAdapter && !m_isUSBCapture) {
                // Allow all frames to be processed to maximize video quality

                // Ensure we have a staging texture on the capture device
                if (!m_captureStagingTexture) {
                    D3D11_TEXTURE2D_DESC desc;
                    frame.texture->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    desc.MiscFlags = 0;
                    m_captureD3D11Device->CreateTexture2D(&desc, nullptr, &m_captureStagingTexture);
                }

                if (m_captureStagingTexture) {
                    // Copy GPU texture to staging (on capture device)
                    m_captureD3D11Context->CopyResource(m_captureStagingTexture.Get(), frame.texture.Get());
                    m_captureD3D11Context->Flush();

                    D3D11_MAPPED_SUBRESOURCE mapped;
                    HRESULT hr = m_captureD3D11Context->Map(m_captureStagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                    if (SUCCEEDED(hr)) {
                        if (!m_crossAdapterEncodeTexture) {
                            D3D11_TEXTURE2D_DESC desc;
                            frame.texture->GetDesc(&desc);
                            desc.Usage = D3D11_USAGE_DEFAULT;
                            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                            desc.CPUAccessFlags = 0;
                            desc.MiscFlags = 0;
                            m_sharedD3D11Device->CreateTexture2D(&desc, nullptr, &m_crossAdapterEncodeTexture);
                        }

                        if (m_crossAdapterEncodeTexture) {
                            m_sharedD3D11Context->UpdateSubresource(m_crossAdapterEncodeTexture.Get(), 0, nullptr, mapped.pData, mapped.RowPitch, 0);
                            frame.texture = m_crossAdapterEncodeTexture;
                        }
                        m_captureD3D11Context->Unmap(m_captureStagingTexture.Get(), 0);
                    }
                    else {
                        // Failed to map - skip this frame but return texture
                        if (m_frameCapture) m_frameCapture->ReturnTexture(originalTexture);
                        continue;
                    }
                }
            }

            if (!m_encoder || !m_diskWriter) break;

            encodedPackets.clear();
            if (m_encoder->EncodeFrame(frame, encodedPackets)) {
                // If this is a keyframe and we don't have extradata in DiskWriter yet, try to update it
                if (frame.isKeyframe && m_diskWriter && !m_diskWriter->IsHeaderWritten()) {
                    auto extradata = m_encoder->GetExtradata();
                    if (!extradata.empty()) {
                        m_diskWriter->UpdateVideoExtradata(extradata);
                    }
                }

                for (auto& packet : encodedPackets) {
                    WriteTask task;
                    task.data = std::move(packet.data);
                    // Use packet PTS as the task timestamp. 
                    // This is the original QPC timestamp of the frame.
                    task.timestamp = static_cast<uint64_t>(packet.pts);
                    task.isVideo = true;
                    task.pts = packet.pts;
                    task.keyframe = packet.keyframe;
                    m_diskWriter->QueueWriteTask(std::move(task));
                }

                // FIX: Mark first frame as sent after successful encode
                if (!firstFrameSent && !encodedPackets.empty()) {
                    firstFrameSent = true;
                }
            }

            // Return texture to pool
            if (m_isUSBCapture && m_usbCapture) {
                m_usbCapture->ReturnTexture(usbFrame.texture);
            }
            else if (m_isCameraCapture && m_cameraCapture) {
                m_cameraCapture->ReturnTexture(frame.texture);
            }
            else if (m_frameCapture) {
                m_frameCapture->ReturnTexture(originalTexture);
            }
        }
        spdlog::info("Processing thread exiting");
    }

    void AudioLoop() {
        spdlog::info("Audio loop started");
        AudioPacket packet;

        while (m_recording) {
            if (m_audioCapture && m_diskWriter && m_audioCapture->GetNextPacket(packet, 50)) {
                // USE RAW CAPTURE TIMESTAMP
                m_diskWriter->QueueAudioData(packet.data.data(), packet.data.size(), packet.timestamp);
            }
        }
        spdlog::info("Audio loop exiting");
    }

    void USBAudioLoop() {
        spdlog::info("USB audio loop started");
        USBAudioPacket packet;

        while (m_recording) {
            if (m_usbAudioCapture && m_diskWriter && m_usbAudioCapture->GetNextPacket(packet, 50)) {
                // USE RAW CAPTURE TIMESTAMP
                m_diskWriter->QueueAudioData(packet.data.data(), packet.data.size(), packet.timestamp);
            }
        }
        spdlog::info("USB audio loop exiting");
    }

    void SyncLoop() {
        spdlog::info("AV sync thread started");
        while (m_recording) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        spdlog::info("AV sync thread stopped");
    }

    // FIX: Helper function for consistent timestamp generation
    static uint64_t GetCurrentTimestampUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    std::unique_ptr<GPUDetector> m_gpuDetector;
    std::unique_ptr<VirtualDisplayManager> m_displayManager;
    std::unique_ptr<FrameCapture> m_frameCapture;
    std::unique_ptr<SimpleUSBCapture> m_usbCapture;
    std::unique_ptr<CameraCapture> m_cameraCapture;
    std::unique_ptr<USBAudioCapture> m_usbAudioCapture;
    std::unique_ptr<AudioCapture> m_audioCapture;
    std::unique_ptr<HardwareEncoder> m_encoder;
    std::unique_ptr<DiskWriter> m_diskWriter;
    std::unique_ptr<MouseCapture> m_mouseCapture;  // Mouse cursor capture

    ComPtr<ID3D12Device> m_d3d12Device;
    ComPtr<ID3D11Device> m_sharedD3D11Device;
    ComPtr<ID3D11DeviceContext> m_sharedD3D11Context;
    ComPtr<ID3D11Device> m_captureD3D11Device;
    ComPtr<ID3D11DeviceContext> m_captureD3D11Context;
    ComPtr<ID3D11Texture2D> m_crossAdapterEncodeTexture;
    ComPtr<ID3D11Texture2D> m_captureStagingTexture;

    RecordingSettings m_settings;
    ExtendedGPUInfo m_gpuInfo;
    uint64_t m_systemMemory;

    std::atomic<bool> m_recording;
    std::atomic<uint32_t> m_droppedFrames;
    std::atomic<uint64_t> m_cameraFrameCount{0};
    bool m_isSameAdapter;
    bool m_isUSBCapture;
    bool m_isCameraCapture = false;

    // FIX: A/V sync - shared recording clock
    std::atomic<uint64_t> m_recordingStartTime;   // When recording started (common reference)
    std::atomic<uint64_t> m_firstVideoTimestamp;  // First video frame's device timestamp

    // Mouse click animation state
    bool m_lastLeftButtonState = false;
    bool m_clickAnimationActive = false;
    std::chrono::steady_clock::time_point m_clickAnimationStart;
    int m_clickX = 0;
    int m_clickY = 0;

    std::thread m_processThread;
    std::thread m_syncThread;
    std::thread m_audioThread;
    std::thread m_usbAudioThread;  // FIX: Separate thread for USB audio

    std::function<void(const std::string&)> m_statusCallback;
    std::function<void(const std::string&)> m_errorCallback;
};

inline std::unique_ptr<IRecordingEngine> CreateRecordingEngine() {
    return std::make_unique<RecordingPipeline>();
}
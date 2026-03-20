#pragma once
#include "RecordingEngine.h"
#include "GPUDetector.h"
#include "VirtualDisplayManager.h"
#include "FrameCapture.h"
#include "usbcapture.h"
#include "USBAudioCapture.h"
#include "AudioCapture.h"
#include "HardwareEncoder.h"
#include "DiskWriter.h"
#include <d3d11_4.h> 
#include <fstream>
#include <filesystem>

class RecordingPipeline : public IRecordingEngine {
public:
    RecordingPipeline() : m_recording(false), m_droppedFrames(0), m_systemMemory(0), m_isSameAdapter(true), m_isUSBCapture(false) {}

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
    }

    bool StartRecording(const RecordingSettings& settings) override {
        if (m_recording) StopRecording();

        m_settings = settings;
        m_droppedFrames = 0;

        m_isUSBCapture = (settings.usbDeviceIndex >= 0);

        if (m_isUSBCapture) {
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

        // FIX: Wait for threads BEFORE stopping captures
        // This prevents accessing freed memory
        spdlog::info("Waiting for processing thread...");
        if (m_processThread.joinable()) m_processThread.join();

        spdlog::info("Waiting for sync thread...");
        if (m_syncThread.joinable()) m_syncThread.join();

        spdlog::info("Waiting for audio thread...");
        if (m_audioThread.joinable()) m_audioThread.join();

        // FIX: Wait for USB audio thread too
        spdlog::info("Waiting for USB audio thread...");
        if (m_usbAudioThread.joinable()) m_usbAudioThread.join();

        // NOW stop captures (threads are already done)
        spdlog::info("Stopping captures...");
        if (m_frameCapture) m_frameCapture->StopCapture();
        if (m_usbCapture) m_usbCapture->Stop();
        if (m_usbAudioCapture) m_usbAudioCapture->Stop();
        if (m_audioCapture) m_audioCapture->StopCapture();

        // Flush Encoder to ensure final frames are saved
        spdlog::info("Flushing encoder...");
        if (m_encoder && m_diskWriter) {
            std::vector<EncodedPacket> finalPackets;
            m_encoder->Flush(finalPackets);
            for (auto& packet : finalPackets) {
                WriteTask task;
                task.data = std::move(packet.data);
                task.timestamp = packet.sourceTimestamp;
                task.isVideo = true;
                task.pts = packet.pts;
                task.keyframe = packet.keyframe;
                m_diskWriter->QueueWriteTask(std::move(task));
            }
        }

        spdlog::info("Stopping disk writer...");
        if (m_diskWriter) m_diskWriter->StopWriter();

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

    std::vector<ExtendedGPUInfo> GetAvailableGPUs() const override {
        if (!m_gpuDetector) return {};
        return m_gpuDetector->DetectGPUs();
    }

    void SetStatusCallback(std::function<void(const std::string&)> callback) override { m_statusCallback = callback; }
    void SetErrorCallback(std::function<void(const std::string&)> callback) override { m_errorCallback = callback; }

private:
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

        ComPtr<ID3D11Multithread> multiThread;
        if (SUCCEEDED(m_sharedD3D11Context.As(&multiThread))) {
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
        if (!m_diskWriter->Initialize(m_settings, m_encoder->GetExtradata())) {
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

        uint32_t ringBufferSize = std::max(settings.ringBufferSize, 64u);
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

        const auto& activeDisplay = m_displayManager->GetActiveDisplayInfo();
        if (activeDisplay.width > 0 && activeDisplay.height > 0) {
            m_settings.width = activeDisplay.width;
            m_settings.height = activeDisplay.height;
            if (activeDisplay.refreshRate > 0) {
                m_settings.fps = activeDisplay.refreshRate;
                spdlog::info("Using display refresh rate: {}Hz", m_settings.fps);
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

        ComPtr<ID3D11Multithread> multiThread;
        if (SUCCEEDED(m_sharedD3D11Context.As(&multiThread))) {
            multiThread->SetMultithreadProtected(TRUE);
        }

        if (m_isSameAdapter) {
            m_captureD3D11Device = m_sharedD3D11Device;
            m_captureD3D11Context = m_sharedD3D11Context;
        }
        else {
            spdlog::warn("Cross-Adapter Capture Detected. Enabling Dual GPU split workload.");
            hr = D3D11CreateDevice(displayAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_captureD3D11Device, nullptr, &m_captureD3D11Context);
            if (FAILED(hr)) return false;

            ComPtr<ID3D11Multithread> capMultiThread;
            if (SUCCEEDED(m_captureD3D11Context.As(&capMultiThread))) {
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
        if (!m_diskWriter->Initialize(m_settings, m_encoder->GetExtradata())) {
            spdlog::error("Failed to initialize disk writer");
            CleanupOnFailure();
            return false;
        }

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

        uint32_t ringBufferSize = std::max(settings.ringBufferSize, 64u);
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
        if (m_usbAudioCapture) m_usbAudioCapture->Stop();
        if (m_audioCapture) m_audioCapture->StopCapture();
        if (m_diskWriter) m_diskWriter->StopWriter();
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

        while (m_recording) {
            bool gotFrame = false;

            if (m_isUSBCapture) {
                // FIX: Check if capture is still valid
                if (!m_usbCapture) break;

                gotFrame = m_usbCapture->GetFrame(usbFrame, 50);
                if (gotFrame && usbFrame.texture) {
                    frame.texture = usbFrame.texture;
                    frame.timestamp = usbFrame.timestamp;
                    frame.frameIndex = usbFrame.frameIndex;
                    frame.isKeyframe = (usbFrame.frameIndex % 60 == 0);  // Calculate here since USBFrame doesn't have it
                }
            }
            else {
                // FIX: Check if capture is still valid
                if (!m_frameCapture) break;

                gotFrame = m_frameCapture->GetNextFrame(frame, 50);
                if (gotFrame) {
                    frame.isKeyframe = (frame.frameIndex % 60 == 0);
                }
            }

            if (!gotFrame || !frame.texture) continue;

            // FIX: Check encoder is valid
            if (!m_encoder || !m_diskWriter) break;

            encodedPackets.clear();
            if (m_encoder->EncodeFrame(frame, encodedPackets)) {
                for (auto& packet : encodedPackets) {
                    WriteTask task;
                    task.data = std::move(packet.data);
                    task.timestamp = packet.sourceTimestamp;
                    task.isVideo = true;
                    task.pts = packet.pts;
                    task.keyframe = packet.keyframe;
                    m_diskWriter->QueueWriteTask(std::move(task));
                }
            }

            // Return texture to pool (only for display capture - USB capture doesn't have ReturnTexture)
            if (!m_isUSBCapture && m_frameCapture) {
                m_frameCapture->ReturnTexture(frame.texture);
            }
        }
        spdlog::info("Processing thread exiting");
    }

    void AudioLoop() {
        spdlog::info("Audio loop started");
        AudioPacket packet;
        while (m_recording) {
            if (m_audioCapture && m_diskWriter && m_audioCapture->GetNextPacket(packet, 50)) {
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

    std::unique_ptr<GPUDetector> m_gpuDetector;
    std::unique_ptr<VirtualDisplayManager> m_displayManager;
    std::unique_ptr<FrameCapture> m_frameCapture;
    std::unique_ptr<SimpleUSBCapture> m_usbCapture;
    std::unique_ptr<USBAudioCapture> m_usbAudioCapture;
    std::unique_ptr<AudioCapture> m_audioCapture;
    std::unique_ptr<HardwareEncoder> m_encoder;
    std::unique_ptr<DiskWriter> m_diskWriter;

    ComPtr<ID3D12Device> m_d3d12Device;
    ComPtr<ID3D11Device> m_sharedD3D11Device;
    ComPtr<ID3D11DeviceContext> m_sharedD3D11Context;
    ComPtr<ID3D11Device> m_captureD3D11Device;
    ComPtr<ID3D11DeviceContext> m_captureD3D11Context;
    ComPtr<ID3D11Texture2D> m_crossAdapterEncodeTexture;

    RecordingSettings m_settings;
    ExtendedGPUInfo m_gpuInfo;
    uint64_t m_systemMemory;

    std::atomic<bool> m_recording;
    std::atomic<uint32_t> m_droppedFrames;
    bool m_isSameAdapter;
    bool m_isUSBCapture;

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
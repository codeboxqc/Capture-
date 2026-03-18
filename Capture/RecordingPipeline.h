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
        if (!m_displayManager->Initialize()) spdlog::warn("Virtual display driver not installed");
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
        return m_isUSBCapture ? StartUSBRecording(settings) : StartDisplayRecording(settings);
    }

    void StopRecording() override {
        if (!m_recording) return;
        m_recording = false;
        if (m_frameCapture) m_frameCapture->StopCapture();
        if (m_usbCapture) m_usbCapture->Stop();
        if (m_usbAudioCapture) m_usbAudioCapture->Stop();
        if (m_audioCapture) m_audioCapture->StopCapture();
        if (m_processThread.joinable()) m_processThread.join();
        if (m_syncThread.joinable()) m_syncThread.join();
        if (m_audioThread.joinable()) m_audioThread.join();

        if (m_encoder && m_diskWriter) {
            std::vector<EncodedPacket> finalPackets;
            m_encoder->Flush(finalPackets);
            for (auto& packet : finalPackets) {
                WriteTask task = { std::move(packet.data), packet.sourceTimestamp, true, packet.pts, packet.keyframe };
                m_diskWriter->QueueWriteTask(std::move(task));
            }
        }
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
        return m_gpuDetector ? m_gpuDetector->DetectGPUs() : std::vector<ExtendedGPUInfo>();
    }

    void SetStatusCallback(std::function<void(const std::string&)> callback) override { m_statusCallback = callback; }
    void SetErrorCallback(std::function<void(const std::string&)> callback) override { m_errorCallback = callback; }

private:
    bool StartUSBRecording(const RecordingSettings& settings) {
        try { m_gpuInfo = m_gpuDetector->GetGPUByIndex(settings.gpuIndex); }
        catch (...) { return false; }

        if (!CreateSharedD3D11Device(m_gpuInfo)) return false;

        m_usbCapture = std::make_unique<SimpleUSBCapture>();
        if (!m_usbCapture->Initialize(settings.usbDeviceIndex, m_sharedD3D11Device)) return false;

        m_settings.width = m_usbCapture->GetWidth();
        m_settings.height = m_usbCapture->GetHeight();

        if (settings.captureAudio) InitUSBAudio(settings.usbDeviceIndex);

        if (!InitEncoderAndWriter()) return false;
        if (!m_diskWriter->StartWriter()) return false;

        m_recording = true;
        m_processThread = std::thread(&RecordingPipeline::ProcessLoop, this);
        SetThreadPriority(m_processThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
        m_syncThread = std::thread(&RecordingPipeline::SyncLoop, this);

        if (m_usbAudioCapture && m_usbAudioCapture->Start(128)) {
            m_audioThread = std::thread(&RecordingPipeline::USBAudioLoop, this);
        }

        if (!m_usbCapture->Start(128)) return false;
        return true;
    }

    bool StartDisplayRecording(const RecordingSettings& settings) {
        try { m_gpuInfo = m_gpuDetector->GetGPUByIndex(settings.gpuIndex); }
        catch (...) { return false; }

        m_displayManager->SetActiveDisplay(settings.displayIndex);
        ComPtr<IDXGIOutput> targetOutput = m_displayManager->GetActiveDisplayOutput();
        if (!targetOutput) return false;

        const auto& displayInfo = m_displayManager->GetActiveDisplayInfo();
        m_settings.width = displayInfo.width;
        m_settings.height = displayInfo.height;
        if (displayInfo.refreshRate > 0) m_settings.fps = displayInfo.refreshRate;

        if (!CreateSharedD3D11Device(m_gpuInfo)) return false;

        ComPtr<IDXGIAdapter> displayAdapter;
        targetOutput->GetParent(IID_PPV_ARGS(&displayAdapter));
        DXGI_ADAPTER_DESC desc;
        displayAdapter->GetDesc(&desc);
        m_isSameAdapter = (desc.AdapterLuid.LowPart == m_gpuInfo.adapterLuid.LowPart && desc.AdapterLuid.HighPart == m_gpuInfo.adapterLuid.HighPart);

        if (m_isSameAdapter) {
            m_captureD3D11Device = m_sharedD3D11Device;
            m_captureD3D11Context = m_sharedD3D11Context;
        } else {
            D3D11CreateDevice(displayAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_captureD3D11Device, nullptr, &m_captureD3D11Context);
        }

        m_frameCapture = std::make_unique<FrameCapture>();
        if (!m_frameCapture->Initialize(!m_isSameAdapter, m_d3d12Device, m_gpuInfo, m_captureD3D11Device, m_captureD3D11Context, targetOutput)) return false;

        if (!InitEncoderAndWriter()) return false;
        if (!m_diskWriter->StartWriter()) return false;

        if (settings.captureAudio) {
            m_audioCapture = std::make_unique<AudioCapture>();
            if (m_audioCapture->Initialize(settings.audioSampleRate, settings.audioBitDepth)) {
                m_diskWriter->SetAudioFormat(m_audioCapture->GetSampleRate(), m_audioCapture->GetChannels(), m_audioCapture->GetBitDepth());
                m_audioCapture->StartCapture();
                m_audioThread = std::thread(&RecordingPipeline::AudioLoop, this);
            }
        }

        m_recording = true;
        m_processThread = std::thread(&RecordingPipeline::ProcessLoop, this);
        SetThreadPriority(m_processThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
        m_syncThread = std::thread(&RecordingPipeline::SyncLoop, this);

        if (!m_frameCapture->StartCapture(128, m_settings.fps)) return false;
        return true;
    }

    bool CreateSharedD3D11Device(const ExtendedGPUInfo& gpu) {
        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == gpu.adapterLuid.LowPart && desc.AdapterLuid.HighPart == gpu.adapterLuid.HighPart) break;
        }
        if (!adapter) return false;
        return SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_sharedD3D11Device, nullptr, &m_sharedD3D11Context));
    }

    void InitUSBAudio(int videoIndex) {
        auto devices = SimpleUSBCapture::EnumerateDevices();
        if (videoIndex >= 0 && videoIndex < (int)devices.size()) {
            int audioIndex = USBAudioCapture::FindMatchingAudioDevice(devices[videoIndex].name);
            if (audioIndex >= 0) {
                m_usbAudioCapture = std::make_unique<USBAudioCapture>();
                if (!m_usbAudioCapture->Initialize(audioIndex)) m_usbAudioCapture.reset();
            }
        }
    }

    bool InitEncoderAndWriter() {
        m_encoder = std::make_unique<HardwareEncoder>();
        if (!m_encoder->Initialize(m_gpuInfo, m_settings, m_sharedD3D11Device, m_sharedD3D11Context)) return false;
        m_diskWriter = std::make_unique<DiskWriter>();
        return m_diskWriter->Initialize(m_settings, m_encoder->GetExtradata());
    }

    void SetupLogging() {
        try {
            auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("recording.log", 1024 * 1024 * 10, 3);
            spdlog::set_default_logger(std::make_shared<spdlog::logger>("multi_sink", spdlog::sinks_init_list{ console, file }));
            spdlog::set_level(spdlog::level::info);
        } catch (...) {}
    }

    void ProcessLoop() {
        spdlog::info("Processing thread started");
        CapturedFrame frame;
        USBFrame usbFrame;
        std::vector<EncodedPacket> packets;

        while (m_recording) {
            bool got = false;
            if (m_isUSBCapture) {
                if ((got = m_usbCapture->GetFrame(usbFrame, 50))) {
                    frame = { usbFrame.texture, usbFrame.timestamp, usbFrame.frameIndex, (usbFrame.frameIndex % 60 == 0), false };
                }
            } else {
                if ((got = m_frameCapture->GetNextFrame(frame, 50))) {
                    frame.isKeyframe = (frame.frameIndex % 60 == 0);
                }
            }

            if (!got || !frame.texture) continue;

            packets.clear();
            if (m_encoder->EncodeFrame(frame, packets)) {
                for (auto& p : packets) {
                    WriteTask task = { std::move(p.data), p.sourceTimestamp, true, p.pts, p.keyframe };
                    m_diskWriter->QueueWriteTask(std::move(task));
                }
            }

            if (m_isUSBCapture) m_usbCapture->ReturnTexture(usbFrame.texture);
            else m_frameCapture->ReturnTexture(frame.texture);
        }
        spdlog::info("Processing thread exiting");
    }

    void AudioLoop() {
        AudioPacket p;
        while (m_recording) if (m_audioCapture && m_audioCapture->GetNextPacket(p, 50)) m_diskWriter->QueueAudioData(p.data.data(), p.data.size(), p.timestamp);
    }

    void USBAudioLoop() {
        USBAudioPacket p;
        while (m_recording) if (m_usbAudioCapture && m_usbAudioCapture->GetNextPacket(p, 50)) m_diskWriter->QueueAudioData(p.data.data(), p.data.size(), p.timestamp);
    }

    void SyncLoop() {
        while (m_recording) std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    bool m_isSameAdapter, m_isUSBCapture;
    std::thread m_processThread, m_syncThread, m_audioThread;
    std::function<void(const std::string&)> m_statusCallback, m_errorCallback;
};

inline std::unique_ptr<IRecordingEngine> CreateRecordingEngine() {
    return std::make_unique<RecordingPipeline>();
}
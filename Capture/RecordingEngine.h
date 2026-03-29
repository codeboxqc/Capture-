#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <vector>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <exception>

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <avrt.h>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>               
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using namespace Microsoft::WRL;

// ========== Types ==========
enum class EncoderType { NVIDIA_NVENC, AMD_AMF, INTEL_QSV, SOFTWARE };
enum class Codec { H264, H265, AV1 };
enum class RecordingMode { Lossless, NearLossless, CustomBitrate };
enum class ContainerFormat { MKV, MP4, RAW };

// ========== Base GPU Info ==========
struct GPUInfo {
    std::wstring name;
    std::wstring vendorId;
    uint64_t dedicatedVideoMemory;
    uint64_t sharedSystemMemory;
    EncoderType encoderType;
    bool supportsAV1;
    bool supportsHDR;
    LUID adapterLuid; // Added to map exact hardware across DirectX
};

// ========== GPU Capabilities ==========
struct GPUCapabilities {
    bool supportsH264Encode = false;
    bool supportsH265Encode = false;
    bool supportsAV1Encode = false;
    bool supportsH264Decode = false;
    bool supportsH265Decode = false;
    bool supportsAV1Decode = false;
    bool supportsHDR = false;
    bool supportsHardwareAcceleration = false;
    bool supportsLowLatency = false;
    uint32_t maxEncodedResolution = 0;
    uint32_t maxFramerate = 0;
    uint32_t computeUnits = 0;
};

// ========== Extended GPU Info ==========
struct ExtendedGPUInfo : public GPUInfo {
    GPUCapabilities capabilities;
    std::string driverVersion;
    double performanceScore = 0.0;
};

// ========== Recording Settings ==========
struct RecordingSettings {
    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t fps = 120;
    uint32_t bitrate = 50000000;
    RecordingMode mode = RecordingMode::CustomBitrate;
    Codec codec = Codec::H265;
    ContainerFormat container = ContainerFormat::MKV;
    bool captureAudio = true;
    bool captureHDR = false;
    uint32_t audioSampleRate = 48000;
    uint16_t audioBitDepth = 24;
    std::filesystem::path outputPath = L"C:\\Recordings\\capture.mkv";

    uint64_t maxFileSize = 0;
    uint32_t ringBufferSize = 32;  // Increased from 8 to 32
    uint64_t ramBufferSize = 4ULL * 1024 * 1024 * 1024;
    bool enableBurstBuffering = true;
    bool enableTimestampMetadata = true;

    // --- NEW FEATURE: TIME LIMIT ---
    uint32_t recordDurationSeconds = 0; // 0 = infinite, >0 = auto stop limit

    // --- SELECTION VARIABLES ---
    uint32_t gpuIndex = 0;
    uint32_t displayIndex = 0;
    int32_t usbDeviceIndex = -1;
};

// ========== Performance Metrics ==========
struct PerformanceMetrics {
    double captureFPS = 0.0;
    double encodeFPS = 0.0;
    double writeSpeedMBs = 0.0;
    uint32_t droppedFrames = 0;
    uint32_t encodedFrames = 0;
    uint64_t totalBytesWritten = 0;
    double cpuUsage = 0.0;
    double gpuUsage = 0.0;
    double encoderLatency = 0.0;
};

// ========== Interface ==========
class IRecordingEngine {
public:
    virtual ~IRecordingEngine() = default;
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual bool StartRecording(const RecordingSettings& settings) = 0;
    virtual void StopRecording() = 0;
    virtual bool IsRecording() const = 0;
    virtual PerformanceMetrics GetMetrics() const = 0;
    virtual std::vector<ExtendedGPUInfo> GetAvailableGPUs() const = 0;
    virtual void SetStatusCallback(std::function<void(const std::string&)> callback) = 0;
    virtual void SetErrorCallback(std::function<void(const std::string&)> callback) = 0;
    virtual void UpdateSettings(const RecordingSettings& settings) = 0;

    // --- NEW FEATURE: SCREENSHOT CAPTURE ---
    virtual bool CaptureScreenshot(const std::string& outputPath) = 0;
};

std::unique_ptr<IRecordingEngine> CreateRecordingEngine();
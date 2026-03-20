#pragma once
#include "RecordingEngine.h"

class GPUDetector {
public:
    GPUDetector() = default;
    ~GPUDetector() = default;

    std::vector<ExtendedGPUInfo> DetectGPUs() {
        std::vector<ExtendedGPUInfo> gpus;

        // FIX: Try DXGI 1.6 first, fall back to 1.1 for older systems
        ComPtr<IDXGIFactory6> factory6;
        ComPtr<IDXGIFactory1> factory1;

        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6));
        if (FAILED(hr)) {
            // Fallback for older Windows versions
            hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory1));
            if (FAILED(hr)) {
                spdlog::error("Failed to create DXGI factory: 0x{:08X}", static_cast<uint32_t>(hr));
                return gpus;
            }
        }

        UINT adapterIndex = 0;

        while (true) {
            ComPtr<IDXGIAdapter1> adapter1;
            ComPtr<IDXGIAdapter4> adapter4;

            // Try to get adapter
            if (factory6) {
                // Use GPU preference API (Windows 10 1803+)
                hr = factory6->EnumAdapterByGpuPreference(
                    adapterIndex,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&adapter4));
            }
            else {
                // Fallback enumeration
                hr = factory1->EnumAdapters1(adapterIndex, &adapter1);
            }

            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;  // No more adapters
            }

            if (FAILED(hr)) {
                adapterIndex++;
                continue;
            }

            // FIX: Initialize all fields to safe defaults
            ExtendedGPUInfo info = {};
            info.encoderType = EncoderType::SOFTWARE;
            info.supportsAV1 = false;
            info.supportsHDR = false;
            info.dedicatedVideoMemory = 0;
            info.sharedSystemMemory = 0;
            info.performanceScore = 0.0;
            info.capabilities = {};  // Zero-initialize capabilities

            bool gotDesc = false;
            DXGI_ADAPTER_DESC1 desc1 = {};

            if (adapter4) {
                DXGI_ADAPTER_DESC3 desc3;
                if (SUCCEEDED(adapter4->GetDesc3(&desc3))) {
                    info.name = desc3.Description;
                    info.vendorId = std::to_wstring(desc3.VendorId);
                    info.dedicatedVideoMemory = desc3.DedicatedVideoMemory;
                    info.sharedSystemMemory = desc3.SharedSystemMemory;
                    info.adapterLuid = desc3.AdapterLuid;
                    desc1.VendorId = desc3.VendorId;
                    desc1.DeviceId = desc3.DeviceId;
                    desc1.Flags = desc3.Flags;
                    gotDesc = true;
                }
            }
            else if (adapter1) {
                if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
                    info.name = desc1.Description;
                    info.vendorId = std::to_wstring(desc1.VendorId);
                    info.dedicatedVideoMemory = desc1.DedicatedVideoMemory;
                    info.sharedSystemMemory = desc1.SharedSystemMemory;
                    info.adapterLuid = desc1.AdapterLuid;
                    gotDesc = true;
                }
            }

            if (!gotDesc) {
                adapterIndex++;
                continue;
            }

            // FIX: Skip software adapters (like Microsoft Basic Render Driver)
            if (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                spdlog::debug("Skipping software adapter: {}", WStringToString(info.name));
                adapterIndex++;
                continue;
            }

            // Detect vendor and capabilities
            if (desc1.VendorId == 0x10DE) {
                info.encoderType = EncoderType::NVIDIA_NVENC;
                DetectNVIDIACapabilities(desc1.DeviceId, info);
            }
            else if (desc1.VendorId == 0x1002) {
                info.encoderType = EncoderType::AMD_AMF;
                DetectAMDCapabilities(desc1.DeviceId, info);
            }
            else if (desc1.VendorId == 0x8086) {
                info.encoderType = EncoderType::INTEL_QSV;
                DetectIntelCapabilities(desc1.DeviceId, info);
            }
            else {
                info.encoderType = EncoderType::SOFTWARE;
                info.capabilities.supportsHardwareAcceleration = false;
            }

            // Check HDR support
            IDXGIAdapter* baseAdapter = adapter4 ? static_cast<IDXGIAdapter*>(adapter4.Get())
                : static_cast<IDXGIAdapter*>(adapter1.Get());
            info.capabilities.supportsHDR = CheckHDRSupport(baseAdapter);
            info.supportsHDR = info.capabilities.supportsHDR;

            // Calculate performance score
            info.performanceScore = CalculatePerformanceScore(info);

            spdlog::info("Detected GPU: {} (Score: {:.1f})", WStringToString(info.name), info.performanceScore);
            gpus.push_back(info);

            adapterIndex++;
        }

        // Sort by performance score (highest first)
        std::sort(gpus.begin(), gpus.end(), [](const ExtendedGPUInfo& a, const ExtendedGPUInfo& b) {
            return a.performanceScore > b.performanceScore;
            });

        spdlog::info("Total GPUs detected: {}", gpus.size());
        return gpus;
    }

    ExtendedGPUInfo GetOptimalGPU() {
        auto gpus = DetectGPUs();
        if (gpus.empty()) {
            spdlog::error("No GPU detected!");
            throw std::runtime_error("No GPU detected");
        }
        return gpus[0];
    }

    // FIX: Added safe version that doesn't throw
    bool TryGetOptimalGPU(ExtendedGPUInfo& outGPU) {
        auto gpus = DetectGPUs();
        if (gpus.empty()) {
            return false;
        }
        outGPU = gpus[0];
        return true;
    }

    uint64_t GetSystemMemory() {
        MEMORYSTATUSEX status = {};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status)) {
            spdlog::warn("Failed to get system memory info");
            return 0;
        }
        return status.ullTotalPhys;
    }

    ExtendedGPUInfo GetGPUByIndex(size_t index) {
        auto gpus = DetectGPUs();
        if (index >= gpus.size()) {
            spdlog::error("GPU index {} out of range (max: {})", index, gpus.size() - 1);
            throw std::out_of_range("GPU index out of range");
        }
        return gpus[index];
    }

    // FIX: Added safe version that doesn't throw
    bool TryGetGPUByIndex(size_t index, ExtendedGPUInfo& outGPU) {
        auto gpus = DetectGPUs();
        if (index >= gpus.size()) {
            return false;
        }
        outGPU = gpus[index];
        return true;
    }

private:
    // Helper to convert wstring to string for logging
    static std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        std::string str(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, nullptr, nullptr);
        return str;
    }

    void DetectNVIDIACapabilities(UINT deviceId, ExtendedGPUInfo& info) {
        // NVIDIA Device ID ranges (approximate):
        // RTX 40 series (Ada): 0x2600 - 0x28FF (AV1 encode support)
        // RTX 30 series (Ampere): 0x2200 - 0x25FF (No AV1 encode, but AV1 decode on some)
        // RTX 20 series (Turing): 0x1E00 - 0x21FF
        // GTX 16 series (Turing): 0x2180 - 0x21FF
        // GTX 10 series (Pascal): 0x1B00 - 0x1DFF

        // FIX: More accurate AV1 detection (RTX 40 series and newer)
        info.supportsAV1 = (deviceId >= 0x2600 && deviceId <= 0x28FF);

        info.capabilities.supportsH264Encode = true;
        info.capabilities.supportsH265Encode = true;
        info.capabilities.supportsH264Decode = true;
        info.capabilities.supportsH265Decode = true;
        info.capabilities.supportsAV1Encode = info.supportsAV1;
        info.capabilities.supportsAV1Decode = (deviceId >= 0x2200);  // RTX 30 series and newer
        info.capabilities.supportsHardwareAcceleration = true;
        info.capabilities.maxFramerate = 240;
        info.capabilities.maxEncodedResolution = 8192;

        // Turing and newer support low latency
        info.capabilities.supportsLowLatency = (deviceId >= 0x1E00);
    }

    void DetectAMDCapabilities(UINT deviceId, ExtendedGPUInfo& info) {
        // AMD Device ID ranges (approximate):
        // RX 7000 series (RDNA3): 0x7400 - 0x74FF (AV1 encode support)
        // RX 6000 series (RDNA2): 0x73A0 - 0x73FF (AV1 decode only)
        // RX 5000 series (RDNA1): 0x7310 - 0x739F

        // FIX: More accurate AV1 detection
        info.supportsAV1 = (deviceId >= 0x7400 && deviceId <= 0x74FF);

        info.capabilities.supportsH264Encode = true;
        info.capabilities.supportsH265Encode = true;
        info.capabilities.supportsH264Decode = true;   // FIX: Added decode
        info.capabilities.supportsH265Decode = true;   // FIX: Added decode
        info.capabilities.supportsAV1Encode = info.supportsAV1;
        info.capabilities.supportsAV1Decode = (deviceId >= 0x73A0);  // RDNA2 and newer
        info.capabilities.supportsHardwareAcceleration = true;
        info.capabilities.maxFramerate = 120;
        info.capabilities.maxEncodedResolution = 4096;
        info.capabilities.supportsLowLatency = (deviceId >= 0x7310);  // RDNA1 and newer
    }

    void DetectIntelCapabilities(UINT deviceId, ExtendedGPUInfo& info) {
        // Intel Device ID ranges (approximate):
        // Arc A-series (Alchemist): 0x5690 - 0x56FF (AV1 encode support)
        // Iris Xe (Gen12): 0x9A40 - 0x9AFF
        // UHD (Gen11): 0x8A00 - 0x8AFF

        // FIX: More accurate AV1 detection (Arc GPUs)
        info.supportsAV1 = (deviceId >= 0x5690 && deviceId <= 0x56FF);

        info.capabilities.supportsH264Encode = true;
        info.capabilities.supportsH265Encode = true;
        info.capabilities.supportsH264Decode = true;   // FIX: Added decode
        info.capabilities.supportsH265Decode = true;   // FIX: Added decode
        info.capabilities.supportsAV1Encode = info.supportsAV1;
        info.capabilities.supportsAV1Decode = info.supportsAV1;  // Arc only
        info.capabilities.supportsHardwareAcceleration = true;
        info.capabilities.maxFramerate = 120;
        info.capabilities.maxEncodedResolution = 4096;
        info.capabilities.supportsLowLatency = (deviceId >= 0x5690);  // Arc series
    }

    bool CheckHDRSupport(IDXGIAdapter* adapter) {
        if (!adapter) return false;

        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, &output))) {
            return false;
        }

        // FIX: Use IDXGIOutput6 for proper HDR detection
        ComPtr<IDXGIOutput6> output6;
        if (SUCCEEDED(output.As(&output6))) {
            DXGI_OUTPUT_DESC1 desc1;
            if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                // Check color space support
                bool supportsHDR = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                    desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

                // Also check bits per color
                if (desc1.BitsPerColor >= 10) {
                    return supportsHDR || true;  // 10-bit display likely supports HDR
                }
                return supportsHDR;
            }
        }

        // Fallback: check if display is at least capable
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor) {
            HDC hdc = CreateDCW(desc.DeviceName, desc.DeviceName, NULL, NULL);
            if (hdc) {
                // Check color depth
                int colorDepth = GetDeviceCaps(hdc, BITSPIXEL);
                DeleteDC(hdc);
                return (colorDepth >= 30);  // 10-bit per channel
            }
        }

        return false;
    }

    double CalculatePerformanceScore(const ExtendedGPUInfo& gpu) {
        double score = 0.0;

        // VRAM score (capped at 16GB)
        double dedicatedVRAM_GB = gpu.dedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
        score += std::min(dedicatedVRAM_GB, 16.0) * 8.0;  // Max 128 points

        // Encoder support
        score += gpu.capabilities.supportsH264Encode ? 5.0 : 0.0;
        score += gpu.capabilities.supportsH265Encode ? 15.0 : 0.0;
        score += gpu.capabilities.supportsAV1Encode ? 25.0 : 0.0;

        // Hardware acceleration bonus
        score += gpu.capabilities.supportsHardwareAcceleration ? 30.0 : 0.0;

        // Low latency bonus
        score += gpu.capabilities.supportsLowLatency ? 10.0 : 0.0;

        // HDR bonus
        score += gpu.capabilities.supportsHDR ? 5.0 : 0.0;

        // High framerate bonus
        if (gpu.capabilities.maxFramerate >= 240) score += 10.0;
        else if (gpu.capabilities.maxFramerate >= 120) score += 5.0;

        return score;
    }
};
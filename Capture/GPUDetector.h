 
#pragma once
#include "RecordingEngine.h"

class GPUDetector {
public:
    GPUDetector() = default;
    ~GPUDetector() = default;

    std::vector<ExtendedGPUInfo> DetectGPUs() {
        std::vector<ExtendedGPUInfo> gpus;

        ComPtr<IDXGIFactory6> factory;
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return gpus;

        ComPtr<IDXGIAdapter4> adapter;
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; i++) {

            ExtendedGPUInfo info;
            DXGI_ADAPTER_DESC3 desc;
            if (SUCCEEDED(adapter->GetDesc3(&desc))) {
                info.name = desc.Description;
                info.vendorId = std::to_wstring(desc.VendorId);
                info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
                info.sharedSystemMemory = desc.SharedSystemMemory;
                info.adapterLuid = desc.AdapterLuid; // Capture exact hardware map

                if (desc.VendorId == 0x10DE) { info.encoderType = EncoderType::NVIDIA_NVENC; DetectNVIDIACapabilities(desc.DeviceId, info); }
                else if (desc.VendorId == 0x1002) { info.encoderType = EncoderType::AMD_AMF; DetectAMDCapabilities(desc.DeviceId, info); }
                else if (desc.VendorId == 0x8086) { info.encoderType = EncoderType::INTEL_QSV; DetectIntelCapabilities(desc.DeviceId, info); }
                else { info.encoderType = EncoderType::SOFTWARE; }

                info.capabilities.supportsHDR = CheckHDRSupport(adapter.Get());
                info.performanceScore = CalculatePerformanceScore(info);

                gpus.push_back(info);
            }
        }

        std::sort(gpus.begin(), gpus.end(), [](const ExtendedGPUInfo& a, const ExtendedGPUInfo& b) { return a.performanceScore > b.performanceScore; });
        return gpus;
    }

    ExtendedGPUInfo GetOptimalGPU() {
        auto gpus = DetectGPUs();
        if (gpus.empty()) throw std::runtime_error("No GPU detected");
        return gpus[0];
    }

    uint64_t GetSystemMemory() {
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        GlobalMemoryStatusEx(&status);
        return status.ullTotalPhys;
    }

    ExtendedGPUInfo GetGPUByIndex(size_t index) {
        auto gpus = DetectGPUs();
        if (index >= gpus.size()) throw std::out_of_range("GPU index out of range");
        return gpus[index];
    }

private:
    void DetectNVIDIACapabilities(UINT deviceId, ExtendedGPUInfo& info) {
        info.supportsAV1 = (deviceId >= 0x2700);
        info.capabilities.supportsH264Encode = true;
        info.capabilities.supportsH265Encode = true;
        info.capabilities.supportsH264Decode = true;
        info.capabilities.supportsH265Decode = true;
        info.capabilities.supportsAV1Encode = info.supportsAV1;
        info.capabilities.supportsAV1Decode = info.supportsAV1;
        info.capabilities.supportsHardwareAcceleration = true;
        info.capabilities.maxFramerate = 240;
        info.capabilities.maxEncodedResolution = 8192;
    }

    void DetectAMDCapabilities(UINT deviceId, ExtendedGPUInfo& info) {
        info.supportsAV1 = (deviceId >= 0x1700);
        info.capabilities.supportsH264Encode = true;
        info.capabilities.supportsH265Encode = true;
        info.capabilities.supportsAV1Encode = info.supportsAV1;
        info.capabilities.supportsHardwareAcceleration = true;
        info.capabilities.maxFramerate = 120;
    }

    void DetectIntelCapabilities(UINT deviceId, ExtendedGPUInfo& info) {
        info.supportsAV1 = (deviceId >= 0x5600);
        info.capabilities.supportsH264Encode = true;
        info.capabilities.supportsH265Encode = true;
        info.capabilities.supportsAV1Encode = info.supportsAV1;
        info.capabilities.supportsHardwareAcceleration = true;
        info.capabilities.maxFramerate = 120;
    }

    bool CheckHDRSupport(IDXGIAdapter* adapter) {
        ComPtr<IDXGIOutput> output;
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor) {
                HDC hdc = CreateDCW(desc.DeviceName, desc.DeviceName, NULL, NULL);
                if (hdc) {
                    bool isDisplay = (GetDeviceCaps(hdc, TECHNOLOGY) == DT_RASDISPLAY);
                    DeleteDC(hdc); return isDisplay;
                }
            }
        }
        return false;
    }

    double CalculatePerformanceScore(const ExtendedGPUInfo& gpu) {
        double score = 0.0;
        double dedicatedVRAM_GB = gpu.dedicatedVideoMemory / (1024.0 * 1024 * 1024);
        score += std::min(dedicatedVRAM_GB, 12.0) * 10.0;
        score += gpu.capabilities.supportsH264Encode ? 5.0 : 0.0;
        score += gpu.capabilities.supportsH265Encode ? 10.0 : 0.0;
        score += gpu.capabilities.supportsAV1Encode ? 20.0 : 0.0;
        score += gpu.capabilities.supportsHardwareAcceleration ? 25.0 : 0.0;
        return score;
    }
};
 
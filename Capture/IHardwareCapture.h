#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Generic frame structure
struct HardwareFrame {
    void* data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;
    bool isKeyframe;
};

// Generic device info
struct HardwareDeviceInfo {
    std::string id;
    std::string name;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_fps;
};

// Hardware Abstraction Layer interface
class IHardwareCapture {
public:
    virtual ~IHardwareCapture() = default;

    // Device enumeration and initialization
    virtual std::vector<HardwareDeviceInfo> EnumerateDevices() = 0;
    virtual bool OpenDevice(const std::string& deviceId) = 0;
    
    // Format negotiation
    virtual bool NegotiateFormat(uint32_t& width, uint32_t& height, uint32_t& fps, std::string& pixelFormat) = 0;

    // Capture control
    virtual bool StartCapture() = 0;
    virtual bool GetNextFrame(HardwareFrame& frame, uint32_t timeoutMs) = 0;
    virtual void StopCapture() = 0;
    virtual void CloseDevice() = 0;
};

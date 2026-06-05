#pragma once

#include "IHardwareCapture.h"

#ifdef USE_LIBUSB
#include <libusb-1.0/libusb.h>
#endif

class LibusbCapture : public IHardwareCapture {
public:
    LibusbCapture() : m_ctx(nullptr), m_handle(nullptr), m_running(false) {}
    ~LibusbCapture() override { CloseDevice(); }

    std::vector<HardwareDeviceInfo> EnumerateDevices() override {
        std::vector<HardwareDeviceInfo> devices;
#ifdef USE_LIBUSB
        if (libusb_init(&m_ctx) < 0) return devices;

        libusb_device** devs;
        ssize_t count = libusb_get_device_list(m_ctx, &devs);
        if (count < 0) return devices;

        for (ssize_t i = 0; i < count; ++i) {
            struct libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(devs[i], &desc) < 0) continue;

            // Check if Video Class (0x0E) or vendor specific
            if (desc.bDeviceClass == 0x0E || desc.bDeviceClass == 0xFF) {
                HardwareDeviceInfo info;
                info.id = std::to_string(desc.idVendor) + ":" + std::to_string(desc.idProduct);
                info.name = "USB Capture Device";
                info.max_width = 1920;
                info.max_height = 1080;
                info.max_fps = 60;
                devices.push_back(info);
            }
        }
        libusb_free_device_list(devs, 1);
#endif
        return devices;
    }

    bool OpenDevice(const std::string& deviceId) override {
#ifdef USE_LIBUSB
        if (!m_ctx) libusb_init(&m_ctx);

        size_t colon = deviceId.find(':');
        if (colon == std::string::npos) return false;

        uint16_t vid = std::stoi(deviceId.substr(0, colon), nullptr, 16);
        uint16_t pid = std::stoi(deviceId.substr(colon + 1), nullptr, 16);

        m_handle = libusb_open_device_with_vid_pid(m_ctx, vid, pid);
        return m_handle != nullptr;
#else
        return false;
#endif
    }

    bool NegotiateFormat(uint32_t& width, uint32_t& height, uint32_t& fps, std::string& pixelFormat) override {
        // Mock UVC negotiation
        width = 1920; height = 1080; fps = 60; pixelFormat = "YUYV";
        return m_handle != nullptr;
    }

    bool StartCapture() override {
#ifdef USE_LIBUSB
        if (!m_handle) return false;
        libusb_claim_interface(m_handle, 0); // Video Control
        libusb_claim_interface(m_handle, 1); // Video Streaming
        m_running = true;
        return true;
#else
        return false;
#endif
    }

    bool GetNextFrame(HardwareFrame& frame, uint32_t timeoutMs) override {
        // Would poll libusb async transfers, here returning false mock
        return false;
    }

    void StopCapture() override {
        m_running = false;
    }

    void CloseDevice() override {
        StopCapture();
#ifdef USE_LIBUSB
        if (m_handle) {
            libusb_release_interface(m_handle, 1);
            libusb_release_interface(m_handle, 0);
            libusb_close(m_handle);
            m_handle = nullptr;
        }
        if (m_ctx) {
            libusb_exit(m_ctx);
            m_ctx = nullptr;
        }
#endif
    }

private:
#ifdef USE_LIBUSB
    libusb_context* m_ctx;
    libusb_device_handle* m_handle;
#else
    void* m_ctx;
    void* m_handle;
#endif
    bool m_running;
};

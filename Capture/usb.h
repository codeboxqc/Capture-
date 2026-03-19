#pragma once
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <spdlog/spdlog.h>
#include <vector>
#include <string>
#include <algorithm>  // For std::transform

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

// USB Device Information
struct USBDeviceInfo {
    std::string deviceName;
    std::string devicePath;
    std::string manufacturer;
    std::string description;
    std::string serialNumber;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    bool isVideoCapture = false;
    bool isAudioCapture = false;
    std::string deviceType;  // "Webcam", "Capture Card", "Unknown"
};

class USBDeviceDetector {
public:
    static std::vector<USBDeviceInfo> DetectUSBDevices() {
        std::vector<USBDeviceInfo> devices;

        // Detect video capture devices (webcams, capture cards)
        auto videoDevices = DetectVideoCaptureDevices();
        devices.insert(devices.end(), videoDevices.begin(), videoDevices.end());

        spdlog::info("Detected {} USB capture device(s)", devices.size());
        return devices;
    }

    static std::vector<USBDeviceInfo> DetectVideoCaptureDevices() {
        std::vector<USBDeviceInfo> devices;
        spdlog::info("Scanning for video capture devices (filtering audio-only)...");

        // Look for devices in GUID_DEVCLASS_MEDIA class
        HDEVINFO deviceInfo = SetupDiGetClassDevsW(
            &GUID_DEVCLASS_MEDIA,
            nullptr,
            nullptr,
            DIGCF_PRESENT
        );

        if (deviceInfo == INVALID_HANDLE_VALUE) {
            spdlog::warn("Failed to get device info");
            return devices;
        }

        SP_DEVINFO_DATA deviceInfoData;
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfo, i, &deviceInfoData); i++) {
            // ===== FILTER: Skip Bluetooth devices =====
            WCHAR deviceInstanceId[MAX_PATH] = { 0 };
            if (SetupDiGetDeviceInstanceIdW(deviceInfo, &deviceInfoData, deviceInstanceId, MAX_PATH, nullptr)) {
                std::wstring instanceId(deviceInstanceId);
                // Skip Bluetooth devices
                if (instanceId.find(L"BTHENUM") != std::wstring::npos ||
                    instanceId.find(L"BTHUSB") != std::wstring::npos) {
                    continue;
                }
            }

            USBDeviceInfo device;
            device.isVideoCapture = true;
            device.deviceType = "Video Capture";

            // Get device name
            WCHAR friendlyName[MAX_PATH] = { 0 };
            DWORD requiredSize = 0;
            if (SetupDiGetDeviceRegistryPropertyW(
                deviceInfo, &deviceInfoData, SPDRP_FRIENDLYNAME,
                nullptr, (PBYTE)friendlyName, sizeof(friendlyName), &requiredSize)) {
                int size = WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nullptr, 0, nullptr, nullptr);
                if (size > 0) {
                    device.deviceName.resize(size - 1);
                    WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, &device.deviceName[0], size, nullptr, nullptr);
                }
            }

            // Get manufacturer
            WCHAR mfg[MAX_PATH] = { 0 };
            if (SetupDiGetDeviceRegistryPropertyW(
                deviceInfo, &deviceInfoData, SPDRP_MFG,
                nullptr, (PBYTE)mfg, sizeof(mfg), &requiredSize)) {
                int size = WideCharToMultiByte(CP_UTF8, 0, mfg, -1, nullptr, 0, nullptr, nullptr);
                if (size > 0) {
                    device.manufacturer.resize(size - 1);
                    WideCharToMultiByte(CP_UTF8, 0, mfg, -1, &device.manufacturer[0], size, nullptr, nullptr);
                }
            }

            // ===== FILTER: Skip audio-only devices =====
            std::string nameLower = device.deviceName;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find("stereo") != std::string::npos ||
                nameLower.find("a2dp") != std::string::npos ||
                nameLower.find("hands-free") != std::string::npos ||
                nameLower.find("headset") != std::string::npos) {
                continue;  // Skip Bluetooth audio
            }

            // Categorize device type
            if (device.deviceName.find("Webcam") != std::string::npos ||
                device.deviceName.find("Camera") != std::string::npos) {
                device.deviceType = "Webcam";
            }
            else if (device.deviceName.find("Elgato") != std::string::npos ||
                device.deviceName.find("AVerMedia") != std::string::npos ||
                device.deviceName.find("Magewell") != std::string::npos ||
                device.deviceName.find("Blackmagic") != std::string::npos) {
                device.deviceType = "Capture Card";
            }

            if (!device.deviceName.empty()) {
                spdlog::info("  Video: {} (Type: {})", device.deviceName, device.deviceType);
                devices.push_back(device);
            }
        }

        SetupDiDestroyDeviceInfoList(deviceInfo);
        return devices;
    }

    // Get best USB device for recording
    static USBDeviceInfo GetBestUSBDevice(const std::vector<USBDeviceInfo>& devices) {
        // Prefer capture cards over webcams
        for (const auto& device : devices) {
            if (device.deviceType == "Capture Card" && device.isVideoCapture) {
                spdlog::info("Selected best USB device: {} (Capture Card)", device.deviceName);
                return device;
            }
        }

        // Fall back to webcam
        for (const auto& device : devices) {
            if (device.isVideoCapture) {
                spdlog::info("Selected best USB device: {} (Webcam)", device.deviceName);
                return device;
            }
        }

        // Return empty device if none found
        spdlog::warn("No suitable USB device found");
        return USBDeviceInfo();
    }

private:
    static void ExtractUSBIdentifiers(const WCHAR* deviceInstanceId, USBDeviceInfo& device) {
        // Device instance ID format: USB\VID_XXXX&PID_YYYY\...
        std::wstring instanceId(deviceInstanceId);

        size_t vidPos = instanceId.find(L"VID_");
        size_t pidPos = instanceId.find(L"PID_");

        if (vidPos != std::wstring::npos) {
            vidPos += 4;  // Skip "VID_"
            std::wstring vidStr = instanceId.substr(vidPos, 4);
            device.vendorId = static_cast<uint16_t>(std::wcstol(vidStr.c_str(), nullptr, 16));
        }

        if (pidPos != std::wstring::npos) {
            pidPos += 4;  // Skip "PID_"
            std::wstring pidStr = instanceId.substr(pidPos, 4);
            device.productId = static_cast<uint16_t>(std::wcstol(pidStr.c_str(), nullptr, 16));
        }
    }
};
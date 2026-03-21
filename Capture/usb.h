#pragma once
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <spdlog/spdlog.h>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")

// Known Capture Card Vendor IDs
namespace CaptureCardVendors {
    constexpr uint16_t ELGATO = 0x0FD9;
    constexpr uint16_t AVERMEDIA = 0x07CA;
    constexpr uint16_t MAGEWELL = 0x2935;
    constexpr uint16_t BLACKMAGIC = 0x1EDB;
    constexpr uint16_t STARTECH = 0x1BCF;
    constexpr uint16_t RAZER = 0x1532;
    constexpr uint16_t CORSAIR = 0x1B1C;
    constexpr uint16_t HAUPPAUGE = 0x2040;
    constexpr uint16_t YUAN = 0x1164;
    constexpr uint16_t EPIPHAN = 0x1D6B;
    constexpr uint16_t DATAPATH = 0x0955;
    constexpr uint16_t INOGENI = 0x25A4;
    constexpr uint16_t PENGO = 0x534D;
    constexpr uint16_t EZCAP = 0x1B80;
    constexpr uint16_t GENKI = 0x28DE;
    constexpr uint16_t J5CREATE = 0x2109;
    constexpr uint16_t ATOMOS = 0x0547;
    constexpr uint16_t DECKLINK = 0x1EDB;  // Blackmagic DeckLink
    constexpr uint16_t OSPREY = 0x1D6C;
    constexpr uint16_t MOKOSE = 0x345F;
    constexpr uint16_t DIGITNOW = 0x1F4D;
    constexpr uint16_t ACASIS = 0x0BDA;    // Realtek-based capture cards
    constexpr uint16_t LINKSTABLE = 0x1908;
}

// USB Device Information
struct USBDeviceInfo {
    std::string deviceName;
    std::string devicePath;
    std::string manufacturer;
    std::string description;
    std::string serialNumber;
    std::string hardwareId;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    bool isVideoCapture = false;
    bool isAudioCapture = false;
    bool isKnownCaptureCard = false;
    std::string deviceType;      // "Webcam", "Capture Card", "Game Capture", "HDMI Capture", etc.
    std::string connectionType;  // "USB 2.0", "USB 3.0", "Thunderbolt", "PCIe"
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    uint32_t maxFps = 0;
};

class USBDeviceDetector {
public:
    // Main detection function - combines multiple methods for best results
    static std::vector<USBDeviceInfo> DetectUSBDevices() {
        std::vector<USBDeviceInfo> devices;
        std::unordered_set<std::string> seenDevices;  // Prevent duplicates

        // Method 1: Media Foundation enumeration (most reliable for actual capture devices)
        auto mfDevices = DetectViaMFEnumeration();
        for (auto& dev : mfDevices) {
            if (seenDevices.find(dev.deviceName) == seenDevices.end()) {
                seenDevices.insert(dev.deviceName);
                devices.push_back(dev);
            }
        }

        // Method 2: SetupAPI enumeration (catches some devices MF misses)
        auto setupDevices = DetectViaSetupAPI();
        for (auto& dev : setupDevices) {
            if (seenDevices.find(dev.deviceName) == seenDevices.end()) {
                seenDevices.insert(dev.deviceName);
                devices.push_back(dev);
            }
        }

        // Sort: Capture cards first, then by name
        std::sort(devices.begin(), devices.end(), [](const USBDeviceInfo& a, const USBDeviceInfo& b) {
            if (a.isKnownCaptureCard != b.isKnownCaptureCard) {
                return a.isKnownCaptureCard > b.isKnownCaptureCard;
            }
            if (a.deviceType != b.deviceType) {
                if (a.deviceType == "Capture Card") return true;
                if (b.deviceType == "Capture Card") return false;
            }
            return a.deviceName < b.deviceName;
        });

        spdlog::info("Detected {} USB capture device(s)", devices.size());
        return devices;
    }

    // Get best USB device for recording (prioritizes capture cards)
    static USBDeviceInfo GetBestUSBDevice(const std::vector<USBDeviceInfo>& devices) {
        // Priority 1: Known capture card brands
        for (const auto& device : devices) {
            if (device.isKnownCaptureCard && device.isVideoCapture) {
                spdlog::info("Selected best USB device: {} (Known Capture Card)", device.deviceName);
                return device;
            }
        }

        // Priority 2: Any capture card
        for (const auto& device : devices) {
            if (device.deviceType == "Capture Card" && device.isVideoCapture) {
                spdlog::info("Selected best USB device: {} (Capture Card)", device.deviceName);
                return device;
            }
        }

        // Priority 3: HDMI/Game capture
        for (const auto& device : devices) {
            if ((device.deviceType == "HDMI Capture" || device.deviceType == "Game Capture") && device.isVideoCapture) {
                spdlog::info("Selected best USB device: {} ({})", device.deviceName, device.deviceType);
                return device;
            }
        }

        // Priority 4: Any video capture device
        for (const auto& device : devices) {
            if (device.isVideoCapture) {
                spdlog::info("Selected best USB device: {} ({})", device.deviceName, device.deviceType);
                return device;
            }
        }

        spdlog::warn("No suitable USB device found");
        return USBDeviceInfo();
    }

    // Check if a vendor ID is a known capture card manufacturer
    static bool IsKnownCaptureCardVendor(uint16_t vendorId) {
        switch (vendorId) {
            case CaptureCardVendors::ELGATO:
            case CaptureCardVendors::AVERMEDIA:
            case CaptureCardVendors::MAGEWELL:
            case CaptureCardVendors::BLACKMAGIC:
            case CaptureCardVendors::STARTECH:
            case CaptureCardVendors::RAZER:
            case CaptureCardVendors::HAUPPAUGE:
            case CaptureCardVendors::YUAN:
            case CaptureCardVendors::EPIPHAN:
            case CaptureCardVendors::DATAPATH:
            case CaptureCardVendors::INOGENI:
            case CaptureCardVendors::PENGO:
            case CaptureCardVendors::EZCAP:
            case CaptureCardVendors::GENKI:
            case CaptureCardVendors::J5CREATE:
            case CaptureCardVendors::ATOMOS:
            case CaptureCardVendors::OSPREY:
            case CaptureCardVendors::MOKOSE:
            case CaptureCardVendors::DIGITNOW:
            case CaptureCardVendors::LINKSTABLE:
                return true;
            default:
                return false;
        }
    }

private:
    // Method 1: Use Media Foundation to enumerate video capture devices
    static std::vector<USBDeviceInfo> DetectViaMFEnumeration() {
        std::vector<USBDeviceInfo> devices;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needsUninit = SUCCEEDED(hr);

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (needsUninit) CoUninitialize();
            return devices;
        }

        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 1);
        if (FAILED(hr)) {
            MFShutdown();
            if (needsUninit) CoUninitialize();
            return devices;
        }

        // Get video capture devices
        hr = attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
        );

        IMFActivate** ppDevices = nullptr;
        UINT32 count = 0;

        hr = MFEnumDeviceSources(attributes.Get(), &ppDevices, &count);
        if (SUCCEEDED(hr) && count > 0) {
            for (UINT32 i = 0; i < count; i++) {
                USBDeviceInfo device;
                device.isVideoCapture = true;

                // Get friendly name
                WCHAR* friendlyName = nullptr;
                UINT32 nameLen = 0;
                hr = ppDevices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                    &friendlyName,
                    &nameLen
                );

                if (SUCCEEDED(hr) && friendlyName) {
                    device.deviceName = WideToUtf8(friendlyName);
                    CoTaskMemFree(friendlyName);
                }

                // Get symbolic link (device path)
                WCHAR* symbolicLink = nullptr;
                UINT32 linkLen = 0;
                hr = ppDevices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &symbolicLink,
                    &linkLen
                );

                if (SUCCEEDED(hr) && symbolicLink) {
                    device.devicePath = WideToUtf8(symbolicLink);
                    
                    // Extract VID/PID from symbolic link
                    ExtractVidPidFromPath(symbolicLink, device);
                    CoTaskMemFree(symbolicLink);
                }

                // Get max resolution by checking media types
                GetDeviceCapabilities(ppDevices[i], device);

                // Categorize the device
                CategorizeDevice(device);

                // Filter out audio-only devices
                if (!IsAudioOnlyDevice(device.deviceName)) {
                    spdlog::info("  MF Video: {} (Type: {}, VID:{:04X} PID:{:04X})", 
                        device.deviceName, device.deviceType, device.vendorId, device.productId);
                    devices.push_back(device);
                }

                ppDevices[i]->Release();
            }
            CoTaskMemFree(ppDevices);
        }

        MFShutdown();
        if (needsUninit) CoUninitialize();

        return devices;
    }

    // Method 2: Use SetupAPI to enumerate devices
    static std::vector<USBDeviceInfo> DetectViaSetupAPI() {
        std::vector<USBDeviceInfo> devices;

        // Look for devices in GUID_DEVCLASS_MEDIA and GUID_DEVCLASS_IMAGE
        std::vector<const GUID*> classGuids = { &GUID_DEVCLASS_MEDIA, &GUID_DEVCLASS_IMAGE };

        for (const GUID* classGuid : classGuids) {
            HDEVINFO deviceInfo = SetupDiGetClassDevsW(
                classGuid,
                nullptr,
                nullptr,
                DIGCF_PRESENT
            );

            if (deviceInfo == INVALID_HANDLE_VALUE) continue;

            SP_DEVINFO_DATA deviceInfoData;
            deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

            for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfo, i, &deviceInfoData); i++) {
                USBDeviceInfo device;

                // Get device instance ID
                WCHAR deviceInstanceId[MAX_PATH] = { 0 };
                if (!SetupDiGetDeviceInstanceIdW(deviceInfo, &deviceInfoData, deviceInstanceId, MAX_PATH, nullptr)) {
                    continue;
                }

                std::wstring instanceId(deviceInstanceId);

                // Skip Bluetooth devices
                if (instanceId.find(L"BTHENUM") != std::wstring::npos ||
                    instanceId.find(L"BTHUSB") != std::wstring::npos ||
                    instanceId.find(L"BTH\\") != std::wstring::npos) {
                    continue;
                }

                // Extract VID/PID
                ExtractVidPidFromPath(deviceInstanceId, device);
                device.hardwareId = WideToUtf8(deviceInstanceId);

                // Get device name
                WCHAR friendlyName[MAX_PATH] = { 0 };
                DWORD requiredSize = 0;
                if (SetupDiGetDeviceRegistryPropertyW(
                    deviceInfo, &deviceInfoData, SPDRP_FRIENDLYNAME,
                    nullptr, (PBYTE)friendlyName, sizeof(friendlyName), &requiredSize)) {
                    device.deviceName = WideToUtf8(friendlyName);
                }
                else {
                    // Try device description if friendly name not available
                    WCHAR description[MAX_PATH] = { 0 };
                    if (SetupDiGetDeviceRegistryPropertyW(
                        deviceInfo, &deviceInfoData, SPDRP_DEVICEDESC,
                        nullptr, (PBYTE)description, sizeof(description), &requiredSize)) {
                        device.deviceName = WideToUtf8(description);
                    }
                }

                // Skip if no name
                if (device.deviceName.empty()) continue;

                // Get manufacturer
                WCHAR mfg[MAX_PATH] = { 0 };
                if (SetupDiGetDeviceRegistryPropertyW(
                    deviceInfo, &deviceInfoData, SPDRP_MFG,
                    nullptr, (PBYTE)mfg, sizeof(mfg), &requiredSize)) {
                    device.manufacturer = WideToUtf8(mfg);
                }

                // Detect connection type
                DetectConnectionType(instanceId, device);

                // Filter out audio-only devices
                if (IsAudioOnlyDevice(device.deviceName)) continue;

                // Check if it's a video capture device
                if (IsVideoCaptureDevice(device.deviceName, device.hardwareId)) {
                    device.isVideoCapture = true;
                    CategorizeDevice(device);
                    devices.push_back(device);
                }
            }

            SetupDiDestroyDeviceInfoList(deviceInfo);
        }

        return devices;
    }

    // Get device capabilities (max resolution, fps)
    static void GetDeviceCapabilities(IMFActivate* activate, USBDeviceInfo& device) {
        ComPtr<IMFMediaSource> source;
        HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr)) return;

        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
        if (FAILED(hr)) {
            source->Shutdown();
            return;
        }

        // Check native media types
        DWORD typeIndex = 0;
        ComPtr<IMFMediaType> mediaType;

        while (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, typeIndex, &mediaType))) {
            UINT32 width = 0, height = 0;
            MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);

            UINT32 fpsNum = 0, fpsDen = 1;
            MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
            uint32_t fps = (fpsDen > 0) ? (fpsNum / fpsDen) : 0;

            if (width > device.maxWidth || (width == device.maxWidth && height > device.maxHeight)) {
                device.maxWidth = width;
                device.maxHeight = height;
            }
            if (fps > device.maxFps) {
                device.maxFps = fps;
            }

            mediaType.Reset();
            typeIndex++;
        }

        source->Shutdown();
    }

    // Categorize device based on name and vendor
    static void CategorizeDevice(USBDeviceInfo& device) {
        std::string nameLower = device.deviceName;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        
        std::string mfgLower = device.manufacturer;
        std::transform(mfgLower.begin(), mfgLower.end(), mfgLower.begin(), ::tolower);

        // Check if known capture card vendor
        device.isKnownCaptureCard = IsKnownCaptureCardVendor(device.vendorId);

        // Categorize by name keywords
        if (nameLower.find("elgato") != std::string::npos ||
            nameLower.find("game capture") != std::string::npos ||
            nameLower.find("hd60") != std::string::npos ||
            nameLower.find("4k60") != std::string::npos ||
            nameLower.find("cam link") != std::string::npos) {
            device.deviceType = "Game Capture";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("avermedia") != std::string::npos ||
                 nameLower.find("live gamer") != std::string::npos ||
                 nameLower.find("gc5") != std::string::npos) {
            device.deviceType = "Game Capture";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("magewell") != std::string::npos ||
                 nameLower.find("usb capture") != std::string::npos ||
                 nameLower.find("pro capture") != std::string::npos) {
            device.deviceType = "Capture Card";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("blackmagic") != std::string::npos ||
                 nameLower.find("decklink") != std::string::npos ||
                 nameLower.find("intensity") != std::string::npos ||
                 nameLower.find("ultrastudio") != std::string::npos) {
            device.deviceType = "Capture Card";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("hdmi") != std::string::npos ||
                 nameLower.find("video capture") != std::string::npos ||
                 nameLower.find("capture video") != std::string::npos) {
            device.deviceType = "HDMI Capture";
        }
        else if (nameLower.find("razer") != std::string::npos &&
                 nameLower.find("ripsaw") != std::string::npos) {
            device.deviceType = "Game Capture";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("hauppauge") != std::string::npos ||
                 nameLower.find("hd pvr") != std::string::npos ||
                 nameLower.find("colossus") != std::string::npos) {
            device.deviceType = "Capture Card";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("epiphan") != std::string::npos ||
                 nameLower.find("av.io") != std::string::npos) {
            device.deviceType = "Capture Card";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("inogeni") != std::string::npos) {
            device.deviceType = "Capture Card";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("startech") != std::string::npos ||
                 nameLower.find("pexhdcap") != std::string::npos) {
            device.deviceType = "Capture Card";
            device.isKnownCaptureCard = true;
        }
        else if (nameLower.find("j5create") != std::string::npos ||
                 nameLower.find("jva") != std::string::npos) {
            device.deviceType = "HDMI Capture";
        }
        else if (nameLower.find("ezcap") != std::string::npos ||
                 nameLower.find("digitnow") != std::string::npos ||
                 nameLower.find("pengo") != std::string::npos) {
            device.deviceType = "HDMI Capture";
        }
        else if (nameLower.find("webcam") != std::string::npos ||
                 nameLower.find("web cam") != std::string::npos ||
                 nameLower.find("facecam") != std::string::npos ||
                 nameLower.find("brio") != std::string::npos ||
                 nameLower.find("c920") != std::string::npos ||
                 nameLower.find("c922") != std::string::npos ||
                 nameLower.find("c930") != std::string::npos) {
            device.deviceType = "Webcam";
        }
        else if (nameLower.find("camera") != std::string::npos ||
                 nameLower.find("cam") != std::string::npos) {
            device.deviceType = "Webcam";
        }
        else if (nameLower.find("usb3") != std::string::npos ||
                 nameLower.find("usb 3") != std::string::npos) {
            // Generic USB 3.0 capture device (like Magewell USB3.0 Capture)
            device.deviceType = "Capture Card";
        }
        else {
            device.deviceType = "Video Capture";
        }

        // Override if vendor ID indicates capture card
        if (device.isKnownCaptureCard && device.deviceType == "Video Capture") {
            device.deviceType = "Capture Card";
        }
    }

    // Check if device is audio-only
    static bool IsAudioOnlyDevice(const std::string& name) {
        std::string nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

        // Audio-only keywords
        if (nameLower.find("stereo mix") != std::string::npos ||
            nameLower.find("a2dp") != std::string::npos ||
            nameLower.find("hands-free") != std::string::npos ||
            nameLower.find("handsfree") != std::string::npos ||
            nameLower.find("headset") != std::string::npos ||
            nameLower.find("headphone") != std::string::npos ||
            nameLower.find("speaker") != std::string::npos ||
            nameLower.find("microphone") != std::string::npos ||
            nameLower.find("audio only") != std::string::npos ||
            nameLower.find("realtek") != std::string::npos ||
            nameLower.find("high definition audio") != std::string::npos) {
            
            // Exception: capture card audio (USB3.0 Capture Audio)
            if (nameLower.find("capture") != std::string::npos &&
                nameLower.find("audio") != std::string::npos) {
                return false;  // This is capture card audio, not audio-only
            }
            return true;
        }
        return false;
    }

    // Check if device is a video capture device
    static bool IsVideoCaptureDevice(const std::string& name, const std::string& hardwareId) {
        std::string nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

        // Positive indicators
        if (nameLower.find("capture") != std::string::npos ||
            nameLower.find("webcam") != std::string::npos ||
            nameLower.find("camera") != std::string::npos ||
            nameLower.find("video") != std::string::npos ||
            nameLower.find("hdmi") != std::string::npos ||
            nameLower.find("elgato") != std::string::npos ||
            nameLower.find("avermedia") != std::string::npos ||
            nameLower.find("magewell") != std::string::npos ||
            nameLower.find("blackmagic") != std::string::npos) {
            return true;
        }

        return false;
    }

    // Detect USB connection type
    static void DetectConnectionType(const std::wstring& instanceId, USBDeviceInfo& device) {
        if (instanceId.find(L"USB3") != std::wstring::npos ||
            instanceId.find(L"XHCI") != std::wstring::npos) {
            device.connectionType = "USB 3.0";
        }
        else if (instanceId.find(L"USB\\") != std::wstring::npos) {
            device.connectionType = "USB 2.0";
        }
        else if (instanceId.find(L"PCI") != std::wstring::npos) {
            device.connectionType = "PCIe";
        }
        else if (instanceId.find(L"THUNDERBOLT") != std::wstring::npos) {
            device.connectionType = "Thunderbolt";
        }
        else {
            device.connectionType = "Unknown";
        }
    }

    // Extract VID/PID from device path
    static void ExtractVidPidFromPath(const WCHAR* path, USBDeviceInfo& device) {
        std::wstring pathStr(path);
        
        // Convert to uppercase for consistent parsing
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towupper);

        size_t vidPos = pathStr.find(L"VID_");
        size_t pidPos = pathStr.find(L"PID_");

        if (vidPos != std::wstring::npos && vidPos + 8 <= pathStr.length()) {
            std::wstring vidStr = pathStr.substr(vidPos + 4, 4);
            device.vendorId = static_cast<uint16_t>(std::wcstol(vidStr.c_str(), nullptr, 16));
        }

        if (pidPos != std::wstring::npos && pidPos + 8 <= pathStr.length()) {
            std::wstring pidStr = pathStr.substr(pidPos + 4, 4);
            device.productId = static_cast<uint16_t>(std::wcstol(pidStr.c_str(), nullptr, 16));
        }
    }

    // Helper: Wide string to UTF-8
    static std::string WideToUtf8(const WCHAR* wstr) {
        if (!wstr || !*wstr) return std::string();
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return std::string();
        std::string str(size - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size, nullptr, nullptr);
        return str;
    }

    // ComPtr helper (if not using wrl)
    template<typename T>
    class ComPtr {
    public:
        ComPtr() : ptr(nullptr) {}
        ~ComPtr() { if (ptr) ptr->Release(); }
        T** operator&() { return &ptr; }
        T* operator->() { return ptr; }
        T* Get() { return ptr; }
        void Reset() { if (ptr) { ptr->Release(); ptr = nullptr; } }
    private:
        T* ptr;
    };
};

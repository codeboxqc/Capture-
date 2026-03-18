#pragma once
#include "RecordingEngine.h"
#include <functional>
#include <array>

// Display information structure
struct DisplayInfo {
    std::wstring displayName;
    std::string deviceName;  // For logging/reference
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshRate = 0;
    int32_t positionX = 0;
    int32_t positionY = 0;
    bool isPrimary = false;
    bool isAttachedToDesktop = true;
    HMONITOR hMonitor = nullptr;
};

class VirtualDisplayManager {
public:
    VirtualDisplayManager() : m_currentDisplay(0) {}

    bool Initialize() {
        spdlog::info("Initializing Virtual Display Manager");
        DetectAllDisplays();

        if (m_availableDisplays.empty()) {
            spdlog::warn("No displays detected");
            return false;
        }

        spdlog::info("Found {} display(s)", m_availableDisplays.size());
        for (size_t i = 0; i < m_availableDisplays.size(); i++) {
            spdlog::info("  Display {}: {} ({}x{}@{}Hz)",
                i,
                m_availableDisplays[i].deviceName,
                m_availableDisplays[i].width,
                m_availableDisplays[i].height,
                m_availableDisplays[i].refreshRate);
        }

        return true;
    }

    bool ConfigureDisplay(uint32_t width, uint32_t height, uint32_t fps) {
        if (m_currentDisplay >= m_availableDisplays.size()) {
            spdlog::error("Invalid display index: {}", m_currentDisplay);
            return false;
        }

        spdlog::info("Configured for capture: {}x{}@{}Hz (Display: {})",
            width, height, fps,
            m_availableDisplays[m_currentDisplay].deviceName);
        return true;
    }

    // ========== Display Selection Methods ==========

    // Get total number of displays
    size_t GetDisplayCount() const {
        return m_availableDisplays.size();
    }

    // Get display info by index
    const DisplayInfo& GetDisplayInfo(size_t index) const {
        if (index >= m_availableDisplays.size()) {
            throw std::out_of_range("Display index out of range");
        }
        return m_availableDisplays[index];
    }

    // Get all display names for UI selection
    std::vector<std::string> GetDisplayNames() const {
        std::vector<std::string> names;
        for (const auto& display : m_availableDisplays) {
            names.push_back(display.deviceName);
        }
        return names;
    }

    // Set which display to record from
    bool SetActiveDisplay(size_t index) {
        if (index >= m_availableDisplays.size()) {
            spdlog::error("Invalid display index: {}", index);
            return false;
        }
        m_currentDisplay = index;
        spdlog::info("Active display set to: {}", m_availableDisplays[index].deviceName);
        return true;
    }

    // Get currently selected display
    size_t GetActiveDisplay() const {
        return m_currentDisplay;
    }

    const DisplayInfo& GetActiveDisplayInfo() const {
        return m_availableDisplays[m_currentDisplay];
    }

    // Get primary display
    size_t GetPrimaryDisplayIndex() const {
        for (size_t i = 0; i < m_availableDisplays.size(); i++) {
            if (m_availableDisplays[i].isPrimary) {
                return i;
            }
        }
        return 0;  // Fallback to first display
    }

    // Get DXGI output for active display
    ComPtr<IDXGIOutput> GetActiveDisplayOutput() {
        if (m_currentDisplay >= m_availableDisplays.size()) {
            return nullptr;
        }

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            spdlog::error("Failed to create DXGI factory");
            return nullptr;
        }

        ComPtr<IDXGIAdapter> adapter;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters(i, &adapter)); i++) {
            ComPtr<IDXGIOutput> output;
            for (UINT j = 0; SUCCEEDED(adapter->EnumOutputs(j, &output)); j++) {
                DXGI_OUTPUT_DESC desc;
                if (SUCCEEDED(output->GetDesc(&desc))) {
                    // Match by monitor handle or device name
                    if (desc.Monitor == m_availableDisplays[m_currentDisplay].hMonitor ||
                        wcscmp(desc.DeviceName, m_availableDisplays[m_currentDisplay].displayName.c_str()) == 0) {
                        return output;
                    }
                }
            }
        }

        // Fallback: return first available output
        spdlog::warn("Could not match display by handle, using first available output");
        ComPtr<IDXGIAdapter> fallbackAdapter;
        ComPtr<IDXGIOutput> fallbackOutput;

        if (SUCCEEDED(factory->EnumAdapters(0, &fallbackAdapter)) &&
            SUCCEEDED(fallbackAdapter->EnumOutputs(0, &fallbackOutput))) {
            return fallbackOutput;
        }

        return nullptr;
    }

    // Get primary display output
    ComPtr<IDXGIOutput> GetPrimaryDisplayOutput() {
        SetActiveDisplay(GetPrimaryDisplayIndex());
        return GetActiveDisplayOutput();
    }

    // Get all available display modes for active display
    std::vector<DXGI_MODE_DESC> GetDisplayModes() const {
        std::vector<DXGI_MODE_DESC> modes;

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return modes;

        ComPtr<IDXGIOutput> output = const_cast<VirtualDisplayManager*>(this)->GetActiveDisplayOutput();
        if (!output) return modes;

        // Get supported modes
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT numModes = 0;

        hr = output->GetDisplayModeList(format, 0, &numModes, nullptr);
        if (SUCCEEDED(hr) && numModes > 0) {
            std::vector<DXGI_MODE_DESC> tempModes(numModes);
            hr = output->GetDisplayModeList(format, 0, &numModes, tempModes.data());
            if (SUCCEEDED(hr)) {
                modes = tempModes;
            }
        }

        return modes;
    }

private:
    std::vector<DisplayInfo> m_availableDisplays;
    size_t m_currentDisplay;

    // ========== Display Detection ==========
    void DetectAllDisplays() {
        m_availableDisplays.clear();

        // Use EnumDisplayDevices for more detailed info
        DISPLAY_DEVICE displayDevice;
        displayDevice.cb = sizeof(DISPLAY_DEVICE);

        for (UINT i = 0; EnumDisplayDevicesW(nullptr, i, &displayDevice, 0); i++) {
            // Skip mirroring devices
            if (displayDevice.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) {
                continue;
            }

            // Only process active displays
            if (!(displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
                continue;
            }

            DisplayInfo info;
            info.displayName = displayDevice.DeviceName;

            // Convert wide string to regular string for device name
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, &displayDevice.DeviceName[0],
                (int)wcslen(displayDevice.DeviceName), nullptr, 0, nullptr, nullptr);
            info.deviceName.resize(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, &displayDevice.DeviceName[0],
                (int)wcslen(displayDevice.DeviceName), &info.deviceName[0], size_needed, nullptr, nullptr);

            info.isPrimary = (displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

            // Get display settings
            DEVMODEW devMode;
            ZeroMemory(&devMode, sizeof(devMode));
            devMode.dmSize = sizeof(devMode);

            if (EnumDisplaySettingsW(displayDevice.DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
                info.width = devMode.dmPelsWidth;
                info.height = devMode.dmPelsHeight;
                info.refreshRate = devMode.dmDisplayFrequency;
                info.positionX = devMode.dmPosition.x;
                info.positionY = devMode.dmPosition.y;
            }

            // Get monitor handle
            POINT pt = { info.positionX, info.positionY };
            info.hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (!info.hMonitor) {
                // Try to get from window
                HWND hwnd = FindWindowW(nullptr, nullptr);
                if (hwnd) {
                    info.hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
                }
            }

            m_availableDisplays.push_back(info);
        }

        // Alternative detection using DXGI if EnumDisplayDevices didn't find all
        if (m_availableDisplays.empty()) {
            DetectDisplaysViaDXGI();
        }

        // Set primary display to active if not already set
        if (!m_availableDisplays.empty()) {
            bool primaryFound = false;
            for (size_t i = 0; i < m_availableDisplays.size(); i++) {
                if (m_availableDisplays[i].isPrimary) {
                    m_currentDisplay = i;
                    primaryFound = true;
                    break;
                }
            }
            if (!primaryFound) {
                m_currentDisplay = 0;  // Fallback to first
            }
        }
    }

    // Fallback display detection via DXGI
    void DetectDisplaysViaDXGI() {
        spdlog::info("Detecting displays via DXGI (fallback)");

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return;

        ComPtr<IDXGIAdapter> adapter;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters(i, &adapter)); i++) {
            ComPtr<IDXGIOutput> output;
            for (UINT j = 0; SUCCEEDED(adapter->EnumOutputs(j, &output)); j++) {
                DXGI_OUTPUT_DESC desc;
                if (SUCCEEDED(output->GetDesc(&desc))) {
                    DisplayInfo info;
                    info.displayName = desc.DeviceName;

                    // Convert to string
                    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &desc.DeviceName[0],
                        (int)wcslen(desc.DeviceName), nullptr, 0, nullptr, nullptr);
                    info.deviceName.resize(size_needed, 0);
                    WideCharToMultiByte(CP_UTF8, 0, &desc.DeviceName[0],
                        (int)wcslen(desc.DeviceName), &info.deviceName[0], size_needed, nullptr, nullptr);

                    RECT bounds = desc.DesktopCoordinates;
                    info.width = bounds.right - bounds.left;
                    info.height = bounds.bottom - bounds.top;
                    info.positionX = bounds.left;
                    info.positionY = bounds.top;
                    info.hMonitor = desc.Monitor;

                    m_availableDisplays.push_back(info);
                }
            }
        }
    }
};
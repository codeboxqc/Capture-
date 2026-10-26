void DetectAllDisplays() {
        m_availableDisplays.clear();

        // Use DISPLAY_DEVICEW explicitly
        DISPLAY_DEVICEW displayDevice;
        ZeroMemory(&displayDevice, sizeof(displayDevice));
        displayDevice.cb = sizeof(DISPLAY_DEVICEW);

        for (UINT i = 0; EnumDisplayDevicesW(nullptr, i, &displayDevice, 0); i++) {
            if (displayDevice.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) {
                continue;
            }

            if (!(displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
                continue;
            }

            DisplayInfo info;
            info.displayName = displayDevice.DeviceName;

            int size_needed = WideCharToMultiByte(CP_UTF8, 0, displayDevice.DeviceName,
                (int)wcslen(displayDevice.DeviceName), nullptr, 0, nullptr, nullptr);
            info.deviceName.resize(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, displayDevice.DeviceName,
                (int)wcslen(displayDevice.DeviceName), &info.deviceName[0], size_needed, nullptr, nullptr);

            info.isPrimary = (displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

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

            POINT pt = { info.positionX, info.positionY };
            info.hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (!info.hMonitor) {
                HWND hwnd = FindWindowW(nullptr, nullptr);
                if (hwnd) {
                    info.hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
                }
            }

            m_availableDisplays.push_back(info);
        }

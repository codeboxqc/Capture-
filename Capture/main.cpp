#include "RecordingEngine.h"
#include "RecordingPipeline.h"
#include "GPUDetector.h"
#include "VirtualDisplayManager.h"
#include "USB.h"
#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <thread>
#include <atomic>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern void EnableLargePages();

std::shared_ptr<IRecordingEngine> g_engine;
std::mutex g_engineMutex;
RecordingSettings g_settings;
std::vector<ExtendedGPUInfo> g_availableGPUs;
std::atomic<bool> g_recording{ false };
bool g_showSettings = true;
PerformanceMetrics g_metrics;
std::string g_statusMessage = "Ready";
std::mutex g_statusMutex;
char g_outputPath[512] = "C:\\Recordings\\capture.hevc";
float g_ramBufferSizeGB = 4.0f;
float g_bitrateMbps = 50.0f;

// New time recording variables
int g_recordingMode = 0; // 0 = Duration, 1 = Schedule
int g_durationHours = 0;
int g_durationMinutes = 2; // Default 2 minutes
int g_durationSeconds = 0;
int g_startHour = 7; // Default 7:00
int g_startMinute = 0;
int g_stopHour = 8; // Default 8:00
int g_stopMinute = 0;
std::chrono::system_clock::time_point g_recordingStartTime;
std::chrono::system_clock::time_point g_scheduledStopTime;
char g_currentTimerDisplay[64] = "00:00:00";
bool g_scheduleActive = false;

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

const char* GetEncoderName(EncoderType type) {
    switch (type) {
    case EncoderType::NVIDIA_NVENC: return "NVIDIA NVENC (Hardware)";
    case EncoderType::AMD_AMF: return "AMD AMF VCE (Hardware)";
    case EncoderType::INTEL_QSV: return "Intel Quick Sync (Hardware)";
    default: return "Software / Unknown";
    }
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

// Open folder in Windows Explorer
void OpenOutputFolder() {
    std::filesystem::path outputPath(g_outputPath);
    std::filesystem::path folderPath = outputPath.parent_path();
    
    // Create folder if it doesn't exist
    if (!std::filesystem::exists(folderPath)) {
        std::filesystem::create_directories(folderPath);
    }
    
    // Open folder in Explorer
    ShellExecuteW(nullptr, L"open", folderPath.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Generate unique filename with auto-increment if file exists
std::string GetUniqueFilename(const std::string& basePath) {
    std::filesystem::path path(basePath);
    
    if (!std::filesystem::exists(path)) {
        return basePath;  // File doesn't exist, use as-is
    }
    
    // File exists, need to increment
    std::filesystem::path parent = path.parent_path();
    std::string stem = path.stem().string();
    std::string ext = path.extension().string();
    
    // Remove existing number suffix if present (e.g., "capture_001" -> "capture")
    size_t underscorePos = stem.rfind('_');
    std::string baseStem = stem;
    if (underscorePos != std::string::npos) {
        std::string suffix = stem.substr(underscorePos + 1);
        bool isNumber = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit);
        if (isNumber) {
            baseStem = stem.substr(0, underscorePos);
        }
    }
    
    // Find next available number
    int counter = 1;
    std::filesystem::path newPath;
    do {
        char numStr[16];
        snprintf(numStr, sizeof(numStr), "_%03d", counter);
        newPath = parent / (baseStem + numStr + ext);
        counter++;
    } while (std::filesystem::exists(newPath) && counter < 9999);
    
    return newPath.string();
}

// Modern Black & Red Theme Setup
void SetupModernDarkRedTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Main Colors - Deep Black with Red Accents
    ImVec4 bgDark = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);        // Near black background
    ImVec4 bgMedium = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);      // Slightly lighter panels
    ImVec4 bgLight = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);       // Borders/frames
    ImVec4 redPrimary = ImVec4(0.90f, 0.15f, 0.15f, 1.00f);    // Primary red
    ImVec4 redHover = ImVec4(1.00f, 0.25f, 0.25f, 1.00f);      // Hover red
    ImVec4 redActive = ImVec4(0.70f, 0.10f, 0.10f, 1.00f);     // Active/pressed red
    ImVec4 redDark = ImVec4(0.50f, 0.08f, 0.08f, 1.00f);       // Dark red for headers
    ImVec4 textWhite = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);     // Primary text
    ImVec4 textDim = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);       // Dimmed text
    ImVec4 textRed = ImVec4(1.00f, 0.40f, 0.40f, 1.00f);       // Red text accent

    // Backgrounds
    colors[ImGuiCol_WindowBg] = bgDark;
    colors[ImGuiCol_ChildBg] = bgDark;
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.98f);
    colors[ImGuiCol_MenuBarBg] = bgMedium;

    // Borders
    colors[ImGuiCol_Border] = ImVec4(0.25f, 0.08f, 0.08f, 0.60f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Text
    colors[ImGuiCol_Text] = textWhite;
    colors[ImGuiCol_TextDisabled] = textDim;

    // Headers (Tab bars, collapsing headers)
    colors[ImGuiCol_Header] = redDark;
    colors[ImGuiCol_HeaderHovered] = redPrimary;
    colors[ImGuiCol_HeaderActive] = redActive;

    // Buttons
    colors[ImGuiCol_Button] = redPrimary;
    colors[ImGuiCol_ButtonHovered] = redHover;
    colors[ImGuiCol_ButtonActive] = redActive;

    // Frame backgrounds (input fields, sliders, etc.)
    colors[ImGuiCol_FrameBg] = bgLight;
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.10f, 0.10f, 1.00f);

    // Tabs
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TabHovered] = redPrimary;
    colors[ImGuiCol_TabActive] = redPrimary;
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.04f, 0.04f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = redDark;

    // Title bar
    colors[ImGuiCol_TitleBg] = bgDark;
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.04f, 0.04f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = bgDark;

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg] = bgDark;
    colors[ImGuiCol_ScrollbarGrab] = redDark;
    colors[ImGuiCol_ScrollbarGrabHovered] = redPrimary;
    colors[ImGuiCol_ScrollbarGrabActive] = redHover;

    // Checkmark & Slider
    colors[ImGuiCol_CheckMark] = redHover;
    colors[ImGuiCol_SliderGrab] = redPrimary;
    colors[ImGuiCol_SliderGrabActive] = redHover;

    // Resize grip
    colors[ImGuiCol_ResizeGrip] = redDark;
    colors[ImGuiCol_ResizeGripHovered] = redPrimary;
    colors[ImGuiCol_ResizeGripActive] = redHover;

    // Separator
    colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.10f, 0.10f, 0.80f);
    colors[ImGuiCol_SeparatorHovered] = redPrimary;
    colors[ImGuiCol_SeparatorActive] = redHover;

    // Plot lines and histograms
    colors[ImGuiCol_PlotLines] = textRed;
    colors[ImGuiCol_PlotLinesHovered] = redHover;
    colors[ImGuiCol_PlotHistogram] = redPrimary;
    colors[ImGuiCol_PlotHistogramHovered] = redHover;

    // Table
    colors[ImGuiCol_TableHeaderBg] = redDark;
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.04f, 0.04f, 0.40f);

    // Nav highlight
    colors[ImGuiCol_NavHighlight] = redPrimary;
    colors[ImGuiCol_NavWindowingHighlight] = redHover;
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);

    // Modal dim
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.70f);

    // Text selection
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.50f, 0.15f, 0.15f, 0.60f);

    // Drag/Drop
    colors[ImGuiCol_DragDropTarget] = redHover;

    // ============ Modern Style Settings - Extra Rounded ============
    style.WindowRounding = 12.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 10.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    // Compact padding for smaller window
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
}

class RecordingApp {
public:
    int Run(HINSTANCE hInstance, int nCmdShow) {
        EnableLargePages();

        if (!CreateMainWindow(hInstance, nCmdShow)) return 1;
        if (!CreateDeviceD3D(m_hWnd)) {
            CleanupDeviceD3D();
            return 1;
        }

        if (!SetupImGui()) {
            Cleanup();
            return 1;
        }

        {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            g_engine = CreateRecordingEngine();
            g_engine->SetStatusCallback([](const std::string& msg) {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_statusMessage = msg;
            });
            g_engine->SetErrorCallback([](const std::string& msg) {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_statusMessage = "ERROR: " + msg;
            });

            if (!g_engine->Initialize()) {
                g_engine.reset();
                Cleanup();
                return 1;
            }
        }

        DetectGPUs();
        DetectDisplays();
        DetectUSBDevices();

        ShowWindow(m_hWnd, nCmdShow);
        UpdateWindow(m_hWnd);

        MSG msg = {};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                continue;
            }
            RenderFrame();
        }

        Cleanup();
        return 0;
    }

private:
    int m_currentResolution = 2; // Default 4K
    int m_currentFps = 1;        // Default 60/120
    int m_currentCodec = 1;      // Default HEVC

    int m_selectedGPU = 0;
    int m_selectedDisplay = 0;
    int m_selectedUSBDevice = -1;
    int m_durationMinutes = 0;   // Auto-stop limit (kept for backward compatibility)

    std::vector<DisplayInfo> m_displayList;
    std::vector<USBDeviceInfo> m_usbDeviceList;
    std::vector<std::string> m_displayNames;
    std::vector<std::string> m_usbDeviceNames;

    VirtualDisplayManager m_displayManager;
    HWND m_hWnd = nullptr;

    bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"RecordingEngine", nullptr };
        ::RegisterClassExW(&wc);
        m_hWnd = ::CreateWindowW(wc.lpszClassName, L"GPU Recording Engine", WS_OVERLAPPEDWINDOW, 100, 100, 820, 620, nullptr, nullptr, wc.hInstance, nullptr);
        return m_hWnd != nullptr;
    }

    bool SetupImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        // Apply modern black & red theme
        SetupModernDarkRedTheme();

        // Load a nicer font (optional - uses default if not available)
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();

        ImGui_ImplWin32_Init(m_hWnd);
        ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
        return true;
    }

    void DetectGPUs() {
        try {
            GPUDetector detector;
            g_availableGPUs = detector.DetectGPUs();
            if (!g_availableGPUs.empty()) m_selectedGPU = 0;
        }
        catch (const std::exception& e) { spdlog::error("GPU detection failed"); }
    }

    void DetectDisplays() {
        if (!m_displayManager.Initialize()) return;
        size_t displayCount = m_displayManager.GetDisplayCount();
        for (size_t i = 0; i < displayCount; i++) {
            const auto& displayInfo = m_displayManager.GetDisplayInfo(i);
            m_displayList.push_back(displayInfo);
            std::string displayName = displayInfo.deviceName + " (" + std::to_string(displayInfo.width) + "x" + std::to_string(displayInfo.height) + "@" + std::to_string(displayInfo.refreshRate) + "Hz)";
            if (displayInfo.isPrimary) displayName += " [Primary]";
            m_displayNames.push_back(displayName);
        }
        if (!m_displayList.empty()) m_selectedDisplay = m_displayManager.GetPrimaryDisplayIndex();
    }

    void DetectUSBDevices() {
        m_usbDeviceList = USBDeviceDetector::DetectUSBDevices();
        for (const auto& device : m_usbDeviceList) {
            if (device.isVideoCapture) m_usbDeviceNames.push_back(device.deviceName + " [" + device.deviceType + "]");
        }
    }

    void UpdateTimerDisplay() {
        if (!g_recording) {
            strcpy_s(g_currentTimerDisplay, "00:00:00");
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_recordingStartTime).count();

        int hours = static_cast<int>(elapsed / 3600);
        int minutes = static_cast<int>((elapsed % 3600) / 60);
        int seconds = static_cast<int>(elapsed % 60);

        snprintf(g_currentTimerDisplay, sizeof(g_currentTimerDisplay), "%02d:%02d:%02d", hours, minutes, seconds);
    }

    bool CheckScheduledTime() {
        if (!g_scheduleActive) return true;

        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm local_now;
        localtime_s(&local_now, &now_time_t);

        int currentMinutesSinceMidnight = local_now.tm_hour * 60 + local_now.tm_min;

        // Check if current time is between start and stop
        bool shouldBeRecording = (currentMinutesSinceMidnight >= (g_startHour * 60 + g_startMinute) &&
            currentMinutesSinceMidnight < (g_stopHour * 60 + g_stopMinute));

        if (shouldBeRecording && !g_recording) {
            // Auto-start recording
            StartRecording();
        }
        else if (!shouldBeRecording && g_recording) {
            // Auto-stop recording
            StopRecording();
        }

        return shouldBeRecording;
    }

    void RenderFrame() {
        // Automatically sync UI if the backend engine auto-stopped due to the time limit
        {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            if (g_recording && g_engine && !g_engine->IsRecording()) {
                g_recording = false;
            }
        }

        // Check for scheduled recording
        if (g_recordingMode == 1) {
            CheckScheduledTime();
        }

        // Update timer display
        UpdateTimerDisplay();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // MAKE IT A SINGLE APPLICATION WINDOW
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

        // Thread-safe access to status message
        std::string statusMsg;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            statusMsg = g_statusMessage;
        }

        if (ImGui::Begin("Recording Engine Control Panel", nullptr, windowFlags)) {

            // ============ MODERN HEADER ============
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::SetWindowFontScale(1.3f);
            ImGui::Text("GPU RECORDING ENGINE");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            ImGui::SameLine(ImGui::GetWindowWidth() - 120);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::Text("PRO EDITION");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.5f, 0.1f, 0.1f, 0.8f));
            ImGui::Separator();
            ImGui::PopStyleColor();
            ImGui::Spacing();

            if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {

                // --- TAB 1: RECORDING CONTROLS ---
                if (ImGui::BeginTabItem("  Recording  ")) {
                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Status Section
                    ImGui::PushStyleColor(ImGuiCol_Text, g_recording ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.3f, 0.9f, 0.3f, 1.0f));
                    ImGui::Text(g_recording ? "● RECORDING" : "● READY");
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                    ImGui::Text("  |  %s", statusMsg.c_str());
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Timer Display - Large and prominent
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::SetWindowFontScale(2.0f);
                    ImGui::Text("%s", g_currentTimerDisplay);
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::PopStyleColor();

                    // System time
                    auto now = std::chrono::system_clock::now();
                    auto now_time_t = std::chrono::system_clock::to_time_t(now);
                    struct tm local_now;
                    localtime_s(&local_now, &now_time_t);
                    char timeStr[9];
                    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &local_now);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("System: %s", timeStr);
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Large Start/Stop Button
                    ImVec2 buttonSize(180, 40);
                    if (!g_recording) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.75f, 0.20f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.55f, 0.10f, 1.0f));
                        if (ImGui::Button("START RECORDING", buttonSize)) { StartRecording(); }
                        ImGui::PopStyleColor(3);
                    }
                    else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.15f, 0.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.10f, 0.10f, 1.0f));
                        if (ImGui::Button("STOP RECORDING", buttonSize)) { StopRecording(); }
                        ImGui::PopStyleColor(3);
                    }

                    ImGui::SameLine();

                    // Screenshot Button
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.85f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 1.0f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.10f, 0.65f, 1.0f));
                    if (ImGui::Button("TAKE SCREENSHOT", ImVec2(180, 40))) {
                        TakeScreenshot();
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::Spacing();
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Recording Mode Section
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("RECORDING MODE");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();

                    const char* modes[] = { "Duration Based", "Schedule Based" };
                    ImGui::SetNextItemWidth(200);
                    ImGui::Combo("##Mode", &g_recordingMode, modes, IM_ARRAYSIZE(modes));

                    ImGui::Spacing();

                    if (g_recordingMode == 0) {
                        // Duration-based recording
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                        ImGui::Text("Set Duration:");
                        ImGui::PopStyleColor();

                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Hours##dur", &g_durationHours);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Min##dur", &g_durationMinutes);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Sec##dur", &g_durationSeconds);

                        m_durationMinutes = g_durationHours * 60 + g_durationMinutes;

                        if (g_durationHours < 0) g_durationHours = 0;
                        if (g_durationMinutes < 0) g_durationMinutes = 0;
                        if (g_durationMinutes > 59) g_durationMinutes = 59;
                        if (g_durationSeconds < 0) g_durationSeconds = 0;
                        if (g_durationSeconds > 59) g_durationSeconds = 59;
                    }
                    else {
                        // Schedule-based recording
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                        ImGui::Text("Start Time:");
                        ImGui::PopStyleColor();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Hour##start", &g_startHour);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Min##start", &g_startMinute);

                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                        ImGui::Text("Stop Time:");
                        ImGui::PopStyleColor();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Hour##stop", &g_stopHour);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("Min##stop", &g_stopMinute);

                        // Validate inputs
                        if (g_startHour < 0) g_startHour = 0;
                        if (g_startHour > 23) g_startHour = 23;
                        if (g_startMinute < 0) g_startMinute = 0;
                        if (g_startMinute > 59) g_startMinute = 59;
                        if (g_stopHour < 0) g_stopHour = 0;
                        if (g_stopHour > 23) g_stopHour = 23;
                        if (g_stopMinute < 0) g_stopMinute = 0;
                        if (g_stopMinute > 59) g_stopMinute = 59;

                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, g_scheduleActive ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                        ImGui::Text("Schedule: %s", g_scheduleActive ? "ACTIVE" : "INACTIVE");
                        ImGui::PopStyleColor();

                        ImGui::SameLine();
                        if (ImGui::Button(g_scheduleActive ? "Deactivate" : "Activate", ImVec2(100, 0))) {
                            g_scheduleActive = !g_scheduleActive;
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Output Settings
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("OUTPUT");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    
                    // Output path with Open Folder button
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110);
                    ImGui::InputText("##OutputPath", g_outputPath, sizeof(g_outputPath));
                    ImGui::SameLine();
                    if (ImGui::Button("Open Folder", ImVec2(100, 0))) {
                        OpenOutputFolder();
                    }

                    ImGui::EndTabItem();
                }

                // --- TAB 2: ENCODING SETTINGS ---
                if (ImGui::BeginTabItem("  Encoding  ")) {
                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Resolution
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("RESOLUTION");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    const char* resolutions[] = { "1920x1080 (FHD)", "2560x1440 (QHD)", "3840x2160 (4K)", "7680x4320 (8K)" };
                    ImGui::SetNextItemWidth(250);
                    ImGui::Combo("##Resolution", &m_currentResolution, resolutions, IM_ARRAYSIZE(resolutions));

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Frame Rate
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("FRAME RATE");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    const char* frameRates[] = { "60 FPS", "120 FPS", "144 FPS", "240 FPS" };
                    ImGui::SetNextItemWidth(250);
                    ImGui::Combo("##FPS", &m_currentFps, frameRates, IM_ARRAYSIZE(frameRates));

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Codec
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("CODEC");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    const char* codecs[] = { "H.264 (AVC)", "H.265 (HEVC)", "AV1" };
                    ImGui::SetNextItemWidth(250);
                    ImGui::Combo("##Codec", &m_currentCodec, codecs, IM_ARRAYSIZE(codecs));

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Bitrate
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("BITRATE");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(300);
                    ImGui::SliderFloat("##Bitrate", &g_bitrateMbps, 5.0f, 500.0f, "%.1f Mbps");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::Text("(CQP mode - bitrate serves as quality guide)");
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // RAM Buffer
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("RAM BUFFER");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(300);
                    ImGui::SliderFloat("##RAMBuffer", &g_ramBufferSizeGB, 1.0f, 16.0f, "%.1f GB");

                    ImGui::EndTabItem();
                }

                // --- TAB 3: HARDWARE SELECTION ---
                if (ImGui::BeginTabItem("  Hardware  ")) {
                    ImGui::Spacing();

                    // GPU Selection
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("GPU SELECTION");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();

                    if (ImGui::BeginListBox("##GPUs", ImVec2(-1, 60))) {
                        for (size_t i = 0; i < g_availableGPUs.size(); i++) {
                            const bool isSelected = (m_selectedGPU == (int)i);
                            std::string gpuLabel = WStringToString(g_availableGPUs[i].name) + " (Score: " + std::to_string((int)g_availableGPUs[i].performanceScore) + ")";
                            if (ImGui::Selectable(gpuLabel.c_str(), isSelected)) m_selectedGPU = i;
                        }
                        ImGui::EndListBox();
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Display Selection
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("DISPLAY SELECTION");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();

                    if (ImGui::BeginListBox("##Displays", ImVec2(-1, 60))) {
                        for (size_t i = 0; i < m_displayNames.size(); i++) {
                            const bool isSelected = (m_selectedDisplay == (int)i);
                            if (ImGui::Selectable(m_displayNames[i].c_str(), isSelected)) m_selectedDisplay = i;
                        }
                        ImGui::EndListBox();
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // USB Device Selection
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("USB CAPTURE DEVICES");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();

                    if (!m_usbDeviceNames.empty()) {
                        if (ImGui::BeginListBox("##USBDevices", ImVec2(-1, 60))) {
                            for (size_t i = 0; i < m_usbDeviceNames.size(); i++) {
                                const bool isSelected = (m_selectedUSBDevice == (int)i);
                                if (ImGui::Selectable(m_usbDeviceNames[i].c_str(), isSelected)) m_selectedUSBDevice = i;
                            }
                            ImGui::EndListBox();
                        }
                        if (ImGui::Button("Clear Selection", ImVec2(120, 0))) { m_selectedUSBDevice = -1; }
                    }
                    else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                        ImGui::Text("No USB capture devices detected");
                        ImGui::PopStyleColor();
                    }

                    ImGui::EndTabItem();
                }

                // --- TAB 4: DIAGNOSTICS ---
                if (ImGui::BeginTabItem("  Diagnostics  ")) {
                    ImGui::Spacing();
                    ImGui::Spacing();

                    // System Memory
                    MEMORYSTATUSEX memInfo;
                    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
                    GlobalMemoryStatusEx(&memInfo);
                    float totalSysRAM = (float)memInfo.ullTotalPhys / (1024.0f * 1024.0f * 1024.0f);
                    float availSysRAM = (float)memInfo.ullAvailPhys / (1024.0f * 1024.0f * 1024.0f);
                    float usedPercent = (totalSysRAM - availSysRAM) / totalSysRAM;

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("SYSTEM MEMORY");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();

                    // Memory bar
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                    ImGui::ProgressBar(usedPercent, ImVec2(300, 20), "");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::Text("%.1f / %.1f GB", totalSysRAM - availSysRAM, totalSysRAM);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // GPU Info
                    if (!g_availableGPUs.empty()) {
                        auto& gpu = g_availableGPUs[m_selectedGPU];
                        float dedicatedVRAM = (float)gpu.dedicatedVideoMemory / (1024.0f * 1024.0f * 1024.0f);
                        float sharedVRAM = (float)gpu.sharedSystemMemory / (1024.0f * 1024.0f * 1024.0f);

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                        ImGui::Text("SELECTED GPU");
                        ImGui::PopStyleColor();
                        ImGui::Spacing();

                        ImGui::Text("Name: %s", WStringToString(gpu.name).c_str());
                        ImGui::Text("Encoder: %s", GetEncoderName(gpu.encoderType));
                        ImGui::Text("Dedicated VRAM: %.2f GB", dedicatedVRAM);
                        ImGui::Text("Shared Memory: %.2f GB", sharedVRAM);

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                        ImGui::Text("CODEC SUPPORT");
                        ImGui::PopStyleColor();
                        ImGui::Spacing();

                        // H.264
                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsH264Encode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("H.264  Enc: %s", gpu.capabilities.supportsH264Encode ? "YES" : "NO");
                        ImGui::PopStyleColor();
                        ImGui::SameLine(150);
                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsH264Decode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("Dec: %s", gpu.capabilities.supportsH264Decode ? "YES" : "NO");
                        ImGui::PopStyleColor();

                        // H.265
                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsH265Encode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("H.265  Enc: %s", gpu.capabilities.supportsH265Encode ? "YES" : "NO");
                        ImGui::PopStyleColor();
                        ImGui::SameLine(150);
                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsH265Decode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("Dec: %s", gpu.capabilities.supportsH265Decode ? "YES" : "NO");
                        ImGui::PopStyleColor();

                        // AV1
                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsAV1Encode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("AV1    Enc: %s", gpu.capabilities.supportsAV1Encode ? "YES" : "NO");
                        ImGui::PopStyleColor();
                        ImGui::SameLine(150);
                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsAV1Decode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("Dec: %s", gpu.capabilities.supportsAV1Decode ? "YES" : "NO");
                        ImGui::PopStyleColor();

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                        ImGui::Text("CAPABILITIES");
                        ImGui::PopStyleColor();
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, gpu.capabilities.supportsHDR ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("HDR Support: %s", gpu.capabilities.supportsHDR ? "YES" : "NO");
                        ImGui::PopStyleColor();
                        ImGui::Text("Max Framerate: %u FPS", gpu.capabilities.maxFramerate);
                        ImGui::Text("Max Resolution: %u px", gpu.capabilities.maxEncodedResolution);
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::End();
        }

        ImGui::Render();
        const float clear_color[4] = { 0.04f, 0.04f, 0.04f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    void SetStatus(const std::string& msg) {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_statusMessage = msg;
    }

    void TakeScreenshot() {
        std::shared_ptr<IRecordingEngine> engine;
        {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            engine = g_engine;
        }
        if (!engine) return;

        // Sync current UI selections to settings for single-frame capture
        g_settings.gpuIndex = m_selectedGPU;
        g_settings.displayIndex = m_selectedDisplay;
        g_settings.usbDeviceIndex = m_selectedUSBDevice;

        std::string currentOutputPath = g_outputPath;
        std::filesystem::path outputPath(currentOutputPath);
        std::filesystem::path folderPath = outputPath.parent_path();

        // Ensure folder exists
        if (!std::filesystem::exists(folderPath)) {
            try {
                std::filesystem::create_directories(folderPath);
            } catch (...) {
                SetStatus("Error: Could not create directory");
                return;
            }
        }

        std::string screenshotBase = (folderPath / "screenshot.png").string();
        std::string uniquePath = GetUniqueFilename(screenshotBase);

        SetStatus("Capturing screenshot...");

        // Run in a separate thread to prevent UI hang
        // We use a lambda that doesn't capture 'this' to avoid use-after-free
        std::thread([engine, uniquePath]() {
            bool success = engine->CaptureScreenshot(uniquePath);

            if (success) {
                std::string filename = std::filesystem::path(uniquePath).filename().string();
                // Check if it might have fallen back to BMP
                if (!std::filesystem::exists(uniquePath)) {
                    std::string bmpPath = uniquePath;
                    if (bmpPath.size() > 4) bmpPath.replace(bmpPath.size() - 3, 3, "bmp");
                    if (std::filesystem::exists(bmpPath)) {
                        filename = std::filesystem::path(bmpPath).filename().string();
                    }
                }

                // Use the global mutex-protected status update
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_statusMessage = "Screenshot saved: " + filename;
            } else {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_statusMessage = "Failed to take screenshot";
            }
        }).detach();
    }

    void StartRecording() {
        std::shared_ptr<IRecordingEngine> engine;
        {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            engine = g_engine;
        }
        if (!engine) return;

        // Auto-correct the file extension based on the selected codec
        std::string currentPath = g_outputPath;
        size_t dotPos = currentPath.find_last_of('.');
        std::string basePath = (dotPos != std::string::npos) ? currentPath.substr(0, dotPos) : currentPath;

        // Set extension based on codec
        std::string ext = ".mkv";  // Default MKV container
        if (m_currentCodec == 0) {
            g_settings.codec = Codec::H264;
            ext = ".mkv";
        }
        else if (m_currentCodec == 1) {
            g_settings.codec = Codec::H265;
            ext = ".mkv";
        }
        else {
            g_settings.codec = Codec::AV1;
            ext = ".mkv";
        }
        
        currentPath = basePath + ext;

        // AUTO-INCREMENT: Get unique filename if file already exists
        currentPath = GetUniqueFilename(currentPath);

        // Create output directory if it doesn't exist
        std::filesystem::path outputDir = std::filesystem::path(currentPath).parent_path();
        if (!outputDir.empty() && !std::filesystem::exists(outputDir)) {
            std::filesystem::create_directories(outputDir);
        }

        strcpy_s(g_outputPath, sizeof(g_outputPath), currentPath.c_str());
        g_settings.outputPath = g_outputPath;

        g_settings.gpuIndex = m_selectedGPU;
        g_settings.displayIndex = m_selectedDisplay;
        g_settings.usbDeviceIndex = m_selectedUSBDevice;

        // Calculate duration based on mode
        if (g_recordingMode == 0) {
            // Duration mode
            g_settings.recordDurationSeconds = (g_durationHours * 3600) + (g_durationMinutes * 60) + g_durationSeconds;
        }
        else {
            // Schedule mode - calculate duration from current time to stop time
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            struct tm local_now;
            localtime_s(&local_now, &now_time_t);

            int currentMinutesSinceMidnight = local_now.tm_hour * 60 + local_now.tm_min;
            int stopMinutesSinceMidnight = g_stopHour * 60 + g_stopMinute;
            int startMinutesSinceMidnight = g_startHour * 60 + g_startMinute;

            // If current time is within schedule window
            if (currentMinutesSinceMidnight >= startMinutesSinceMidnight &&
                currentMinutesSinceMidnight < stopMinutesSinceMidnight) {
                int remainingMinutes = stopMinutesSinceMidnight - currentMinutesSinceMidnight;
                g_settings.recordDurationSeconds = remainingMinutes * 60;
            }
            else {
                // Not in schedule window, don't start
                g_statusMessage = "Cannot start: Current time is outside schedule window";
                return;
            }
        }

        g_settings.ramBufferSize = static_cast<uint64_t>(g_ramBufferSizeGB * 1024.0f * 1024.0f * 1024.0f);
        g_settings.bitrate = static_cast<uint32_t>(g_bitrateMbps * 1000000.0f);

        if (engine->StartRecording(g_settings)) {
            g_recording = true;
            g_recordingStartTime = std::chrono::system_clock::now();
        }
    }

    void StopRecording() {
        std::shared_ptr<IRecordingEngine> engine;
        {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            engine = g_engine;
        }
        if (engine) {
            engine->StopRecording();
            g_recording = false;
        }
    }

    void Cleanup() {
        {
            std::lock_guard<std::mutex> lock(g_engineMutex);
            if (g_engine) {
                g_engine->StopRecording();
                g_engine->Shutdown();
                g_engine.reset();
            }
        }
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        CleanupDeviceD3D(); ::DestroyWindow(m_hWnd);
    }
};

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) return false;
    CreateRenderTarget(); return true;
}
void CleanupDeviceD3D() { CleanupRenderTarget(); if (g_pSwapChain) g_pSwapChain->Release(); if (g_pd3dDeviceContext) g_pd3dDeviceContext->Release(); if (g_pd3dDevice) g_pd3dDevice->Release(); }
void CreateRenderTarget() { ID3D11Texture2D* pBackBuffer; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)); g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView); pBackBuffer->Release(); }
void CleanupRenderTarget() { if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; } }
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE: if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) { CleanupRenderTarget(); g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0); CreateRenderTarget(); } return 0;
    case WM_DESTROY: ::PostQuitMessage(0); return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RecordingApp app; int res = app.Run(hInstance, nCmdShow); CoUninitialize(); return res;
}
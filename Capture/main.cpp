 
#include "RecordingEngine.h"
#include "RecordingPipeline.h"
#include "GPUDetector.h"
#include "VirtualDisplayManager.h"
#include "USB.h"
#include <filesystem>
#include <windows.h>
#include <shellapi.h>

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

std::unique_ptr<IRecordingEngine> g_engine;
RecordingSettings g_settings;
std::vector<ExtendedGPUInfo> g_availableGPUs;
bool g_recording = false;
bool g_showSettings = true;
PerformanceMetrics g_metrics;
std::string g_statusMessage = "Ready";
char g_outputPath[512] = "C:\\Recordings\\capture.hevc";
float g_ramBufferSizeGB = 4.0f;
float g_bitrateMbps = 50.0f;

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

        g_engine = CreateRecordingEngine();
        g_engine->SetStatusCallback([](const std::string& msg) { g_statusMessage = msg; });
        g_engine->SetErrorCallback([](const std::string& msg) { g_statusMessage = "ERROR: " + msg; });

        if (!g_engine->Initialize()) {
            Cleanup();
            return 1;
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
    int m_durationMinutes = 0;   // Auto-stop limit

    std::vector<DisplayInfo> m_displayList;
    std::vector<USBDeviceInfo> m_usbDeviceList;
    std::vector<std::string> m_displayNames;
    std::vector<std::string> m_usbDeviceNames;

    VirtualDisplayManager m_displayManager;
    HWND m_hWnd = nullptr;

    bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"RecordingEngine", nullptr };
        ::RegisterClassExW(&wc);
        m_hWnd = ::CreateWindowW(wc.lpszClassName, L"GPU Recording Engine - Pro", WS_OVERLAPPEDWINDOW, 100, 100, 1400, 900, nullptr, nullptr, wc.hInstance, nullptr);
        return m_hWnd != nullptr;
    }

    bool SetupImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
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

    void RenderFrame() {
        // Automatically sync UI if the backend engine auto-stopped due to the time limit
        if (g_recording && g_engine && !g_engine->IsRecording()) {
            g_recording = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // MAKE IT A SINGLE APPLICATION WINDOW
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin("Recording Engine Control Panel", nullptr, windowFlags)) {

            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "GPU Recording Engine - Professional");
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::BeginTabBar("MainTabs")) {

                // --- TAB 1: RECORDING CONTROLS ---
                if (ImGui::BeginTabItem("Recording")) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Recording Status");
                    ImGui::Text("Status: %s", g_statusMessage.c_str());

                    ImGui::Spacing();
                    if (!g_recording) {
                        if (ImGui::Button("START RECORDING", ImVec2(200, 40))) { StartRecording(); }
                    }
                    else {
                        if (ImGui::Button("STOP RECORDING", ImVec2(200, 40))) { StopRecording(); }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Time Recording Limit (Auto Stop)");
                    ImGui::InputInt("Minutes (0 = Unlimited)", &m_durationMinutes);
                    if (m_durationMinutes < 0) m_durationMinutes = 0;

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Output Settings");
                    ImGui::InputText("Output Path", g_outputPath, sizeof(g_outputPath));
                    ImGui::EndTabItem();
                }

                // --- TAB 2: ENCODING SETTINGS ---
                if (ImGui::BeginTabItem("Encoding")) {
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Resolution");
                    const char* resolutions[] = { "1920x1080", "2560x1440", "3840x2160", "7680x4320" };
                    ImGui::Combo("Select Resolution", &m_currentResolution, resolutions, IM_ARRAYSIZE(resolutions));

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Frame Rate");
                    const char* frameRates[] = { "60 FPS", "120 FPS", "144 FPS", "240 FPS" };
                    ImGui::Combo("Select FPS", &m_currentFps, frameRates, IM_ARRAYSIZE(frameRates));

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Codec");
                    const char* codecs[] = { "H.264", "H.265 (HEVC)", "AV1" };
                    ImGui::Combo("Select Codec", &m_currentCodec, codecs, IM_ARRAYSIZE(codecs));

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Bitrate (Mbps)");
                    ImGui::SliderFloat("Bitrate", &g_bitrateMbps, 5.0f, 500.0f, "%.1f Mbps");
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Note: Engine currently uses CQP for optimal quality, ignoring bitrate)");

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "RAM Buffer (GB)");
                    ImGui::SliderFloat("RAM Buffer", &g_ramBufferSizeGB, 1.0f, 16.0f, "%.1f GB");

                    ImGui::EndTabItem();
                }

                // --- TAB 3: HARDWARE SELECTION ---
                if (ImGui::BeginTabItem("Hardware")) {
                    ImGui::Spacing();

                    // GPU Selection
                    ImGui::TextColored(ImVec4(0.8f, 0.2f, 1.0f, 1.0f), "GPU Selection");
                    if (ImGui::BeginListBox("##GPUs", ImVec2(-1, 100))) {
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
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Display Selection");
                    if (ImGui::BeginListBox("##Displays", ImVec2(-1, 100))) {
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
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "USB Capture Devices");
                    if (!m_usbDeviceNames.empty()) {
                        if (ImGui::BeginListBox("##USBDevices", ImVec2(-1, 100))) {
                            for (size_t i = 0; i < m_usbDeviceNames.size(); i++) {
                                const bool isSelected = (m_selectedUSBDevice == (int)i);
                                if (ImGui::Selectable(m_usbDeviceNames[i].c_str(), isSelected)) m_selectedUSBDevice = i;
                            }
                            ImGui::EndListBox();
                        }
                        if (ImGui::Button("Clear USB Selection")) { m_selectedUSBDevice = -1; }
                    }
                    else {
                        ImGui::Text("No USB capture devices detected.");
                    }

                    ImGui::EndTabItem();
                }

                // --- TAB 4: DIAGNOSTICS ---
                if (ImGui::BeginTabItem("Diagnostics")) {
                    ImGui::Spacing();

                    // System Memory
                    MEMORYSTATUSEX memInfo;
                    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
                    GlobalMemoryStatusEx(&memInfo);
                    float totalSysRAM = (float)memInfo.ullTotalPhys / (1024.0f * 1024.0f * 1024.0f);
                    float availSysRAM = (float)memInfo.ullAvailPhys / (1024.0f * 1024.0f * 1024.0f);

                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "System Memory:");
                    ImGui::Text("Total RAM: %.2f GB", totalSysRAM);
                    ImGui::Text("Available: %.2f GB", availSysRAM);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // GPU Info
                    if (!g_availableGPUs.empty()) {
                        auto& gpu = g_availableGPUs[m_selectedGPU];
                        float dedicatedVRAM = (float)gpu.dedicatedVideoMemory / (1024.0f * 1024.0f * 1024.0f);
                        float sharedVRAM = (float)gpu.sharedSystemMemory / (1024.0f * 1024.0f * 1024.0f);

                        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Selected GPU Capabilities:");
                        ImGui::Text("Name: %s", WStringToString(gpu.name).c_str());
                        ImGui::Text("Encoder: %s", GetEncoderName(gpu.encoderType));
                        ImGui::Text("Dedicated VRAM: %.2f GB", dedicatedVRAM);
                        ImGui::Text("Shared VRAM: %.2f GB", sharedVRAM);

                        ImGui::Spacing();
                        ImGui::Text("Hardware Decode/Encode Support:");
                        ImGui::BulletText("H.264 - Encode: %s | Decode: %s", gpu.capabilities.supportsH264Encode ? "YES" : "NO", gpu.capabilities.supportsH264Decode ? "YES" : "NO");
                        ImGui::BulletText("H.265 - Encode: %s | Decode: %s", gpu.capabilities.supportsH265Encode ? "YES" : "NO", gpu.capabilities.supportsH265Decode ? "YES" : "NO");
                        ImGui::BulletText("AV1   - Encode: %s | Decode: %s", gpu.capabilities.supportsAV1Encode ? "YES" : "NO", gpu.capabilities.supportsAV1Decode ? "YES" : "NO");

                        ImGui::Spacing();
                        ImGui::Text("Features:");
                        ImGui::BulletText("HDR Support: %s", gpu.capabilities.supportsHDR ? "YES" : "NO");
                        ImGui::BulletText("Max Framerate: %u fps", gpu.capabilities.maxFramerate);
                        ImGui::BulletText("Max Resolution: %u pixels", gpu.capabilities.maxEncodedResolution);
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::End();
        }

        ImGui::Render();
        const float clear_color[4] = { 0.08f, 0.08f, 0.09f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    void StartRecording() {
        if (!g_engine) return;

        // Auto-correct the file extension based on the selected codec so players like VLC can read the file natively!
        std::string currentPath = g_outputPath;
        size_t dotPos = currentPath.find_last_of('.');
        std::string basePath = (dotPos != std::string::npos) ? currentPath.substr(0, dotPos) : currentPath;

        if (m_currentCodec == 0) {
            g_settings.codec = Codec::H264;
            currentPath = basePath + ".h264";
        }
        else if (m_currentCodec == 1) {
            g_settings.codec = Codec::H265;
            currentPath = basePath + ".hevc";
        }
        else {
            g_settings.codec = Codec::AV1;
            currentPath = basePath + ".av1";
        }

        strcpy_s(g_outputPath, sizeof(g_outputPath), currentPath.c_str());
        g_settings.outputPath = g_outputPath;

        g_settings.gpuIndex = m_selectedGPU;
        g_settings.displayIndex = m_selectedDisplay;
        g_settings.usbDeviceIndex = m_selectedUSBDevice;

        g_settings.recordDurationSeconds = m_durationMinutes * 60;
        g_settings.ramBufferSize = static_cast<uint64_t>(g_ramBufferSizeGB * 1024.0f * 1024.0f * 1024.0f);
        g_settings.bitrate = static_cast<uint32_t>(g_bitrateMbps * 1000000.0f);

        if (g_engine->StartRecording(g_settings)) {
            g_recording = true;
        }
    }

    void StopRecording() {
        if (g_engine) {
            g_engine->StopRecording();
            g_recording = false;
        }
    }

    void Cleanup() {
        if (g_engine) { g_engine->StopRecording(); g_engine->Shutdown(); g_engine.reset(); }
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
 
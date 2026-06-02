#include "region.h"
#include <iostream>
#include <algorithm>

CaptureRegion RegionSelector::s_selectedRegion;
POINT RegionSelector::s_startPoint = {0, 0};
POINT RegionSelector::s_endPoint = {0, 0};
bool RegionSelector::s_isDragging = false;
HWND RegionSelector::s_hwnd = nullptr;

CaptureRegion RegionSelector::SelectRegion() {
    s_selectedRegion = {0, 0, 0, 0, false};
    s_isDragging = false;

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = L"RegionSelectorClass";

    RegisterClassEx(&wc);

    // Get the virtual screen coordinates covering all monitors
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    s_hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"RegionSelectorClass",
        nullptr,
        WS_POPUP | WS_VISIBLE,
        x, y, cx, cy,
        nullptr, nullptr, hInstance, nullptr
    );

    // Make the window semi-transparent
    SetLayeredWindowAttributes(s_hwnd, 0, 100, LWA_ALPHA);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!IsWindow(s_hwnd)) break;
    }

    UnregisterClass(L"RegionSelectorClass", hInstance);

    return s_selectedRegion;
}

LRESULT CALLBACK RegionSelector::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            s_isDragging = true;
            s_startPoint.x = (short)LOWORD(lParam);
            s_startPoint.y = (short)HIWORD(lParam);
            s_endPoint = s_startPoint;
            SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (s_isDragging) {
                s_endPoint.x = (short)LOWORD(lParam);
                s_endPoint.y = (short)HIWORD(lParam);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            s_isDragging = false;
            ReleaseCapture();

            s_endPoint.x = (short)LOWORD(lParam);
            s_endPoint.y = (short)HIWORD(lParam);

            // Convert local coordinates to screen coordinates
            POINT ptStart = s_startPoint;
            POINT ptEnd = s_endPoint;
            ClientToScreen(hwnd, &ptStart);
            ClientToScreen(hwnd, &ptEnd);

            s_selectedRegion.x = std::min(ptStart.x, ptEnd.x);
            s_selectedRegion.y = std::min(ptStart.y, ptEnd.y);
            s_selectedRegion.width = abs(ptEnd.x - ptStart.x);
            s_selectedRegion.height = abs(ptEnd.y - ptStart.y);
            s_selectedRegion.isValid = (s_selectedRegion.width > 0 && s_selectedRegion.height > 0);

            DestroyWindow(hwnd);
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_KEYDOWN: {
            if (msg == WM_KEYDOWN && wParam != VK_ESCAPE) break;
            // Cancel selection
            s_selectedRegion.isValid = false;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Fill background with a semi-transparent dark color
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            if (s_isDragging) {
                // Clear the selected region
                int left = std::min(s_startPoint.x, s_endPoint.x);
                int top = std::min(s_startPoint.y, s_endPoint.y);
                int right = std::max(s_startPoint.x, s_endPoint.x);
                int bottom = std::max(s_startPoint.y, s_endPoint.y);

                // Draw selection border
                HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
                HBRUSH clearBrush = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
                HGDIOBJ oldPen = SelectObject(hdc, borderPen);
                HGDIOBJ oldBrush = SelectObject(hdc, clearBrush);

                Rectangle(hdc, left, top, right, bottom);

                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
                DeleteObject(borderPen);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // Handled in WM_PAINT to avoid flicker
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

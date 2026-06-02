#pragma once

#include <windows.h>
#include <cstdint>

struct CaptureRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool isValid = false;
};

class RegionSelector {
public:
    static CaptureRegion SelectRegion();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static CaptureRegion s_selectedRegion;
    static POINT s_startPoint;
    static POINT s_endPoint;
    static bool s_isDragging;
    static HWND s_hwnd;
};

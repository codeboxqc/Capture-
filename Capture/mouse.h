#pragma once
// ============================================================================
// Mouse.h - GPU-Accelerated Mouse Cursor Capture & Rendering
// ============================================================================
// Captures Windows cursor and renders it onto captured frames using GPU
// Supports: Color cursors, monochrome cursors, animated cursors
// ============================================================================

#include "framework.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>
#include <atomic>

using Microsoft::WRL::ComPtr;

// Cursor shape data
struct CursorData {
    std::vector<uint8_t> pixels;      // BGRA pixel data
    int width = 0;
    int height = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    bool isMonochrome = false;
    bool isValid = false;
};

// Mouse position and state
struct MouseState {
    int x = 0;
    int y = 0;
    bool visible = true;
    bool leftButton = false;
    bool rightButton = false;
    HCURSOR currentCursor = nullptr;
};

class MouseCapture {
public:
    MouseCapture() = default;
    ~MouseCapture() { Shutdown(); }

    bool Initialize(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context) {
        if (!device || !context) return false;

        m_device = device;
        m_context = context;
        m_initialized = true;

        spdlog::info("MouseCapture initialized");
        return true;
    }

    void Shutdown() {
        m_cursorTexture.Reset();
        m_cursorSRV.Reset();
        m_device.Reset();
        m_context.Reset();
        m_initialized = false;
    }

    // Get current mouse state (position, buttons, visibility)
    MouseState GetMouseState() {
        MouseState state;

        CURSORINFO ci = { sizeof(CURSORINFO) };
        if (GetCursorInfo(&ci)) {
            state.x = ci.ptScreenPos.x;
            state.y = ci.ptScreenPos.y;
            state.visible = (ci.flags & CURSOR_SHOWING) != 0;
            state.currentCursor = ci.hCursor;
        }

        state.leftButton = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        state.rightButton = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        return state;
    }

    // Capture current cursor image
    CursorData CaptureCursor(HCURSOR hCursor = nullptr) {
        CursorData data;

        if (!hCursor) {
            CURSORINFO ci = { sizeof(CURSORINFO) };
            if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) {
                return data;
            }
            hCursor = ci.hCursor;
        }

        if (!hCursor) return data;

        ICONINFO iconInfo;
        if (!GetIconInfo(hCursor, &iconInfo)) {
            return data;
        }

        data.hotspotX = iconInfo.xHotspot;
        data.hotspotY = iconInfo.yHotspot;
        data.isMonochrome = (iconInfo.hbmColor == nullptr);

        BITMAP bm;
        HBITMAP hBitmap = iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask;

        if (!GetObject(hBitmap, sizeof(BITMAP), &bm)) {
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
            return data;
        }

        data.width = bm.bmWidth;
        data.height = data.isMonochrome ? bm.bmHeight / 2 : bm.bmHeight;

        // Create compatible DC and bitmap for extraction
        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = data.width;
        bmi.bmiHeader.biHeight = -data.height;  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        data.pixels.resize(data.width * data.height * 4);

        if (data.isMonochrome) {
            // Monochrome cursor - need to process AND/XOR masks
            std::vector<uint8_t> maskBits(data.width * data.height * 2 * 4);

            HBITMAP hDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
            HGDIOBJ hOld = SelectObject(hdcMem, hDib);

            // Draw cursor onto white background
            RECT rc = { 0, 0, data.width, data.height };
            HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
            FillRect(hdcMem, &rc, whiteBrush);
            DrawIconEx(hdcMem, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_NORMAL);

            std::vector<uint8_t> whitePixels(data.width * data.height * 4);
            GetDIBits(hdcMem, hDib, 0, data.height, whitePixels.data(), &bmi, DIB_RGB_COLORS);

            // Draw cursor onto black background
            HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(hdcMem, &rc, blackBrush);
            DrawIconEx(hdcMem, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_NORMAL);

            std::vector<uint8_t> blackPixels(data.width * data.height * 4);
            GetDIBits(hdcMem, hDib, 0, data.height, blackPixels.data(), &bmi, DIB_RGB_COLORS);

            // Calculate alpha from difference
            for (int i = 0; i < data.width * data.height; i++) {
                int idx = i * 4;

                uint8_t convergence =
                    std::abs((int)whitePixels[idx] - (int)blackPixels[idx]) +
                    std::abs((int)whitePixels[idx + 1] - (int)blackPixels[idx + 1]) +
                    std::abs((int)whitePixels[idx + 2] - (int)blackPixels[idx + 2]);

                if (convergence < 10) {
                    // Solid color (same on both backgrounds)
                    data.pixels[idx + 0] = blackPixels[idx + 0];  // B
                    data.pixels[idx + 1] = blackPixels[idx + 1];  // G
                    data.pixels[idx + 2] = blackPixels[idx + 2];  // R
                    data.pixels[idx + 3] = 255;                    // A (opaque)
                }
                else {
                    // Inverted/transparent
                    data.pixels[idx + 0] = 0;
                    data.pixels[idx + 1] = 0;
                    data.pixels[idx + 2] = 0;
                    data.pixels[idx + 3] = 0;  // Transparent
                }
            }

            SelectObject(hdcMem, hOld);
            DeleteObject(hDib);
        }
        else {
            // Color cursor - extract directly with alpha
            HBITMAP hDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
            HGDIOBJ hOld = SelectObject(hdcMem, hDib);

            // Clear to transparent
            RECT rc = { 0, 0, data.width, data.height };
            HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdcMem, &rc, clearBrush);
            DeleteObject(clearBrush);

            DrawIconEx(hdcMem, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_NORMAL);
            GetDIBits(hdcMem, hDib, 0, data.height, data.pixels.data(), &bmi, DIB_RGB_COLORS);

            // Check if we have alpha channel data from color bitmap
            bool hasAlpha = false;
            for (size_t i = 3; i < data.pixels.size(); i += 4) {
                if (data.pixels[i] != 0) {
                    hasAlpha = true;
                    break;
                }
            }

            // If no alpha, generate it from mask
            if (!hasAlpha && iconInfo.hbmMask) {
                std::vector<uint8_t> maskData(data.width * data.height * 4);

                // Get mask bitmap
                BITMAPINFO maskBmi = bmi;
                maskBmi.bmiHeader.biHeight = -data.height;

                HDC hdcMask = CreateCompatibleDC(hdcScreen);
                HBITMAP hMaskDib = CreateDIBSection(hdcMask, &maskBmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
                HGDIOBJ hMaskOld = SelectObject(hdcMask, hMaskDib);

                // Draw mask
                DrawIconEx(hdcMask, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_MASK);
                GetDIBits(hdcMask, hMaskDib, 0, data.height, maskData.data(), &maskBmi, DIB_RGB_COLORS);

                // Apply mask as alpha
                for (int i = 0; i < data.width * data.height; i++) {
                    int idx = i * 4;
                    // Mask is white (255) where transparent
                    uint8_t maskVal = maskData[idx];  // R, G, B should be same
                    data.pixels[idx + 3] = (maskVal > 128) ? 0 : 255;
                }

                SelectObject(hdcMask, hMaskOld);
                DeleteObject(hMaskDib);
                DeleteDC(hdcMask);
            }

            SelectObject(hdcMem, hOld);
            DeleteObject(hDib);
        }

        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);

        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);

        data.isValid = true;
        return data;
    }

    // Draw cursor onto a D3D11 texture (GPU method using staging)
    bool DrawCursorOnTexture(ID3D11Texture2D* targetTexture, int screenX, int screenY,
        int offsetX = 0, int offsetY = 0) {
        if (!m_initialized || !targetTexture) return false;

        MouseState state = GetMouseState();
        if (!state.visible) return true;  // Not an error, just nothing to draw

        CursorData cursor = CaptureCursor(state.currentCursor);
        if (!cursor.isValid || cursor.pixels.empty()) return false;

        // Calculate cursor position on texture
        int cursorX = state.x - offsetX - cursor.hotspotX;
        int cursorY = state.y - offsetY - cursor.hotspotY;

        // Get texture description
        D3D11_TEXTURE2D_DESC texDesc;
        targetTexture->GetDesc(&texDesc);

        // Bounds check
        if (cursorX >= (int)texDesc.Width || cursorY >= (int)texDesc.Height ||
            cursorX + cursor.width <= 0 || cursorY + cursor.height <= 0) {
            return true;  // Cursor outside texture bounds
        }

        // Create or resize staging texture if needed
        if (!m_stagingTexture || m_stagingWidth != texDesc.Width || m_stagingHeight != texDesc.Height) {
            D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            stagingDesc.MiscFlags = 0;

            HRESULT hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
            if (FAILED(hr)) {
                spdlog::error("Failed to create cursor staging texture");
                return false;
            }
            m_stagingWidth = texDesc.Width;
            m_stagingHeight = texDesc.Height;
        }

        // Copy target to staging
        m_context->CopyResource(m_stagingTexture.Get(), targetTexture);

        // Map staging texture
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
        if (FAILED(hr)) return false;

        uint8_t* destData = static_cast<uint8_t*>(mapped.pData);

        // Blend cursor onto texture
        for (int y = 0; y < cursor.height; y++) {
            int destY = cursorY + y;
            if (destY < 0 || destY >= (int)texDesc.Height) continue;

            for (int x = 0; x < cursor.width; x++) {
                int destX = cursorX + x;
                if (destX < 0 || destX >= (int)texDesc.Width) continue;

                int srcIdx = (y * cursor.width + x) * 4;
                int dstIdx = destY * mapped.RowPitch + destX * 4;

                uint8_t srcB = cursor.pixels[srcIdx + 0];
                uint8_t srcG = cursor.pixels[srcIdx + 1];
                uint8_t srcR = cursor.pixels[srcIdx + 2];
                uint8_t srcA = cursor.pixels[srcIdx + 3];

                if (srcA == 0) continue;  // Fully transparent

                if (srcA == 255) {
                    // Fully opaque
                    destData[dstIdx + 0] = srcB;
                    destData[dstIdx + 1] = srcG;
                    destData[dstIdx + 2] = srcR;
                    destData[dstIdx + 3] = 255;
                }
                else {
                    // Alpha blend
                    float alpha = srcA / 255.0f;
                    float invAlpha = 1.0f - alpha;
                    destData[dstIdx + 0] = static_cast<uint8_t>(srcB * alpha + destData[dstIdx + 0] * invAlpha);
                    destData[dstIdx + 1] = static_cast<uint8_t>(srcG * alpha + destData[dstIdx + 1] * invAlpha);
                    destData[dstIdx + 2] = static_cast<uint8_t>(srcR * alpha + destData[dstIdx + 2] * invAlpha);
                    destData[dstIdx + 3] = 255;
                }
            }
        }

        m_context->Unmap(m_stagingTexture.Get(), 0);

        // Copy back to target
        m_context->CopyResource(targetTexture, m_stagingTexture.Get());

        return true;
    }

    // Draw highlight circle around cursor
    bool DrawCursorHighlight(ID3D11Texture2D* targetTexture, int screenX, int screenY,
        int offsetX, int offsetY, float radius,
        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (!m_initialized || !targetTexture) return false;

        MouseState state = GetMouseState();

        int centerX = state.x - offsetX;
        int centerY = state.y - offsetY;

        D3D11_TEXTURE2D_DESC texDesc;
        targetTexture->GetDesc(&texDesc);

        // Bounds check
        int radiusInt = static_cast<int>(radius);
        if (centerX + radiusInt < 0 || centerX - radiusInt >= (int)texDesc.Width ||
            centerY + radiusInt < 0 || centerY - radiusInt >= (int)texDesc.Height) {
            return true;
        }

        // Ensure staging texture exists
        if (!m_stagingTexture || m_stagingWidth != texDesc.Width || m_stagingHeight != texDesc.Height) {
            D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            stagingDesc.MiscFlags = 0;

            m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
            m_stagingWidth = texDesc.Width;
            m_stagingHeight = texDesc.Height;
        }

        m_context->CopyResource(m_stagingTexture.Get(), targetTexture);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped))) {
            return false;
        }

        uint8_t* destData = static_cast<uint8_t*>(mapped.pData);
        float alpha = a / 255.0f;
        float invAlpha = 1.0f - alpha;
        float radiusSq = radius * radius;

        // Draw filled circle with alpha blending
        for (int y = -radiusInt; y <= radiusInt; y++) {
            int destY = centerY + y;
            if (destY < 0 || destY >= (int)texDesc.Height) continue;

            for (int x = -radiusInt; x <= radiusInt; x++) {
                int destX = centerX + x;
                if (destX < 0 || destX >= (int)texDesc.Width) continue;

                float distSq = static_cast<float>(x * x + y * y);
                if (distSq > radiusSq) continue;

                int dstIdx = destY * mapped.RowPitch + destX * 4;

                // Soft edge (anti-aliasing at border)
                float edgeDist = radius - std::sqrt(distSq);
                float edgeAlpha = std::min(1.0f, edgeDist * 2.0f);  // Fade over 0.5 pixels
                float finalAlpha = alpha * edgeAlpha;
                float finalInvAlpha = 1.0f - finalAlpha;

                destData[dstIdx + 0] = static_cast<uint8_t>(b * finalAlpha + destData[dstIdx + 0] * finalInvAlpha);
                destData[dstIdx + 1] = static_cast<uint8_t>(g * finalAlpha + destData[dstIdx + 1] * finalInvAlpha);
                destData[dstIdx + 2] = static_cast<uint8_t>(r * finalAlpha + destData[dstIdx + 2] * finalInvAlpha);
            }
        }

        m_context->Unmap(m_stagingTexture.Get(), 0);
        m_context->CopyResource(targetTexture, m_stagingTexture.Get());

        return true;
    }

    // Draw click animation (expanding ring)
    bool DrawClickAnimation(ID3D11Texture2D* targetTexture, int clickX, int clickY,
        int offsetX, int offsetY, float progress,
        uint8_t r, uint8_t g, uint8_t b) {
        if (!m_initialized || !targetTexture || progress < 0 || progress > 1) return false;

        int centerX = clickX - offsetX;
        int centerY = clickY - offsetY;

        D3D11_TEXTURE2D_DESC texDesc;
        targetTexture->GetDesc(&texDesc);

        // Animation parameters
        float maxRadius = 40.0f;
        float radius = maxRadius * progress;
        float ringWidth = 4.0f;
        float alpha = (1.0f - progress) * 0.8f;  // Fade out as it expands

        if (radius < 1) return true;

        // Ensure staging texture
        if (!m_stagingTexture || m_stagingWidth != texDesc.Width || m_stagingHeight != texDesc.Height) {
            D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            stagingDesc.MiscFlags = 0;

            m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
            m_stagingWidth = texDesc.Width;
            m_stagingHeight = texDesc.Height;
        }

        m_context->CopyResource(m_stagingTexture.Get(), targetTexture);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped))) {
            return false;
        }

        uint8_t* destData = static_cast<uint8_t*>(mapped.pData);
        int radiusInt = static_cast<int>(radius + ringWidth);
        float innerRadiusSq = (radius - ringWidth) * (radius - ringWidth);
        float outerRadiusSq = (radius + ringWidth) * (radius + ringWidth);

        for (int y = -radiusInt; y <= radiusInt; y++) {
            int destY = centerY + y;
            if (destY < 0 || destY >= (int)texDesc.Height) continue;

            for (int x = -radiusInt; x <= radiusInt; x++) {
                int destX = centerX + x;
                if (destX < 0 || destX >= (int)texDesc.Width) continue;

                float distSq = static_cast<float>(x * x + y * y);
                if (distSq < innerRadiusSq || distSq > outerRadiusSq) continue;

                int dstIdx = destY * mapped.RowPitch + destX * 4;

                // Soft edges
                float dist = std::sqrt(distSq);
                float ringDist = std::abs(dist - radius);
                float edgeAlpha = std::max(0.0f, 1.0f - ringDist / ringWidth);
                float finalAlpha = alpha * edgeAlpha;
                float finalInvAlpha = 1.0f - finalAlpha;

                destData[dstIdx + 0] = static_cast<uint8_t>(b * finalAlpha + destData[dstIdx + 0] * finalInvAlpha);
                destData[dstIdx + 1] = static_cast<uint8_t>(g * finalAlpha + destData[dstIdx + 1] * finalInvAlpha);
                destData[dstIdx + 2] = static_cast<uint8_t>(r * finalAlpha + destData[dstIdx + 2] * finalInvAlpha);
            }
        }

        m_context->Unmap(m_stagingTexture.Get(), 0);
        m_context->CopyResource(targetTexture, m_stagingTexture.Get());

        return true;
    }

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11Texture2D> m_cursorTexture;
    ComPtr<ID3D11ShaderResourceView> m_cursorSRV;
    ComPtr<ID3D11Texture2D> m_stagingTexture;

    uint32_t m_stagingWidth = 0;
    uint32_t m_stagingHeight = 0;

    bool m_initialized = false;
};
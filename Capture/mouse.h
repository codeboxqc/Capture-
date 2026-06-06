#pragma once
// ============================================================================
// Mouse.h - GPU-Accelerated Mouse Cursor Capture & Rendering
// ============================================================================
// Captures Windows cursor and renders it onto captured frames using GPU
// Supports: Color cursors, monochrome cursors, animated cursors
// ============================================================================

#include "framework.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
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
    ~MouseCapture()  ;  

    bool Initialize(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context);
    void Shutdown();
    MouseState GetMouseState();
    CursorData CaptureCursor(HCURSOR hCursor = nullptr);
    bool DrawCursorOnTexture(ID3D11Texture2D* targetTexture, int screenX, int screenY, int offsetX = 0, int offsetY = 0);
    bool DrawCursorHighlight(ID3D11Texture2D* targetTexture, int screenX, int screenY, int offsetX, int offsetY, float radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    bool DrawClickAnimation(ID3D11Texture2D* targetTexture, int clickX, int clickY, int offsetX, int offsetY, float progress, uint8_t r, uint8_t g, uint8_t b);


private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    
    // Compute Shader resources
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11Buffer> m_constantBuffer;
    
    // Cursor texture and SRV
    ComPtr<ID3D11Texture2D> m_cursorTexture;
    ComPtr<ID3D11ShaderResourceView> m_cursorSRV;
    
    bool m_initialized = false;
    
    // Shader Constant Buffer struct (must be 16-byte aligned)
    __declspec(align(16))
    struct CursorBuffer {
        int cursorX;
        int cursorY;
        int cursorWidth;
        int cursorHeight;
        
        int highlightX;
        int highlightY;
        float highlightRadius;
        float clickProgress;
        
        float highlightColorR;
        float highlightColorG;
        float highlightColorB;
        float highlightColorA;
        
        float clickColorR;
        float clickColorG;
        float clickColorB;
        float clickColorA;
        
        int drawType; // 0 = cursor, 1 = highlight, 2 = click
        float padding[3];
    };

};

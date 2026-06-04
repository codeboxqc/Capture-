#include "mouse.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

static const char* g_cursorShaderHLSL = R"(
Texture2D cursorTexture : register(t0);
SamplerState samLinear : register(s0);

cbuffer CursorBuffer : register(b0)
{
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

    int drawType;
    float3 padding;
};

struct VS_INPUT
{
    uint id : SV_VertexID;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
    float2 screenPos : TEXCOORD1;
};

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    // Generate a full screen triangle
    output.tex = float2((input.id << 1) & 2, input.id & 2);
    output.pos = float4(output.tex * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);

    // We don't know screen resolution directly here, but we can rely on SV_Position in the PS
    return output;
}

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 screenPos = input.pos.xy;

    if (drawType == 0)
    {
        // Cursor
        float dx = screenPos.x - cursorX;
        float dy = screenPos.y - cursorY;

        if (dx >= 0 && dx < cursorWidth && dy >= 0 && dy < cursorHeight)
        {
            float2 uv = float2(dx / cursorWidth, dy / cursorHeight);
            float4 color = cursorTexture.Sample(samLinear, uv);
            return color; // Alpha blending handled by D3D11BlendState
        }
        return float4(0,0,0,0);
    }
    else if (drawType == 1)
    {
        // Highlight
        float dx = screenPos.x - highlightX;
        float dy = screenPos.y - highlightY;
        float distSq = dx*dx + dy*dy;
        float radiusSq = highlightRadius * highlightRadius;

        if (distSq <= radiusSq)
        {
            float edgeDist = highlightRadius - sqrt(distSq);
            float edgeAlpha = clamp(edgeDist * 2.0f, 0.0f, 1.0f);
            float alpha = highlightColorA * edgeAlpha;
            return float4(highlightColorR, highlightColorG, highlightColorB, alpha);
        }
        return float4(0,0,0,0);
    }
    else if (drawType == 2)
    {
        // Click animation
        float maxRadius = 40.0f;
        float radius = maxRadius * clickProgress;
        float ringWidth = 4.0f;
        float alpha = (1.0f - clickProgress) * 0.8f;

        if (radius < 1.0f) return float4(0,0,0,0);

        float dx = screenPos.x - highlightX;
        float dy = screenPos.y - highlightY;
        float distSq = dx*dx + dy*dy;

        float innerRadiusSq = (radius - ringWidth) * (radius - ringWidth);
        float outerRadiusSq = (radius + ringWidth) * (radius + ringWidth);

        if (distSq >= innerRadiusSq && distSq <= outerRadiusSq)
        {
            float dist = sqrt(distSq);
            float ringDist = abs(dist - radius);
            float edgeAlpha = max(0.0f, 1.0f - ringDist / ringWidth);
            float finalAlpha = alpha * edgeAlpha;
            return float4(clickColorR, clickColorG, clickColorB, finalAlpha);
        }
        return float4(0,0,0,0);
    }

    return float4(0,0,0,0);
}
)";

MouseCapture::~MouseCapture() { Shutdown(); }

bool MouseCapture::Initialize(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context) {
    if (!device || !context) return false;

    m_device = device;
    m_context = context;

    // Compile Shaders
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompile(
        g_cursorShaderHLSL, strlen(g_cursorShaderHLSL),
        "CursorShader", nullptr, nullptr, "VSMain", "vs_5_0",
        0, 0, &vsBlob, &errorBlob
    );
    if (FAILED(hr)) {
        spdlog::error("VS compile error: {}", errorBlob ? (char*)errorBlob->GetBufferPointer() : "Unknown");
        return false;
    }

    hr = D3DCompile(
        g_cursorShaderHLSL, strlen(g_cursorShaderHLSL),
        "CursorShader", nullptr, nullptr, "PSMain", "ps_5_0",
        0, 0, &psBlob, &errorBlob
    );
    if (FAILED(hr)) {
        spdlog::error("PS compile error: {}", errorBlob ? (char*)errorBlob->GetBufferPointer() : "Unknown");
        return false;
    }

    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);

    // Create Constant Buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CursorBuffer);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

    // Create Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_device->CreateSamplerState(&sampDesc, &m_samplerState);

    // Create Blend State (Alpha Blending)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&blendDesc, &m_blendState);

    m_initialized = true;
    spdlog::info("MouseCapture initialized with GPU Pixel shader");
    return true;
}

void MouseCapture::Shutdown() {
    m_cursorTexture.Reset();
    m_cursorSRV.Reset();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_samplerState.Reset();
    m_blendState.Reset();
    m_constantBuffer.Reset();
    m_device.Reset();
    m_context.Reset();
    m_initialized = false;
}

MouseState MouseCapture::GetMouseState() {
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

CursorData MouseCapture::CaptureCursor(HCURSOR hCursor) {
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

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = data.width;
    bmi.bmiHeader.biHeight = -data.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    data.pixels.resize(data.width * data.height * 4);

    if (data.isMonochrome) {
        std::vector<uint8_t> maskBits(data.width * data.height * 2 * 4);

        HBITMAP hDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
        HGDIOBJ hOld = SelectObject(hdcMem, hDib);

        RECT rc = { 0, 0, data.width, data.height };
        HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(hdcMem, &rc, whiteBrush);
        DrawIconEx(hdcMem, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_NORMAL);

        std::vector<uint8_t> whitePixels(data.width * data.height * 4);
        GetDIBits(hdcMem, hDib, 0, data.height, whitePixels.data(), &bmi, DIB_RGB_COLORS);

        HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(hdcMem, &rc, blackBrush);
        DrawIconEx(hdcMem, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_NORMAL);

        std::vector<uint8_t> blackPixels(data.width * data.height * 4);
        GetDIBits(hdcMem, hDib, 0, data.height, blackPixels.data(), &bmi, DIB_RGB_COLORS);

        for (int i = 0; i < data.width * data.height; i++) {
            int idx = i * 4;

            uint8_t convergence =
                std::abs((int)whitePixels[idx] - (int)blackPixels[idx]) +
                std::abs((int)whitePixels[idx + 1] - (int)blackPixels[idx + 1]) +
                std::abs((int)whitePixels[idx + 2] - (int)blackPixels[idx + 2]);

            if (convergence < 10) {
                // Monochrome cursor outputs BGRA
                data.pixels[idx + 0] = blackPixels[idx + 0];
                data.pixels[idx + 1] = blackPixels[idx + 1];
                data.pixels[idx + 2] = blackPixels[idx + 2];
                data.pixels[idx + 3] = 255;
            }
            else {
                data.pixels[idx + 0] = 0;
                data.pixels[idx + 1] = 0;
                data.pixels[idx + 2] = 0;
                data.pixels[idx + 3] = 0;
            }
        }

        SelectObject(hdcMem, hOld);
        DeleteObject(hDib);
    }
    else {
        HBITMAP hDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
        HGDIOBJ hOld = SelectObject(hdcMem, hDib);

        RECT rc = { 0, 0, data.width, data.height };
        HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdcMem, &rc, clearBrush);
        DeleteObject(clearBrush);

        DrawIconEx(hdcMem, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_NORMAL);
        GetDIBits(hdcMem, hDib, 0, data.height, data.pixels.data(), &bmi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        for (size_t i = 3; i < data.pixels.size(); i += 4) {
            if (data.pixels[i] != 0) {
                hasAlpha = true;
                break;
            }
        }

        if (!hasAlpha && iconInfo.hbmMask) {
            std::vector<uint8_t> maskData(data.width * data.height * 4);

            BITMAPINFO maskBmi = bmi;
            maskBmi.bmiHeader.biHeight = -data.height;

            HDC hdcMask = CreateCompatibleDC(hdcScreen);
            HBITMAP hMaskDib = CreateDIBSection(hdcMask, &maskBmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
            HGDIOBJ hMaskOld = SelectObject(hdcMask, hMaskDib);

            DrawIconEx(hdcMask, 0, 0, hCursor, data.width, data.height, 0, nullptr, DI_MASK);
            GetDIBits(hdcMask, hMaskDib, 0, data.height, maskData.data(), &maskBmi, DIB_RGB_COLORS);

            for (int i = 0; i < data.width * data.height; i++) {
                int idx = i * 4;
                uint8_t maskVal = maskData[idx];
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

void SetupPipelineAndDraw(ComPtr<ID3D11DeviceContext> ctx, ComPtr<ID3D11Texture2D> target, ComPtr<ID3D11Device> device, ComPtr<ID3D11Buffer> cb, ComPtr<ID3D11VertexShader> vs, ComPtr<ID3D11PixelShader> ps, ComPtr<ID3D11BlendState> bs, ComPtr<ID3D11SamplerState> samp, ComPtr<ID3D11ShaderResourceView> srv) {
    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
    if (!rtv) return;

    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)desc.Width;
    vp.Height = (float)desc.Height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    // Save state
    ComPtr<ID3D11RenderTargetView> oldRTV;
    ComPtr<ID3D11DepthStencilView> oldDSV;
    ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    D3D11_VIEWPORT oldVP;
    UINT numVPs = 1;
    ctx->RSGetViewports(&numVPs, &oldVP);

    ComPtr<ID3D11VertexShader> oldVS;
    ctx->VSGetShader(&oldVS, nullptr, nullptr);

    ComPtr<ID3D11PixelShader> oldPS;
    ctx->PSGetShader(&oldPS, nullptr, nullptr);

    ComPtr<ID3D11BlendState> oldBlend;
    float oldBlendFactor[4];
    UINT oldSampleMask;
    ctx->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);

    D3D11_PRIMITIVE_TOPOLOGY oldTopology;
    ctx->IAGetPrimitiveTopology(&oldTopology);

    // Set new state
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);

    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());

    if (srv) {
        ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
        ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
    }

    ctx->OMSetBlendState(bs.Get(), nullptr, 0xFFFFFFFF);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->Draw(3, 0);

    // Restore state
    ctx->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    ctx->RSSetViewports(numVPs, &oldVP);
    ctx->VSSetShader(oldVS.Get(), nullptr, 0);
    ctx->PSSetShader(oldPS.Get(), nullptr, 0);
    ctx->OMSetBlendState(oldBlend.Get(), oldBlendFactor, oldSampleMask);
    ctx->IASetPrimitiveTopology(oldTopology);
}

bool MouseCapture::DrawCursorOnTexture(ID3D11Texture2D* targetTexture, int screenX, int screenY, int offsetX, int offsetY) {
    if (!m_initialized || !targetTexture) return false;

    MouseState state = GetMouseState();
    if (!state.visible) return true;

    CursorData cursor = CaptureCursor(state.currentCursor);
    if (!cursor.isValid) return true;

    int cursorX = state.x - offsetX - cursor.hotspotX;
    int cursorY = state.y - offsetY - cursor.hotspotY;

    D3D11_TEXTURE2D_DESC curDesc;
    if (m_cursorTexture) m_cursorTexture->GetDesc(&curDesc);

    if (!m_cursorTexture || curDesc.Width != cursor.width || curDesc.Height != cursor.height) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = cursor.width;
        desc.Height = cursor.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = cursor.pixels.data();
        sd.SysMemPitch = cursor.width * 4;

        if (FAILED(m_device->CreateTexture2D(&desc, &sd, &m_cursorTexture))) return false;
        if (FAILED(m_device->CreateShaderResourceView(m_cursorTexture.Get(), nullptr, &m_cursorSRV))) return false;
    } else {
        m_context->UpdateSubresource(m_cursorTexture.Get(), 0, nullptr, cursor.pixels.data(), cursor.width * 4, 0);
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        CursorBuffer* cb = (CursorBuffer*)mapped.pData;
        memset(cb, 0, sizeof(CursorBuffer));
        cb->drawType = 0;
        cb->cursorX = cursorX;
        cb->cursorY = cursorY;
        cb->cursorWidth = cursor.width;
        cb->cursorHeight = cursor.height;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    SetupPipelineAndDraw(m_context, targetTexture, m_device, m_constantBuffer, m_vertexShader, m_pixelShader, m_blendState, m_samplerState, m_cursorSRV);

    return true;
}

bool MouseCapture::DrawCursorHighlight(ID3D11Texture2D* targetTexture, int screenX, int screenY, int offsetX, int offsetY, float radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!m_initialized || !targetTexture) return false;

    MouseState state = GetMouseState();
    int centerX = state.x - offsetX;
    int centerY = state.y - offsetY;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        CursorBuffer* cb = (CursorBuffer*)mapped.pData;
        memset(cb, 0, sizeof(CursorBuffer));
        cb->drawType = 1;
        cb->highlightX = centerX;
        cb->highlightY = centerY;
        cb->highlightRadius = radius;
        cb->highlightColorR = r / 255.0f;
        cb->highlightColorG = g / 255.0f;
        cb->highlightColorB = b / 255.0f;
        cb->highlightColorA = a / 255.0f;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    SetupPipelineAndDraw(m_context, targetTexture, m_device, m_constantBuffer, m_vertexShader, m_pixelShader, m_blendState, nullptr, nullptr);

    return true;
}

bool MouseCapture::DrawClickAnimation(ID3D11Texture2D* targetTexture, int clickX, int clickY, int offsetX, int offsetY, float progress, uint8_t r, uint8_t g, uint8_t b) {
    if (!m_initialized || !targetTexture || progress < 0 || progress > 1) return false;

    int centerX = clickX - offsetX;
    int centerY = clickY - offsetY;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        CursorBuffer* cb = (CursorBuffer*)mapped.pData;
        memset(cb, 0, sizeof(CursorBuffer));
        cb->drawType = 2;
        cb->highlightX = centerX;
        cb->highlightY = centerY;
        cb->clickProgress = progress;
        cb->clickColorR = r / 255.0f;
        cb->clickColorG = g / 255.0f;
        cb->clickColorB = b / 255.0f;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    SetupPipelineAndDraw(m_context, targetTexture, m_device, m_constantBuffer, m_vertexShader, m_pixelShader, m_blendState, nullptr, nullptr);

    return true;
}

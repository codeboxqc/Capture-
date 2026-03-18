#pragma once
#include "RecordingEngine.h"
#include <queue>

struct CapturedFrame {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t timestamp;
    uint32_t frameIndex;
    bool isKeyframe; // This must match the name used in ProcessLoop
    bool hdrMetadata;
};

class FrameCapture {
public:
    FrameCapture() : m_running(false), m_frameCount(0), m_needsStaging(false), m_targetFPS(60) {}
    ~FrameCapture() { StopCapture(); }

    bool Initialize(bool needsStaging, ComPtr<ID3D12Device> d3d12Device, const ExtendedGPUInfo& gpuInfo,
        ComPtr<ID3D11Device> sharedD3D11Device, ComPtr<ID3D11DeviceContext> sharedD3D11Context,
        ComPtr<IDXGIOutput> targetOutput) {

        m_needsStaging = needsStaging;
        m_gpuInfo = gpuInfo;
        m_sharedD3D11Device = sharedD3D11Device;
        m_sharedD3D11Context = sharedD3D11Context;
        m_targetOutput = targetOutput;

        if (!m_sharedD3D11Device || !m_sharedD3D11Context || !m_targetOutput) {
            spdlog::error("FrameCapture missing D3D11 device or target output");
            return false;
        }
        return true;
    }

    bool StartCapture(uint32_t ringBufferSize, uint32_t targetFPS) {
        m_ringBufferSize = ringBufferSize;
        m_targetFPS = targetFPS;
        m_running = true;
        m_captureThread = std::thread(&FrameCapture::CaptureLoop, this);
        return true;
    }

    void StopCapture() {
        m_running = false;
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        while (!m_ringBuffer.empty()) m_ringBuffer.pop();
        while (!m_texturePool.empty()) m_texturePool.pop();
    }

    bool GetNextFrame(CapturedFrame& frame, uint32_t timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(m_bufferMutex);
        if (m_ringBuffer.empty()) {
            if (!m_frameAvailable.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                [this] { return !m_ringBuffer.empty() || !m_running; })) {
                return false;
            }
        }
        if (m_ringBuffer.empty()) return false;
        frame = m_ringBuffer.front();
        m_ringBuffer.pop();
        return true;
    }

    void ReturnTexture(ComPtr<ID3D11Texture2D> texture) {
        std::lock_guard<std::mutex> lock(m_texturePoolMutex);
        m_texturePool.push(texture);
    }

    uint64_t GetTimestamp() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

private:
    void CaptureLoop() {
        spdlog::info("Frame capture thread started for selected monitor");

        ComPtr<IDXGIOutputDuplication> duplication;
        HRESULT hr = InitializeDesktopDuplication(&duplication);
        if (FAILED(hr)) {
            spdlog::error("Failed to initialize Desktop Duplication: 0x{:08X}", static_cast<uint32_t>(hr));
            return;
        }

        // --- HIGH PRECISION FRAMERATE LIMITER ---
        auto frameDuration = std::chrono::microseconds(1000000 / (m_targetFPS > 0 ? m_targetFPS : 60));
        auto nextFrameTime = std::chrono::steady_clock::now();

        while (m_running) {
            auto now = std::chrono::steady_clock::now();

            // Throttle to requested FPS. Prevents overflowing the buffer!
            if (now < nextFrameTime) {
                std::this_thread::sleep_until(nextFrameTime);
                now = std::chrono::steady_clock::now();
            }

            ComPtr<IDXGIResource> desktopResource;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;

            hr = duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // Desktop is static. Advance clock and continue.
                nextFrameTime = now + frameDuration;
                continue;
            }
            if (FAILED(hr)) {
                spdlog::warn("Desktop duplication access lost. Error: 0x{:08X}", static_cast<uint32_t>(hr));
                break; // Handle device lost/monitor disconnect
            }

            // Sync clock to prevent burst-fire after a long timeout
            nextFrameTime = now + frameDuration;

            ComPtr<ID3D11Texture2D> sourceTexture;
            hr = desktopResource.As(&sourceTexture);

            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC srcDesc;
                sourceTexture->GetDesc(&srcDesc);

                ComPtr<ID3D11Texture2D> destTexture = GetRingBufferTexture(srcDesc.Width, srcDesc.Height, srcDesc.Format);

                // GPU memory copy
                m_sharedD3D11Context->CopyResource(destTexture.Get(), sourceTexture.Get());

                CapturedFrame frame;
                frame.texture = destTexture;
                frame.timestamp = GetTimestamp();
                frame.frameIndex = m_frameCount++;

                {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    if (m_ringBuffer.size() >= m_ringBufferSize) {
                        ReturnTexture(m_ringBuffer.front().texture);
                        m_ringBuffer.pop();
                        spdlog::warn("Frame dropped - buffer full");
                    }
                    m_ringBuffer.push(frame);
                }
                m_frameAvailable.notify_one();
            }
            duplication->ReleaseFrame();
        }
        if (duplication) duplication->ReleaseFrame();
        spdlog::info("Frame capture thread stopped");
    }

    HRESULT InitializeDesktopDuplication(IDXGIOutputDuplication** duplication) {
        if (!m_targetOutput) return E_INVALIDARG;
        ComPtr<IDXGIOutput1> output1;
        HRESULT hr = m_targetOutput.As(&output1);
        if (FAILED(hr)) return hr;

        // Force desktop duplication to capture the exact screen you selected
        return output1->DuplicateOutput(m_sharedD3D11Device.Get(), duplication);
    }

    ComPtr<ID3D11Texture2D> CreateRingBufferTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;

        if (m_needsStaging) {
            // Needed if cross-GPU mapping is required
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        }
        else {
            // Normal Zero-Copy
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
        }

        m_sharedD3D11Device->CreateTexture2D(&desc, nullptr, &texture);
        return texture;
    }

    ComPtr<ID3D11Texture2D> GetRingBufferTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
        std::lock_guard<std::mutex> lock(m_texturePoolMutex);
        if (!m_texturePool.empty()) {
            auto texture = m_texturePool.front();
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);

            if (desc.Width == width && desc.Height == height && desc.Format == format) {
                m_texturePool.pop();
                return texture;
            }
            while (!m_texturePool.empty()) m_texturePool.pop();
        }
        return CreateRingBufferTexture(width, height, format);
    }

    ExtendedGPUInfo m_gpuInfo;
    ComPtr<ID3D11Device> m_sharedD3D11Device;
    ComPtr<ID3D11DeviceContext> m_sharedD3D11Context;
    ComPtr<IDXGIOutput> m_targetOutput;

    bool m_needsStaging;
    std::atomic<bool> m_running;
    std::thread m_captureThread;

    uint32_t m_targetFPS;
    uint32_t m_ringBufferSize = 8;
    std::queue<CapturedFrame> m_ringBuffer;
    std::mutex m_bufferMutex;
    std::condition_variable m_frameAvailable;

    std::queue<ComPtr<ID3D11Texture2D>> m_texturePool;
    std::mutex m_texturePoolMutex;

    uint64_t m_frameCount;
};
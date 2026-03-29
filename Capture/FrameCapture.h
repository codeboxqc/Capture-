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
    FrameCapture() 
        : m_running(false)
        , m_frameCount(0)          // FIX: Explicitly initialize
        , m_needsStaging(false)
        , m_targetFPS(60)
        , m_ringBufferSize(8)
        , m_droppedFrames(0)       // FIX: Track dropped frames
    {}
    
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
        m_droppedFrames = 0;
        m_frameCount = 0;
        m_captureThread = std::thread(&FrameCapture::CaptureLoop, this);
        return true;
    }

    void StopCapture() {
        m_running = false;
        
        // FIX: Wake up any waiting threads
        m_frameAvailable.notify_all();
        
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        
        // FIX: Clear buffers safely
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            while (!m_ringBuffer.empty()) m_ringBuffer.pop();
        }
        {
            std::lock_guard<std::mutex> lock(m_texturePoolMutex);
            while (!m_texturePool.empty()) m_texturePool.pop();
        }
        
        if (m_droppedFrames > 0) {
            spdlog::warn("Total frames dropped during capture: {}", m_droppedFrames.load());
        }
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
        if (!texture) return;  // FIX: Null check
        std::lock_guard<std::mutex> lock(m_texturePoolMutex);
        m_texturePool.push(texture);
    }

    uint64_t GetTimestamp() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
    
    // FIX: Added stats getters
    uint64_t GetFrameCount() const { return m_frameCount.load(); }
    uint64_t GetDroppedFrames() const { return m_droppedFrames.load(); }

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
        
        // FIX: Calculate keyframe interval (every 2 seconds)
        const uint32_t keyframeInterval = m_targetFPS * 2;

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
                // FIX: Try to recover from device lost
                if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                    spdlog::warn("Desktop duplication lost, attempting recovery...");
                    duplication.Reset();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    hr = InitializeDesktopDuplication(&duplication);
                    if (SUCCEEDED(hr)) {
                        spdlog::info("Desktop duplication recovered");
                        continue;
                    }
                }
                spdlog::error("Desktop duplication failed: 0x{:08X}", static_cast<uint32_t>(hr));
                break;
            }

            // Sync clock to prevent burst-fire after a long timeout
            nextFrameTime = now + frameDuration;

            ComPtr<ID3D11Texture2D> sourceTexture;
            hr = desktopResource.As(&sourceTexture);

            if (SUCCEEDED(hr) && sourceTexture) {
                D3D11_TEXTURE2D_DESC srcDesc;
                sourceTexture->GetDesc(&srcDesc);

                ComPtr<ID3D11Texture2D> destTexture = GetRingBufferTexture(srcDesc.Width, srcDesc.Height, srcDesc.Format);
                
                // FIX: Check texture allocation
                if (!destTexture) {
                    spdlog::error("Failed to allocate ring buffer texture");
                    duplication->ReleaseFrame();
                    continue;
                }

                // GPU memory copy
                m_sharedD3D11Context->CopyResource(destTexture.Get(), sourceTexture.Get());

                uint64_t currentFrameIndex = m_frameCount.fetch_add(1);  // FIX: Thread-safe increment

                CapturedFrame frame;
                frame.texture = destTexture;

                // Use high-precision timestamp from DXGI
                LARGE_INTEGER qpcFreq;
                QueryPerformanceFrequency(&qpcFreq);
                frame.timestamp = (frameInfo.LastPresentTime.QuadPart * 1000000) / qpcFreq.QuadPart;

                frame.frameIndex = static_cast<uint32_t>(currentFrameIndex);
                frame.isKeyframe = (currentFrameIndex % keyframeInterval == 0);  // FIX: Set keyframe flag
                frame.hdrMetadata = false;  // FIX: Initialize to false

                // FIX: DEADLOCK FIX - Don't call ReturnTexture while holding m_bufferMutex
                ComPtr<ID3D11Texture2D> textureToReturn = nullptr;
                {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    if (m_ringBuffer.size() >= m_ringBufferSize) {
                        textureToReturn = m_ringBuffer.front().texture;  // Save texture
                        m_ringBuffer.pop();
                        m_droppedFrames++;
                    }
                    m_ringBuffer.push(frame);
                }
                
                // Return texture OUTSIDE the lock
                if (textureToReturn) {
                    ReturnTexture(textureToReturn);
                }
                
                m_frameAvailable.notify_one();
            }
            
            duplication->ReleaseFrame();
        }
        
        // FIX: Removed duplicate ReleaseFrame call (was causing potential crash)
        spdlog::info("Frame capture thread stopped. Captured: {}, Dropped: {}", 
            m_frameCount.load(), m_droppedFrames.load());
    }

    HRESULT InitializeDesktopDuplication(IDXGIOutputDuplication** duplication) {
        if (!m_targetOutput) return E_INVALIDARG;
        if (!duplication) return E_POINTER;  // FIX: Null check
        
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

        HRESULT hr = m_sharedD3D11Device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr)) {
            spdlog::error("Failed to create texture: 0x{:08X}", static_cast<uint32_t>(hr));
            return nullptr;
        }
        return texture;
    }

    ComPtr<ID3D11Texture2D> GetRingBufferTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
        std::lock_guard<std::mutex> lock(m_texturePoolMutex);
        if (!m_texturePool.empty()) {
            auto texture = m_texturePool.front();
            
            // FIX: Null check before use
            if (texture) {
                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc(&desc);

                if (desc.Width == width && desc.Height == height && desc.Format == format) {
                    m_texturePool.pop();
                    return texture;
                }
            }
            
            // Size changed, clear pool
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
    uint32_t m_ringBufferSize;
    std::queue<CapturedFrame> m_ringBuffer;
    std::mutex m_bufferMutex;
    std::condition_variable m_frameAvailable;

    std::queue<ComPtr<ID3D11Texture2D>> m_texturePool;
    std::mutex m_texturePoolMutex;

    std::atomic<uint64_t> m_frameCount;      // FIX: Made atomic
    std::atomic<uint64_t> m_droppedFrames;   // FIX: Track dropped frames
};

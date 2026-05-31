#pragma once
#include <string>
#include <vector>
#include <memory>
#include <d3d11.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct CameraInfo {
    std::string name;
    std::string url;
    bool isIPCamera;
};

struct CameraFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    uint64_t timestamp;
};

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    static std::vector<CameraInfo> DetectLocalCameras();

    // Connects to an IP camera stream or local directshow device
    bool Initialize(const std::string& url, Microsoft::WRL::ComPtr<ID3D11Device> device);
    bool Start();
    void Stop();
    bool GetNextFrame(CameraFrame& outFrame, int timeoutMs = 100);

private:
    void CaptureLoop();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateStagingTexture(int width, int height);

    std::string m_url;
    std::atomic<bool> m_running;
    std::thread m_thread;

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int m_videoStreamIndex = -1;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    std::mutex m_frameMutex;
    CameraFrame m_latestFrame;
    bool m_hasNewFrame = false;
};

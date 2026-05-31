#include "camera.h"
#include <spdlog/spdlog.h>
#include <chrono>

extern "C" {
#include <libavdevice/avdevice.h>
}

CameraCapture::CameraCapture() : m_running(false) {
    avformat_network_init();
    avdevice_register_all();
}

CameraCapture::~CameraCapture() {
    Stop();
}

std::vector<CameraInfo> CameraCapture::DetectLocalCameras() {
    std::vector<CameraInfo> cameras;

    // In FFmpeg, dshow is the primary Windows device interface
    const AVInputFormat* iformat = av_find_input_format("dshow");
    if (!iformat) return cameras;

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    AVDictionary* options = nullptr;
    av_dict_set(&options, "list_devices", "true", 0);

    // Attempting to list devices - FFmpeg handles this gracefully but prints to stderr
    // A more native approach uses MediaFoundation, but we provide FFmpeg dshow fallback
    avformat_open_input(&fmt_ctx, "video=dummy", iformat, &options);

    avformat_free_context(fmt_ctx);
    return cameras;
}

bool CameraCapture::Initialize(const std::string& url, Microsoft::WRL::ComPtr<ID3D11Device> device) {
    m_url = url;
    m_device = device;
    m_device->GetImmediateContext(&m_context);

    AVDictionary* options = nullptr;

    // Set fast parameters for IP cameras
    if (url.find("rtsp://") != std::string::npos || url.find("http://") != std::string::npos) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "buffer_size", "1024000", 0);
        av_dict_set(&options, "max_delay", "500000", 0);
        av_dict_set(&options, "fflags", "nobuffer", 0);
    }

    // Support DirectShow
    const AVInputFormat* ifmt = nullptr;
    if (url.find("video=") == 0) {
        ifmt = av_find_input_format("dshow");
    }

    if (avformat_open_input(&m_formatCtx, url.c_str(), ifmt, &options) != 0) {
        spdlog::error("Camera: Failed to open stream {}", url);
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        spdlog::error("Camera: Failed to find stream info");
        return false;
    }

    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;
            break;
        }
    }

    if (m_videoStreamIndex == -1) {
        spdlog::error("Camera: No video stream found");
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(m_formatCtx->streams[m_videoStreamIndex]->codecpar->codec_id);
    m_codecCtx = avcodec_alloc_context3(codec);
    m_streamWidth = m_formatCtx->streams[m_videoStreamIndex]->codecpar->width;
    m_streamHeight = m_formatCtx->streams[m_videoStreamIndex]->codecpar->height;
    avcodec_parameters_to_context(m_codecCtx, m_formatCtx->streams[m_videoStreamIndex]->codecpar);

    // Optimization for latency
    m_codecCtx->thread_count = 2;
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        spdlog::error("Camera: Failed to open decoder");
        return false;
    }

    m_swsCtx = sws_getContext(
        m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_BGRA,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    return true;
}

bool CameraCapture::Start() {
    if (m_running) return true;
    m_running = true;
    m_thread = std::thread(&CameraCapture::CaptureLoop, this);
    return true;
}

void CameraCapture::Stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }
    if (m_formatCtx) { avformat_close_input(&m_formatCtx); m_formatCtx = nullptr; }
}

bool CameraCapture::GetNextFrame(CameraFrame& outFrame, int timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::milliseconds(timeoutMs)) {
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_hasNewFrame && m_latestFrame.texture) {
                outFrame = m_latestFrame;
                m_hasNewFrame = false;
                return true;
            }
        }
        if (!m_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> CameraCapture::CreateStagingTexture(int width, int height) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;

    m_device->CreateTexture2D(&desc, nullptr, &tex);
    return tex;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> CameraCapture::GetTextureFromPool(int width, int height) {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (!m_texturePool.empty()) {
        auto tex = m_texturePool.back();
        m_texturePool.pop_back();
        return tex;
    }
    return CreateStagingTexture(width, height);
}

void CameraCapture::ReturnTexture(Microsoft::WRL::ComPtr<ID3D11Texture2D> tex) {
    if (!tex) return;
    std::lock_guard<std::mutex> lock(m_poolMutex);
    m_texturePool.push_back(tex);
}

void CameraCapture::CaptureLoop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();

    rgbFrame->format = AV_PIX_FMT_BGRA;
    rgbFrame->width = m_codecCtx->width;
    rgbFrame->height = m_codecCtx->height;
    av_frame_get_buffer(rgbFrame, 32);

    while (m_running) {
        int readResult = av_read_frame(m_formatCtx, packet);
        if (readResult >= 0) {
            if (packet->stream_index == m_videoStreamIndex) {
                if (avcodec_send_packet(m_codecCtx, packet) == 0) {
                    while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
                        sws_scale(m_swsCtx, frame->data, frame->linesize, 0, m_codecCtx->height,
                                  rgbFrame->data, rgbFrame->linesize);

                        auto d3dTexture = GetTextureFromPool(m_codecCtx->width, m_codecCtx->height);
                        if (d3dTexture && m_context) {
                            // Protect multithreaded immediate context access
                            ComPtr<ID3D11Multithread> multiThread;
                            if (SUCCEEDED(m_context.As(&multiThread))) {
                                multiThread->Enter();
                                m_context->UpdateSubresource(d3dTexture.Get(), 0, nullptr,
                                                             rgbFrame->data[0], rgbFrame->linesize[0], 0);
                                m_context->Flush();
                                multiThread->Leave();
                            } else {
                                m_context->UpdateSubresource(d3dTexture.Get(), 0, nullptr,
                                                             rgbFrame->data[0], rgbFrame->linesize[0], 0);
                            }

                            std::lock_guard<std::mutex> lock(m_frameMutex);
                            if (m_hasNewFrame && m_latestFrame.texture) {
                                ReturnTexture(m_latestFrame.texture);
                            }
                            m_latestFrame.texture = d3dTexture;
                            m_latestFrame.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                            m_hasNewFrame = true;
                        }
                    }
                }
            }
            av_packet_unref(packet);
        } else {
            // Stream disconnected or EOF, wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    av_frame_free(&rgbFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
}

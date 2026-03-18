#pragma once
#include "RecordingEngine.h"
#include "FrameCapture.h"
#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

struct EncodedPacket {
    std::vector<uint8_t> data;
    uint64_t sourceTimestamp;
    int64_t pts;
    bool keyframe;
};

class HardwareEncoder {
public:
    HardwareEncoder() : m_codecContext(nullptr), m_packet(nullptr), m_hwDeviceCtx(nullptr), m_hwFramesCtx(nullptr), m_swsContext(nullptr), m_useSystemMemory(false) {}

    ~HardwareEncoder() {
        if (m_swsContext) sws_freeContext(m_swsContext);
        if (m_codecContext) avcodec_free_context(&m_codecContext);
        if (m_packet) av_packet_free(&m_packet);
        if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
        if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    }

    bool Initialize(const GPUInfo& gpuInfo, const RecordingSettings& settings,
        ComPtr<ID3D11Device> d3d11Device, ComPtr<ID3D11DeviceContext> d3d11Context) {

        m_d3d11Device = d3d11Device;
        m_d3d11Context = d3d11Context;

        if (!m_d3d11Device || !m_d3d11Context) {
            spdlog::error("D3D11 device/context is null");
            return false;
        }

        av_log_set_level(AV_LOG_INFO);

        const AVCodec* codec = nullptr;
        std::string codecName;

        if (gpuInfo.encoderType == EncoderType::NVIDIA_NVENC) {
            if (settings.codec == Codec::H265) {
                codecName = "hevc_nvenc";
                codec = avcodec_find_encoder_by_name("hevc_nvenc");
            }
            else if (settings.codec == Codec::AV1) {
                codecName = "av1_nvenc";
                codec = avcodec_find_encoder_by_name("av1_nvenc");
            }
            else {
                codecName = "h264_nvenc";
                codec = avcodec_find_encoder_by_name("h264_nvenc");
            }
        }
        else if (gpuInfo.encoderType == EncoderType::AMD_AMF) {
            if (settings.codec == Codec::H265) codec = avcodec_find_encoder_by_name("hevc_amf");
            else codec = avcodec_find_encoder_by_name("h264_amf");
        }
        else if (gpuInfo.encoderType == EncoderType::INTEL_QSV) {
            if (settings.codec == Codec::H265) codec = avcodec_find_encoder_by_name("hevc_qsv");
            else codec = avcodec_find_encoder_by_name("h264_qsv");
        }

        if (!codec) {
            spdlog::warn("Hardware encoder '{}' not found, falling back to software", codecName);
            if (settings.codec == Codec::H265) codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
            else codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }

        if (!codec) {
            spdlog::error("Failed to find any encoder");
            return false;
        }

        spdlog::info("Using Encoder: {}", codec->name);

        m_codecContext = avcodec_alloc_context3(codec);
        if (!m_codecContext) return false;

        m_useSystemMemory = (gpuInfo.encoderType == EncoderType::INTEL_QSV || gpuInfo.encoderType == EncoderType::SOFTWARE);

        if (!m_useSystemMemory) {
            m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!m_hwDeviceCtx) return false;

            AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
            AVD3D11VADeviceContext* d3d11hwCtx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;

            d3d11hwCtx->device = m_d3d11Device.Get();
            d3d11hwCtx->device->AddRef();
            d3d11hwCtx->device_context = m_d3d11Context.Get();
            d3d11hwCtx->device_context->AddRef();

            if (av_hwdevice_ctx_init(m_hwDeviceCtx) < 0) {
                spdlog::error("Failed to initialize hardware device context");
                return false;
            }

            m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
            if (!m_hwFramesCtx) return false;

            AVHWFramesContext* framesCtx = (AVHWFramesContext*)m_hwFramesCtx->data;
            framesCtx->format = AV_PIX_FMT_D3D11;
            framesCtx->sw_format = AV_PIX_FMT_BGRA;
            framesCtx->width = settings.width;
            framesCtx->height = settings.height;
            // FIX 1: Larger pre-allocated VRAM pool to reduce allocation stuttering
            framesCtx->initial_pool_size = 96;  // Increased from 64 to 96

            if (av_hwframe_ctx_init(m_hwFramesCtx) < 0) {
                spdlog::error("Failed to initialize hardware frames context");
                return false;
            }

            m_codecContext->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
            m_codecContext->pix_fmt = AV_PIX_FMT_D3D11;
            m_codecContext->sw_pix_fmt = AV_PIX_FMT_BGRA;
        }
        else {
            m_codecContext->pix_fmt = AV_PIX_FMT_NV12;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = settings.width;
            desc.Height = settings.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            HRESULT hr = m_d3d11Device->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
            if (FAILED(hr)) return false;

            m_swsContext = sws_getContext(
                settings.width, settings.height, AV_PIX_FMT_BGRA,
                settings.width, settings.height, AV_PIX_FMT_NV12,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            if (!m_swsContext) return false;

            if (gpuInfo.encoderType == EncoderType::INTEL_QSV) {
                m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
                if (m_hwDeviceCtx) {
                    AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
                    AVD3D11VADeviceContext* d3d11hwCtx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
                    d3d11hwCtx->device = m_d3d11Device.Get();
                    d3d11hwCtx->device->AddRef();
                    d3d11hwCtx->device_context = m_d3d11Context.Get();
                    d3d11hwCtx->device_context->AddRef();

                    if (av_hwdevice_ctx_init(m_hwDeviceCtx) >= 0) {
                        m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                    }
                }
            }
        }

        m_codecContext->width = settings.width;
        m_codecContext->height = settings.height;
        m_codecContext->time_base = { 1, static_cast<int>(settings.fps) };
        m_codecContext->framerate = { static_cast<int>(settings.fps), 1 };
        m_codecContext->gop_size = settings.fps * 2;

        // FIX 2: NO B-FRAMES! B-Frames force the encoder to delay packets, overflowing the buffer in real-time capture!
        m_codecContext->max_b_frames = 0;

        m_codecContext->bit_rate = 0;
        int targetQuality = 18;

        if (gpuInfo.encoderType == EncoderType::NVIDIA_NVENC) {
            av_opt_set(m_codecContext->priv_data, "preset", "p4", 0);
            av_opt_set(m_codecContext->priv_data, "tune", "ull", 0);
            av_opt_set(m_codecContext->priv_data, "delay", "0", 0);
            av_opt_set(m_codecContext->priv_data, "zerolatency", "1", 0); // Strict zero latency
            av_opt_set(m_codecContext->priv_data, "rc", "constqp", 0);
            av_opt_set_int(m_codecContext->priv_data, "qp", targetQuality, 0);
        }
        else if (gpuInfo.encoderType == EncoderType::AMD_AMF) {
            av_opt_set(m_codecContext->priv_data, "quality", "balanced", 0);
            av_opt_set(m_codecContext->priv_data, "rc", "cqp", 0);
            av_opt_set_int(m_codecContext->priv_data, "qp_i", targetQuality, 0);
            av_opt_set_int(m_codecContext->priv_data, "qp_p", targetQuality, 0);
        }
        else if (gpuInfo.encoderType == EncoderType::INTEL_QSV) {
            av_opt_set(m_codecContext->priv_data, "preset", "faster", 0);
            av_opt_set(m_codecContext->priv_data, "async_depth", "1", 0);
            av_opt_set_int(m_codecContext->priv_data, "global_quality", targetQuality, 0);
        }

        if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
            spdlog::error("Failed to open codec");
            return false;
        }

        m_packet = av_packet_alloc();
        m_frameCount = 0;
        return true;
    }

    bool EncodeFrame(const CapturedFrame& frame, std::vector<EncodedPacket>& outPackets) {
        if (!m_codecContext || !m_packet || !frame.texture) return false;

        AVFrame* avFrame = av_frame_alloc();
        if (!avFrame) return false;

        if (m_useSystemMemory) {
            avFrame->format = AV_PIX_FMT_NV12;
            avFrame->width = m_codecContext->width;
            avFrame->height = m_codecContext->height;

            if (av_frame_get_buffer(avFrame, 32) < 0) {
                av_frame_free(&avFrame);
                return false;
            }

            m_d3d11Context->CopyResource(m_stagingTexture.Get(), frame.texture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_d3d11Context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(mapped.pData) };
                int inLinesize[1] = { static_cast<int>(mapped.RowPitch) };

                sws_scale(m_swsContext, inData, inLinesize, 0, m_codecContext->height, avFrame->data, avFrame->linesize);
                m_d3d11Context->Unmap(m_stagingTexture.Get(), 0);
            }
            else {
                av_frame_free(&avFrame);
                return false;
            }
        }
        else {
            avFrame->format = AV_PIX_FMT_D3D11;
            avFrame->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);

            if (av_hwframe_get_buffer(m_hwFramesCtx, avFrame, 0) < 0) {
                spdlog::error("Failed to allocate hardware frame buffer");
                av_frame_free(&avFrame);
                return false;
            }

            ID3D11Texture2D* dstTex = (ID3D11Texture2D*)avFrame->data[0];
            UINT dstSubresource = (UINT)(intptr_t)avFrame->data[1];
            ID3D11Texture2D* srcTex = (ID3D11Texture2D*)frame.texture.Get();

            m_d3d11Context->CopySubresourceRegion(dstTex, dstSubresource, 0, 0, 0, srcTex, 0, nullptr);
        }

        avFrame->pts = frame.frameIndex;

        if (frame.isKeyframe) {
            avFrame->pict_type = AV_PICTURE_TYPE_I;
        }

        int ret = avcodec_send_frame(m_codecContext, avFrame);
        av_frame_free(&avFrame);

        if (ret < 0) return false;

        while (ret >= 0) {
            ret = avcodec_receive_packet(m_codecContext, m_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) return false;

            EncodedPacket ep;
            ep.data.assign(m_packet->data, m_packet->data + m_packet->size);
            ep.pts = m_packet->pts;
            ep.sourceTimestamp = frame.timestamp;
            ep.keyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;

            outPackets.push_back(std::move(ep));
            av_packet_unref(m_packet);
        }

        m_frameCount++;
        return true;
    }

    void Flush(std::vector<EncodedPacket>& outPackets) {
        if (!m_codecContext || !m_packet) return;

        int ret = avcodec_send_frame(m_codecContext, nullptr);

        while (ret >= 0) {
            ret = avcodec_receive_packet(m_codecContext, m_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            EncodedPacket ep;
            ep.data.assign(m_packet->data, m_packet->data + m_packet->size);
            ep.pts = m_packet->pts;
            ep.sourceTimestamp = 0;
            ep.keyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;

            outPackets.push_back(std::move(ep));
            av_packet_unref(m_packet);
        }
    }

private:
    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;

    AVCodecContext* m_codecContext;
    AVPacket* m_packet;
    AVBufferRef* m_hwDeviceCtx;
    AVBufferRef* m_hwFramesCtx;

    bool m_useSystemMemory = false;
    SwsContext* m_swsContext = nullptr;
    ComPtr<ID3D11Texture2D> m_stagingTexture;

    uint64_t m_frameCount = 0;
};
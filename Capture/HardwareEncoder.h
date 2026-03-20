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
    HardwareEncoder()
        : m_codecContext(nullptr)
        , m_packet(nullptr)
        , m_hwDeviceCtx(nullptr)
        , m_hwFramesCtx(nullptr)
        , m_swsContext(nullptr)
        , m_useSystemMemory(false)
        , m_frameCount(0)
        , m_encodedFrames(0)
        , m_keyframeInterval(60)
    {
    }

    ~HardwareEncoder() {
        Cleanup();
    }

    bool Initialize(const GPUInfo& gpuInfo, const RecordingSettings& settings,
        ComPtr<ID3D11Device> d3d11Device, ComPtr<ID3D11DeviceContext> d3d11Context) {

        m_d3d11Device = d3d11Device;
        m_d3d11Context = d3d11Context;
        m_settings = settings;

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
            if (settings.codec == Codec::AV1) {
                codecName = "av1_amf";
                codec = avcodec_find_encoder_by_name("av1_amf");
            }
            if (!codec) {
                if (settings.codec == Codec::H265) {
                    codecName = "hevc_amf";
                    codec = avcodec_find_encoder_by_name("hevc_amf");
                }
                else {
                    codecName = "h264_amf";
                    codec = avcodec_find_encoder_by_name("h264_amf");
                }
            }
        }
        else if (gpuInfo.encoderType == EncoderType::INTEL_QSV) {
            if (settings.codec == Codec::AV1) {
                codecName = "av1_qsv";
                codec = avcodec_find_encoder_by_name("av1_qsv");
            }
            if (!codec) {
                if (settings.codec == Codec::H265) {
                    codecName = "hevc_qsv";
                    codec = avcodec_find_encoder_by_name("hevc_qsv");
                }
                else {
                    codecName = "h264_qsv";
                    codec = avcodec_find_encoder_by_name("h264_qsv");
                }
            }
        }

        if (!codec) {
            spdlog::warn("Hardware encoder '{}' not found, falling back to software", codecName);
            if (settings.codec == Codec::H265) codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
            else if (settings.codec == Codec::AV1) codec = avcodec_find_encoder(AV_CODEC_ID_AV1);
            else codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }

        if (!codec) {
            spdlog::error("Failed to find any encoder");
            return false;
        }

        spdlog::info("Using Encoder: {}", codec->name);

        m_codecContext = avcodec_alloc_context3(codec);
        if (!m_codecContext) {
            spdlog::error("Failed to allocate codec context");
            return false;
        }

        m_useSystemMemory = (gpuInfo.encoderType == EncoderType::INTEL_QSV || gpuInfo.encoderType == EncoderType::SOFTWARE);

        if (!m_useSystemMemory) {
            if (!InitializeHardwareContext(settings)) {
                spdlog::error("Failed to initialize hardware context");
                Cleanup();
                return false;
            }
        }
        else {
            if (!InitializeSystemMemoryContext(settings, gpuInfo)) {
                spdlog::error("Failed to initialize system memory context");
                Cleanup();
                return false;
            }
        }

        m_codecContext->width = settings.width;
        m_codecContext->height = settings.height;
        m_codecContext->time_base = { 1, static_cast<int>(settings.fps) };
        m_codecContext->framerate = { static_cast<int>(settings.fps), 1 };
        m_codecContext->gop_size = settings.fps * 2;
        m_codecContext->max_b_frames = 0;
        m_codecContext->bit_rate = 0;

        // Set flags for global headers (needed for MKV)
        m_codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // FIX: Save keyframe interval
        m_keyframeInterval = m_codecContext->gop_size;

        int targetQuality = 15;

        if (!ConfigureEncoderOptions(gpuInfo.encoderType, targetQuality)) {
            spdlog::warn("Some encoder options may not have been applied");
        }

        int ret = avcodec_open2(m_codecContext, codec, nullptr);
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            spdlog::error("Failed to open codec: {}", errBuf);
            Cleanup();
            return false;
        }

        m_packet = av_packet_alloc();
        if (!m_packet) {
            spdlog::error("Failed to allocate packet");
            Cleanup();
            return false;
        }

        m_frameCount = 0;
        m_encodedFrames = 0;

        spdlog::info("HardwareEncoder initialized: {}x{} @ {} fps, GOP: {}",
            settings.width, settings.height, settings.fps, m_codecContext->gop_size);

        return true;
    }

    bool EncodeFrame(const CapturedFrame& frame, std::vector<EncodedPacket>& outPackets) {
        if (!m_codecContext || !m_packet) {
            spdlog::error("Encoder not initialized");
            return false;
        }

        if (!frame.texture) {
            spdlog::warn("Frame texture is null");
            return false;
        }

        AVFrame* avFrame = av_frame_alloc();
        if (!avFrame) {
            spdlog::error("Failed to allocate AVFrame");
            return false;
        }

        bool success = false;

        if (m_useSystemMemory) {
            success = PrepareSystemMemoryFrame(frame, avFrame);
        }
        else {
            success = PrepareHardwareFrame(frame, avFrame);
        }

        if (!success) {
            av_frame_free(&avFrame);
            return false;
        }

        avFrame->pts = frame.frameIndex;

        // FIX: Properly request keyframe (pict_type only - key_frame removed in newer FFmpeg)
        if (frame.isKeyframe || (m_frameCount % m_keyframeInterval == 0)) {
            avFrame->pict_type = AV_PICTURE_TYPE_I;
        }
        else {
            avFrame->pict_type = AV_PICTURE_TYPE_NONE;  // Let encoder decide
        }

        int ret = avcodec_send_frame(m_codecContext, avFrame);
        av_frame_free(&avFrame);

        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                // Encoder needs to output packets first
                spdlog::debug("Encoder buffer full, draining...");
            }
            else {
                char errBuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errBuf, sizeof(errBuf));
                spdlog::error("Failed to send frame: {}", errBuf);
                return false;
            }
        }

        // Receive all available packets
        while (true) {
            ret = avcodec_receive_packet(m_codecContext, m_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                char errBuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errBuf, sizeof(errBuf));
                spdlog::error("Failed to receive packet: {}", errBuf);
                return false;
            }

            EncodedPacket ep;
            ep.data.assign(m_packet->data, m_packet->data + m_packet->size);
            ep.pts = m_packet->pts;
            ep.sourceTimestamp = frame.timestamp;
            ep.keyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;

            outPackets.push_back(std::move(ep));
            m_encodedFrames++;

            av_packet_unref(m_packet);
        }

        m_frameCount++;
        return true;
    }

    void Flush(std::vector<EncodedPacket>& outPackets) {
        if (!m_codecContext || !m_packet) return;

        int ret = avcodec_send_frame(m_codecContext, nullptr);

        while (ret >= 0 || ret == AVERROR(EAGAIN)) {
            ret = avcodec_receive_packet(m_codecContext, m_packet);
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            if (ret == AVERROR_EOF || ret < 0) {
                break;
            }

            EncodedPacket ep;
            ep.data.assign(m_packet->data, m_packet->data + m_packet->size);
            ep.pts = m_packet->pts;
            ep.sourceTimestamp = 0;
            ep.keyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;

            outPackets.push_back(std::move(ep));
            m_encodedFrames++;

            av_packet_unref(m_packet);
        }

        spdlog::info("Encoder flushed. Total frames: {}, Encoded packets: {}",
            m_frameCount, m_encodedFrames.load());
    }

    std::vector<uint8_t> GetExtradata() const {
        if (!m_codecContext) return {};
        if (!m_codecContext->extradata || m_codecContext->extradata_size <= 0) return {};

        return std::vector<uint8_t>(
            m_codecContext->extradata,
            m_codecContext->extradata + m_codecContext->extradata_size
        );
    }

    // FIX: Added stats getters
    uint64_t GetFrameCount() const { return m_frameCount; }
    uint64_t GetEncodedFrames() const { return m_encodedFrames.load(); }
    bool IsInitialized() const { return m_codecContext != nullptr && m_packet != nullptr; }

private:
    void Cleanup() {
        if (m_swsContext) {
            sws_freeContext(m_swsContext);
            m_swsContext = nullptr;
        }

        if (m_packet) {
            av_packet_free(&m_packet);
            m_packet = nullptr;
        }

        if (m_codecContext) {
            avcodec_free_context(&m_codecContext);
            m_codecContext = nullptr;
        }

        if (m_hwFramesCtx) {
            av_buffer_unref(&m_hwFramesCtx);
            m_hwFramesCtx = nullptr;
        }

        if (m_hwDeviceCtx) {
            av_buffer_unref(&m_hwDeviceCtx);
            m_hwDeviceCtx = nullptr;
        }

        m_stagingTexture.Reset();
    }

    bool InitializeHardwareContext(const RecordingSettings& settings) {
        m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!m_hwDeviceCtx) {
            spdlog::error("Failed to allocate hardware device context");
            return false;
        }

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
        AVD3D11VADeviceContext* d3d11hwCtx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;

        d3d11hwCtx->device = m_d3d11Device.Get();
        d3d11hwCtx->device->AddRef();
        d3d11hwCtx->device_context = m_d3d11Context.Get();
        d3d11hwCtx->device_context->AddRef();

        int ret = av_hwdevice_ctx_init(m_hwDeviceCtx);
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            spdlog::error("Failed to initialize hardware device context: {}", errBuf);
            return false;
        }

        m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
        if (!m_hwFramesCtx) {
            spdlog::error("Failed to allocate hardware frames context");
            return false;
        }

        AVHWFramesContext* framesCtx = (AVHWFramesContext*)m_hwFramesCtx->data;
        framesCtx->format = AV_PIX_FMT_D3D11;
        framesCtx->sw_format = AV_PIX_FMT_BGRA;
        framesCtx->width = settings.width;
        framesCtx->height = settings.height;
        framesCtx->initial_pool_size = 64;

        ret = av_hwframe_ctx_init(m_hwFramesCtx);
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            spdlog::error("Failed to initialize hardware frames context: {}", errBuf);
            return false;
        }

        m_codecContext->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
        if (!m_codecContext->hw_frames_ctx) {
            spdlog::error("Failed to reference hardware frames context");
            return false;
        }

        m_codecContext->pix_fmt = AV_PIX_FMT_D3D11;
        m_codecContext->sw_pix_fmt = AV_PIX_FMT_BGRA;

        return true;
    }

    bool InitializeSystemMemoryContext(const RecordingSettings& settings, const GPUInfo& gpuInfo) {
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
        if (FAILED(hr)) {
            spdlog::error("Failed to create staging texture: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        m_swsContext = sws_getContext(
            settings.width, settings.height, AV_PIX_FMT_BGRA,
            settings.width, settings.height, AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        if (!m_swsContext) {
            spdlog::error("Failed to create SWS context");
            return false;
        }

        // Intel QSV still benefits from hardware device context
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
                    if (!m_codecContext->hw_device_ctx) {
                        spdlog::warn("Failed to reference QSV hardware device context");
                    }
                }
            }
        }

        return true;
    }

    bool ConfigureEncoderOptions(EncoderType encoderType, int targetQuality) {
        if (!m_codecContext || !m_codecContext->priv_data) {
            return false;
        }

        int ret = 0;

        if (encoderType == EncoderType::NVIDIA_NVENC) {
            ret |= av_opt_set(m_codecContext->priv_data, "preset", "p4", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "tune", "hq", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "delay", "0", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "zerolatency", "1", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "rc", "constqp", 0);
            ret |= av_opt_set_int(m_codecContext->priv_data, "qp", targetQuality, 0);
            // FIX: Force IDR frames for keyframes
            ret |= av_opt_set(m_codecContext->priv_data, "forced-idr", "1", 0);
        }
        else if (encoderType == EncoderType::AMD_AMF) {
            ret |= av_opt_set(m_codecContext->priv_data, "quality", "balanced", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "rc", "cqp", 0);
            ret |= av_opt_set_int(m_codecContext->priv_data, "qp_i", targetQuality, 0);
            ret |= av_opt_set_int(m_codecContext->priv_data, "qp_p", targetQuality, 0);
        }
        else if (encoderType == EncoderType::INTEL_QSV) {
            ret |= av_opt_set(m_codecContext->priv_data, "preset", "balanced", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "async_depth", "2", 0);
            ret |= av_opt_set_int(m_codecContext->priv_data, "global_quality", targetQuality, 0);
        }
        else if (encoderType == EncoderType::SOFTWARE) {
            ret |= av_opt_set(m_codecContext->priv_data, "preset", "ultrafast", 0);
            ret |= av_opt_set(m_codecContext->priv_data, "crf", std::to_string(targetQuality).c_str(), 0);
        }

        return (ret >= 0);  // Some options may not exist, that's OK
    }

    bool PrepareSystemMemoryFrame(const CapturedFrame& frame, AVFrame* avFrame) {
        avFrame->format = AV_PIX_FMT_NV12;
        avFrame->width = m_codecContext->width;
        avFrame->height = m_codecContext->height;

        if (av_frame_get_buffer(avFrame, 32) < 0) {
            spdlog::error("Failed to allocate frame buffer");
            return false;
        }

        // Copy to staging texture
        m_d3d11Context->CopyResource(m_stagingTexture.Get(), frame.texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_d3d11Context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            spdlog::error("Failed to map staging texture: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(mapped.pData) };
        int inLinesize[1] = { static_cast<int>(mapped.RowPitch) };

        sws_scale(m_swsContext, inData, inLinesize, 0, m_codecContext->height, avFrame->data, avFrame->linesize);

        m_d3d11Context->Unmap(m_stagingTexture.Get(), 0);

        return true;
    }

    bool PrepareHardwareFrame(const CapturedFrame& frame, AVFrame* avFrame) {
        avFrame->format = AV_PIX_FMT_D3D11;

        // FIX: Check hw_frames_ctx before referencing
        if (!m_hwFramesCtx) {
            spdlog::error("Hardware frames context is null");
            return false;
        }

        avFrame->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
        if (!avFrame->hw_frames_ctx) {
            spdlog::error("Failed to reference hardware frames context");
            return false;
        }

        int ret = av_hwframe_get_buffer(m_hwFramesCtx, avFrame, 0);
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            spdlog::error("Failed to allocate hardware frame buffer: {}", errBuf);
            // FIX: Clean up the hw_frames_ctx reference we just made
            av_buffer_unref(&avFrame->hw_frames_ctx);
            return false;
        }

        ID3D11Texture2D* dstTex = (ID3D11Texture2D*)avFrame->data[0];
        UINT dstSubresource = (UINT)(intptr_t)avFrame->data[1];
        ID3D11Texture2D* srcTex = (ID3D11Texture2D*)frame.texture.Get();

        if (!dstTex || !srcTex) {
            spdlog::error("Invalid texture pointers");
            return false;
        }

        m_d3d11Context->CopySubresourceRegion(dstTex, dstSubresource, 0, 0, 0, srcTex, 0, nullptr);

        return true;
    }

    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    ComPtr<ID3D11Texture2D> m_stagingTexture;

    AVCodecContext* m_codecContext;
    AVPacket* m_packet;
    AVBufferRef* m_hwDeviceCtx;
    AVBufferRef* m_hwFramesCtx;
    SwsContext* m_swsContext;

    RecordingSettings m_settings;
    bool m_useSystemMemory;
    uint64_t m_frameCount;
    std::atomic<uint64_t> m_encodedFrames;
    int m_keyframeInterval;
};
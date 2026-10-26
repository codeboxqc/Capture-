#pragma once
#include "RecordingEngine.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#ifndef FF_PROFILE_AAC_LOW
#define FF_PROFILE_AAC_LOW 1
#endif
}

struct WriteTask {
    std::vector<uint8_t> data;
    std::shared_ptr<AVPacket> pkt;
    uint64_t timestamp=0;
    bool isVideo=0;
    int64_t pts=0;
    bool keyframe=0;
};

class DiskWriter {
public:
    DiskWriter() : m_running(false), m_formatContext(nullptr), m_videoStream(nullptr), m_audioStream(nullptr),
        m_audioCodecCtx(nullptr), m_headerWritten(false), m_startTimestamp(0), m_lastVideoTimestamp(0),
        m_bytesWritten(0), m_framesWritten(0), m_audioPacketsWritten(0),
        m_lastVideoPts(-1), m_lastAudioPts(-1), m_nextAudioSamplePts(-1),
        m_fps(60), m_firstVideoReceived(false), m_inputSampleRate(48000), m_inputChannels(2),
        m_inputBitsPerSample(32), m_inputIsFloat(true) {
    }

    ~DiskWriter() { StopWriter(); }

    bool Initialize(const RecordingSettings& settings, const std::vector<uint8_t>& extradata = {}, AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P) {
        m_settings = settings;
        m_fps = settings.fps;
        m_outputPath = settings.outputPath;
        m_outputPath.replace_extension(".mkv");

        if (!m_outputPath.parent_path().empty()) {
            std::error_code ec;
            std::filesystem::create_directories(m_outputPath.parent_path(), ec);
            if (ec) {
                spdlog::error("Failed to create output directory: {}", ec.message());
                return false;
            }
        }

        if (avformat_alloc_output_context2(&m_formatContext, nullptr, "matroska", m_outputPath.string().c_str()) < 0) {
            spdlog::error("Failed to allocate output context");
            return false;
        }

        // 1. Video Stream Configuration
        m_videoStream = avformat_new_stream(m_formatContext, nullptr);
        if (!m_videoStream) {
            spdlog::error("Failed to create video stream");
            CleanupContext();
            return false;
        }

        m_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        m_videoStream->codecpar->codec_id = (settings.codec == Codec::H264) ? AV_CODEC_ID_H264 :
            (settings.codec == Codec::AV1) ? AV_CODEC_ID_AV1 : AV_CODEC_ID_HEVC;
        m_videoStream->codecpar->width = settings.width;
        m_videoStream->codecpar->height = settings.height;
        m_videoStream->codecpar->format = pixelFormat;
        m_videoStream->codecpar->color_primaries = AVCOL_PRI_BT709;
        m_videoStream->codecpar->color_trc = AVCOL_TRC_BT709;
        m_videoStream->codecpar->color_space = AVCOL_SPC_BT709;
        m_videoStream->codecpar->color_range = AVCOL_RANGE_MPEG;

        // FIX: Use millisecond timebase - VLC handles this well
        m_videoStream->time_base = { 1, 1000 };
        m_videoStream->avg_frame_rate = { static_cast<int>(settings.fps), 1 };
        m_videoStream->r_frame_rate = m_videoStream->avg_frame_rate;

        if (!extradata.empty()) {
            m_videoStream->codecpar->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (m_videoStream->codecpar->extradata) {
                memcpy(m_videoStream->codecpar->extradata, extradata.data(), extradata.size());
                m_videoStream->codecpar->extradata_size = (int)extradata.size();
            }
        }

        // 2. Audio Stream Configuration
        if (settings.captureAudio) {
            m_audioStream = avformat_new_stream(m_formatContext, nullptr);
            if (!m_audioStream) {
                spdlog::error("Failed to create audio stream");
                CleanupContext();
                return false;
            }

            m_inputSampleRate = 48000;
            m_inputChannels = 2;
            m_inputBitsPerSample = 32;
            m_inputIsFloat = true;
            ConfigureAudioEncoder(m_inputSampleRate, m_inputChannels, m_inputBitsPerSample, m_inputIsFloat);
        }

        av_dict_set(&m_formatContext->metadata, "creation_time", "now", 0);

        if (!(m_formatContext->oformat->flags & AVFMT_NOFILE)) {
            int ret = avio_open(&m_formatContext->pb, m_outputPath.string().c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                char errBuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errBuf, sizeof(errBuf));
                spdlog::error("Failed to open output file: {}", errBuf);
                CleanupContext();
                return false;
            }
        }

        spdlog::info("DiskWriter initialized: {}", m_outputPath.string());
        return true;
    }

    void SetAudioFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample, bool isFloat = true) {
        std::lock_guard<std::mutex> hdrLock(m_headerMutex);
        if (!m_audioStream || m_headerWritten.load()) return;
        m_inputSampleRate = sampleRate > 0 ? sampleRate : 48000;
        m_inputChannels = channels > 0 ? channels : 2;
        m_inputBitsPerSample = bitsPerSample > 0 ? bitsPerSample : 32;
        m_inputIsFloat = (m_inputBitsPerSample == 32) ? isFloat : false;
        ConfigureAudioEncoder(m_inputSampleRate, m_inputChannels, m_inputBitsPerSample, m_inputIsFloat);

        spdlog::info("Audio format set: {}Hz / {}-bit ({}) / {} channels",
            m_inputSampleRate, m_inputBitsPerSample, m_inputIsFloat ? "Float" : "PCM Int", m_inputChannels);
    }

    bool StartWriter() {
        m_running = true;
        m_lastVideoTimestamp = 0;
        m_writerThread = std::thread([this]() {
            while (true) {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_taskAvailable.wait(lock, [this] { return !m_taskQueue.empty() || !m_running; });

                // Exit only if running is false AND the queue is empty
                // This ensures all buffered video/audio frames are written to disk before stopping
                if (!m_running && m_taskQueue.empty()) {
                    break;
                }

                if (m_taskQueue.empty()) continue;

                WriteTask task = std::move(m_taskQueue.front());
                m_taskQueue.pop();
                lock.unlock();



                // Track last video timestamp
                if (task.isVideo) {
                    m_lastVideoTimestamp = task.timestamp;
                }

                if (!m_headerWritten.load()) {
                    // Start on the first video keyframe
                    if (task.isVideo && task.keyframe) {
                        std::lock_guard<std::mutex> hdrLock(m_headerMutex);
                        if (m_startTimestamp == 0 && task.timestamp > 0) {
                            m_startTimestamp = task.timestamp;
                            spdlog::info("Recording start timestamp set from first video keyframe: {}", m_startTimestamp);
                        }

                        int ret = avformat_write_header(m_formatContext, nullptr);
                        if (ret < 0) {
                            char errBuf[AV_ERROR_MAX_STRING_SIZE];
                            av_strerror(ret, errBuf, sizeof(errBuf));
                            spdlog::error("Failed to write header: {}", errBuf);
                            continue;
                        }
                        m_headerWritten.store(true);
                        m_firstVideoReceived = true;
                        spdlog::info("MKV header written, start timestamp: {}", m_startTimestamp);
                    }
                    else {
                        // Queue audio until first video keyframe arrives
                        if (!task.isVideo) {
                            m_pendingAudio.push(std::move(task));
                            if (m_pendingAudio.size() > 500) {
                                m_pendingAudio.pop();
                            }
                        }
                        continue;
                    }

                    // Write first video keyframe at PTS=0 before draining aligned audio
                    WriteVideo(task);

                    // Drain pending audio, discarding pre-keyframe audio packets that ended before m_startTimestamp
                    while (!m_pendingAudio.empty()) {
                        WriteTask audioTask = std::move(m_pendingAudio.front());
                        m_pendingAudio.pop();
                        WriteAudio(audioTask);
                    }
                    continue;
                }

                (task.isVideo) ? WriteVideo(task) : WriteAudio(task);
            }

            spdlog::info("Writer thread stopped. Frames: {}, Audio packets: {}, Bytes: {}",
                m_framesWritten.load(), m_audioPacketsWritten.load(), m_bytesWritten.load());
            });

        SetThreadPriority(m_writerThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        return true;
    }

    void StopWriter() {
        m_running = false;
        m_taskAvailable.notify_all();

        if (m_writerThread.joinable()) {
            m_writerThread.join();
        }

        if (m_formatContext) {
            if (m_headerWritten.load()) {
                FlushAudioEncoder();
                // Flush buffered packets
                av_interleaved_write_frame(m_formatContext, nullptr);

                int ret = av_write_trailer(m_formatContext);
                if (ret < 0) {
                    spdlog::warn("Failed to write trailer");
                }
            }
            if (m_formatContext->pb) {
                avio_closep(&m_formatContext->pb);
            }
            avformat_free_context(m_formatContext);
            m_formatContext = nullptr;
        }

        if (m_audioCodecCtx) {
            avcodec_free_context(&m_audioCodecCtx);
            m_audioCodecCtx = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<WriteTask> empty;
            std::swap(m_taskQueue, empty);
        }

        m_videoStream = nullptr;
        m_audioStream = nullptr;
        m_headerWritten.store(false);
        m_startTimestamp = 0;
        m_lastVideoPts = -1;
        m_lastAudioPts = -1;
        m_nextAudioSamplePts = -1;
        m_firstVideoReceived = false;
        m_audioLeftFifo.clear();
        m_audioRightFifo.clear();

        // Clear pending audio queue
        std::queue<WriteTask> emptyAudio;
        std::swap(m_pendingAudio, emptyAudio);
        
    }

    void QueueWriteTask(WriteTask&& task) {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        // No arbitrary warning limit - handle max RAM size buffer efficiently

        m_taskQueue.push(std::move(task));
        m_taskAvailable.notify_one();
    }

    void QueueAudioData(const uint8_t* data, size_t size, uint64_t timestamp) {
        if (!m_audioStream) return;
        if (!data || size == 0) return;

        WriteTask task;
        task.data.assign(data, data + size);
        task.timestamp = timestamp;
        task.isVideo = false;
        task.keyframe = true;
        QueueWriteTask(std::move(task));
    }

    uint64_t GetBytesWritten() const { return m_bytesWritten.load(); }
    uint64_t GetFramesWritten() const { return m_framesWritten.load(); }
    uint64_t GetAudioPacketsWritten() const { return m_audioPacketsWritten.load(); }

    size_t GetQueueSize() const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_taskQueue.size();
    }

    bool IsHeaderWritten() const { return m_headerWritten.load(); }

    void UpdateVideoExtradata(const std::vector<uint8_t>& extradata) {
        std::lock_guard<std::mutex> hdrLock(m_headerMutex);
        if (!m_videoStream || m_headerWritten.load()) return;

        if (m_videoStream->codecpar->extradata) {
            av_free(m_videoStream->codecpar->extradata);
        }

        m_videoStream->codecpar->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (m_videoStream->codecpar->extradata) {
            memcpy(m_videoStream->codecpar->extradata, extradata.data(), extradata.size());
            m_videoStream->codecpar->extradata_size = (int)extradata.size();
            spdlog::info("Video extradata updated ({} bytes)", extradata.size());
        }
    }

private:
    void CleanupContext() {
        if (m_audioCodecCtx) {
            avcodec_free_context(&m_audioCodecCtx);
            m_audioCodecCtx = nullptr;
        }
        if (m_formatContext) {
            if (m_formatContext->pb) {
                avio_closep(&m_formatContext->pb);
            }
            avformat_free_context(m_formatContext);
            m_formatContext = nullptr;
        }
        m_videoStream = nullptr;
        m_audioStream = nullptr;
    }

    void ConfigureAudioEncoder(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample, bool isFloat) {
        if (!m_audioStream) return;
        if (m_audioCodecCtx) {
            avcodec_free_context(&m_audioCodecCtx);
            m_audioCodecCtx = nullptr;
        }

        const AVCodec* aacCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (aacCodec) {
            m_audioCodecCtx = avcodec_alloc_context3(aacCodec);
            if (m_audioCodecCtx) {
                m_audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
                m_audioCodecCtx->sample_rate = static_cast<int>(sampleRate);
                av_channel_layout_default(&m_audioCodecCtx->ch_layout, 2);
                m_audioCodecCtx->bit_rate = 192000;
                m_audioCodecCtx->profile = FF_PROFILE_AAC_LOW;
                m_audioCodecCtx->time_base = { 1, static_cast<int>(sampleRate) };
                m_audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                if (avcodec_open2(m_audioCodecCtx, aacCodec, nullptr) >= 0) {
                    avcodec_parameters_from_context(m_audioStream->codecpar, m_audioCodecCtx);
                    m_audioStream->time_base = { 1, static_cast<int>(sampleRate) };
                    spdlog::info("Audio encoder initialized: AAC-LC 192kbps @ {}Hz stereo (extradata={} bytes)",
                        sampleRate, m_audioStream->codecpar->extradata_size);
                    return;
                }
                avcodec_free_context(&m_audioCodecCtx);
                m_audioCodecCtx = nullptr;
            }
        }

        // Fallback to format-matched uncompressed PCM if AAC encoder is unavailable
        m_audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        if (bitsPerSample == 16) {
            m_audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
            m_audioStream->codecpar->format = AV_SAMPLE_FMT_S16;
        } else if (bitsPerSample == 24) {
            m_audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_S24LE;
            m_audioStream->codecpar->format = AV_SAMPLE_FMT_S32;
        } else if (bitsPerSample == 32 && !isFloat) {
            m_audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_S32LE;
            m_audioStream->codecpar->format = AV_SAMPLE_FMT_S32;
        } else {
            m_audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
            m_audioStream->codecpar->format = AV_SAMPLE_FMT_FLT;
        }
        m_audioStream->codecpar->sample_rate = static_cast<int>(sampleRate);
        av_channel_layout_default(&m_audioStream->codecpar->ch_layout, channels);
        m_audioStream->codecpar->bits_per_coded_sample = bitsPerSample;
        m_audioStream->codecpar->block_align = channels * (bitsPerSample / 8);
        m_audioStream->time_base = { 1, static_cast<int>(sampleRate) };
    }

    void WriteVideo(WriteTask& task) {
        AVPacket* pkt = task.pkt.get();
        AVPacket* allocated_pkt = nullptr;
        
        if (!pkt) {
            // Fallback for legacy pipeline
            allocated_pkt = av_packet_alloc();
            if (!allocated_pkt) {
                spdlog::error("Failed to allocate video packet");
                return;
            }

            int ret = av_new_packet(allocated_pkt, (int)task.data.size());
            if (ret < 0) {
                av_packet_free(&allocated_pkt);
                return;
            }

            memcpy(allocated_pkt->data, task.data.data(), task.data.size());
            pkt = allocated_pkt;
        }

        pkt->stream_index = m_videoStream->index;

        // Calculate PTS in milliseconds from recording start, handling negative values safely
        int64_t elapsed_us = static_cast<int64_t>(task.timestamp) - static_cast<int64_t>(m_startTimestamp);
        int64_t pts_ms = elapsed_us / 1000;  // Convert to milliseconds

        // FIX: Ensure strictly monotonic PTS
        if (pts_ms <= m_lastVideoPts) {
            pts_ms = m_lastVideoPts + 1;
        }
        m_lastVideoPts = pts_ms;

        pkt->pts = pts_ms;
        pkt->dts = pts_ms;

        // Calculate accurate duration using av_rescale_q and stream timebase to avoid integer truncation issues
        pkt->duration = av_rescale_q(1, AVRational{1, (int)m_fps}, m_videoStream->time_base);

        if (task.keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

        int ret = av_interleaved_write_frame(m_formatContext, pkt);
        if (ret >= 0) {
            m_bytesWritten += pkt->size;
            m_framesWritten++;
        }

        if (allocated_pkt) {
            av_packet_free(&allocated_pkt);
        }
    }

    void EncodeAndWriteAudioFrame(int frameSamples) {
        if (!m_audioCodecCtx || !m_audioStream) return;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return;

        frame->nb_samples = frameSamples;
        frame->format = m_audioCodecCtx->sample_fmt;
        av_channel_layout_copy(&frame->ch_layout, &m_audioCodecCtx->ch_layout);
        frame->sample_rate = m_audioCodecCtx->sample_rate;

        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return;
        }

        memcpy(frame->data[0], m_audioLeftFifo.data(), frameSamples * sizeof(float));
        memcpy(frame->data[1], m_audioRightFifo.data(), frameSamples * sizeof(float));

        m_audioLeftFifo.erase(m_audioLeftFifo.begin(), m_audioLeftFifo.begin() + frameSamples);
        m_audioRightFifo.erase(m_audioRightFifo.begin(), m_audioRightFifo.begin() + frameSamples);

        frame->pts = m_nextAudioSamplePts;
        m_nextAudioSamplePts += frameSamples;

        if (avcodec_send_frame(m_audioCodecCtx, frame) >= 0) {
            ReceiveAndWriteAudioPackets();
        }
        av_frame_free(&frame);
    }

    void ReceiveAndWriteAudioPackets() {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return;

        while (avcodec_receive_packet(m_audioCodecCtx, pkt) >= 0) {
            pkt->stream_index = m_audioStream->index;
            av_packet_rescale_ts(pkt, m_audioCodecCtx->time_base, m_audioStream->time_base);

            if (pkt->pts <= m_lastAudioPts) {
                pkt->pts = m_lastAudioPts + 1;
            }
            pkt->dts = pkt->pts;
            m_lastAudioPts = pkt->pts;

            int ret = av_interleaved_write_frame(m_formatContext, pkt);
            if (ret >= 0) {
                m_bytesWritten += pkt->size;
                m_audioPacketsWritten++;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    void FlushAudioEncoder() {
        if (!m_audioCodecCtx || !m_audioStream) return;

        if (!m_audioLeftFifo.empty()) {
            int remaining = static_cast<int>(m_audioLeftFifo.size());
            int frameSize = m_audioCodecCtx->frame_size > 0 ? m_audioCodecCtx->frame_size : remaining;
            if (remaining < frameSize) {
                m_audioLeftFifo.resize(frameSize, 0.0f);
                m_audioRightFifo.resize(frameSize, 0.0f);
            }
            if (m_nextAudioSamplePts < 0) m_nextAudioSamplePts = 0;
            EncodeAndWriteAudioFrame(frameSize);
        }

        if (avcodec_send_frame(m_audioCodecCtx, nullptr) >= 0) {
            ReceiveAndWriteAudioPackets();
        }
    }

    void WriteAudio(WriteTask& task) {
        if (!m_audioStream || task.data.empty()) return;

        int channels = m_inputChannels > 0 ? m_inputChannels : 2;
        int bytesPerSample = (m_inputBitsPerSample > 0) ? (m_inputBitsPerSample / 8) : 4;
        int inputFrameSize = channels * bytesPerSample;
        if (inputFrameSize <= 0) return;

        int totalSamples = static_cast<int>(task.data.size()) / inputFrameSize;
        if (totalSamples <= 0) return;

        int64_t packetDurationUs = (static_cast<int64_t>(totalSamples) * 1000000LL) / m_inputSampleRate;
        int64_t elapsed_us = static_cast<int64_t>(task.timestamp) - static_cast<int64_t>(m_startTimestamp);

        // Discard pre-roll audio packets that finished before the first video keyframe
        if (elapsed_us + packetDurationUs <= 0) {
            return;
        }

        int startSampleOffset = 0;
        if (elapsed_us < 0) {
            startSampleOffset = static_cast<int>(((-elapsed_us) * m_inputSampleRate) / 1000000LL);
            if (startSampleOffset >= totalSamples) return;
            elapsed_us = 0;
        }

        int validSamples = totalSamples - startSampleOffset;
        const uint8_t* rawPtr = task.data.data() + static_cast<size_t>(startSampleOffset) * inputFrameSize;

        if (m_audioCodecCtx) {
            int64_t targetSamplePts = av_rescale_q(elapsed_us, { 1, 1000000 }, m_audioCodecCtx->time_base);
            if (m_nextAudioSamplePts < 0) {
                m_nextAudioSamplePts = std::max<int64_t>(0, targetSamplePts);
            } else {
                int64_t bufferedSamples = static_cast<int64_t>(m_audioLeftFifo.size());
                int64_t expectedPts = m_nextAudioSamplePts + bufferedSamples;
                // Only resync if clock drift exceeds 100ms
                if (std::abs(targetSamplePts - expectedPts) > static_cast<int64_t>(m_inputSampleRate / 10)) {
                    m_nextAudioSamplePts = std::max<int64_t>(m_nextAudioSamplePts, targetSamplePts - bufferedSamples);
                }
            }

            size_t oldSize = m_audioLeftFifo.size();
            m_audioLeftFifo.resize(oldSize + validSamples);
            m_audioRightFifo.resize(oldSize + validSamples);
            float* dstL = m_audioLeftFifo.data() + oldSize;
            float* dstR = m_audioRightFifo.data() + oldSize;

            if (m_inputBitsPerSample == 32 && m_inputIsFloat) {
                const float* src = reinterpret_cast<const float*>(rawPtr);
                for (int i = 0; i < validSamples; ++i) {
                    dstL[i] = std::clamp(src[i * channels + 0], -1.0f, 1.0f);
                    dstR[i] = std::clamp(src[i * channels + (channels > 1 ? 1 : 0)], -1.0f, 1.0f);
                }
            } else if (m_inputBitsPerSample == 16) {
                const int16_t* src = reinterpret_cast<const int16_t*>(rawPtr);
                for (int i = 0; i < validSamples; ++i) {
                    dstL[i] = src[i * channels + 0] * (1.0f / 32768.0f);
                    dstR[i] = src[i * channels + (channels > 1 ? 1 : 0)] * (1.0f / 32768.0f);
                }
            } else if (m_inputBitsPerSample == 24) {
                for (int i = 0; i < validSamples; ++i) {
                    const uint8_t* pL = rawPtr + (i * channels + 0) * 3;
                    int32_t vL = (static_cast<int32_t>(static_cast<int8_t>(pL[2])) << 16) | (static_cast<int32_t>(pL[1]) << 8) | pL[0];
                    const uint8_t* pR = rawPtr + (i * channels + (channels > 1 ? 1 : 0)) * 3;
                    int32_t vR = (static_cast<int32_t>(static_cast<int8_t>(pR[2])) << 16) | (static_cast<int32_t>(pR[1]) << 8) | pR[0];
                    dstL[i] = vL * (1.0f / 8388608.0f);
                    dstR[i] = vR * (1.0f / 8388608.0f);
                }
            } else {
                const int32_t* src = reinterpret_cast<const int32_t*>(rawPtr);
                for (int i = 0; i < validSamples; ++i) {
                    dstL[i] = static_cast<float>(src[i * channels + 0] * (1.0 / 2147483648.0));
                    dstR[i] = static_cast<float>(src[i * channels + (channels > 1 ? 1 : 0)] * (1.0 / 2147483648.0));
                }
            }

            int frameSamples = m_audioCodecCtx->frame_size > 0 ? m_audioCodecCtx->frame_size : 1024;
            while (static_cast<int>(m_audioLeftFifo.size()) >= frameSamples) {
                EncodeAndWriteAudioFrame(frameSamples);
            }
            return;
        }

        // Uncompressed PCM fallback path with sample-accurate PTS
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return;

        size_t payloadBytes = static_cast<size_t>(validSamples) * inputFrameSize;
        if (av_new_packet(pkt, static_cast<int>(payloadBytes)) < 0) {
            av_packet_free(&pkt);
            return;
        }

        memcpy(pkt->data, rawPtr, payloadBytes);
        pkt->stream_index = m_audioStream->index;

        if (m_nextAudioSamplePts < 0) {
            m_nextAudioSamplePts = std::max<int64_t>(0, av_rescale_q(elapsed_us, { 1, 1000000 }, { 1, static_cast<int>(m_inputSampleRate) }));
        }

        int64_t pts_stream = av_rescale_q(m_nextAudioSamplePts, { 1, static_cast<int>(m_inputSampleRate) }, m_audioStream->time_base);
        m_nextAudioSamplePts += validSamples;

        if (pts_stream <= m_lastAudioPts) {
            pts_stream = m_lastAudioPts + 1;
        }
        m_lastAudioPts = pts_stream;

        pkt->pts = pts_stream;
        pkt->dts = pts_stream;
        pkt->duration = av_rescale_q(validSamples, AVRational{ 1, static_cast<int>(m_inputSampleRate) }, m_audioStream->time_base);

        int ret = av_interleaved_write_frame(m_formatContext, pkt);
        if (ret >= 0) {
            m_bytesWritten += pkt->size;
            m_audioPacketsWritten++;
        }

        av_packet_free(&pkt);
    }

    AVFormatContext* m_formatContext;
    AVStream* m_videoStream, * m_audioStream;
    AVCodecContext* m_audioCodecCtx;
    RecordingSettings m_settings;
    std::filesystem::path m_outputPath;
    std::atomic<bool> m_running;
    std::thread m_writerThread;
    std::queue<WriteTask> m_taskQueue;
    mutable std::mutex m_queueMutex;
    mutable std::mutex m_headerMutex;
    std::condition_variable m_taskAvailable;
    std::atomic<bool> m_headerWritten;

    uint64_t m_startTimestamp;
    uint64_t m_lastVideoTimestamp;  // Track last video for A/V sync on stop
    int64_t m_lastVideoPts;
    int64_t m_lastAudioPts;
    int64_t m_nextAudioSamplePts;
    uint32_t m_fps;
    bool m_firstVideoReceived;
    uint32_t m_inputSampleRate;
    uint16_t m_inputChannels;
    uint16_t m_inputBitsPerSample;
    bool m_inputIsFloat;
    std::vector<float> m_audioLeftFifo;
    std::vector<float> m_audioRightFifo;
    std::queue<WriteTask> m_pendingAudio;  // Buffer audio until first video keyframe

    std::atomic<uint64_t> m_bytesWritten;
    std::atomic<uint64_t> m_framesWritten;
    std::atomic<uint64_t> m_audioPacketsWritten;
};
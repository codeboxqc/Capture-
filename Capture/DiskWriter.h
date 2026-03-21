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
}

struct WriteTask {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    bool isVideo;
    int64_t pts;
    bool keyframe;
};

class DiskWriter {
public:
    DiskWriter() : m_running(false), m_formatContext(nullptr), m_videoStream(nullptr), m_audioStream(nullptr),
        m_headerWritten(false), m_startTimestamp(0), m_bytesWritten(0), m_framesWritten(0), m_audioPacketsWritten(0) {
    }

    ~DiskWriter() { StopWriter(); }

    bool Initialize(const RecordingSettings& settings, const std::vector<uint8_t>& extradata = {}, AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P) {
        m_settings = settings;
        m_outputPath = settings.outputPath;
        m_outputPath.replace_extension(".mkv");
        
        // FIX: Check if parent path exists before creating
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

        // Match encoder pixel format (important for 4:4:4)
        m_videoStream->codecpar->format = pixelFormat;

        // Use high-precision timebase for MKV
        m_videoStream->time_base = { 1, 1000000 };

        if (!extradata.empty()) {
            // FIX: Check allocation success
            m_videoStream->codecpar->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (m_videoStream->codecpar->extradata) {
                memcpy(m_videoStream->codecpar->extradata, extradata.data(), extradata.size());
                m_videoStream->codecpar->extradata_size = (int)extradata.size();
            } else {
                spdlog::warn("Failed to allocate extradata buffer");
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
            
            m_audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            m_audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
            m_audioStream->codecpar->format = AV_SAMPLE_FMT_FLT;
            m_audioStream->codecpar->sample_rate = 48000;
            m_audioStream->codecpar->ch_layout.nb_channels = 2;
            av_channel_layout_default(&m_audioStream->codecpar->ch_layout, 2);
            m_audioStream->codecpar->bits_per_coded_sample = 32;
            m_audioStream->codecpar->block_align = 8;
            m_audioStream->time_base = { 1, 48000 };
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

    void SetAudioFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
        if (!m_audioStream || m_headerWritten) return;
        m_audioStream->codecpar->sample_rate = sampleRate;
        m_audioStream->codecpar->ch_layout.nb_channels = channels;
        av_channel_layout_default(&m_audioStream->codecpar->ch_layout, channels);
        m_audioStream->codecpar->bits_per_coded_sample = bitsPerSample;
        m_audioStream->codecpar->block_align = channels * (bitsPerSample / 8);
        m_audioStream->time_base = { 1, (int)sampleRate };
        
        spdlog::info("Audio format set: {}Hz / {}-bit / {} channels", sampleRate, bitsPerSample, channels);
    }

    bool StartWriter() {
        m_running = true;
        m_writerThread = std::thread([this]() {
            while (m_running || !m_taskQueue.empty()) {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_taskAvailable.wait(lock, [this] { return !m_taskQueue.empty() || !m_running; });
                if (m_taskQueue.empty()) break;
                WriteTask task = std::move(m_taskQueue.front());
                m_taskQueue.pop();
                lock.unlock();

                if (!m_headerWritten) {
                    // We MUST start with a video keyframe to avoid scramble/grey frames in VLC.
                    // Audio can wait for the first video keyframe.
                    if (task.isVideo && task.keyframe) {
                        // Anchoring start to EXACT timestamp of the first keyframe
                        m_startTimestamp = task.timestamp;
                        int ret = avformat_write_header(m_formatContext, nullptr);
                        if (ret < 0) {
                            char errBuf[AV_ERROR_MAX_STRING_SIZE];
                            av_strerror(ret, errBuf, sizeof(errBuf));
                            spdlog::error("Failed to write header: {}", errBuf);
                            continue;
                        }
                        m_headerWritten = true;
                        spdlog::info("MKV header written anchored to video keyframe: {}", m_startTimestamp);
                    } else {
                        // Drop everything until the first video keyframe arrives.
                        // This ensures the video is immediately seekable and has no scrambled start.
                        continue;
                    }
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
            if (m_headerWritten) {
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
        
        // FIX: Clear queue to free memory
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<WriteTask> empty;
            std::swap(m_taskQueue, empty);
        }
        
        // FIX: Reset state for potential reuse
        m_videoStream = nullptr;
        m_audioStream = nullptr;
        m_headerWritten = false;
        m_startTimestamp = 0;
    }

    void QueueWriteTask(WriteTask&& task) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        
        // FIX: Warn if queue is getting too large (potential bottleneck)
        if (m_taskQueue.size() > 500) {
            static uint64_t lastWarnTime = 0;
            uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - lastWarnTime > 5) {  // Warn at most every 5 seconds
                spdlog::warn("Write queue backing up: {} tasks pending", m_taskQueue.size());
                lastWarnTime = now;
            }
        }
        
        m_taskQueue.push(std::move(task));
        m_taskAvailable.notify_one();
    }

    void QueueAudioData(const uint8_t* data, size_t size, uint64_t timestamp) {
        if (!m_audioStream) return;
        if (!data || size == 0) return;  // FIX: Validate input
        
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
    
    bool IsHeaderWritten() const { return m_headerWritten; }

private:
    void CleanupContext() {
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

    void WriteVideo(WriteTask& task) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            spdlog::error("Failed to allocate video packet");
            return;
        }
        
        int ret = av_new_packet(pkt, (int)task.data.size());
        if (ret < 0) {
            spdlog::error("Failed to allocate packet data");
            av_packet_free(&pkt);
            return;
        }
        
        memcpy(pkt->data, task.data.data(), task.data.size());

        pkt->stream_index = m_videoStream->index;

        // Use encoder's PTS/DTS directly if available, relative to recording start
        int64_t pts_us = (int64_t)(task.timestamp - m_startTimestamp);
        pkt->pts = av_rescale_q(pts_us, { 1, 1000000 }, m_videoStream->time_base);
        pkt->dts = pkt->pts; // For near-lossless / low-latency capture, DTS usually matches PTS

        if (task.keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

        ret = av_interleaved_write_frame(m_formatContext, pkt);
        if (ret < 0) {
            // FIX: Don't spam logs, just count errors
            static std::atomic<uint64_t> videoWriteErrors{0};
            videoWriteErrors++;
            if (videoWriteErrors % 100 == 1) {
                spdlog::warn("Video write errors: {}", videoWriteErrors.load());
            }
        } else {
            m_bytesWritten += pkt->size;
            m_framesWritten++;
        }
        
        av_packet_free(&pkt);
    }

    void WriteAudio(WriteTask& task) {
        if (task.timestamp < m_startTimestamp) return;
        if (!m_audioStream) return;  // FIX: Extra safety check

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            spdlog::error("Failed to allocate audio packet");
            return;
        }
        
        int ret = av_new_packet(pkt, (int)task.data.size());
        if (ret < 0) {
            spdlog::error("Failed to allocate packet data");
            av_packet_free(&pkt);
            return;
        }
        
        memcpy(pkt->data, task.data.data(), task.data.size());

        pkt->stream_index = m_audioStream->index;
        int64_t pts_us = (int64_t)(task.timestamp - m_startTimestamp);
        pkt->pts = pkt->dts = av_rescale_q(pts_us, { 1, 1000000 }, m_audioStream->time_base);

        int channels = m_audioStream->codecpar->ch_layout.nb_channels;
        int bitsPerSample = m_audioStream->codecpar->bits_per_coded_sample;
        int bytesPerSample = (bitsPerSample > 0) ? (bitsPerSample / 8) : 4;
        
        // FIX: Prevent division by zero
        int frameSize = channels * bytesPerSample;
        if (frameSize > 0) {
            pkt->duration = (int)task.data.size() / frameSize;
        }

        ret = av_interleaved_write_frame(m_formatContext, pkt);
        if (ret < 0) {
            // FIX: Don't spam logs
            static std::atomic<uint64_t> audioWriteErrors{0};
            audioWriteErrors++;
            if (audioWriteErrors % 100 == 1) {
                spdlog::warn("Audio write errors: {}", audioWriteErrors.load());
            }
        } else {
            m_bytesWritten += pkt->size;
            m_audioPacketsWritten++;
        }
        
        av_packet_free(&pkt);
    }

    AVFormatContext* m_formatContext;
    AVStream* m_videoStream, * m_audioStream;
    RecordingSettings m_settings;
    std::filesystem::path m_outputPath;
    std::atomic<bool> m_running;
    std::thread m_writerThread;
    std::queue<WriteTask> m_taskQueue;
    mutable std::mutex m_queueMutex;  // FIX: mutable for const methods
    std::condition_variable m_taskAvailable;
    bool m_headerWritten;
    uint64_t m_startTimestamp;
    std::atomic<uint64_t> m_bytesWritten;
    std::atomic<uint64_t> m_framesWritten;      // FIX: Added stats
    std::atomic<uint64_t> m_audioPacketsWritten; // FIX: Added stats
};

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
        m_headerWritten(false), m_firstKeyframeReceived(false), m_startTimestamp(0),
        m_bytesWritten(0), m_ioBuffer(nullptr) {
    }

    ~DiskWriter() { StopWriter(); }

    bool Initialize(const RecordingSettings& settings) {
        m_settings = settings;
        m_outputPath = settings.outputPath;
        m_outputPath.replace_extension(".mkv");
        std::filesystem::create_directories(m_outputPath.parent_path());

        // Use matroska for reliability and high quality
        if (avformat_alloc_output_context2(&m_formatContext, nullptr, "matroska", m_outputPath.string().c_str()) < 0)
            return false;

        // 1. Video Stream Configuration
        m_videoStream = avformat_new_stream(m_formatContext, nullptr);
        m_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        m_videoStream->codecpar->codec_id = (settings.codec == Codec::H264) ? AV_CODEC_ID_H264 :
            (settings.codec == Codec::AV1) ? AV_CODEC_ID_AV1 : AV_CODEC_ID_HEVC;
        m_videoStream->codecpar->width = settings.width;
        m_videoStream->codecpar->height = settings.height;
        m_videoStream->codecpar->format = AV_PIX_FMT_YUV420P;
        m_videoStream->time_base = { 1, 1000 }; // 1ms precision for MKV

        // 2. Audio Stream Configuration (32-bit Float PCM)
        if (settings.captureAudio) {
            m_audioStream = avformat_new_stream(m_formatContext, nullptr);
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

        // Matroska specific options for high performance
        av_dict_set(&m_formatContext->metadata, "creation_time", "now", 0);

        if (!(m_formatContext->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&m_formatContext->pb, m_outputPath.string().c_str(), AVIO_FLAG_WRITE) < 0) return false;

            // Optimization: Increase internal IO buffer for high-bitrate recording
            // We must do this AFTER avio_open which creates the default pb
            size_t ioBufferSize = 2 * 1024 * 1024; // 2MB buffer
            m_ioBuffer = (uint8_t*)av_malloc(ioBufferSize);
            if (m_ioBuffer) {
                // Replace the default buffer with our larger one
                // Note: FFmpeg will take ownership of m_ioBuffer if we don't set it up carefully,
                // but here we just want to expand the existing one or use avio_alloc_context.
                // Actually, the simplest way in modern FFmpeg to increase buffer for a file is:
                m_formatContext->pb->buffer_size = (int)ioBufferSize;
                uint8_t* oldBuffer = m_formatContext->pb->buffer;
                m_formatContext->pb->buffer = m_ioBuffer;
                m_formatContext->pb->buf_ptr = m_ioBuffer;
                m_formatContext->pb->buf_end = m_ioBuffer + ioBufferSize;
                av_free(oldBuffer);
                m_ioBuffer = nullptr; // FFmpeg now owns it
            }
        }

        return true;
    }

    void SetAudioFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
        if (!m_audioStream || m_headerWritten) return;
        m_audioStream->codecpar->sample_rate = sampleRate;
        m_audioStream->codecpar->ch_layout.nb_channels = channels;
        av_channel_layout_default(&m_audioStream->codecpar->ch_layout, channels);
        m_audioStream->codecpar->block_align = channels * (bitsPerSample / 8);
        m_audioStream->time_base = { 1, (int)sampleRate };
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
                (task.isVideo) ? WriteVideo(task) : WriteAudio(task);
            }
        });

        // Increase thread priority for the writer to handle high-bitrate bursts
        SetThreadPriority(m_writerThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);

        return true;
    }

    void StopWriter() {
        m_running = false;
        m_taskAvailable.notify_all();
        if (m_writerThread.joinable()) m_writerThread.join();
        if (m_formatContext) {
            if (m_headerWritten) av_write_trailer(m_formatContext);
            if (m_formatContext->pb) avio_closep(&m_formatContext->pb);
            avformat_free_context(m_formatContext);
            m_formatContext = nullptr;
        }
        if (m_ioBuffer) {
            av_free(m_ioBuffer);
            m_ioBuffer = nullptr;
        }
    }

    void QueueWriteTask(WriteTask&& task) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_taskQueue.push(std::move(task));
        m_taskAvailable.notify_one();
    }

    void QueueAudioData(const uint8_t* data, size_t size, uint64_t timestamp) {
        if (!m_audioStream) return;
        WriteTask task;
        task.data.assign(data, data + size);
        task.timestamp = timestamp;
        task.isVideo = false;
        task.keyframe = true;
        QueueWriteTask(std::move(task));
    }

    uint64_t GetBytesWritten() const { return m_bytesWritten.load(); }

private:
    void WriteVideo(WriteTask& task) {
        if (!m_firstKeyframeReceived) {
            if (!task.keyframe) return;
            m_firstKeyframeReceived = true;
            m_startTimestamp = task.timestamp;
            if (!task.data.empty()) {
                m_videoStream->codecpar->extradata = (uint8_t*)av_mallocz(task.data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                memcpy(m_videoStream->codecpar->extradata, task.data.data(), task.data.size());
                m_videoStream->codecpar->extradata_size = (int)task.data.size();
            }
            avformat_write_header(m_formatContext, nullptr);
            m_headerWritten = true;
        }

        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, (int)task.data.size());
        memcpy(pkt->data, task.data.data(), task.data.size());

        pkt->stream_index = m_videoStream->index;
        int64_t pts_us = (int64_t)(task.timestamp - m_startTimestamp);
        pkt->pts = pkt->dts = av_rescale_q(pts_us, { 1, 1000000 }, m_videoStream->time_base);

        if (task.keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

        av_interleaved_write_frame(m_formatContext, pkt);
        m_bytesWritten += pkt->size;
        av_packet_free(&pkt);
    }

    void WriteAudio(WriteTask& task) {
        if (!m_headerWritten || task.timestamp < m_startTimestamp) return;

        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, (int)task.data.size());
        memcpy(pkt->data, task.data.data(), task.data.size());

        pkt->stream_index = m_audioStream->index;
        int64_t pts_us = (int64_t)(task.timestamp - m_startTimestamp);
        pkt->pts = pkt->dts = av_rescale_q(pts_us, { 1, 1000000 }, m_audioStream->time_base);

        int channels = m_audioStream->codecpar->ch_layout.nb_channels;
        int bytesPerSample = m_audioStream->codecpar->bits_per_coded_sample / 8;
        pkt->duration = (int)task.data.size() / (channels * bytesPerSample);

        av_interleaved_write_frame(m_formatContext, pkt);
        m_bytesWritten += pkt->size;
        av_packet_free(&pkt);
    }

    AVFormatContext* m_formatContext;
    AVStream* m_videoStream, * m_audioStream;
    RecordingSettings m_settings;
    std::filesystem::path m_outputPath;
    std::atomic<bool> m_running;
    std::thread m_writerThread;
    std::queue<WriteTask> m_taskQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_taskAvailable;
    bool m_headerWritten, m_firstKeyframeReceived;
    uint64_t m_startTimestamp;
    std::atomic<uint64_t> m_bytesWritten;
    uint8_t* m_ioBuffer;
};
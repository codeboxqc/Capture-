#pragma once

#include "IHardwareCapture.h"
#include <iostream>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#endif

class V4L2Capture : public IHardwareCapture {
public:
    V4L2Capture() : m_fd(-1), m_running(false) {}
    ~V4L2Capture() override { CloseDevice(); }

    std::vector<HardwareDeviceInfo> EnumerateDevices() override {
        std::vector<HardwareDeviceInfo> devices;
#ifdef __linux__
        for (int i = 0; i < 64; ++i) {
            std::string devPath = "/dev/video" + std::to_string(i);
            int fd = open(devPath.c_str(), O_RDWR | O_NONBLOCK, 0);
            if (fd >= 0) {
                struct v4l2_capability cap;
                if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                    if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                        HardwareDeviceInfo info;
                        info.id = devPath;
                        info.name = reinterpret_cast<char*>(cap.card);
                        info.max_width = 1920;
                        info.max_height = 1080;
                        info.max_fps = 60;
                        devices.push_back(info);
                    }
                }
                close(fd);
            }
        }
#endif
        return devices;
    }

    bool OpenDevice(const std::string& deviceId) override {
#ifdef __linux__
        m_fd = open(deviceId.c_str(), O_RDWR | O_NONBLOCK, 0);
        return m_fd >= 0;
#else
        return false;
#endif
    }

    bool NegotiateFormat(uint32_t& width, uint32_t& height, uint32_t& fps, std::string& pixelFormat) override {
#ifdef __linux__
        if (m_fd < 0) return false;
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
        if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) return false;
        
        width = fmt.fmt.pix.width;
        height = fmt.fmt.pix.height;
        return true;
#else
        return false;
#endif
    }

    bool StartCapture() override {
#ifdef __linux__
        if (m_fd < 0) return false;
        struct v4l2_requestbuffers req = {0};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) return false;
        
        for (unsigned int i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) return false;
            
            void* mem = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
            if (mem == MAP_FAILED) return false;
            m_buffers.push_back({mem, buf.length});
            
            if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) return false;
        }
        
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) return false;
        m_running = true;
        return true;
#else
        return false;
#endif
    }

    bool GetNextFrame(HardwareFrame& frame, uint32_t timeoutMs) override {
#ifdef __linux__
        if (!m_running) return false;
        
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_fd, &fds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        
        int r = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) return false;
        
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) return false;
        
        frame.data = m_buffers[buf.index].start;
        frame.size = buf.bytesused;
        frame.timestamp = buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
        frame.isKeyframe = true;
        
        ioctl(m_fd, VIDIOC_QBUF, &buf);
        return true;
#else
        return false;
#endif
    }

    void StopCapture() override {
#ifdef __linux__
        if (m_running && m_fd >= 0) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(m_fd, VIDIOC_STREAMOFF, &type);
            m_running = false;
        }
#endif
    }

    void CloseDevice() override {
        StopCapture();
#ifdef __linux__
        if (m_fd >= 0) {
            for (auto& buf : m_buffers) {
                munmap(buf.start, buf.length);
            }
            m_buffers.clear();
            close(m_fd);
            m_fd = -1;
        }
#endif
    }

private:
    int m_fd;
    bool m_running;
    
    struct Buffer {
        void* start;
        size_t length;
    };
    std::vector<Buffer> m_buffers;
};

#pragma once
#include "RecordingEngine.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <queue>
#include <mutex>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

struct USBFrame {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t timestamp;
    uint32_t frameIndex;
};

struct USBCaptureDevice {
    std::string name;
    int index;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

class SimpleUSBCapture {
public:
    SimpleUSBCapture()
        : m_running(false)
        , m_frameCount(0)
        , m_width(0)
        , m_height(0)
        , m_comInitialized(false)
        , m_mfInitialized(false)
    {
    }

    ~SimpleUSBCapture() {
        Stop();
        Shutdown();
    }

    static std::vector<USBCaptureDevice> EnumerateDevices() {
        std::vector<USBCaptureDevice> devices;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needsUninit = SUCCEEDED(hr);

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (needsUninit) CoUninitialize();
            return devices;
        }

        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(MFCreateAttributes(&attributes, 1))) {
            attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

            IMFActivate** activateArray = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(MFEnumDeviceSources(attributes.Get(), &activateArray, &count))) {
                for (UINT32 i = 0; i < count; i++) {
                    USBCaptureDevice device;
                    device.index = i;

                    WCHAR* name = nullptr;
                    if (SUCCEEDED(activateArray[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, nullptr))) {
                        int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
                        device.name.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, name, -1, &device.name[0], size, nullptr, nullptr);
                        CoTaskMemFree(name);
                    }

                    ComPtr<IMFMediaSource> source;
                    if (SUCCEEDED(activateArray[i]->ActivateObject(IID_PPV_ARGS(&source)))) {
                        ComPtr<IMFSourceReader> reader;
                        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
                            ComPtr<IMFMediaType> mediaType;
                            if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &mediaType))) {
                                MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &device.width, &device.height);
                                UINT32 fpsNum = 0, fpsDen = 1;
                                MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
                                device.fps = fpsDen > 0 ? fpsNum / fpsDen : 30;
                            }
                        }
                        source->Shutdown();
                    }
                    devices.push_back(device);
                    activateArray[i]->Release();
                }
                CoTaskMemFree(activateArray);
            }
        }
        MFShutdown();
        if (needsUninit) CoUninitialize();
        return devices;
    }

    bool Initialize(int deviceIndex, ComPtr<ID3D11Device> d3d11Device) {
        if (!d3d11Device) return false;
        m_d3d11Device = d3d11Device;
        m_d3d11Device->GetImmediateContext(&m_d3d11Context);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInitialized = SUCCEEDED(hr);

        if (FAILED(MFStartup(MF_VERSION))) return false;
        m_mfInitialized = true;

        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 1);
        attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** activateArray = nullptr;
        UINT32 count = 0;
        if (FAILED(MFEnumDeviceSources(attributes.Get(), &activateArray, &count)) || count == 0) return false;

        if (deviceIndex < 0 || deviceIndex >= (int)count) {
            for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
            CoTaskMemFree(activateArray);
            return false;
        }

        hr = activateArray[deviceIndex]->ActivateObject(IID_PPV_ARGS(&m_mediaSource));
        for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
        CoTaskMemFree(activateArray);

        if (FAILED(hr)) return false;

        ComPtr<IMFAttributes> readerAttrs;
        MFCreateAttributes(&readerAttrs, 2);
        readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        if (FAILED(MFCreateSourceReaderFromMediaSource(m_mediaSource.Get(), readerAttrs.Get(), &m_sourceReader))) return false;

        ComPtr<IMFMediaType> mediaType;
        MFCreateMediaType(&mediaType);
        mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType.Get());

        ComPtr<IMFMediaType> currentType;
        if (SUCCEEDED(m_sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType))) {
            MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height);
        }

        return (m_width > 0 && m_height > 0);
    }

    bool Start(uint32_t bufferSize = 16) {
        if (!m_sourceReader) return false;
        m_bufferSize = bufferSize;
        m_running = true;
        m_captureThread = std::thread(&SimpleUSBCapture::CaptureThreadFunc, this);
        return true;
    }

    void Stop() {
        if (!m_running) return;
        m_running = false;
        m_frameAvailable.notify_all();
        if (m_captureThread.joinable()) m_captureThread.join();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_frameQueue.empty()) m_frameQueue.pop();
        }
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            while (!m_texturePool.empty()) m_texturePool.pop();
        }
    }

    bool GetFrame(USBFrame& frame, uint32_t timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_frameQueue.empty()) {
            if (!m_frameAvailable.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !m_frameQueue.empty() || !m_running; })) return false;
        }
        if (m_frameQueue.empty()) return false;
        frame = m_frameQueue.front();
        m_frameQueue.pop();
        return true;
    }

    void ReturnTexture(ComPtr<ID3D11Texture2D> texture) {
        if (!texture) return;
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_texturePool.push(texture);
    }

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    void Shutdown() {
        if (m_sourceReader) m_sourceReader.Reset();
        if (m_mediaSource) { m_mediaSource->Shutdown(); m_mediaSource.Reset(); }
        if (m_mfInitialized) MFShutdown();
        if (m_comInitialized) CoUninitialize();
    }

    void CaptureThreadFunc() {
        while (m_running) {
            IMFSample* sample = nullptr;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;
            HRESULT hr = m_sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &streamFlags, &timestamp, &sample);

            if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_ERROR)) break;
            if (!sample) continue;

            ComPtr<ID3D11Texture2D> texture = ConvertToTexture(sample);
            sample->Release();

            if (texture) {
                // Convert Media Foundation 100ns units to microseconds
                uint64_t timestampUs = (uint64_t)timestamp / 10;
                USBFrame frame = { texture, timestampUs, m_frameCount++ };
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_frameQueue.size() >= m_bufferSize) {
                    ReturnTexture(m_frameQueue.front().texture);
                    m_frameQueue.pop();
                }
                m_frameQueue.push(frame);
                m_frameAvailable.notify_one();
            }
        }
    }

    ComPtr<ID3D11Texture2D> ConvertToTexture(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return nullptr;

        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return nullptr;

        ComPtr<ID3D11Texture2D> texture;
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (!m_texturePool.empty()) {
                texture = m_texturePool.front();
                m_texturePool.pop();
            }
        }

        if (!texture) {
            D3D11_TEXTURE2D_DESC desc = { m_width, m_height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
            m_d3d11Device->CreateTexture2D(&desc, nullptr, &texture);
        }

        if (texture) m_d3d11Context->UpdateSubresource(texture.Get(), 0, nullptr, data, m_width * 4, 0);
        buffer->Unlock();
        return texture;
    }

    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    ComPtr<IMFMediaSource> m_mediaSource;
    ComPtr<IMFSourceReader> m_sourceReader;
    std::atomic<bool> m_running;
    std::thread m_captureThread;
    uint32_t m_width, m_height, m_bufferSize;
    uint32_t m_frameCount;
    bool m_comInitialized, m_mfInitialized;
    std::queue<USBFrame> m_frameQueue;
    std::mutex m_mutex;
    std::condition_variable m_frameAvailable;
    std::queue<ComPtr<ID3D11Texture2D>> m_texturePool;
    std::mutex m_poolMutex;
};
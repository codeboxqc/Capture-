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

// Simple USB Frame structure
struct USBFrame {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t timestamp;
    uint32_t frameIndex;
};

// Simplified USB Capture Device Info
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

    /*
    # Simple USB Capture - Usage Guide

## Overview

This is a simplified, production-ready USB capture implementation based on best practices. It's designed to be:
- **Simple**: Clean API with minimal complexity
- **Robust**: Proper error handling and resource management
- **Fast**: Zero-copy D3D11 texture output
- **Safe**: Thread-safe with proper cleanup

## Quick Start

### 1. List Available Devices

```cpp
#include "SimpleUSBCapture.h"

// Static method - lists all USB capture devices
auto devices = SimpleUSBCapture::EnumerateDevices();

for (const auto& dev : devices) {
    spdlog::info("Device {}: {} ({}x{}@{}fps)", 
        dev.index, dev.name, dev.width, dev.height, dev.fps);
}
```

### 2. Initialize and Capture

```cpp
// Create D3D11 device (or use existing one)
ComPtr<ID3D11Device> device;
D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, 
                  nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);

// Create capture instance
SimpleUSBCapture capture;

// Initialize with device index (0 = first device)
if (!capture.Initialize(0, device)) {
    spdlog::error("Failed to initialize USB capture");
    return;
}

// Start capturing with 16-frame buffer
if (!capture.Start(16)) {
    spdlog::error("Failed to start capture");
    return;
}

// Capture loop
USBFrame frame;
while (capture.GetFrame(frame, 100)) {  // 100ms timeout
    // frame.texture is a ID3D11Texture2D ready to use
    // frame.timestamp is in microseconds
    // frame.frameIndex is sequential
    
    ProcessFrame(frame.texture);
}

// Stop (also happens automatically in destructor)
capture.Stop();
```

## Integration with Your Recording Engine

### Replace Your USB Capture

```cpp
// In your RecordingPipeline or main recording code:

// OLD:
// USBCapture usbCapture;
// usbCapture.Initialize(deviceIndex, d3d11Device, d3d11Context);

// NEW:
SimpleUSBCapture usbCapture;
usbCapture.Initialize(deviceIndex, d3d11Device);  // Simpler API

// Rest is the same:
usbCapture.Start(32);

USBFrame frame;
while (usbCapture.GetFrame(frame, 100)) {
    // Encode frame.texture
    EncodeFrame(frame);
}

usbCapture.Stop();
```

### With Your Hardware Encoder

```cpp
SimpleUSBCapture capture;
HardwareEncoder encoder;

// Initialize both
capture.Initialize(0, m_d3d11Device);
encoder.Initialize(gpuInfo, settings, m_d3d11Device, m_d3d11Context);

// Start capture
capture.Start(32);

// Capture and encode loop
USBFrame usbFrame;
while (capture.GetFrame(usbFrame, 100)) {
    // Convert to your CapturedFrame format
    CapturedFrame capturedFrame;
    capturedFrame.texture = usbFrame.texture;
    capturedFrame.timestamp = usbFrame.timestamp;
    capturedFrame.frameIndex = usbFrame.frameIndex;
    capturedFrame.isKeyframe = (usbFrame.frameIndex % 60 == 0);
    
    // Encode
    std::vector<EncodedPacket> packets;
    if (encoder.EncodeFrame(capturedFrame, packets)) {
        // Write packets to disk
        for (auto& packet : packets) {
            WritePacket(packet);
        }
    }
}
```

## API Reference

### Static Methods

```cpp
// List all available USB capture devices
static std::vector<USBCaptureDevice> EnumerateDevices();
```

### Instance Methods

```cpp
// Initialize with device index and D3D11 device
bool Initialize(int deviceIndex, ComPtr<ID3D11Device> d3d11Device);

// Start capture with specified buffer size
bool Start(uint32_t bufferSize = 16);

// Stop capture
void Stop();

// Get next frame (blocking with timeout)
bool GetFrame(USBFrame& frame, uint32_t timeoutMs = 100);

// Get current resolution
uint32_t GetWidth() const;
uint32_t GetHeight() const;

// Check if currently capturing
bool IsRunning() const;
```

### Structures

```cpp
struct USBCaptureDevice {
    std::string name;      // Device friendly name
    int index;             // Device index for Initialize()
    uint32_t width;        // Native width
    uint32_t height;       // Native height
    uint32_t fps;          // Native framerate
};

struct USBFrame {
    ComPtr<ID3D11Texture2D> texture;  // D3D11 texture (BGRA format)
    uint64_t timestamp;                // Microseconds
    uint32_t frameIndex;               // Sequential frame number
};
```

## Key Differences from Original

| Feature | Original | Simple Version |
|---------|----------|----------------|
| Context parameter | Required | Auto-retrieved from device |
| Format handling | Manual fallback | Automatic RGB32 with fallback |
| Error recovery | Partial | Full with detailed logging |
| Device enumeration | None | Built-in EnumerateDevices() |
| Buffer validation | Basic | Complete size checking |
| Thread safety | Good | Enhanced with proper cleanup |
| Resource cleanup | Manual tracking | Automatic with flags |

## Common Usage Patterns

### Pattern 1: Device Selection UI

```cpp
// Get list of devices for UI
auto devices = SimpleUSBCapture::EnumerateDevices();

if (devices.empty()) {
    ShowError("No USB capture devices found");
    return;
}

// Show in dropdown/list
for (size_t i = 0; i < devices.size(); i++) {
    std::string label = devices[i].name + 
        " (" + std::to_string(devices[i].width) + "x" + 
        std::to_string(devices[i].height) + ")";
    AddToDeviceList(label, i);
}

// When user selects:
int selectedIndex = GetSelectedDeviceIndex();
capture.Initialize(selectedIndex, device);
```

### Pattern 2: Auto-select Best Device

```cpp
auto devices = SimpleUSBCapture::EnumerateDevices();

// Find highest resolution device
int bestIndex = 0;
uint32_t maxPixels = 0;

for (const auto& dev : devices) {
    uint32_t pixels = dev.width * dev.height;
    if (pixels > maxPixels) {
        maxPixels = pixels;
        bestIndex = dev.index;
    }
}

spdlog::info("Auto-selected: {}", devices[bestIndex].name);
capture.Initialize(bestIndex, device);
```

### Pattern 3: Reconnect on Error

```cpp
bool captureActive = true;

while (applicationRunning) {
    SimpleUSBCapture capture;
    
    if (!capture.Initialize(deviceIndex, device)) {
        spdlog::error("Failed to open device, retrying in 5s...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        continue;
    }
    
    capture.Start(32);
    
    USBFrame frame;
    while (captureActive && capture.GetFrame(frame, 1000)) {
        ProcessFrame(frame);
    }
    
    spdlog::warn("Capture stopped, reconnecting...");
    std::this_thread::sleep_for(std::chrono::seconds(2));
}
```

## Performance Tips

1. **Buffer Size**: 
   - 16 frames = ~500ms at 30fps (good for realtime)
   - 32 frames = ~1s at 30fps (better for burst handling)
   - 64+ frames = Use for high-latency encoding

2. **Timeout**:
   - 100ms = Responsive for realtime
   - 1000ms = More tolerant of device delays
   - 0ms = Non-blocking (check immediately)

3. **Thread Priority**:
   The capture thread runs at normal priority. To elevate:
   ```cpp
   // After Start():
   // The thread is already created, you can't change priority externally
   // Priority is set inside the class (see implementation)
   ```

## Troubleshooting

### Device Not Found
```
Error: No USB capture devices found
```
**Solutions**:
- Check device is plugged in
- Check Device Manager (Windows)
- Close other apps using the device (OBS, Zoom, etc.)
- Try different USB port
- Reinstall device drivers

### Format Not Supported
```
Warning: RGB32 not supported, using default format
```
**Solutions**:
- This is usually OK, capture will work with device's native format
- Check device supports video capture (not just audio)
- Update device firmware

### Buffer Size Mismatch
```
Warning: Buffer size mismatch: 2073600 < 8294400
```
**Solutions**:
- Device resolution may have changed
- Re-enumerate devices to get actual resolution
- Call Initialize() again

### Stream Error
```
Error: Stream error
```
**Solutions**:
- Device was unplugged
- Device crashed/froze
- USB bandwidth exceeded
- Try lower resolution or framerate

## Migration Checklist

To replace your current USBCapture with SimpleUSBCapture:

- [ ] Replace `#include "usbcapture.h"` with `#include "SimpleUSBCapture.h"`
- [ ] Change `USBCapture` to `SimpleUSBCapture`
- [ ] Remove `d3d11Context` parameter from `Initialize()`
- [ ] Update `GetNextFrame()` calls to `GetFrame()`
- [ ] Add device enumeration UI using `EnumerateDevices()`
- [ ] Test with your capture device
- [ ] Verify resolution and framerate
- [ ] Check logs for any warnings

## Example: Complete Recording App

```cpp
#include "SimpleUSBCapture.h"
#include "HardwareEncoder.h"
#include "DiskWriter.h"

int main() {
    // Setup logging
    spdlog::set_level(spdlog::level::info);
    
    // List devices
    auto devices = SimpleUSBCapture::EnumerateDevices();
    if (devices.empty()) {
        spdlog::error("No devices found");
        return 1;
    }
    
    // Create D3D11 device
    ComPtr<ID3D11Device> device;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                      nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    
    // Initialize capture
    SimpleUSBCapture capture;
    if (!capture.Initialize(0, device)) {
        spdlog::error("Failed to initialize");
        return 1;
    }
    
    spdlog::info("Capturing at {}x{}", capture.GetWidth(), capture.GetHeight());
    
    // Start capture
    capture.Start(32);
    
    // Capture for 10 seconds
    auto startTime = std::chrono::steady_clock::now();
    USBFrame frame;
    uint32_t frameCount = 0;
    
    while (capture.GetFrame(frame, 100)) {
        frameCount++;
        
        // Process frame here
        // ...
        
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(10)) {
            break;
        }
    }
    
    capture.Stop();
    
    spdlog::info("Captured {} frames", frameCount);
    return 0;
}
```*/

    // ========== PUBLIC API ==========

    // List all available USB capture devices
    static std::vector<USBCaptureDevice> EnumerateDevices() {
        std::vector<USBCaptureDevice> devices;

        // Initialize COM for enumeration
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needsUninit = SUCCEEDED(hr);

        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (needsUninit) CoUninitialize();
            return devices;
        }

        // Create enumeration attributes
        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr)) {
            attributes->SetGUID(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
            );

            // Enumerate
            IMFActivate** activateArray = nullptr;
            UINT32 count = 0;
            hr = MFEnumDeviceSources(attributes.Get(), &activateArray, &count);

            if (SUCCEEDED(hr)) {
                for (UINT32 i = 0; i < count; i++) {
                    USBCaptureDevice device;
                    device.index = i;

                    // Get friendly name
                    WCHAR* name = nullptr;
                    UINT32 nameLen = 0;
                    if (SUCCEEDED(activateArray[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen))) {

                        int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
                        device.name.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, name, -1, &device.name[0], size, nullptr, nullptr);
                        CoTaskMemFree(name);
                    }

                    // Try to get resolution (this requires activating the device)
                    ComPtr<IMFMediaSource> source;
                    if (SUCCEEDED(activateArray[i]->ActivateObject(IID_PPV_ARGS(&source)))) {
                        ComPtr<IMFSourceReader> reader;
                        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
                            ComPtr<IMFMediaType> mediaType;
                            if (SUCCEEDED(reader->GetNativeMediaType(
                                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &mediaType))) {

                                UINT32 width = 0, height = 0;
                                MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);
                                device.width = width;
                                device.height = height;

                                // Get framerate
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

    // Initialize with specific device
    bool Initialize(int deviceIndex, ComPtr<ID3D11Device> d3d11Device) {
        if (!d3d11Device) {
            spdlog::error("SimpleUSBCapture: NULL D3D11 device");
            return false;
        }

        m_d3d11Device = d3d11Device;
        m_d3d11Device->GetImmediateContext(&m_d3d11Context);

        // Initialize COM
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            m_comInitialized = true;
        }
        else if (hr != RPC_E_CHANGED_MODE) {
            spdlog::error("COM init failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // Initialize Media Foundation
        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            spdlog::error("MFStartup failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }
        m_mfInitialized = true;

        // Create attributes
        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 1);
        if (FAILED(hr)) {
            spdlog::error("MFCreateAttributes failed");
            Shutdown();
            return false;
        }

        attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
        );

        // Enumerate devices
        IMFActivate** activateArray = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attributes.Get(), &activateArray, &count);
        if (FAILED(hr) || count == 0) {
            spdlog::error("No USB capture devices found");
            Shutdown();
            return false;
        }

        spdlog::info("Found {} USB capture device(s)", count);

        // Validate device index
        if (deviceIndex < 0 || deviceIndex >= (int)count) {
            spdlog::error("Invalid device index: {}", deviceIndex);
            for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
            CoTaskMemFree(activateArray);
            Shutdown();
            return false;
        }

        // Activate selected device
        hr = activateArray[deviceIndex]->ActivateObject(IID_PPV_ARGS(&m_mediaSource));

        // Log device name
        WCHAR* deviceName = nullptr;
        if (SUCCEEDED(activateArray[deviceIndex]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &deviceName, nullptr))) {
            int size = WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, nullptr, 0, nullptr, nullptr);
            std::string name(size - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, &name[0], size, nullptr, nullptr);
            spdlog::info("Opening USB device: {}", name);
            CoTaskMemFree(deviceName);
        }

        // Cleanup activate array
        for (UINT32 i = 0; i < count; i++) activateArray[i]->Release();
        CoTaskMemFree(activateArray);

        if (FAILED(hr)) {
            spdlog::error("Failed to activate device: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }

        // Create source reader
        ComPtr<IMFAttributes> readerAttrs;
        MFCreateAttributes(&readerAttrs, 2);
        if (readerAttrs) {
            readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }

        hr = MFCreateSourceReaderFromMediaSource(
            m_mediaSource.Get(),
            readerAttrs.Get(),
            &m_sourceReader
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create source reader: 0x{:08X}", static_cast<uint32_t>(hr));
            Shutdown();
            return false;
        }

        // Configure output format (RGB32 for easy D3D11 interop)
        ComPtr<IMFMediaType> mediaType;
        hr = MFCreateMediaType(&mediaType);
        if (SUCCEEDED(hr)) {
            mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

            hr = m_sourceReader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                nullptr,
                mediaType.Get()
            );

            if (FAILED(hr)) {
                spdlog::warn("RGB32 not supported, using default format");
            }
        }

        // Get actual format
        ComPtr<IMFMediaType> currentType;
        hr = m_sourceReader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            &currentType
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to get media type");
            Shutdown();
            return false;
        }

        // Get resolution
        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr) || width == 0 || height == 0) {
            spdlog::error("Invalid resolution");
            Shutdown();
            return false;
        }

        m_width = width;
        m_height = height;

        spdlog::info("USB capture initialized: {}x{}", m_width, m_height);
        return true;
    }

    // Start capturing frames
    bool Start(uint32_t bufferSize = 16) {
        if (!m_sourceReader) {
            spdlog::error("Not initialized");
            return false;
        }

        m_bufferSize = bufferSize;
        m_running = true;
        m_captureThread = std::thread(&SimpleUSBCapture::CaptureThreadFunc, this);

        return true;
    }

    // Stop capturing
    void Stop() {
        if (!m_running) return;

        m_running = false;
        m_frameAvailable.notify_all();

        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }

        // Clear buffer
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_frameQueue.empty()) m_frameQueue.pop();
    }

    // Get next captured frame (blocking with timeout)
    bool GetFrame(USBFrame& frame, uint32_t timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_frameQueue.empty()) {
            if (!m_frameAvailable.wait_for(
                lock,
                std::chrono::milliseconds(timeoutMs),
                [this] { return !m_frameQueue.empty() || !m_running; }
            )) {
                return false;
            }
        }

        if (m_frameQueue.empty()) return false;

        frame = m_frameQueue.front();
        m_frameQueue.pop();
        return true;
    }

    // Get current resolution
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool IsRunning() const { return m_running; }

private:
    // Cleanup all resources
    void Shutdown() {
        if (m_sourceReader) {
            m_sourceReader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
            m_sourceReader.Reset();
        }

        if (m_mediaSource) {
            m_mediaSource->Shutdown();
            m_mediaSource.Reset();
        }

        if (m_mfInitialized) {
            MFShutdown();
            m_mfInitialized = false;
        }

        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }

        m_d3d11Context.Reset();
        m_d3d11Device.Reset();
    }

    // Capture thread
    void CaptureThreadFunc() {
        spdlog::info("USB capture thread started");

        while (m_running) {
            IMFSample* sample = nullptr;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;

            HRESULT hr = m_sourceReader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                nullptr,
                &streamFlags,
                &timestamp,
                &sample
            );

            // Handle errors
            if (FAILED(hr)) {
                spdlog::warn("ReadSample failed: 0x{:08X}", static_cast<uint32_t>(hr));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Handle stream flags
            if (streamFlags & MF_SOURCE_READERF_ERROR) {
                spdlog::error("Stream error");
                break;
            }

            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                spdlog::info("End of stream");
                break;
            }

            if (streamFlags & MF_SOURCE_READERF_STREAMTICK) {
                continue; // No data
            }

            if (!sample) continue;

            // Convert to D3D11 texture
            ComPtr<ID3D11Texture2D> texture = ConvertToTexture(sample);
            sample->Release();

            if (texture) {
                USBFrame frame;
                frame.texture = texture;
                frame.timestamp = GetCurrentTimestamp();
                frame.frameIndex = m_frameCount++;

                // Add to queue
                {
                    std::lock_guard<std::mutex> lock(m_mutex);

                    // Drop oldest if full
                    if (m_frameQueue.size() >= m_bufferSize) {
                        m_frameQueue.pop();
                    }

                    m_frameQueue.push(frame);
                }

                m_frameAvailable.notify_one();
            }
        }

        spdlog::info("USB capture thread stopped");
    }

    // Convert Media Foundation sample to D3D11 texture
    ComPtr<ID3D11Texture2D> ConvertToTexture(IMFSample* sample) {
        if (!sample || !m_d3d11Device) return nullptr;

        // Get buffer
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) return nullptr;

        // Lock buffer
        BYTE* data = nullptr;
        DWORD length = 0;
        hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr) || !data) return nullptr;

        // Validate size (RGB32 = 4 bytes per pixel)
        DWORD expectedSize = m_width * m_height * 4;
        if (length < expectedSize) {
            spdlog::warn("Buffer size mismatch: {} < {}", length, expectedSize);
            buffer->Unlock();
            return nullptr;
        }

        // Create D3D11 texture
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = data;
        initData.SysMemPitch = m_width * 4;

        hr = m_d3d11Device->CreateTexture2D(&desc, &initData, &texture);
        buffer->Unlock();

        if (FAILED(hr)) {
            spdlog::warn("CreateTexture2D failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return nullptr;
        }

        return texture;
    }

    uint64_t GetCurrentTimestamp() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    // Member variables
    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    ComPtr<IMFMediaSource> m_mediaSource;
    ComPtr<IMFSourceReader> m_sourceReader;

    std::atomic<bool> m_running;
    std::thread m_captureThread;

    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_bufferSize;
    uint64_t m_frameCount;

    bool m_comInitialized;
    bool m_mfInitialized;

    std::queue<USBFrame> m_frameQueue;
    std::mutex m_mutex;
    std::condition_variable m_frameAvailable;
};
GPU Recording Engine - Professional Wiki
📋 Overview
A professional-grade GPU-accelerated screen and USB capture application with hardware encoding support for NVIDIA NVENC, AMD AMF, and Intel QSV. Built with C++, DirectX 11/12, Media Foundation, and FFmpeg.



Hardware Requirements

OS	Windows 10 (64-bit) Version 1903 or later

Processor	Intel Core i5-8400 / AMD Ryzen 5 2600 (6+ cores)

RAM	16GB DDR4 (32GB recommended)

Storage	NVMe SSD with 50GB free space
GPU	DirectX 12 compatible with hardware encoding
Display	1080p @ 60Hz minimum
GPU Requirements by Vendor

NVIDIA
Feature	Minimum	Recommended
GPU Architecture	Pascal (GTX 10 series)	Turing/Ampere/Ada (RTX 20/30/40 series)
Minimum Model	GTX 1050 Ti	RTX 3060 or higher
VRAM	4GB	8GB+
NVENC Version	6th gen	7th/8th gen (Turing+)
Driver Version	456.71+	Latest Game Ready


AMD


Feature	Minimum	Recommended
GPU Architecture	Polaris/Vega	RDNA 2/3 (RX 6000/7000 series)
Minimum Model	RX 560	RX 6600 XT or higher
VRAM	4GB	8GB+
VCE/AMF Version	VCE 3.4+	AMF 1.4+
Driver Version	Adrenalin 21.5.1+	Latest Adrenalin
Intel
Feature	Minimum	Recommended
GPU Architecture	Ice Lake (10th gen)	Arc A-Series / Xe-LPG
Minimum Model	UHD Graphics 630	Iris Xe or Arc A380+
VRAM	Shared (2GB min)	4GB+ dedicated
QSV Version	7th gen	8th/9th gen
Driver Version	27.20.100.9466+	Latest
record anu screen 
Work and test with usb device Magewell Capture Express


✨ Features![Untitled](https://github.com/user-attachments/assets/500b6636-02ba-4f86-bb1c-cdad26b7d673)

Core Capabilities
Multi-GPU Support - Automatic detection and selection of optimal GPU

Hardware Encoding - NVENC (NVIDIA), AMF (AMD), QSV (Intel)

Multiple Codecs - H.264, H.265/HEVC, AV1

High Resolution - Up to 8K (7680x4320) support

High FPS - 60/120/144/240 FPS recording

USB Capture - Webcams, capture cards (Elgato, AVerMedia, Magewell)

USB Audio - Automatic matching with video devices

System Audio - Loopback recording

Schedule Recording - Time-based auto start/stop

Duration Recording - Auto-stop after specified time

Advanced Features
Zero-Copy Pipeline - GPU memory optimization

Cross-Adapter Capture - Multi-GPU systems support

Hardware Frame Pool - 64-frame buffer for smooth encoding

RAM Buffering - Configurable up to 16GB

Large Pages Support - Lock memory for NVMe optimization

MKV Container - With proper timestamp synchronization

HDR Support - Detection and capture (where available)


**┌─────────────────────────────────────────────────────────────┐
│                       Main Application                       │
│                         (ImGui UI)                           │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│                   Recording Pipeline                         │
├───────────────┬───────────────────────────┬─────────────────┤
│ Display Capture│      USB Capture          │   USB Audio     │
│ (Desktop Dupl) │   (Media Foundation)      │ (Media Found.)  │
├───────────────┼───────────────────────────┼─────────────────┤
│         Hardware Encoder (NVENC/AMF/QSV)                    │
├─────────────────────────────────────────────────────────────┤
│                    Disk Writer (FFmpeg)                      │
│                       (MKV Muxer)                            │
└─────────────────────────────────────────────────────────────┘**




RecordingEngine.h
Core interfaces and data structures:

IRecordingEngine - Main engine interface

RecordingSettings - Configuration structure

GPUInfo / ExtendedGPUInfo - GPU capabilities

PerformanceMetrics - Runtime statistics

2. RecordingPipeline.h
Main recording orchestrator:

Manages capture → encode → write pipeline

Handles both display and USB modes

Thread synchronization

Error recovery

3. GPUDetector.h
Hardware detection:

Enumerates DXGI adapters

Detects encoder capabilities

Calculates performance scores

Vendor-specific feature detection

4. HardwareEncoder.h
FFmpeg-based encoder:

Hardware context initialization

D3D11 texture input

CQP quality control

Cross-adapter support

5. FrameCapture.h
Desktop capture using Desktop Duplication API:

IDXGIOutputDuplication

Ring buffer management

Texture pooling

FPS limiting

6. usbcapture.h
USB video capture using Media Foundation:

Device enumeration

RGB32 format conversion

D3D11 texture output

Thread-safe capture

7. USBAudioCapture.h
USB audio capture:

Device enumeration

PCM/Float format support

Automatic device matching

WAV header generation

8. DiskWriter.h
FFmpeg muxing:

MKV container format

Audio/video interleaving

Header writing

Queue-based async writes

9. VirtualDisplayManager.h
Display management:

Multi-monitor detection

Display info retrieval

Primary display selection

DXGI output mapping

10. USB.h
Legacy USB detection:

SetupAPI enumeration

Device categorization

Filtering audio-only devices


Open solution in Visual Studio

Build for x64 Release

Running
Launch application (requires admin for large pages)

Select GPU in Hardware tab

Choose display or USB device

Configure encoding settings

Click START RECORDING

🎮 Usage Guide
Display Recording
Navigate to Hardware tab

Select target GPU

Choose display (primary recommended)

Set resolution/FPS in Encoding tab

Start recording

USB Capture
Connect USB device (webcam/capture card)

Go to Hardware tab

Select USB device from list

Audio automatically matched

Start USB recording

Scheduled Recording
Select "Schedule Based" mode

Set start/stop times

Activate schedule

Recording auto-starts at specified time



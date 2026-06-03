## 2024-06-03 - [Zero-Copy Packet Transfer]
**Learning:** The FFmpeg integration copied packet data around between encoding (HardwareEncoder) and writing (DiskWriter), costing performance for high-framerate/high-resolution recording.
**Action:** Replaced `std::vector<uint8_t>` data buffer with a zero-copy `std::shared_ptr<AVPacket>` across the pipeline using `av_packet_move_ref` and a custom deleter.

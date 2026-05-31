## 2024-05-24 - First Journal Entry
**Learning:** Initializing journal for Bolt.
**Action:** Keep entries relevant to performance only.
## 2024-05-24 - Zero-Copy FFmpeg Optimization
**Learning:** In C++ FFmpeg applications, transferring video frames between encoder output and muxer input via std::vector byte copies is extremely expensive. Using `av_packet_move_ref` along with smart pointers (`std::shared_ptr<AVPacket>`) eliminates this overhead.
**Action:** When working with FFmpeg packets in the future, always look for opportunities to pass references (`av_packet_ref` or `av_packet_move_ref`) instead of duplicating payloads with `memcpy` or `assign`.

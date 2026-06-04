## 2024-05-19 - [Zero-copy AV packet recording]
**Learning:** Copying encoded FFmpeg packets dynamically via `std::vector` and `memcpy` inside loops is a huge performance bottleneck leading to degraded fps during long recording.
**Action:** Always prefer `av_packet_move_ref` and wrap standard `AVPacket` objects within `std::shared_ptr` to implement zero-copy memory pipelines when passing encoded data from encoder hardware to disk writers.

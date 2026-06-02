1. **Define a shared packet structure**
   Update `Capture/HardwareEncoder.h` to use `std::shared_ptr<AVPacket>` instead of copying bytes to `std::vector<uint8_t>`.
   ```cpp
   struct EncodedPacket {
       std::shared_ptr<AVPacket> pkt;
   };
   ```
   Provide a custom deleter for `shared_ptr<AVPacket>`:
   ```cpp
   inline void AVPacketDeleter(AVPacket* p) { av_packet_free(&p); }
   ```

2. **Update `HardwareEncoder` to produce zero-copy packets**
   In `HardwareEncoder::EncodeFrame` and `HardwareEncoder::Flush`, allocate a new `AVPacket`, use `av_packet_move_ref(new_pkt, m_packet)`, and put it into `EncodedPacket`.

3. **Update `WriteTask` and `DiskWriter` to consume shared packets**
   In `Capture/DiskWriter.h`, update `WriteTask` to use `std::shared_ptr<AVPacket>` for video, or optionally keep `std::vector<uint8_t>` for audio.
   Update `QueueWriteTask` to accept the new structure.
   Update `WriteVideo` to write directly from `task.pkt` without reallocating and copying.

4. **Update `RecordingPipeline` to pass `EncodedPacket` directly to `WriteTask`**
   In `Capture/RecordingPipeline.h`, pass the `shared_ptr<AVPacket>` from `EncodedPacket` to `WriteTask`.

5. **Pre-commit checks**
   Run CMake build to verify the code changes.

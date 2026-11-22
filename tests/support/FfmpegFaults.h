#pragma once

// Makes FFmpeg's read and decode calls fail the way a damaged file or a
// failing disk does, and their allocations fail as they do when memory runs
// out and their pixel conversions fail, for the readers' error tests. A test executable links this with
// `-Wl,--wrap=` for each function below (the CMake function
// orbis_wrap_ffmpeg_faults); every call from the program's own code (not
// FFmpeg's calls inside its libraries) then goes through here, and passes
// straight on to FFmpeg unless a test armed a fault for it. Not thread-safe:
// arm and read from the test's thread.

namespace ffmpegfaults {

enum class Call {
    ReadFrame,       // av_read_frame
    SendPacket,      // avcodec_send_packet with a packet
    SendFlush,       // avcodec_send_packet(ctx, nullptr), the flush at the end
    ReceiveFrame,    // avcodec_receive_frame, any time
    ReceiveDraining, // avcodec_receive_frame on a decoder that took the flush (since reset())
    SeekFrame,       // av_seek_frame
    AllocContext,    // avcodec_alloc_context3 (fails as null)
    ParamsToContext, // avcodec_parameters_to_context
    PacketAlloc,     // av_packet_alloc (fails as null)
    FrameAlloc,      // av_frame_alloc (fails as null)
    BufferRef,       // av_buffer_ref (fails as null)
    ScalerSetup,     // sws_getContext, sws_getCachedContext (null), sws_init_context
    Scale,           // sws_scale, sws_scale_frame
};

// From the `after`+1-th call of `call` on (counting from now), `times` calls
// in a row (-1: every one) return `rc` (null for an allocation) without
// reaching FFmpeg: the packet isn't read, sent or received, nothing is
// allocated or referenced. Replaces what was armed for `call`.
void arm(Call call, int after, int rc, int times = 1);
// Every call passes through again; the counts start over. Call before a
// test that arms ReceiveDraining (it forgets which decoders were flushed).
void reset();
// How many calls of `call` failed since it was armed.
int failed(Call call);

} // namespace ffmpegfaults

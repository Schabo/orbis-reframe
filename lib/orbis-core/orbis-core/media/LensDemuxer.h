// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/metadata/FramePts.h>
#include <orbis-core/stitch/LensSource.h>
#include <orbis-core/Ycbcr.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// The demux / decode core shared by FisheyeDemux (CPU decode, RGB Image out) and
// VulkanVideoDecoder (Vulkan hwaccel, AVVkFrame out): a format context per
// file the lenses come from (one `.insv` holding both on the X5, a file per
// lens on the ONE X2), one persistent decoder per fisheye video stream
// ("lens"), a FIFO of decoded frames per lens, and the strictly-forward pump
// that fills them. A lens's read pumps only its own file. A clip in several
// parts (orbiscore::ClipPart, a recording the camera split into files, or a
// series of photos, one a part) plays them back to back with one set of
// decoders: at a part's end they are drained and go on into the next part's
// files, whose frames are handed out in clip time; a seek goes to the part
// the target is in. A part's files are opened when the clip first needs
// them, and no more than kMaxOpenFiles stay open (the least recently used
// closed, never the part being read's), so a series of a thousand photos
// doesn't hold a thousand files.
// Two lenses in one stream (a camera's proxy: both fisheyes side by side)
// share one decoder: each pair hands out its frame twice (a new reference),
// and each lens's LensSource::rect says which part of it is that lens's.
// Seeking is frame-exact, and a target a little ahead of the last frame read,
// with no keyframe in between, decodes on from there instead of seeking back
// to the keyframe (frame steps, short forward scrubs).
//
// The owner picks the decoder set-up (hwaccel or not) through open()'s hook
// and turns the AVFrames it gets back into whatever it hands out. Part of
// orbiscore_media (needs FFmpeg). Not thread-safe.

struct AVCodec;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

namespace orbiscore {

// The keyframe of `stream` nearest `timestampMs`, in ms as a read reports
// its frame (MsOfUs; see FisheyeDemux::nearestKeyframeMs).
int64_t NearestKeyframeMs(AVFormatContext* fmt, int stream, int64_t timestampMs);
// The keyframes of `stream` at or before / at or after `timestampMs`, in ms
// as NearestKeyframeMs; -1 where the index has none.
void KeyframesAroundMs(AVFormatContext* fmt, int stream, int64_t timestampMs, int64_t& before,
                       int64_t& after);
// The last keyframe of `stream` at or before `timestampMs`, in seconds; -1 if
// the index has none (unknown).
double KeyframeAtOrBeforeSec(AVFormatContext* fmt, int stream, int64_t timestampMs);

struct InsvStreamInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t durationMs = 0;
    // The same to the microsecond, rounded from the stream's time base:
    // where the next part of a split recording starts.
    int64_t durationUs = 0;
    int bitDepth = 8; // bits per sample: 8, or 10 for HEVC Main 10 (the X6's I-Log)
    // One decoded picture's bytes, unpadded, in the stream's pixel format: a
    // 10-bit sample takes two. The GPU decode's NV12 / P010 hold as much as
    // yuv420p / yuv420p10.
    int64_t pictureBytes = 0;
    // The stream's matrix and range (its VUI). The X5 tags its streams BT.709
    // full range, the X6's Dolby Vision mode BT.2020 limited range.
    YcbcrCoding ycbcr;
};

class LensDemuxer {
public:
    // Called per video stream between avcodec_parameters_to_context and
    // avcodec_open2, to set the decoder up. False (with `err` set) fails open().
    using DecoderSetup =
        std::function<bool(const AVStream* st, const AVCodec* dec, AVCodecContext* ctx, std::string& err)>;

    LensDemuxer() = default;
    ~LensDemuxer();
    LensDemuxer(const LensDemuxer&) = delete;
    LensDemuxer& operator=(const LensDemuxer&) = delete;

    // Opens `path` and a decoder per video stream, each a lens. False with
    // `err` set on failure, or if there is no video stream.
    bool open(const std::string& path, const DecoderSetup& setup, std::string& err);
    // Opens a decoder for each of `lenses`, in that order, and each file they
    // name once. False with `err` set on failure (naming the file when it
    // isn't lens 0's), or if `lenses` is empty.
    bool open(const std::vector<LensSource>& lenses, const DecoderSetup& setup, std::string& err);
    // Opens a clip in parts, in time order: the decoders as for the first
    // part's lenses, which every other part's must match (the same codec,
    // picture and time base). lens()'s length is then the whole clip's. The
    // first and the last part are opened and checked here; any other when a
    // seek or a read first reaches it, which then fails as open() would
    // (the part and file named) if it doesn't open or match.
    bool open(const std::vector<ClipPart>& parts, const DecoderSetup& setup, std::string& err);
    // The most files kept open at once (more when one part has more).
    static constexpr int kMaxOpenFiles = 8;
    // The files open now; for tests.
    int openFileCount() const;
    void close();

    int lensCount() const { return m_shared ? 2 : static_cast<int>(m_dec.size()); }
    // The lens's stream: with both lenses in one stream, the same for both.
    const InsvStreamInfo& lens(int i) const { return m_lenses.at(decoderOf(i)); }
    // The bytes a pair's decoded pictures hold: both lenses in one stream
    // share one picture.
    int64_t pairPictureBytes() const;

    // Frame-exact: the next read returns the first frame at or after
    // `timestampMs` -- the first whose time, rounded to the ms as reads
    // report it (MsOfUs), is `timestampMs` or later (see the class comment
    // for the decode-on shortcut). seekToUs(FirstUsOfMs(timestampMs)).
    // A seek that fails (`err` set) leaves no position: the frames buffered
    // are dropped, and every read fails with the seek's error until a seek
    // works -- with a file per lens, one file may have moved and the other
    // not, so nothing read on would be a pair.
    bool seekTo(int64_t timestampMs, std::string& err);
    // The same to the microsecond: the next read returns the first frame
    // whose exact time (FramePts::us) is `fromUs` or later. Past a frame
    // read: seekToUs(its us + 1).
    bool seekToUs(int64_t fromUs, std::string& err);
    // Lens 0's index; see orbiscore::NearestKeyframeMs / KeyframesAroundMs.
    int64_t nearestKeyframeMs(int64_t timestampMs) const;
    void keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const;

    // The next lens-0 / lens-1 frame pair in play order; the caller owns both
    // (av_frame_free). The lenses are paired by time: two frames less than
    // half a frame apart; a frame the other lens has none for is left out
    // (the first one logged), so a lost frame costs one pair, not the
    // lenses' agreement. `*pts` (may be null) gets lens 0's time. False with
    // `err` empty at end of stream, `err` set on a read or decode error
    // (the file and time in it); a read after an error carries on from
    // where it failed. Needs 2 lenses.
    bool readNextPair(AVFrame*& out0, AVFrame*& out1, FramePts* pts, std::string& err);
    // The next frame of `lens` alone, for a single-stream file; the caller
    // owns it. False as readNextPair.
    bool readNextFrame(int lens, AVFrame*& out, std::string& err);
    // Lens `lens`'s next frame without consuming it: the next read still hands
    // it out. For reading the decoded frames' properties before the first
    // read. Null at end of stream or on error (then `err`, if given, says
    // why; empty at end of stream).
    AVFrame* peekFrame(int lens, std::string* err = nullptr);

private:
    // The decoder lens `i` is read with: 0 for both of a shared pair.
    int decoderOf(int i) const { return m_shared && i == 1 ? 0 : i; }

    // One part's lenses (`sources`, one for a shared pair), and once the
    // part is opened (`opened`): per lens its input and stream, and where the
    // part starts in clip time in that stream's time base. `still`: a still
    // picture, its frame at `startUs`, for `durationUs` (ClipPart::stillUs).
    struct Part {
        int64_t startUs = 0;
        int64_t durationUs = 0;
        std::vector<LensSource> sources;
        bool opened = false;
        bool still = false;
        std::vector<int> lensInput;
        std::vector<int> lensStream;
        std::vector<int64_t> offset;
    };
    // No keyframe (partKeyframes).
    static constexpr int64_t kNoKeyframe = INT64_MIN;

    // One file a part reads, open (`fmt`) or closed again to keep the files
    // open few (kMaxOpenFiles); `lastUse` orders them for that.
    struct Input {
        std::string path;
        AVFormatContext* fmt = nullptr;
        bool drainStarted = false; // read to its end, its decoders took the flush
        uint64_t lastUse = 0;
    };

    void clearFifos();
    void resetDecoders(); // flush + clear fifos, keep contexts
    // The input for `path`, opened now if it isn't (again); -1 with `err`
    // set. Closes inputs over kMaxOpenFiles, but not the part being read's
    // or `keep`'s.
    int openInput(const std::string& path, std::string& err, const Part* keep = nullptr) const;
    // Closes the least recently used inputs over kMaxOpenFiles, but not the
    // part being read's, `keep`'s or input `also`.
    void closeSpareInputs(const Part* keep, int also) const;
    // A decoder for stream `streamIndex` of input `input`, as the next lens.
    bool addLens(int input, int streamIndex, const DecoderSetup& setup, std::string& err);
    bool finishOpen(std::string& err);
    // Opens `part`'s files and the stream each lens names; with `setup` a
    // decoder per lens (the first part), else checked against the decoders'
    // streams. On failure `failedPath` is the file it was at.
    bool openPart(Part& part, const DecoderSetup* setup, std::string& failedPath,
                  std::string& err) const;
    // Part `part` opened (again, if its files were closed), for reading or
    // its index; false with `err` set as open() would have it.
    bool ensurePart(size_t part, std::string& err) const;
    size_t partAt(int64_t timestampMs) const;
    size_t partAtUs(int64_t us) const;
    void usePart(size_t part);
    // At the current part's end: on into the next part's files. False when
    // there is none (or on error, `err` set).
    bool nextPart(std::string& err);
    // Lens 0's keyframes of part `part` at or before / after clip time
    // `timestampMs`, in clip µs; kNoKeyframe where its index has none (a
    // still's one frame is one).
    void partKeyframes(size_t part, int64_t timestampMs, int64_t& before, int64_t& after) const;
    // Around `timestampMs` across the parts: a part's last keyframe and the
    // next part's first count for a target near the join.
    void keyframesAround(int64_t timestampMs, int64_t& before, int64_t& after) const;
    AVStream* lensStream(int lens) const;
    int streamToLens(int input, int streamIndex) const;
    // Pulls the frames lens `lens`'s decoder has ready into m_fifo[lens].
    // False with `err` set on a decode error.
    bool drainDecoder(int lens, std::string& err);
    // Hands `pkt` (null: the flush at the file's end) to lens `lens`'s
    // decoder, draining it first if it is full. False with `err` set if
    // the decoder refuses it.
    bool sendPacket(int lens, const AVPacket* pkt, std::string& err);
    // " of <input's file>[ after <last frame read> ms]", for error messages.
    std::string where(int input) const;
    // Reads one packet of `input` into its decoders, or at the file's end
    // drains them. False with `err` set on a read or decode error: the end
    // of the file is not an error, and a read error doesn't end the file.
    bool pumpOnce(int input, std::string& err);
    void dropStale(int lens);                   // frames before the seek target
    // Drops `lens`'s next frame, which the other lens has no frame for (its
    // next is at `otherUs`); the first one per open() is logged.
    void dropUnpaired(int lens, int64_t otherUs);
    AVFrame* popFront(int lens);

    // Opened as the clip needs them, from const keyframe lookups too: a
    // cache of the files, not the reading state.
    mutable std::vector<Input> m_inputs;
    mutable uint64_t m_useTick = 0;
    AVPacket* m_pkt = nullptr;
    mutable std::vector<Part> m_parts;
    size_t m_part = 0;                   // the part being read
    std::vector<int> m_lensInput;        // lens -> m_inputs index, in the current part
    std::vector<int> m_videoStreamIndex; // lens -> AVStream index in its input, likewise
    std::vector<InsvStreamInfo> m_lenses;
    std::vector<AVCodecContext*> m_dec;       // lens -> persistent decoder
    std::vector<std::deque<AVFrame*>> m_fifo; // lens -> decoded, not yet consumed
    std::vector<char> m_streamEof;            // lens -> decoder drained (vector<bool> avoided)
    static constexpr int64_t kNoTime = INT64_MIN;
    int64_t m_seekFromUs = kNoTime;           // drop frames earlier than this
    int64_t m_lastReadUs = kNoTime;           // time of the last frame (pair) read; none after open / a seek
    std::string m_seekFailed;                 // the failed seek's error, until a seek works (seekTo)
    bool m_shared = false;                    // two lenses, one stream: one decoder (decoderOf)
    bool m_unpairedLogged = false;            // a frame without a partner was logged (dropUnpaired)
};

} // namespace orbiscore

#pragma once

// Whether the decoder keeps up with the clip while it plays: the time it
// takes to read each frame against the time playback gives that frame, over a
// few seconds. A machine whose decoder falls behind starves the presenter
// (*Buffering…*); that's what this sees, and only that. Frames the presenter
// skips for the display (a 100 fps clip on a 60 Hz monitor) are no sign of a
// slow machine: every frame is decoded either way, so the decode time against
// the clip's own rate is the measure.
//
// Plain C++, fed by the playback controller's decoder thread; reset() at a
// seek, a pause or another clip, whose first read (the seek's decode, the
// open) doesn't count.
class DecodePace {
public:
    // How long a stretch is judged, and how much slower than the clip the
    // reads must be over it: a single slow stretch (a keyframe, the disk
    // waking up) is shorter than this, a machine that can't play the clip
    // is slower all the way.
    static constexpr double kWindowS = 5.0;
    static constexpr double kSlower = 1.1;

    void reset()
    {
        m_startS = -1.0;
        m_readMs = 0.0;
        m_frameMs = 0.0;
        m_skipNext = true;
    }

    // One frame read while playing, taking `readMs`; playback gives each
    // frame `frameMs`. `nowS`: a steady clock in seconds. True once a
    // whole window's reads took longer than its frames play; the window
    // starts afresh after each verdict.
    bool add(double readMs, double frameMs, double nowS)
    {
        if (m_skipNext) {
            m_skipNext = false;
            return false;
        }
        if (m_startS < 0.0)
            m_startS = nowS;
        m_readMs += readMs;
        m_frameMs += frameMs;
        if (nowS - m_startS < kWindowS)
            return false;
        const bool behind = m_readMs > m_frameMs * kSlower;
        m_startS = nowS;
        m_readMs = 0.0;
        m_frameMs = 0.0;
        return behind;
    }

private:
    double m_startS = -1.0;
    double m_readMs = 0.0;  // the window's reads
    double m_frameMs = 0.0; // and the time playback gives those frames
    bool m_skipNext = true;
};

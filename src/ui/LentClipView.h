#pragma once

#include "ClipViewSource.h"
#include "LayerLease.h"

#include <PictureSource.h>

#include <memory>
#include <string>

// The pictures of a crossfade's layer in playback -- the clip after the cut
// blending in, or the one before it blending out: read from playback's own
// source of that clip when it lends it (LayerLease), else from a source of
// its own (ClipViewSource, as the export and a screenshot render it).
// Rendered the same either way.
class LentClipView : public exportpipe::PictureSource {
public:
    // Clip `clipId` (`spec`'s) from `startMs` up to `endMs` (source times):
    // the layer's span, up to the clip's trim-in (`leadIn`) or from its
    // trim-out on.
    LentClipView(ClipViewSource::Spec spec, std::shared_ptr<LayerLender> lender, int clipId,
                 int64_t startMs, int64_t endMs, bool leadIn);
    ~LentClipView() override;

    bool open(std::string &err) override;
    double fps() const override { return m_view->fps(); }
    void setOutputSize(int width, int height) override { m_view->setOutputSize(width, height); }
    const exportpipe::VideoFrame *frameAt(int64_t ms) override;
    bool readNext(exportpipe::VideoFrame &out) override;
    bool waiting() const override { return m_waiting; }
    const std::string &lastError() const override;

private:
    // Ask playback for its source from `fromMs` on (leaving the lease held).
    void ask(int64_t fromMs);
    // From here on its own source: the frame showing at `ms`.
    const exportpipe::VideoFrame *fallBack(int64_t ms);

    std::shared_ptr<ClipViewSource> m_view; // shared with the lease's render
    std::shared_ptr<LayerLender> m_lender;
    const int m_clipId;
    const int64_t m_startMs;
    const int64_t m_endMs;
    const bool m_leadIn;
    std::shared_ptr<LayerLease> m_lease;
    bool m_own = false; // reads its own source (m_view's)
    bool m_waiting = false;
    exportpipe::VideoFrame m_cur; // what frameAt() handed out last
    bool m_haveCur = false;
    exportpipe::VideoFrame m_next; // read ahead of it
    bool m_haveNext = false;
    int64_t m_lastMs = -1; // the newest frame handed out, -1 before one
    std::string m_error;
};

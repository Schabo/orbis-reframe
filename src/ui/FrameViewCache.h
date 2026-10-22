#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "PlaybackController.h" // PlaybackController::ViewProvider

#include "../core/ReframeTrack.h"

class Timeline;

// Each main-track clip's reframe paths, split and colour grade, copied for
// the playback thread: provider() gives a presented frame the view of its
// own clip at its own source time -- a split clip's every pane -- and that
// clip's grade, so they change with the picture at a cut
// (EquirectViewportWindow::FrameView). MainWindow
// refreshes the copy whenever a clip's path or grade, or the clips, change.
class FrameViewCache {
public:
    FrameViewCache();

    // GUI thread: copy every main-track clip's path and grade.
    void update(const Timeline &timeline);
    // While the user drags the view, frames leave it alone (a clip with
    // keys would otherwise pull it back every frame).
    void setDragging(bool dragging);
    // The pane a split clip's frames give the viewport's own pose (the one
    // the user steers, EquirectViewportWindow::SetActivePane).
    void setActivePane(int pane);

    // For PlaybackController::setViewProvider. A clip without keys keeps
    // the view as it is (free look); one not in the copy keeps both. Of a
    // split clip, the active pane's view is the viewport's own pose (free
    // look when that pane has no keys); the other panes show what they
    // export.
    PlaybackController::ViewProvider provider() const;

private:
    struct Clip {
        ReframeTrack reframe;
        color::Grade grade;
        pane::Split split;
        std::array<ReframeTrack, pane::kMaxPanes - 1> panes; // ClipItem::panes
    };
    struct Shared {
        std::mutex mx;
        std::map<int, Clip> clips; // by ClipItem::id
        std::atomic<bool> dragging{false};
        std::atomic<int> activePane{0};
    };
    std::shared_ptr<Shared> m_shared;
};

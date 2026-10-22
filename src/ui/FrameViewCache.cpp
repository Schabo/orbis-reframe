#include "FrameViewCache.h"

#include "../core/Timeline.h"

#include <algorithm>

FrameViewCache::FrameViewCache() : m_shared(std::make_shared<Shared>()) {}

void FrameViewCache::update(const Timeline &timeline)
{
    std::map<int, Clip> clips;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        clips[c.id] = {c.reframe, c.color.render(), c.flat ? pane::Split{} : c.split, c.panes};
    }
    std::lock_guard<std::mutex> lk(m_shared->mx);
    m_shared->clips = std::move(clips);
}

void FrameViewCache::setDragging(bool dragging)
{
    m_shared->dragging = dragging;
}

void FrameViewCache::setActivePane(int pane)
{
    m_shared->activePane = pane;
}

PlaybackController::ViewProvider FrameViewCache::provider() const
{
    return [shared = m_shared](int clipId, qint64 sourceMs,
                               EquirectViewportWindow::FrameView &view) {
        std::lock_guard<std::mutex> lk(shared->mx);
        const auto it = shared->clips.find(clipId);
        if (it == shared->clips.end())
            return;
        const Clip &c = it->second;
        view.hasGrade = true;
        view.grade = c.grade;
        const int count = c.split.count();
        const int active = shared->activePane < count ? shared->activePane.load() : 0;
        const auto track = [&c](int pane) -> const ReframeTrack & {
            return pane > 0 ? c.panes[size_t(pane - 1)] : c.reframe;
        };
        // Every pane's view as it exports (an unkeyed one looks forward).
        view.hasPanes = true;
        view.split = c.split;
        for (int i = 0; i < count; ++i) {
            view.panePoses[i] =
                track(i).isEmpty() ? ReframePose{} : track(i).sample(sourceMs);
        }
        if (track(active).isEmpty() || shared->dragging)
            return;
        view.hasPose = true;
        view.pose = view.panePoses[active];
    };
}

// MainWindow's video effects (ClipEffects.h): putting one on a clip from the
// Effects tab (a drop on the timeline, or a double-click for the clip at the
// playhead), the clip's Effects section, and keeping the viewport and the
// flat clips' layers in step with them.

#include "MainWindow.h"

#include "EffectsPanel.h"
#include "SettingsSidebar.h"
#include "TimelineWidget.h"
#include "panels/ClipEffectsPanel.h"
#include "../core/ClipItem.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

void MainWindow::connectEffects()
{
    connect(m_effectsPanel, &EffectsPanel::addEffectRequested, this,
            [this](const QString &name) { addEffect(editClipId(), name); });
    connect(m_timelineWidget, &TimelineWidget::effectDropped, this, &MainWindow::addEffect);
    connect(m_clipEffectsPanel, &ClipEffectsPanel::effectsEdited, this,
            [this](const fx::ClipEffects &e) {
                if (const int id = editClipId())
                    m_timeline->setClipEffects(id, e);
            });
    connect(m_timeline, &Timeline::clipEffectsChanged, this,
            [this](int id, const fx::ClipEffects &effects) {
                const ClipItem *c = m_timeline->clipById(id);
                if (!c)
                    return;
                if (c->flat)
                    pushInsetsToController(); // its picture is a layer, graded there
                if (id == m_clipId) // on screen
                    applyColorGrade(*c);
                if (id == editClipId()) // in the panel
                    m_clipEffectsPanel->setState(effects);
                refreshTimelineWidget(); // its ✦
            });
}

void MainWindow::addEffect(int clipId, const QString &name)
{
    const ClipItem *c = m_timeline->clipById(clipId);
    if (!c)
        return;
    fx::ClipEffects e = c->effects;
    e.add(name.toLatin1().constData());
    m_timeline->setClipEffects(clipId, e);
    // Its settings in view: the clip selected, the Video tab's Effects
    // section open.
    selectVideoClip(clipId);
    m_sidebar->setCurrentIndex(SettingsSidebar::VideoTab);
    m_clipEffectsPanel->setExpanded(true);
}

// MainWindow's video effects (ClipEffects.h): putting one on a clip from the
// Effects tab (a drop on the timeline, or a double-click for the clip at the
// playhead), the clip's Effects section, a colour look's file (copied into
// the project's looks/), and keeping the viewport and the flat clips' layers
// in step with them.

#include "MainWindow.h"

#include "EffectsPanel.h"
#include "SettingsSidebar.h"
#include "TimelineWidget.h"
#include "panels/ClipEffectsPanel.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

#include <QDir>
#include <QFileInfo>

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
    connect(m_clipEffectsPanel, &ClipEffectsPanel::loadLookRequested, this,
            [this] { loadLook(editClipId()); });
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
                    syncEffectsPanel(*c);
                refreshTimelineWidget(); // its ✦
                Q_UNUSED(effects);
            });
}

void MainWindow::addEffect(int clipId, const QString &name)
{
    const ClipItem *c = m_timeline->clipById(clipId);
    if (!c)
        return;
    if (name == QLatin1String(EffectsPanel::kCustomLook)) {
        loadLook(clipId);
        return;
    }
    fx::ClipEffects e = c->effects;
    const QString lookPrefix = EffectsPanel::lookTile(QString());
    if (name.startsWith(lookPrefix)) {
        // A look tile replaces the clip's look, whatever it was.
        e.look = fx::Look{};
        e.look->preset = name.mid(lookPrefix.size()).toStdString();
    } else {
        e.add(name.toLatin1().constData());
    }
    m_timeline->setClipEffects(clipId, e);
    // Its settings in view: the clip selected, the Video tab's Effects
    // section open.
    selectVideoClip(clipId);
    m_sidebar->setCurrentIndex(SettingsSidebar::VideoTab);
    m_clipEffectsPanel->setExpanded(true);
}

void MainWindow::syncEffectsPanel(const ClipItem &clip)
{
    m_clipEffectsPanel->setLookFiles(availableLooks());
    m_clipEffectsPanel->setState(clip.effects);
    std::string why;
    clip.renderGrade(nullptr, &why);
    m_clipEffectsPanel->setLookProblem(
        why.empty() ? QString() : tr("The look isn't applied: %1.").arg(QString::fromStdString(why)));
}

QStringList MainWindow::availableLooks() const
{
    QStringList looks;
    const QString dir = projectfolder::lutsDir(m_project->filePath(), projectfolder::LutUse::Look);
    if (!dir.isEmpty())
        for (const QFileInfo &f :
             QDir(dir).entryInfoList({QStringLiteral("*.cube"), QStringLiteral("*.CUBE")},
                                     QDir::Files, QDir::Name | QDir::IgnoreCase))
            looks << f.absoluteFilePath();
    for (int i = 0; i < m_timeline->count(); ++i) {
        const auto &look = m_timeline->clipAt(i).effects.look;
        const QString lut = look ? QString::fromStdString(look->lut) : QString();
        if (!lut.isEmpty() && !looks.contains(lut))
            looks << lut;
    }
    return looks;
}

void MainWindow::loadLook(int clipId)
{
    if (!m_timeline->clipById(clipId))
        return;
    const QString lut = pickLut(projectfolder::LutUse::Look);
    const ClipItem *c = m_timeline->clipById(clipId);
    if (lut.isEmpty() || !c)
        return;
    fx::ClipEffects e = c->effects;
    const float strength = e.look ? e.look->strength : 100.0f;
    e.look = fx::Look{};
    e.look->preset.clear();
    e.look->lut = lut.toStdString();
    e.look->strength = strength; // a file picked in the section keeps its strength
    m_timeline->setClipEffects(clipId, e);
    selectVideoClip(clipId);
    m_sidebar->setCurrentIndex(SettingsSidebar::VideoTab);
    m_clipEffectsPanel->setExpanded(true);
}

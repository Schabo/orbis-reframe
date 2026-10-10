#pragma once

#include "../SettingsPanel.h"

#include "../../core/OutputSettings.h"

class QComboBox;

// Project-level output format: aspect ratio (16:9 / 9:16 / 1:1 / 4:3),
// a resolution from that aspect's presets, and the frame rate ("match first
// clip" or a fixed rate). Edits emit outputChanged; MainWindow routes that
// into Project::setOutput (persisted on the .orp), which pins the viewport's
// aspect and sets the export defaults.
class ProjectPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit ProjectPanel(QWidget *parent = nullptr);

    // Reflect settings set elsewhere (project load / reset / undo). Does not emit.
    void setOutput(const OutputSettings &output);
    // The first clip's native rate, shown on the "Match first clip" entry
    // (<= 0: no clip).
    void setFirstClipFps(double fps);

signals:
    void outputChanged(const OutputSettings &output);

private:
    OutputSettings currentOutput() const;
    void fillResolutions(OutputSettings::Aspect aspect, int width, int height);
    void emitIfEdited();

    QComboBox *m_aspect = nullptr;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_fps = nullptr;
    bool m_updating = false; // true while setOutput() is writing the widgets
};

#pragma once

#include "../SettingsPanel.h"

#include "../../core/OutputSettings.h"

class OutputSettingsEditor;

// Project-level output format (OutputSettingsEditor's aspect, resolution and
// frame rate). Edits emit outputChanged; MainWindow routes that into
// Project::setOutput (persisted on the .orp), which pins the viewport's aspect
// and sets the export defaults.
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
    OutputSettingsEditor *m_editor = nullptr;
};

#pragma once

#include <QDialog>

#include "../core/OutputSettings.h"

class OutputSettingsEditor;

// File ▸ Project settings…: the project's output format (aspect, resolution,
// frame rate), as the New project dialog set it. OK hands back output();
// MainWindow writes it to the Project (undoable), which pins the viewport's
// aspect and sets the export defaults.
class ProjectSettingsDialog : public QDialog {
    Q_OBJECT

public:
    // `firstClipFps`: the first clip's rate, shown on "Match first clip"
    // (<= 0: no clip).
    ProjectSettingsDialog(const OutputSettings &output, double firstClipFps,
                          QWidget *parent = nullptr);

    OutputSettings output() const;

private:
    OutputSettingsEditor *m_output = nullptr;
};

#pragma once

#include <QObject>

#include "../core/OutputSettings.h"

class QComboBox;
class QFormLayout;
class QWidget;

// The output format's controls -- aspect ratio (16:9 / 9:16 / 1:1 / 4:3), a
// resolution from that aspect's presets, and the frame rate ("match first
// clip" or a fixed rate) -- as three rows for a form. Shared by the Project
// panel and the New project dialog. Edits emit outputChanged.
class OutputSettingsEditor : public QObject {
    Q_OBJECT

public:
    // The combo boxes are made as children of `parent` and added to `form`.
    OutputSettingsEditor(QWidget *parent, QFormLayout *form);

    OutputSettings output() const;
    // Reflect settings set elsewhere (project load / reset / undo). Does not emit.
    void setOutput(const OutputSettings &output);
    // The first clip's native rate, shown on the "Match first clip" entry
    // (<= 0: no clip).
    void setFirstClipFps(double fps);

signals:
    void outputChanged(const OutputSettings &output);

private:
    void fillResolutions(OutputSettings::Aspect aspect, int width, int height);
    void emitIfEdited();

    QComboBox *m_aspect = nullptr;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_fps = nullptr;
    bool m_updating = false; // true while setOutput() is writing the widgets
};

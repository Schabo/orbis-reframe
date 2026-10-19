#pragma once

#include "../SettingsPanel.h"

namespace camera {
struct MediaInfo;
}

// Read-only facts about the source file of the clip under the playhead:
// container, video / audio streams, camera and telemetry (camera::ProbeMediaInfo).
// MainWindow probes once per source and hands the result in on a clip change.
class MediaInfoPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit MediaInfoPanel(QWidget *parent = nullptr);

    void setInfo(const camera::MediaInfo &info);
    // The probe failed (e.g. the file is missing): show why instead.
    void setError(const QString &path, const QString &error);
    // No clip under the playhead.
    void setNoClip();

private:
    void clearRows();
    void addSection(const QString &title);
    void addValue(const QString &label, const QString &value, const QString &toolTip = {});
    // A value on its own full-width row: file names have no spaces to wrap at.
    void addWideValue(const QString &value, const QString &toolTip = {});
};

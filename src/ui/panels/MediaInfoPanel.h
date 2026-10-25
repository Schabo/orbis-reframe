#pragma once

#include "../SettingsPanel.h"

#include <QDateTime>

namespace camera {
struct MediaInfo;
}

// When the recording started, as the camera's clock showed it -- the time a
// title's date and the panel's Recorded row give. A camera's own record of
// it where the file has one; the container's time is UTC on some cameras and
// local time marked UTC on others, so only a file no camera reader knows
// falls back to it, in this computer's time zone. Invalid when neither says.
QDateTime recordedTime(const camera::MediaInfo &info);

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

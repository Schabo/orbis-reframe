#pragma once

#include "../SettingsPanel.h"

#include <QDateTime>
#include <QList>

namespace camera {
struct MediaInfo;
}

// When the recording started, as the camera's clock showed it -- the time a
// title's date and the panel's Recorded row give. A camera's own record of
// it where the file has one; the container's time is UTC on some cameras and
// local time marked UTC on others, so only a file no camera reader knows
// falls back to it, in this computer's time zone. Invalid when neither says.
QDateTime recordedTime(const camera::MediaInfo &info);

// A file of a recording the camera split over several (MediaItem::parts),
// as the panel lists it: where it plays in the clip, and whether it's there.
// The panel shows a gap where a part starts after the one before ends.
struct RecordingPart {
    QString path;
    qint64 startMs = 0;
    qint64 durationMs = 0;
    bool missing = false;
};

// Read-only facts about the source file of the clip under the playhead (or
// of the file previewed from the Media tab):
// container, video / audio streams, camera and telemetry (camera::ProbeMediaInfo).
// MainWindow probes once per source and hands the result in on a clip change.
class MediaInfoPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit MediaInfoPanel(QWidget *parent = nullptr);

    // `previewed`: the file shown from the Media tab instead of the
    // timeline -- the header and hint say so, not "this clip". `parts`: a
    // split recording's files as the project knows them, which can name a
    // part the disk no longer has; empty takes the probe's.
    void setInfo(const camera::MediaInfo &info, bool previewed = false,
                 const QList<RecordingPart> &parts = {});
    // The probe failed (e.g. the file is missing): show why instead.
    void setError(const QString &path, const QString &error, bool previewed = false);
    // No clip under the playhead.
    void setNoClip();

private:
    void clearRows();
    // The header's scope: "Previewed file", or the one the sidebar gave it.
    void setPreviewed(bool previewed);
    QString m_clipScope; // the sidebar's scope, kept while a preview shows
    void addSection(const QString &title);
    // The Recording section: a recording in several files.
    void addRecording(const camera::MediaInfo &info, QList<RecordingPart> parts);
    void addValue(const QString &label, const QString &value, const QString &toolTip = {});
    // A value on its own full-width row: file names have no spaces to wrap at.
    void addWideValue(const QString &value, const QString &toolTip = {});
};

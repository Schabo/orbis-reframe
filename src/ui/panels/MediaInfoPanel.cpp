#include "MediaInfoPanel.h"

#include <MediaInfo.h>

#include <QDateTime>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>

#include <algorithm>
#include <cmath>

namespace {

QString bitRate(int64_t bps)
{
    if (bps <= 0)
        return {};
    if (bps >= 1'000'000)
        return QStringLiteral("%1 Mb/s").arg(bps / 1e6, 0, 'f', bps >= 10'000'000 ? 0 : 1);
    return QStringLiteral("%1 kb/s").arg(bps / 1000);
}

QString fps(double f)
{
    // 29.97 / 59.94 / 119.88 keep their decimals; whole rates don't get ".00".
    const double rounded = std::round(f);
    if (std::abs(f - rounded) < 0.005)
        return QString::number(int(rounded));
    return QString::number(f, 'f', 2);
}

QString duration(int64_t ms)
{
    const int64_t s = ms / 1000;
    const QString mmss = QStringLiteral("%1:%2.%3")
                             .arg(s / 60)
                             .arg(s % 60, 2, 10, QLatin1Char('0'))
                             .arg(ms % 1000, 3, 10, QLatin1Char('0'));
    return s >= 3600 ? QStringLiteral("%1:%2").arg(s / 3600).arg(mmss.rightJustified(9, u'0'))
                     : mmss;
}

QString upperCodec(const std::string &codec, const std::string &profile)
{
    QString s = QString::fromStdString(codec).toUpper();
    if (!profile.empty())
        s += QLatin1Char(' ') + QString::fromStdString(profile);
    return s;
}

} // namespace

QDateTime recordedTime(const camera::MediaInfo &info)
{
    if (!info.recordedLocal.empty())
        return QDateTime::fromString(QString::fromStdString(info.recordedLocal), Qt::ISODate);
    if (!info.cameraModel.empty())
        return {}; // a camera's file that doesn't say
    return QDateTime::fromString(QString::fromStdString(info.creationTime), Qt::ISODateWithMs)
        .toLocalTime();
}

MediaInfoPanel::MediaInfoPanel(QWidget *parent)
    : SettingsPanel(tr("Media information"), parent)
{
    setNoClip();
}

void MediaInfoPanel::clearRows()
{
    while (form()->rowCount() > 0)
        form()->removeRow(0);
}

void MediaInfoPanel::addSection(const QString &title)
{
    auto *label = new QLabel(title, this);
    label->setObjectName(QStringLiteral("mediaInfoSection"));
    form()->addRow(label);
}

void MediaInfoPanel::addValue(const QString &label, const QString &value, const QString &toolTip)
{
    if (value.isEmpty())
        return;
    auto *v = new QLabel(value, this);
    v->setObjectName(QStringLiteral("mediaInfoValue"));
    v->setWordWrap(true);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setToolTip(toolTip);
    form()->addRow(label, v);
}

void MediaInfoPanel::addWideValue(const QString &value, const QString &toolTip)
{
    auto *v = new QLabel(value, this);
    v->setObjectName(QStringLiteral("mediaInfoValue"));
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setToolTip(toolTip);
    form()->addRow(v);
}

void MediaInfoPanel::setPreviewed(bool previewed)
{
    if (previewed && m_clipScope.isEmpty())
        m_clipScope = scope();
    if (previewed) {
        setScope(tr("Previewed file"));
    } else if (!m_clipScope.isEmpty()) {
        setScope(m_clipScope);
        m_clipScope.clear();
    }
}

void MediaInfoPanel::addRecording(const camera::MediaInfo &m, QList<RecordingPart> parts)
{
    if (parts.isEmpty()) // as the files on the disk have it
        for (const camera::MediaPart &p : m.parts)
            parts.append({QString::fromStdString(p.path), p.startMs, p.durationMs, false});
    // Where a part starts after the one before ends, one was never found.
    qint64 end = 0;
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].startMs > end) {
            parts.insert(i, {QString(), end, parts[i].startMs - end, true});
            ++i;
        }
        end = std::max(end, parts[i].startMs + parts[i].durationMs);
    }
    const QString first = QString::fromStdString(m.missingFirst);
    QString next = QString::fromStdString(m.mayContinueIn);
    for (const RecordingPart &p : parts)
        if (!next.isEmpty() && QFileInfo(p.path).fileName() == next)
            next.clear(); // a part the project knows, shown as one: not a guess
    if (parts.size() < 2 && first.isEmpty() && next.isEmpty())
        return;

    addSection(tr("Recording"));
    if (!first.isEmpty())
        addValue(tr("Starts in"), tr("%1\n⚠ Not here").arg(first),
                 tr("The recording starts in an earlier file, which isn't in this folder. The "
                    "clip starts where this file does, without what's before it. Added from "
                    "the first file, the whole recording comes in as a clip of its own."));
    if (parts.size() >= 2) {
        const RecordingPart &last = parts.back();
        addValue(tr("Length"), duration(last.startMs + last.durationMs),
                 tr("The whole recording, every file of it: the clip's length before "
                    "trimming. The camera starts a new file when one gets too long."));
        const QString notHere = tr("The clip plays black and silent here, and can't be "
                                   "exported over it.");
        for (int i = 0; i < parts.size(); ++i) {
            const RecordingPart &p = parts[i];
            const QString span = QStringLiteral("%1 – %2").arg(duration(p.startMs),
                                                               duration(p.startMs + p.durationMs));
            const QString label = i == 0 ? tr("Files") : QString();
            const QFileInfo f(p.path);
            if (p.path.isEmpty())
                addValue(label, tr("⚠ A file that isn't here\n%1").arg(span),
                         tr("A file of this recording wasn't in the folder when the clip was "
                            "added: the files around it leave a gap.") +
                             QLatin1Char(' ') + notHere);
            else if (p.missing)
                addValue(label, tr("%1\n⚠ %2, not here").arg(f.fileName(), span),
                         tr("This file of the recording isn't where the project expects it: "
                            "%1.")
                                 .arg(f.absoluteFilePath()) +
                             QLatin1Char(' ') + notHere + QLatin1Char(' ') +
                             tr("Put it back in the folder with the other files, under this "
                                "name, and it plays again."));
            else
                addValue(label, QStringLiteral("%1\n%2").arg(f.fileName(), span),
                         f.absoluteFilePath());
        }
    }
    if (!next.isEmpty())
        addValue(tr("May go on in"), tr("%1\nNot here").arg(next),
                 tr("The last file is as long as the camera makes one, so the recording may "
                    "go on in the next file, which isn't in this folder. The files don't say "
                    "whether it does. If you have it, put it beside the others before adding "
                    "the clip, and it comes in as part of the clip."));
}

void MediaInfoPanel::setInfo(const camera::MediaInfo &m, bool previewed,
                             const QList<RecordingPart> &parts)
{
    clearRows();
    setBodyEnabled(true);
    setPreviewed(previewed);
    setHint(previewed ? tr("The file previewed from the Media tab, not on the timeline.")
                      : tr("The source file of the clip under the playhead."));
    const QLocale locale;

    const QFileInfo fi(QString::fromStdString(m.path));
    addSection(tr("File"));
    addWideValue(fi.fileName(), fi.absoluteFilePath());
    addValue(tr("Size"), locale.formattedDataSize(m.fileBytes, 2, QLocale::DataSizeSIFormat));
    addValue(tr("Duration"), duration(m.durationMs));
    if (const QDateTime recorded = recordedTime(m); recorded.isValid())
        addValue(tr("Recorded"), locale.toString(recorded, QLocale::ShortFormat));
    addValue(tr("Bitrate"), bitRate(m.totalBitRate));
    // A take in several files: the other lens's file, which the clip needs.
    for (const std::string &f : m.lensFiles) {
        const QFileInfo lf(QString::fromStdString(f));
        addValue(tr("Back lens file"), lf.fileName(),
                 tr("The back lens's picture is in a file of its own, next to this one: %1")
                     .arg(lf.absoluteFilePath()));
    }
    if (fi.suffix().compare(QLatin1String("insv"), Qt::CaseInsensitive) == 0) // a 360 clip's
        addValue(tr("Preview file"), m.proxyPath.empty() ? tr("Not found") : tr("Found"),
                 m.proxyPath.empty()
                     ? tr("The camera's low-res preview file, expected next to the .insv.")
                     : QString::fromStdString(m.proxyPath));
    if (m.unevenTimeMap)
        addValue(tr("Re-written"), tr("⚠ With cuts or a speed change"),
                 tr("Insta360's software re-wrote this file with stretches cut out of it or its "
                    "speed changed. orbis-reframe lines a re-written file up with the camera's "
                    "motion and GPS as a plain trim, so stabilization, highlight marks and "
                    "titles or gauges from the GPS can be off in it. The camera's own file "
                    "doesn't have this problem."));
    addRecording(m, parts);

    addSection(tr("Video"));
    if (m.video.empty()) {
        addValue(tr("Streams"), tr("None"));
    } else {
        const camera::MediaVideoStream &v = m.video.front();
        const QString mode = QString::fromStdString(m.captureMode);
        if (!mode.isEmpty()) {
            addValue(tr("Mode"), tr("%1 360°").arg(mode),
                     tr("Read from the size of the lens pictures. 5.7K+ records pictures the "
                        "same size as 5.7K, so it shows as 5.7K."));
            addValue(tr("Lenses"), tr("2 × %1×%2 fisheye").arg(v.width).arg(v.height));
            addValue(tr("Sphere"), QStringLiteral("%1×%2").arg(2 * v.width).arg(v.width),
                     tr("The whole 360° picture once the two are stitched together, as a flat "
                        "world-map image."));
        } else {
            addValue(tr("Streams"), QString::number(m.video.size()));
            addValue(tr("Resolution"), QStringLiteral("%1×%2").arg(v.width).arg(v.height));
        }
        addValue(tr("Frame rate"), tr("%1 fps").arg(fps(v.fps)));
        if (m.captureFps > 0 && v.fps > 0)
            addValue(tr("Slow motion"), tr("Filmed at %1 fps").arg(fps(m.captureFps)),
                     tr("The camera filmed %1 frames a second and stored them at %2, so the "
                        "clip plays %3× slower than it happened. Stabilization and the GPS "
                        "follow the time it was filmed in.")
                         .arg(fps(m.captureFps), fps(v.fps))
                         .arg(m.captureFps / v.fps, 0, 'f', 2));
        if (m.timelapseIntervalSec > 0 && v.fps > 0)
            addValue(tr("Timelapse"),
                     tr("A frame every %1 s").arg(m.timelapseIntervalSec, 0, 'f', 1),
                     tr("%1 of recording in %2 of video. Stabilization and the GPS follow the "
                        "time it was filmed in.")
                         .arg(duration(std::llround(double(m.durationMs) *
                                                    m.timelapseIntervalSec * v.fps)),
                              duration(m.durationMs)));
        if (v.frameCount > 0)
            addValue(tr("Frames"), locale.toString(qlonglong(v.frameCount)));
        addValue(tr("Codec"), upperCodec(v.codec, v.profile));
        QString format = v.bitDepth > 0 ? tr("%1-bit").arg(v.bitDepth) : QString();
        if (!v.chroma.empty())
            format += QLatin1Char(' ') + QString::fromStdString(v.chroma);
        addValue(tr("Format"), format.trimmed(), QString::fromStdString(v.pixelFormat));
        QString colour = v.colorSpace == "bt709"                  ? QStringLiteral("BT.709")
                         : v.colorSpace.rfind("bt2020", 0) == 0     ? QStringLiteral("BT.2020")
                         : v.colorSpace == "bt470bg" || v.colorSpace == "smpte170m"
                             ? QStringLiteral("BT.601")
                             : QString::fromStdString(v.colorSpace).toUpper();
        if (!m.hdrFormat.empty())
            colour += (colour.isEmpty() ? QString() : QStringLiteral(", ")) +
                      QString::fromStdString(m.hdrFormat);
        if (!colour.isEmpty())
            colour += QStringLiteral(", ");
        colour += v.fullRange ? tr("full range") : tr("limited range");
        addValue(tr("Colour"), colour);
        const QString hdr = QString::fromStdString(m.hdrFormat);
        if (const QString log = QString::fromStdString(m.logProfile); !log.isEmpty()) {
            addValue(tr("Colour profile"), log,
                     tr("A log profile: the picture looks flat and grey until the LUT the "
                        "camera maker publishes for %1 is applied (Colour section).")
                         .arg(log));
        } else if (!hdr.isEmpty()) {
            // The camera's name for the mode ("Dolby_Vision"), with the HDR
            // format the stream says it is.
            const QString mode = m.colorProfile == "standard"
                                     ? QString()
                                     : QString::fromStdString(m.colorProfile)
                                           .replace(QLatin1Char('_'), QLatin1Char(' '));
            addValue(tr("Colour profile"),
                     mode.isEmpty() ? hdr : tr("%1 (%2)").arg(mode, hdr),
                     tr("HDR (%1): the picture looks dim and flat until a LUT converts it to "
                        "normal (Rec.709) colours (Colour section).")
                         .arg(hdr));
        } else if (m.colorProfile == "standard") {
            addValue(tr("Colour profile"), tr("Standard"));
        } else if (!m.colorProfile.empty()) {
            addValue(tr("Colour profile"), QString::fromStdString(m.colorProfile));
        }
        const QString br = bitRate(v.bitRate);
        addValue(tr("Bitrate"), m.video.size() > 1 && !br.isEmpty() ? tr("%1 per lens").arg(br) : br);
    }

    addSection(tr("Audio"));
    if (m.audio.empty())
        addValue(tr("Tracks"), tr("None"));
    // Two short lines per track: codec + channels, then rate + bitrate.
    for (size_t i = 0; i < m.audio.size(); ++i) {
        const camera::MediaAudioStream &a = m.audio[i];
        QString first = upperCodec(a.codec, a.profile);
        if (!a.channelLayout.empty())
            first += QLatin1Char(' ') + QString::fromStdString(a.channelLayout);
        else if (a.channels > 0)
            first += QLatin1Char(' ') + tr("%n ch", nullptr, a.channels);
        QStringList second;
        if (a.sampleRate > 0)
            second << tr("%1 kHz").arg(a.sampleRate / 1000.0, 0, 'g', 3);
        if (a.bitRate > 0)
            second << bitRate(a.bitRate);
        addValue(tr("Track %1").arg(i + 1),
                 first + QLatin1Char('\n') + second.join(QStringLiteral(", ")));
    }

    if (!m.cameraModel.empty() || !m.serialNumber.empty()) {
        addSection(tr("Camera"));
        addValue(tr("Model"), QString::fromStdString(m.cameraModel));
        addValue(tr("Serial"), QString::fromStdString(m.serialNumber));
        addValue(tr("Firmware"), QString::fromStdString(m.firmware));
        addValue(tr("Motion data"),
                 m.imuRateHz > 0          ? tr("~%1 Hz").arg(qRound(m.imuRateHz))
                 : m.orientationPerFrame ? tr("Orientation per frame")
                                         : tr("Not recorded"),
                 tr("How often the camera recorded its movement, which stabilization works "
                    "from."));
        QString gps = tr("Not recorded");
        if (m.gpsRecords > 0) {
            gps = m.gpsRateHz > 0 ? tr("~%1 Hz").arg(qRound(m.gpsRateHz)) : tr("Recorded");
            if (m.gpsFixes == 0) {
                gps += QLatin1Char('\n') + tr("no position fix");
            } else if (m.gpsTrackSec > 0 && m.durationMs > 0) {
                // The track can drop out, or stop long before the recording does.
                const double share = m.gpsTrackSec * 1000.0 / m.durationMs;
                if (share < 0.9)
                    gps += QLatin1Char('\n')
                           + tr("%1% of the recording").arg(qMax(1, qRound(share * 100)));
            }
        }
        addValue(tr("GPS"), gps,
                 tr("Whether a GPS source, such as the GPS remote, recorded a track. "
                    "The positions aren't shown here."));
        addValue(tr("Highlight marks"),
                 m.highlightMarks > 0 ? QString::number(m.highlightMarks) : tr("None"),
                 tr("The moments marked while recording. They show as dots on the clip in the "
                    "timeline."));
        if (m.rollingShutterMs > 0)
            addValue(tr("Sensor readout"), tr("%1 ms").arg(m.rollingShutterMs, 0, 'f', 1),
                     tr("How long the sensor takes to read one picture from top to bottom (the "
                        "rolling shutter)."));
    }
}

void MediaInfoPanel::setError(const QString &path, const QString &error, bool previewed)
{
    clearRows();
    setBodyEnabled(true);
    setPreviewed(previewed);
    setHint({});
    addSection(tr("File"));
    addWideValue(QFileInfo(path).fileName(), path);
    addValue(tr("Error"), error);
}

void MediaInfoPanel::setNoClip()
{
    clearRows();
    setPreviewed(false);
    setHint(tr("No clip under the playhead."));
    setBodyEnabled(false);
}

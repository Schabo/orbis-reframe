#include "TimelineWidget.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

namespace {

constexpr int kHeaderW = 180; // track headers, left of the clips
constexpr int kMarginX = 3;
constexpr int kRulerH = 14;
constexpr int kMainH = 40;    // the main video track's row
constexpr int kTrackH = 28;   // an audio track's row
constexpr int kRowGap = 2;
constexpr int kEdgeGrabPx = 6;
constexpr int kDragThresholdPx = 4;
constexpr int kSnapPx = 6;
constexpr int kMuteBoxW = 16;
constexpr qint64 kMinTrimMs = 100; // belt-and-braces; Timeline does the real clamp
constexpr int kScrollH = 12;          // the horizontal scrollbar under the tracks
constexpr qint64 kMinVisibleMs = 1000; // the furthest zoom in: 1 s across the view
constexpr double kWheelZoom = 1.25;    // per wheel notch / zoomIn()

// "mm:ss", or "mm:ss.t" when the ruler's ticks are closer than a second.
QString formatTime(qint64 ms, bool tenths = false)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    QString t = QStringLiteral("%1:%2")
                    .arg(s / 60, 2, 10, QLatin1Char('0'))
                    .arg(s % 60, 2, 10, QLatin1Char('0'));
    if (tenths)
        t += QStringLiteral(".%1").arg((ms % 1000) / 100);
    return t;
}

// A "nice" tick interval (ms) so the ruler shows ~4-8 labels.
qint64 tickIntervalMs(qint64 totalMs, int usableW)
{
    if (totalMs <= 0 || usableW <= 0)
        return 1000;
    const int wantTicks = std::clamp(usableW / 90, 3, 10);
    const double raw = double(totalMs) / wantTicks;
    static const qint64 steps[] = {100,    200,    500,    1000,   2000,   5000,
                                   10000,  15000,  30000,  60000,  120000, 300000,
                                   600000, 900000, 1800000, 3600000};
    for (qint64 s : steps)
        if (double(s) >= raw)
            return s;
    return steps[sizeof(steps) / sizeof(steps[0]) - 1];
}

QColor clipFill(int id, bool active, bool missing)
{
    if (missing)
        return active ? QColor(150, 60, 60) : QColor(110, 55, 55);
    // Stable per-id hue, muted.
    const int hue = (id * 47) % 360;
    return QColor::fromHsv(hue, active ? 90 : 55, active ? 165 : 130);
}

QColor audioFill(bool selected, bool muted, bool missing)
{
    if (missing)
        return selected ? QColor(150, 60, 60) : QColor(110, 55, 55);
    if (muted)
        return selected ? QColor(105, 110, 115) : QColor(80, 84, 88);
    return selected ? QColor(60, 150, 140) : QColor(45, 115, 110);
}

QColor overlayFill(bool selected, bool hidden, bool missing)
{
    if (missing)
        return selected ? QColor(150, 60, 60) : QColor(110, 55, 55);
    if (hidden)
        return selected ? QColor(105, 110, 115) : QColor(80, 84, 88);
    return selected ? QColor(140, 100, 170) : QColor(105, 78, 135);
}

QRect muteBox(int rowTop, int rowH)
{
    return QRect(kHeaderW - kMuteBoxW - 5, rowTop + (rowH - 14) / 2, kMuteBoxW, 14);
}

} // namespace

TimelineWidget::TimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_hscroll = new QScrollBar(Qt::Horizontal, this);
    m_hscroll->setEnabled(false);
    connect(m_hscroll, &QScrollBar::valueChanged, this, [this](int v) {
        m_viewStartMs = v;
        rebuildLayout();
        update();
    });
    setAcceptDrops(true);
    m_autoScroll = new QTimer(this);
    m_autoScroll->setInterval(30);
    connect(m_autoScroll, &QTimer::timeout, this, &TimelineWidget::autoScrollTick);
}

int TimelineWidget::mainTop() const
{
    return kRulerH + m_videoTracks * (kTrackH + kRowGap);
}

int TimelineWidget::trackTop(int trackIndex) const
{
    // Overlay tracks above the main track, audio tracks below it.
    if (trackIndex < m_videoTracks)
        return kRulerH + trackIndex * (kTrackH + kRowGap);
    return mainTop() + kMainH + kRowGap + (trackIndex - m_videoTracks) * (kTrackH + kRowGap);
}

int TimelineWidget::scrollBarTop() const
{
    return trackTop(static_cast<int>(m_tracks.size())) + 2;
}

int TimelineWidget::totalHeight() const
{
    return scrollBarTop() + kScrollH;
}

QSize TimelineWidget::sizeHint() const
{
    return QSize(480, totalHeight() + 6);
}

QSize TimelineWidget::minimumSizeHint() const
{
    return QSize(200, totalHeight());
}

void TimelineWidget::setClips(const QVector<Clip> &clips)
{
    m_clips = clips;
    rebuildLayout();
    update();
}

void TimelineWidget::setTracks(const QVector<TrackView> &tracks)
{
    const bool resized = tracks.size() != m_tracks.size();
    m_tracks = tracks;
    m_videoTracks = 0;
    while (m_videoTracks < m_tracks.size() && m_tracks[m_videoTracks].video)
        ++m_videoTracks;
    if (m_drag == Drag::AudioMove || m_drag == Drag::AudioTrimIn || m_drag == Drag::AudioTrimOut)
        m_drag = Drag::None; // the model changed under the drag
    rebuildLayout();
    if (resized)
        updateGeometry();
    update();
}

void TimelineWidget::setPlayhead(qint64 timelineMs)
{
    timelineMs = std::clamp<qint64>(timelineMs, 0, m_totalMs);
    if (timelineMs == m_playheadMs)
        return;
    m_playheadMs = timelineMs;
    // Zoomed in and the playhead ran off screen (playback): page to it. Not
    // mid-drag -- the edge auto-scroll owns the view then.
    const qint64 span = visibleSpanMs();
    if (zoomed() && m_drag == Drag::None &&
        (timelineMs < m_viewStartMs || timelineMs > m_viewStartMs + span))
        scrollToMs(timelineMs - span / 10);
    update();
}

void TimelineWidget::setCanPasteGrade(bool canPaste)
{
    m_canPasteGrade = canPaste;
}

void TimelineWidget::setCanPasteStabilization(bool canPaste)
{
    m_canPasteStab = canPaste;
}

void TimelineWidget::setMainMuted(bool muted)
{
    if (muted == m_mainMuted)
        return;
    m_mainMuted = muted;
    update();
}

void TimelineWidget::setActiveClip(int clipId)
{
    if (clipId == m_activeClipId)
        return;
    m_activeClipId = clipId;
    rebuildLayout();
    update();
}

void TimelineWidget::setSelectedTrackClip(int clipId)
{
    if (clipId == m_selectedAudioId)
        return;
    m_selectedAudioId = clipId;
    update();
}

void TimelineWidget::resizeEvent(QResizeEvent *)
{
    rebuildLayout();
}

void TimelineWidget::rebuildLayout()
{
    m_rows.clear();
    m_totalMs = 0;
    for (const Clip &c : m_clips)
        m_totalMs += c.timelineSpanMs();
    m_extentMs = m_totalMs;
    for (const TrackView &t : m_tracks)
        for (const TrackClipView &c : t.clips)
            m_extentMs = std::max(m_extentMs, c.endMs());
    clampView();
    updateScrollBar();

    qint64 acc = 0;
    for (const Clip &c : m_clips) {
        Row r;
        r.clip = c;
        r.startMs = acc;
        r.durMs = c.timelineSpanMs();
        const int x0 = xForTimelineMs(acc);
        const int x1 = xForTimelineMs(acc + r.durMs);
        r.rect = QRect(x0, mainTop(), std::max(1, x1 - x0), kMainH);
        m_rows.push_back(r);
        acc += r.durMs;
    }
}

int TimelineWidget::contentLeft() const
{
    return kHeaderW + kMarginX;
}

int TimelineWidget::contentWidth() const
{
    return std::max(1, width() - contentLeft() - kMarginX);
}

qint64 TimelineWidget::visibleSpanMs() const
{
    const qint64 ext = std::max<qint64>(m_extentMs, 1);
    return m_visibleMs > 0 && m_visibleMs < ext ? m_visibleMs : ext;
}

int TimelineWidget::xForTimelineMs(qint64 ms) const
{
    if (m_extentMs <= 0)
        return contentLeft();
    const double x = contentLeft() + double(ms - m_viewStartMs) * contentWidth() / visibleSpanMs();
    return int(std::clamp(x, -1.0e6, 1.0e6)); // far off screen when zoomed in; keep it an int
}

qint64 TimelineWidget::timelineMsAt(int x) const
{
    const double ms = m_viewStartMs + double(x - contentLeft()) * visibleSpanMs() / contentWidth();
    return std::clamp<qint64>(std::llround(ms), 0, m_extentMs);
}

void TimelineWidget::clampView()
{
    const qint64 ext = std::max<qint64>(m_extentMs, 1);
    if (m_visibleMs >= ext)
        m_visibleMs = 0; // zoomed out to (or past) the whole: fit
    m_viewStartMs = std::clamp<qint64>(m_viewStartMs, 0, std::max<qint64>(0, ext - visibleSpanMs()));
    if (!zoomed())
        m_viewStartMs = 0;
}

void TimelineWidget::updateScrollBar()
{
    const QSignalBlocker block(m_hscroll);
    const qint64 span = visibleSpanMs();
    const qint64 max = std::max<qint64>(0, m_extentMs - span);
    m_hscroll->setRange(0, int(std::min<qint64>(max, INT_MAX)));
    m_hscroll->setPageStep(int(std::min<qint64>(span, INT_MAX)));
    m_hscroll->setSingleStep(int(std::max<qint64>(1, span / 20)));
    m_hscroll->setValue(int(std::min<qint64>(m_viewStartMs, INT_MAX)));
    m_hscroll->setEnabled(zoomed());
    m_hscroll->setGeometry(contentLeft(), scrollBarTop(), contentWidth(), kScrollH);
}

void TimelineWidget::scrollToMs(qint64 viewStartMs)
{
    m_viewStartMs = viewStartMs;
    rebuildLayout(); // clamps, moves the scrollbar
    update();
}

void TimelineWidget::zoomBy(double factor, int anchorX)
{
    if (m_extentMs <= 0 || factor <= 0.0)
        return;
    const double anchorMs =
        m_viewStartMs + double(anchorX - contentLeft()) * visibleSpanMs() / contentWidth();
    const qint64 ext = std::max<qint64>(m_extentMs, 1);
    const qint64 span = std::clamp<qint64>(std::llround(visibleSpanMs() / factor),
                                           std::min(kMinVisibleMs, ext), ext);
    m_visibleMs = span >= ext ? 0 : span;
    m_viewStartMs = std::llround(anchorMs - double(anchorX - contentLeft()) * span / contentWidth());
    rebuildLayout();
    update();
}

void TimelineWidget::zoomIn()
{
    const int px = xForTimelineMs(m_playheadMs);
    const bool inView = px >= contentLeft() && px <= contentLeft() + contentWidth();
    zoomBy(kWheelZoom, inView ? px : contentLeft() + contentWidth() / 2);
}

void TimelineWidget::zoomOut()
{
    const int px = xForTimelineMs(m_playheadMs);
    const bool inView = px >= contentLeft() && px <= contentLeft() + contentWidth();
    zoomBy(1.0 / kWheelZoom, inView ? px : contentLeft() + contentWidth() / 2);
}

void TimelineWidget::zoomToFit()
{
    m_visibleMs = 0;
    m_viewStartMs = 0;
    rebuildLayout();
    update();
}

void TimelineWidget::wheelEvent(QWheelEvent *e)
{
    const QPoint d = e->angleDelta();
    const int x = int(e->position().x());
    if (e->modifiers() & Qt::ControlModifier) {
        if (d.y() != 0)
            zoomBy(std::pow(kWheelZoom, d.y() / 120.0), x);
    } else if (zoomed()) {
        // Either wheel axis pans; a notch moves a tenth of the view.
        const int notch = d.x() != 0 ? d.x() : d.y();
        scrollToMs(m_viewStartMs - std::llround(notch / 120.0 * visibleSpanMs() / 10.0));
    }
    e->accept();
}

QRect TimelineWidget::trackClipRect(int trackIndex, const TrackClipView &c) const
{
    const int x0 = xForTimelineMs(c.startMs);
    const int x1 = xForTimelineMs(c.endMs());
    return QRect(x0, trackTop(trackIndex), std::max(2, x1 - x0), kTrackH);
}

int TimelineWidget::trackAtY(int y) const
{
    for (int i = 0; i < m_tracks.size(); ++i) {
        const int top = trackTop(i);
        if (y >= top - kRowGap / 2 && y < top + kTrackH + kRowGap)
            return i;
    }
    return -1;
}

TimelineWidget::Hit TimelineWidget::hitTest(QPoint pos) const
{
    Hit h;
    if (pos.y() < kRulerH) {
        h.kind = pos.x() < kHeaderW ? Hit::None : Hit::Ruler;
        return h;
    }
    const bool inHeader = pos.x() < kHeaderW;
    if (pos.y() >= mainTop() - kRowGap / 2 && pos.y() < mainTop() + kMainH + kRowGap / 2) {
        if (inHeader) {
            h.kind = Hit::Header;
            return h;
        }
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].rect.adjusted(0, 0, 1, 0).contains(pos)) {
                h.kind = Hit::MainClip;
                h.row = i;
                return h;
            }
        h.kind = Hit::MainEmpty;
        return h;
    }
    const int t = trackAtY(pos.y());
    if (t < 0)
        return h;
    h.track = t;
    if (inHeader) {
        h.kind = Hit::Header;
        return h;
    }
    // Topmost (last drawn) first.
    const QVector<TrackClipView> &clips = m_tracks[t].clips;
    for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i)
        if (trackClipRect(t, clips[i]).adjusted(-1, 0, 1, 0).contains(pos.x(), trackTop(t) + 1)) {
            h.kind = Hit::TrackClip;
            h.clip = i;
            return h;
        }
    h.kind = Hit::TrackEmpty;
    return h;
}

int TimelineWidget::insertIndexAt(qint64 ms) const
{
    int index = 0;
    while (index < m_rows.size() && m_rows[index].startMs + m_rows[index].durMs / 2 <= ms)
        ++index;
    return index;
}

int TimelineWidget::trackGapAtY(int y) const
{
    int gap = 0;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (y > trackTop(i) + kTrackH / 2)
            gap = i + 1;
    return gap;
}

int TimelineWidget::dropIndexForX(int x) const
{
    int gap = 0;
    for (const Row &r : m_rows) {
        if (x >= (r.rect.left() + r.rect.right()) / 2)
            ++gap;
        else
            break;
    }
    return gap;
}

qint64 TimelineWidget::snapMs(qint64 ms, int excludeClipId) const
{
    const int x = xForTimelineMs(ms);
    qint64 best = ms;
    int bestPx = kSnapPx + 1;
    auto consider = [&](qint64 t) {
        const int d = std::abs(xForTimelineMs(t) - x);
        if (d < bestPx) {
            bestPx = d;
            best = t;
        }
    };
    consider(0);
    consider(m_playheadMs);
    for (const Row &r : m_rows)
        consider(r.startMs + r.durMs);
    for (const TrackView &t : m_tracks)
        for (const TrackClipView &c : t.clips)
            if (c.id != excludeClipId) {
                consider(c.startMs);
                consider(c.endMs());
            }
    return best;
}

double TimelineWidget::msAtX(double x) const
{
    return m_viewStartMs + (x - contentLeft()) * double(visibleSpanMs()) / contentWidth();
}

void TimelineWidget::drawWaveform(QPainter &p, const QRect &rc, const Peaks &peaks, qint64 startMs,
                                  qint64 trimInMs, double speed, double gain, qint64 fadeInMs,
                                  qint64 fadeOutMs, qint64 spanMs, const QColor &color) const
{
    if (!peaks || peaks->empty() || rc.height() < 4)
        return;
    // dB scale: -36 dBFS at the centre line, 0 dBFS filling the height -- a
    // quiet mic still shows, and loud, even footage (a motorcycle ride sits
    // around -15..-6 dBFS) still has some shape.
    constexpr double kFloorDb = -36.0;
    const int x0 = std::max(rc.left() + 1, contentLeft());
    const int x1 = std::min(rc.right() - 1, contentLeft() + contentWidth());
    const double cy = rc.center().y() + 0.5;
    const double half = (rc.height() - 4) / 2.0;
    const qint64 n = static_cast<qint64>(peaks->size());
    p.setPen(QPen(color, 1));
    for (int x = x0; x <= x1; ++x) {
        const double l0 = msAtX(x) - startMs, l1 = msAtX(x + 1) - startMs; // clip-local, timeline
        if (l1 <= 0 || l0 >= spanMs)
            continue;
        const double s0 = trimInMs + std::max(0.0, l0) * speed; // source ms
        const double s1 = trimInMs + std::min<double>(spanMs, l1) * speed;
        qint64 i0 = static_cast<qint64>(std::floor(s0 * kPeaksPerSec / 1000.0));
        const qint64 i1 = std::max(i0 + 1, static_cast<qint64>(std::ceil(s1 * kPeaksPerSec / 1000.0)));
        float peak = 0.0f;
        for (i0 = std::max<qint64>(i0, 0); i0 < std::min(i1, n); ++i0)
            peak = std::max(peak, (*peaks)[size_t(i0)]);
        // The clip's level and fades, as they'll be heard.
        const double mid = (l0 + l1) / 2.0;
        double env = gain;
        if (fadeInMs > 0 && mid < fadeInMs)
            env *= std::max(0.0, mid / fadeInMs);
        if (fadeOutMs > 0 && mid > spanMs - fadeOutMs)
            env *= std::max(0.0, (spanMs - mid) / fadeOutMs);
        const double v = peak * env;
        if (v <= 0.0)
            continue;
        const double h = std::clamp((20.0 * std::log10(v) - kFloorDb) / -kFloorDb, 0.0, 1.0) * half;
        if (h >= 0.5)
            p.drawLine(QPointF(x + 0.5, cy - h), QPointF(x + 0.5, cy + h));
    }
}

QStringList TimelineWidget::droppableFiles(const QMimeData *mime, bool video)
{
    static const QStringList kAudioExt = {QStringLiteral("mp3"),  QStringLiteral("wav"),
                                          QStringLiteral("flac"), QStringLiteral("aac"),
                                          QStringLiteral("m4a"),  QStringLiteral("ogg"),
                                          QStringLiteral("opus")};
    // Flat video for an overlay; not .insv / .lrv (360 footage).
    static const QStringList kVideoExt = {QStringLiteral("mp4"), QStringLiteral("mov"),
                                          QStringLiteral("mkv"), QStringLiteral("m4v"),
                                          QStringLiteral("webm"), QStringLiteral("avi")};
    const QStringList &ext = video ? kVideoExt : kAudioExt;
    QStringList out;
    if (!mime || !mime->hasUrls())
        return out;
    for (const QUrl &u : mime->urls())
        if (u.isLocalFile() && ext.contains(QFileInfo(u.toLocalFile()).suffix().toLower()))
            out << u.toLocalFile();
    return out;
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent *e)
{
    // Audio and overlays need a video track to lie over / under (the main
    // track sets the length).
    const bool audio = !droppableFiles(e->mimeData(), false).isEmpty();
    const bool video = !droppableFiles(e->mimeData(), true).isEmpty();
    if (m_rows.isEmpty() || (!audio && !video)) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    m_dropActive = true;
    m_dropVideo = video && !audio; // a mix goes by the audio rules
    dragMoveEvent(e);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent *e)
{
    if (!m_dropActive)
        return;
    const QPoint pos = e->position().toPoint();
    m_dropTrack = trackAtY(pos.y());
    if (m_dropTrack >= 0 && m_tracks[m_dropTrack].video != m_dropVideo)
        m_dropTrack = -1; // not a track of the files' kind
    m_dropMs = std::min(snapMs(timelineMsAt(pos.x()), -1), m_totalMs);
    e->acceptProposedAction();
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropActive = false;
    update();
}

void TimelineWidget::dropEvent(QDropEvent *e)
{
    const QStringList audio = droppableFiles(e->mimeData(), false);
    const QStringList video = droppableFiles(e->mimeData(), true);
    const bool active = m_dropActive;
    m_dropActive = false;
    update();
    if (!active || (audio.isEmpty() && video.isEmpty()))
        return;
    e->acceptProposedAction();
    const int trackId = m_dropTrack >= 0 && m_dropTrack < m_tracks.size() ? m_tracks[m_dropTrack].id : 0;
    if (!audio.isEmpty())
        emit audioFilesDropped(audio, m_dropVideo ? 0 : trackId, m_dropMs);
    if (!video.isEmpty())
        emit videoFilesDropped(video, m_dropVideo ? trackId : 0, m_dropMs);
}

// ---------------------------------------------------------------------------

void TimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base).darker(115));
    const QFontMetrics fm(font());

    // Everything in timeline time is clipped to the content area (a zoomed
    // view has clips running off both sides); the headers are drawn last,
    // unclipped.
    const int cLeft = contentLeft();
    const int usableW = contentWidth();
    const int rowsBottom = scrollBarTop();
    p.setClipRect(QRect(cLeft, 0, usableW + kMarginX, rowsBottom));

    // Ruler ticks, over the visible span.
    p.setPen(palette().color(QPalette::Mid));
    if (m_extentMs > 0) {
        const qint64 span = visibleSpanMs();
        const qint64 step = tickIntervalMs(span, usableW);
        for (qint64 t = m_viewStartMs / step * step; t <= std::min(m_extentMs, m_viewStartMs + span);
             t += step) {
            const int x = xForTimelineMs(t);
            p.drawLine(x, kRulerH - 4, x, kRulerH);
            const QString label = formatTime(t, step < 1000);
            int lx = x + 2;
            if (lx + fm.horizontalAdvance(label) > width())
                lx = x - 2 - fm.horizontalAdvance(label);
            p.drawText(lx, kRulerH - 4, label);
        }
    }

    // Row backgrounds.
    const QColor headerBg = palette().color(QPalette::Window).darker(108);
    const QColor rowBg = palette().color(QPalette::Base).darker(125);
    p.fillRect(QRect(cLeft, mainTop(), usableW, kMainH), rowBg);
    for (int i = 0; i < m_tracks.size(); ++i)
        p.fillRect(QRect(cLeft, trackTop(i), usableW, kTrackH), rowBg);

    // Past the end of the main track: audio there isn't played.
    if (m_extentMs > m_totalMs) {
        const int x = std::max(cLeft, xForTimelineMs(m_totalMs));
        p.fillRect(QRect(x, kRulerH, width() - x, rowsBottom - kRulerH), QColor(0, 0, 0, 70));
    }

    if (m_rows.isEmpty() && m_tracks.isEmpty()) {
        p.setPen(palette().color(QPalette::Mid));
        p.drawText(QRect(cLeft, mainTop(), usableW, kMainH), Qt::AlignCenter,
                   tr("No clips — open a clip or add one to the timeline"));
    }

    // Main-track clip blocks.
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &r = m_rows[i];
        QRect rc = r.rect;
        // The clip under the playhead reads as selected -- unless an audio clip
        // is: then that one alone is highlighted.
        const bool active = r.clip.id == m_activeClipId && m_selectedAudioId <= 0;

        // Provisional geometry while an edge of this row is being dragged. In
        // a gapless timeline the clip's left edge stays anchored; both trims
        // just change its length (and everything after would shift on commit).
        if ((m_drag == Drag::TrimIn || m_drag == Drag::TrimOut) && m_dragRow == i) {
            const qint64 newSrcDur = m_drag == Drag::TrimIn ? r.clip.trimOutMs - m_provTrimMs
                                                            : m_provTrimMs - r.clip.trimInMs;
            const qint64 newDur = std::llround(newSrcDur / r.clip.speed);
            rc.setLeft(xForTimelineMs(r.startMs));
            rc.setRight(xForTimelineMs(r.startMs + std::max<qint64>(0, newDur)));
        }

        p.setBrush(clipFill(r.clip.id, active, r.clip.missing));
        p.setPen(QPen(active ? palette().color(QPalette::Highlight)
                             : palette().color(QPalette::Dark),
                      active ? 2 : 1));
        p.drawRect(rc.adjusted(0, 0, -1, -1));
        // Its camera audio, faint, in the lower part (to line a detached mic
        // up against).
        drawWaveform(p, QRect(rc.left(), rc.top() + rc.height() / 2, rc.width(), rc.height() / 2 - 1),
                     r.clip.peaks, r.startMs, r.clip.trimInMs, r.clip.speed, r.clip.gain, 0, 0,
                     r.durMs, QColor(255, 255, 255, 60));

        // Label.
        p.setPen(palette().color(QPalette::BrightText));
        // A hollow diamond marks a clip without reframe keys: it exports a
        // static forward view, whatever the viewport showed.
        QString label = (r.clip.missing ? QStringLiteral("⚠ ") : QString()) +
                        (r.clip.keySourceMs.isEmpty() ? QStringLiteral("◇ ") : QString()) +
                        r.clip.label;
        if (r.clip.speed != 1.0)
            label += QStringLiteral("  %1×").arg(r.clip.speed);
        // On the visible part, so a clip that starts off screen keeps its name.
        const QRect vis = rc.intersected(QRect(cLeft, 0, usableW, height()));
        p.drawText(vis.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideMiddle, vis.width() - 10));

        // Keyframe diamonds.
        if (!r.clip.keySourceMs.isEmpty() && r.durMs > 0) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(palette().color(QPalette::BrightText));
            p.setPen(Qt::NoPen);
            const int cy = rc.bottom() - 5;
            for (qint64 ks : r.clip.keySourceMs) {
                if (ks < r.clip.trimInMs || ks > r.clip.trimOutMs)
                    continue;
                const int x = xForTimelineMs(r.startMs + std::llround((ks - r.clip.trimInMs) /
                                                                       r.clip.speed));
                const QPointF d[4] = {{double(x), double(cy - 3)},
                                      {double(x + 3), double(cy)},
                                      {double(x), double(cy + 3)},
                                      {double(x - 3), double(cy)}};
                p.drawPolygon(d, 4);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }

    // Audio clips.
    const bool audioDrag = m_drag == Drag::AudioMove || m_drag == Drag::AudioTrimIn ||
                           m_drag == Drag::AudioTrimOut;
    auto drawAudio = [&](int trackIndex, const TrackClipView &c, bool ghost) {
        const QRect rc = trackClipRect(trackIndex, c);
        const bool sel = c.id == m_selectedAudioId;
        QColor fill = m_tracks[trackIndex].video
                          ? overlayFill(sel, m_tracks[trackIndex].muted, c.missing)
                          : audioFill(sel, c.muted || m_tracks[trackIndex].muted, c.missing);
        if (ghost)
            fill.setAlpha(170);
        p.setBrush(fill);
        p.setPen(QPen(sel ? palette().color(QPalette::Highlight) : palette().color(QPalette::Dark),
                      sel ? 2 : 1));
        p.drawRect(rc.adjusted(0, 0, -1, -1));
        drawWaveform(p, rc.adjusted(0, 1, 0, -1), c.peaks, c.startMs, c.trimInMs, c.speed,
                     c.muted ? 0.0 : c.gain, c.fadeInMs, c.fadeOutMs, c.spanMs(),
                     QColor(255, 255, 255, 90));
        // Fade ramps: a line from silence (transparent) at the edge up to full.
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(255, 255, 255, 150), 1));
        if (c.fadeInMs > 0)
            p.drawLine(rc.left(), rc.bottom(), xForTimelineMs(c.startMs + c.fadeInMs), rc.top() + 1);
        if (c.fadeOutMs > 0)
            p.drawLine(xForTimelineMs(c.endMs() - c.fadeOutMs), rc.top() + 1, rc.right(), rc.bottom());
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(palette().color(QPalette::BrightText));
        QString label = (c.missing ? QStringLiteral("⚠ ") : QString()) +
                        (c.linked ? QStringLiteral("🔗 ") : QString()) + c.label;
        if (c.speed != 1.0)
            label += QStringLiteral("  %1×").arg(c.speed);
        const QRect vis = rc.intersected(QRect(cLeft, 0, usableW, height()));
        p.drawText(vis.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideMiddle, std::max(0, vis.width() - 10)));
        // An overlay's keyframe diamonds, like the main track's reframe keys.
        if (!c.keySourceMs.isEmpty()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(palette().color(QPalette::BrightText));
            p.setPen(Qt::NoPen);
            const int cy = rc.bottom() - 4;
            for (qint64 ks : c.keySourceMs) {
                if (ks < c.trimInMs || ks > c.trimOutMs)
                    continue;
                const int x = xForTimelineMs(c.startMs + std::llround((ks - c.trimInMs) / c.speed));
                const QPointF d[4] = {{double(x), double(cy - 3)},
                                      {double(x + 3), double(cy)},
                                      {double(x), double(cy + 3)},
                                      {double(x - 3), double(cy)}};
                p.drawPolygon(d, 4);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    };
    for (int t = 0; t < m_tracks.size(); ++t)
        for (const TrackClipView &c : m_tracks[t].clips)
            if (!(audioDrag && m_dragMoved && c.id == m_dragAudio.id))
                drawAudio(t, c, false);
    if (audioDrag && m_dragMoved && m_provTrack >= 0 && m_provTrack < m_tracks.size())
        drawAudio(m_provTrack, m_dragAudio, true);

    // Reorder insertion caret.
    if (m_drag == Drag::Reorder && m_dragMoved && m_reorderTarget >= 0 && !m_rows.isEmpty()) {
        const int x = m_reorderTarget < m_rows.size()
                          ? m_rows[m_reorderTarget].rect.left()
                          : m_rows.last().rect.right();
        p.setPen(QPen(palette().color(QPalette::Highlight), 2));
        p.drawLine(x, mainTop(), x, mainTop() + kMainH);
    }

    // Playhead.
    const int px = xForTimelineMs(m_playheadMs);
    p.setPen(QPen(QColor(230, 70, 70), 1));
    p.drawLine(px, 0, px, rowsBottom);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(QColor(230, 70, 70));
    p.setPen(Qt::NoPen);
    const QPointF tri[3] = {{double(px - 4), 0.0}, {double(px + 4), 0.0}, {double(px), 6.0}};
    p.drawPolygon(tri, 3);
    p.setRenderHint(QPainter::Antialiasing, false);

    // A file drag: where it would land.
    if (m_dropActive) {
        const int x = xForTimelineMs(m_dropMs);
        const QColor hl = palette().color(QPalette::Highlight);
        int top = mainTop() + kMainH, bottom = rowsBottom;
        if (m_dropVideo) { // over the main track
            top = kRulerH;
            bottom = mainTop() + kMainH;
        }
        if (m_dropTrack >= 0) {
            top = trackTop(m_dropTrack);
            bottom = top + kTrackH;
            QColor band = hl;
            band.setAlpha(50);
            p.fillRect(QRect(cLeft, top, usableW, kTrackH), band);
        }
        p.setPen(QPen(hl, 2));
        p.drawLine(x, top, x, bottom);
    }

    // Track headers, over anything scrolled under them.
    p.setClipping(false);
    p.fillRect(QRect(0, 0, kHeaderW + kMarginX, kRulerH), palette().color(QPalette::Base).darker(115));
    const auto drawMuteBox = [&](const QRect &mb, bool on, const QString &letter) {
        p.setBrush(on ? QColor(200, 150, 40) : Qt::transparent);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(mb.adjusted(0, 0, -1, -1));
        p.setPen(on ? QColor(20, 20, 20) : palette().color(QPalette::WindowText));
        p.drawText(mb, Qt::AlignCenter, letter);
    };
    p.fillRect(QRect(0, mainTop(), kHeaderW, kMainH), headerBg);
    const QRect mainMb = muteBox(mainTop(), kMainH);
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRect(6, mainTop(), mainMb.left() - 8, kMainH), Qt::AlignVCenter | Qt::AlignLeft,
               tr("Video"));
    drawMuteBox(mainMb, m_mainMuted, QStringLiteral("M"));
    for (int i = 0; i < m_tracks.size(); ++i) {
        const TrackView &t = m_tracks[i];
        const int top = trackTop(i);
        const bool dragged = m_drag == Drag::TrackMove && m_dragMoved && i == m_dragTrackIndex;
        p.fillRect(QRect(0, top, kHeaderW, kTrackH),
                   dragged ? palette().color(QPalette::Highlight).darker(150) : headerBg);
        const QRect mb = muteBox(top, kTrackH);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRect(6, top, mb.left() - 8, kTrackH), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(t.name, Qt::ElideRight, mb.left() - 8));
        drawMuteBox(mb, t.muted, t.video ? QStringLiteral("H") : QStringLiteral("M"));
    }
    // Where a dragged track header would land.
    if (m_drag == Drag::TrackMove && m_dragMoved && m_trackGap >= 0) {
        const bool video = m_dragTrackIndex >= 0 && m_dragTrackIndex < m_tracks.size() &&
                           m_tracks[m_dragTrackIndex].video;
        int y = m_trackGap < m_tracks.size() ? trackTop(m_trackGap) - kRowGap / 2 - 1
                                             : trackTop(m_tracks.size()) - kRowGap / 2;
        if (video && m_trackGap == m_videoTracks) // below the last overlay track
            y = mainTop() - kRowGap / 2 - 1;
        p.setPen(QPen(palette().color(QPalette::Highlight), 2));
        p.drawLine(0, y, width(), y);
    }
}

// ---------------------------------------------------------------------------

void TimelineWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_pressPos = m_lastDragPos = e->pos();
    m_pressMs = timelineMsAt(e->pos().x());
    m_dragMoved = false;
    m_dragRow = -1;
    m_reorderTarget = -1;
    m_drag = Drag::None;

    const Hit h = hitTest(e->pos());
    switch (h.kind) {
    case Hit::Header:
        if (h.track < 0) { // the main video track
            if (muteBox(mainTop(), kMainH).contains(e->pos()))
                emit mainMuteToggled(!m_mainMuted);
            return;
        }
        if (muteBox(trackTop(h.track), kTrackH).contains(e->pos())) {
            emit trackMuteToggled(m_tracks[h.track].id, !m_tracks[h.track].muted);
            return;
        }
        m_drag = Drag::TrackMove; // a drag up / down reorders
        m_dragTrackIndex = h.track;
        m_trackGap = -1;
        return;
    case Hit::MainClip: {
        m_dragRow = h.row;
        emit clipSelected(m_rows[h.row].clip.id);
        const QRect &rc = m_rows[m_dragRow].rect;
        if (std::abs(e->pos().x() - rc.left()) <= kEdgeGrabPx) {
            m_drag = Drag::TrimIn;
            m_provTrimMs = m_rows[m_dragRow].clip.trimInMs;
            return;
        }
        if (std::abs(e->pos().x() - rc.right()) <= kEdgeGrabPx) {
            m_drag = Drag::TrimOut;
            m_provTrimMs = m_rows[m_dragRow].clip.trimOutMs;
            return;
        }
        m_drag = Drag::Reorder; // a click without a drag just selects (no seek)
        return;
    }
    case Hit::TrackClip: {
        const TrackClipView &c = m_tracks[h.track].clips[h.clip];
        emit trackClipSelected(c.id);
        m_dragAudioOrig = m_dragAudio = c;
        m_dragAudioTrack = m_provTrack = h.track;
        m_provStartMs = c.startMs;
        const QRect rc = trackClipRect(h.track, c);
        if (std::abs(e->pos().x() - rc.left()) <= kEdgeGrabPx)
            m_drag = Drag::AudioTrimIn;
        else if (std::abs(e->pos().x() - rc.right()) <= kEdgeGrabPx)
            m_drag = Drag::AudioTrimOut;
        else
            m_drag = Drag::AudioMove;
        return;
    }
    case Hit::Ruler: // the only place that moves the playhead
        // The exact frame at once: a scrub would show a keyframe first.
        m_drag = Drag::Seek;
        emit seekRequested(std::min(timelineMsAt(e->pos().x()), m_totalMs));
        return;
    case Hit::MainEmpty:
    case Hit::TrackEmpty: // just deselect
        emit trackClipSelected(-1);
        emit clipSelected(0);
        return;
    default:
        return;
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_drag == Drag::None) {
        // Hover cursor hint over an edge.
        const Hit h = hitTest(e->pos());
        Qt::CursorShape shape = Qt::ArrowCursor;
        QRect rc;
        if (h.kind == Hit::MainClip)
            rc = m_rows[h.row].rect;
        else if (h.kind == Hit::TrackClip)
            rc = trackClipRect(h.track, m_tracks[h.track].clips[h.clip]);
        if (rc.isValid() && (std::abs(e->pos().x() - rc.left()) <= kEdgeGrabPx ||
                             std::abs(e->pos().x() - rc.right()) <= kEdgeGrabPx))
            shape = Qt::SizeHorCursor;
        else if (h.kind == Hit::Header && h.track >= 0 &&
                 !muteBox(trackTop(h.track), kTrackH).contains(e->pos()))
            shape = Qt::OpenHandCursor; // drag to reorder, double-click to rename
        setCursor(shape);
        return;
    }

    if ((e->pos() - m_pressPos).manhattanLength() > kDragThresholdPx)
        m_dragMoved = true;
    m_lastDragPos = e->pos();
    dragTo(e->pos());

    // Zoomed in and dragging past an edge of the view: scroll while it's
    // held there (a header drag moves vertically only).
    const bool outside = e->pos().x() < contentLeft() ||
                         e->pos().x() > contentLeft() + contentWidth();
    if (zoomed() && m_drag != Drag::TrackMove && outside && !m_autoScroll->isActive())
        m_autoScroll->start();
}

void TimelineWidget::autoScrollTick()
{
    const QPoint pos = m_lastDragPos;
    const int left = contentLeft(), right = contentLeft() + contentWidth();
    if (m_drag == Drag::None || m_drag == Drag::TrackMove || !zoomed() ||
        (pos.x() >= left && pos.x() <= right)) {
        m_autoScroll->stop();
        return;
    }
    // Faster the further past the edge, up to ~a fifth of the view a second.
    const int over = pos.x() < left ? pos.x() - left : pos.x() - right;
    const int px = std::clamp(std::abs(over), 4, 40) * (over < 0 ? -1 : 1);
    scrollToMs(m_viewStartMs + std::llround(double(px) * visibleSpanMs() / contentWidth()));
    dragTo(pos);
}

void TimelineWidget::dragTo(QPoint pos)
{
    switch (m_drag) {
    case Drag::Seek:
        if (m_dragMoved) // a click's jitter isn't a drag
            emit scrubRequested(std::min(timelineMsAt(pos.x()), m_totalMs));
        break;
    case Drag::TrimIn: {
        const Row &r = m_rows[m_dragRow];
        const qint64 newStart = timelineMsAt(pos.x());
        const qint64 deltaSrc = std::llround((newStart - r.startMs) * r.clip.speed);
        m_provTrimMs = std::clamp<qint64>(r.clip.trimInMs + deltaSrc, 0,
                                          r.clip.trimOutMs - kMinTrimMs);
        update();
        break;
    }
    case Drag::TrimOut: {
        const Row &r = m_rows[m_dragRow];
        const qint64 newDur = std::max<qint64>(
            kMinTrimMs, std::llround((timelineMsAt(pos.x()) - r.startMs) * r.clip.speed));
        m_provTrimMs = std::clamp<qint64>(r.clip.trimInMs + newDur,
                                          r.clip.trimInMs + kMinTrimMs, r.clip.sourceDurationMs);
        update();
        break;
    }
    case Drag::Reorder:
        if (m_dragMoved) {
            m_reorderTarget = dropIndexForX(pos.x());
            update();
        }
        break;
    case Drag::AudioMove: {
        if (!m_dragMoved)
            break;
        const TrackClipView &o = m_dragAudioOrig;
        const qint64 delta = o.linked ? 0 // locked in time to its video clip
                                      : timelineMsAt(pos.x()) - m_pressMs;
        qint64 start = std::max<qint64>(0, o.startMs + delta);
        // Snap whichever edge lands closer to something.
        const qint64 s0 = o.linked ? start : snapMs(start, o.id);
        const qint64 s1 = o.linked ? start : snapMs(start + o.spanMs(), o.id) - o.spanMs();
        if (s0 != start && (s1 == start || std::abs(s0 - start) <= std::abs(s1 - start)))
            start = s0;
        else if (s1 != start)
            start = std::max<qint64>(0, s1);
        m_provStartMs = start;
        m_dragAudio.startMs = start;
        const int t = trackAtY(pos.y());
        if (t >= 0 && m_tracks[t].video == m_tracks[m_dragAudioTrack].video)
            m_provTrack = t; // only onto a track of its kind
        update();
        break;
    }
    case Drag::AudioTrimIn: {
        const TrackClipView &o = m_dragAudioOrig;
        const qint64 newStart = snapMs(timelineMsAt(pos.x()), o.id);
        const qint64 lo = std::max<qint64>(0, o.trimInMs - std::llround(o.startMs * o.speed));
        const qint64 in = std::clamp<qint64>(
            o.trimInMs + std::llround((newStart - o.startMs) * o.speed), lo, o.trimOutMs - kMinTrimMs);
        m_dragAudio.trimInMs = in;
        m_dragAudio.startMs = o.startMs + std::llround((in - o.trimInMs) / o.speed);
        update();
        break;
    }
    case Drag::AudioTrimOut: {
        const TrackClipView &o = m_dragAudioOrig;
        const qint64 newEnd = snapMs(timelineMsAt(pos.x()), o.id);
        const qint64 hi = o.sourceDurationMs > 0 ? o.sourceDurationMs : o.trimOutMs;
        m_dragAudio.trimOutMs = std::clamp<qint64>(
            o.trimInMs + std::llround((newEnd - o.startMs) * o.speed), o.trimInMs + kMinTrimMs,
            std::max(hi, o.trimInMs + kMinTrimMs));
        update();
        break;
    }
    case Drag::TrackMove:
        if (m_dragMoved) {
            // Only among tracks of its own kind.
            const bool video = m_tracks[m_dragTrackIndex].video;
            m_trackGap = std::clamp(trackGapAtY(pos.y()), video ? 0 : m_videoTracks,
                                    video ? m_videoTracks : int(m_tracks.size()));
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        break;
    default:
        break;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    const Drag drag = m_drag;
    const int row = m_dragRow;
    m_drag = Drag::None;
    m_autoScroll->stop();

    switch (drag) {
    case Drag::Seek:
        // A drag may have shown keyframes; land on the exact frame. A click
        // already sought on press.
        if (m_dragMoved)
            emit seekRequested(std::min(timelineMsAt(e->pos().x()), m_totalMs));
        break;
    case Drag::TrimIn:
        if (m_dragMoved && row >= 0)
            emit clipTrimInRequested(m_rows[row].clip.id, m_provTrimMs);
        break;
    case Drag::TrimOut:
        if (m_dragMoved && row >= 0)
            emit clipTrimOutRequested(m_rows[row].clip.id, m_provTrimMs);
        break;
    case Drag::Reorder:
        if (m_dragMoved && row >= 0) {
            const int gap = dropIndexForX(e->pos().x());
            int to = gap > row ? gap - 1 : gap;
            to = std::clamp(to, 0, int(m_rows.size()) - 1);
            if (to != row)
                emit clipMoved(m_rows[row].clip.id, to);
        }
        break;
    case Drag::AudioMove:
        if (m_dragMoved && m_provTrack >= 0 && m_provTrack < m_tracks.size() &&
            (m_provTrack != m_dragAudioTrack || m_provStartMs != m_dragAudioOrig.startMs))
            emit trackClipMoved(m_dragAudioOrig.id, m_tracks[m_provTrack].id, m_provStartMs);
        break;
    case Drag::AudioTrimIn:
        if (m_dragMoved && m_dragAudio.trimInMs != m_dragAudioOrig.trimInMs)
            emit trackClipTrimInRequested(m_dragAudioOrig.id, m_dragAudio.trimInMs);
        break;
    case Drag::AudioTrimOut:
        if (m_dragMoved && m_dragAudio.trimOutMs != m_dragAudioOrig.trimOutMs)
            emit trackClipTrimOutRequested(m_dragAudioOrig.id, m_dragAudio.trimOutMs);
        break;
    case Drag::TrackMove:
        setCursor(Qt::ArrowCursor);
        if (m_dragMoved && m_dragTrackIndex >= 0 && m_dragTrackIndex < m_tracks.size() &&
            m_trackGap >= 0) {
            const int to = m_trackGap > m_dragTrackIndex ? m_trackGap - 1 : m_trackGap;
            if (to != m_dragTrackIndex)
                emit trackMoved(m_tracks[m_dragTrackIndex].id, to);
        }
        break;
    default:
        break;
    }
    m_reorderTarget = -1;
    m_trackGap = -1;
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    const Hit h = hitTest(e->pos());
    if (e->button() == Qt::LeftButton && h.kind == Hit::Header && h.track >= 0 &&
        !muteBox(trackTop(h.track), kTrackH).contains(e->pos())) {
        m_drag = Drag::None;
        startRename(h.track);
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TimelineWidget::startRename(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size())
        return;
    if (!m_renameEdit) {
        m_renameEdit = new QLineEdit(this);
        // Enter / Escape are this box's own (MainWindow's app-wide filter
        // otherwise turns them into "leave the field").
        m_renameEdit->setProperty("ownsReturnEscape", true);
        m_renameEdit->installEventFilter(this);
        connect(m_renameEdit, &QLineEdit::editingFinished, this, &TimelineWidget::finishRename);
    }
    m_renameTrackId = m_tracks[trackIndex].id;
    m_renameCancelled = false;
    m_renameEdit->setText(m_tracks[trackIndex].name);
    m_renameEdit->setGeometry(2, trackTop(trackIndex) + 1, std::max(kHeaderW + 80, 170), kTrackH - 2);
    m_renameEdit->show();
    m_renameEdit->raise();
    m_renameEdit->setFocus();
    m_renameEdit->selectAll();
}

void TimelineWidget::finishRename()
{
    const int id = m_renameTrackId;
    if (!id)
        return; // already finished (hiding the box loses focus: a second editingFinished)
    m_renameTrackId = 0;
    const QString name = m_renameEdit->text();
    m_renameEdit->hide();
    if (!m_renameCancelled)
        emit trackRenameRequested(id, name);
}

bool TimelineWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        const auto *he = static_cast<QHelpEvent *>(e);
        const Hit h = hitTest(he->pos());
        if (h.kind == Hit::MainClip && m_rows[h.row].clip.keySourceMs.isEmpty())
            QToolTip::showText(he->globalPos(),
                               tr("No reframe keyframes: this clip exports looking straight "
                                  "forward. Arm reframe authoring and set a key to frame it."),
                               this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

bool TimelineWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_renameEdit && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        m_renameCancelled = true;
        finishRename();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent *e)
{
    const Hit h = hitTest(e->pos());
    QMenu menu(this);
    const qint64 atMs = timelineMsAt(e->pos().x());

    if (h.kind == Hit::MainClip) {
        emit clipSelected(m_rows[h.row].clip.id);
        const Row &r = m_rows[h.row];
        const qint64 splitSrc = r.clip.trimInMs + std::llround((atMs - r.startMs) * r.clip.speed);
        QAction *split = menu.addAction(tr("Split here"));
        split->setEnabled(splitSrc > r.clip.trimInMs + kMinTrimMs
                          && splitSrc < r.clip.trimOutMs - kMinTrimMs);

        // Playback speed. Slow motion is the main use: 4K 120 fps at 0.25x
        // shows every source frame at a 30 fps output.
        QMenu *speedMenu = menu.addMenu(tr("Speed"));
        QList<QAction *> speedActions;
        for (double sp : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0}) {
            QAction *a = speedMenu->addAction(sp == 1.0 ? tr("1× (normal)")
                                                        : QStringLiteral("%1×").arg(sp));
            a->setCheckable(true);
            a->setChecked(std::abs(r.clip.speed - sp) < 1e-6);
            a->setData(sp);
            speedActions << a;
        }

        QAction *detach = nullptr;
        if (r.clip.hasOtherAudioStream)
            detach = menu.addAction(tr("Put the other mic on an audio track"));

        QAction *insertBefore = menu.addAction(tr("Insert clip before…"));
        QAction *insertAfter = menu.addAction(tr("Insert clip after…"));

        menu.addSeparator();
        QAction *copyGrade = menu.addAction(tr("Copy colour grade"));
        QAction *pasteGrade = menu.addAction(tr("Paste colour grade"));
        QAction *pasteGradeAll = menu.addAction(tr("Paste colour grade to all clips"));
        pasteGrade->setEnabled(m_canPasteGrade);
        pasteGradeAll->setEnabled(m_canPasteGrade);
        menu.addSeparator();
        QAction *copyStab = menu.addAction(tr("Copy stabilization"));
        QAction *pasteStab = menu.addAction(tr("Paste stabilization"));
        QAction *pasteStabAll = menu.addAction(tr("Paste stabilization to all clips"));
        pasteStab->setEnabled(m_canPasteStab);
        pasteStabAll->setEnabled(m_canPasteStab);
        menu.addSeparator();

        QAction *moveL = menu.addAction(tr("Move left"));
        moveL->setEnabled(h.row > 0);
        QAction *moveR = menu.addAction(tr("Move right"));
        moveR->setEnabled(h.row < m_rows.size() - 1);
        menu.addSeparator();
        QAction *remove = menu.addAction(tr("Remove clip"));

        QAction *chosen = menu.exec(e->globalPos());
        if (!chosen)
            return;
        if (chosen == split)
            emit clipSplitRequested(r.clip.id, splitSrc);
        else if (chosen == moveL)
            emit clipMoved(r.clip.id, h.row - 1);
        else if (chosen == moveR)
            emit clipMoved(r.clip.id, h.row + 1);
        else if (chosen == remove)
            emit clipRemoveRequested(r.clip.id);
        else if (chosen == detach)
            emit clipDetachAudioRequested(r.clip.id);
        else if (chosen == insertBefore)
            emit addClipRequested(h.row);
        else if (chosen == insertAfter)
            emit addClipRequested(h.row + 1);
        else if (chosen == copyGrade)
            emit gradeCopyRequested(r.clip.id);
        else if (chosen == pasteGrade)
            emit gradePasteRequested(r.clip.id);
        else if (chosen == pasteGradeAll)
            emit gradePasteAllRequested();
        else if (chosen == copyStab)
            emit stabilizationCopyRequested(r.clip.id);
        else if (chosen == pasteStab)
            emit stabilizationPasteRequested(r.clip.id);
        else if (chosen == pasteStabAll)
            emit stabilizationPasteAllRequested();
        else if (speedActions.contains(chosen))
            emit clipSpeedRequested(r.clip.id, chosen->data().toDouble());
        return;
    }

    if (h.kind == Hit::TrackClip) {
        const TrackClipView &c = m_tracks[h.track].clips[h.clip];
        emit trackClipSelected(c.id);
        const qint64 atSrc = c.trimInMs + std::llround((atMs - c.startMs) * c.speed);
        QAction *split = menu.addAction(tr("Split here"));
        split->setEnabled(atSrc > c.trimInMs + kMinTrimMs && atSrc < c.trimOutMs - kMinTrimMs);
        QAction *link = nullptr;
        if (c.linked)
            link = menu.addAction(tr("Unlink from video clip"));
        else if (c.linkable)
            link = menu.addAction(tr("Link to video clip"));
        QAction *sound = nullptr;
        if (c.hasSound)
            sound = menu.addAction(tr("Put its sound on an audio track"));
        const bool wasLinked = c.linked;
        menu.addSeparator();
        QAction *remove = menu.addAction(m_tracks[h.track].video ? tr("Remove overlay clip")
                                                                 : tr("Remove audio clip"));
        const int clipId = c.id; // `c` may not survive the menu's event loop
        QAction *chosen = menu.exec(e->globalPos());
        if (!chosen)
            return;
        if (chosen == split)
            emit trackClipSplitRequested(clipId, atMs);
        else if (chosen == link)
            emit audioClipLinkRequested(clipId, !wasLinked);
        else if (chosen == sound)
            emit overlaySoundRequested(clipId);
        else if (chosen == remove)
            emit trackClipRemoveRequested(clipId);
        return;
    }

    // On an extra track: files of its kind; elsewhere, both.
    const int trackIndex = h.track;
    const bool onVideo = trackIndex >= 0 && m_tracks[trackIndex].video;
    const bool onAudio = trackIndex >= 0 && !onVideo;
    QAction *addClip = nullptr, *addFile = nullptr, *addVideo = nullptr;
    if (trackIndex < 0)
        addClip = menu.addAction(tr("Add clip here…"));
    if (!onVideo) {
        addFile = menu.addAction(tr("Add audio file here…"));
        addFile->setEnabled(!m_rows.isEmpty());
    }
    if (!onAudio) {
        addVideo = menu.addAction(tr("Add video overlay here…"));
        addVideo->setEnabled(!m_rows.isEmpty());
    }
    QAction *addTrack = menu.addAction(tr("Add audio track"));
    QAction *addVideoTrack = menu.addAction(tr("Add overlay track"));
    QAction *rename = nullptr, *up = nullptr, *down = nullptr, *removeTrack = nullptr;
    if (trackIndex >= 0) {
        // Tracks move among their own kind.
        const int first = onVideo ? 0 : m_videoTracks;
        const int last = onVideo ? m_videoTracks - 1 : int(m_tracks.size()) - 1;
        menu.addSeparator();
        rename = menu.addAction(tr("Rename track…"));
        up = menu.addAction(tr("Move track up"));
        up->setEnabled(trackIndex > first);
        down = menu.addAction(tr("Move track down"));
        down->setEnabled(trackIndex < last);
        removeTrack = menu.addAction(tr("Remove track “%1”").arg(m_tracks[trackIndex].name));
    }
    menu.addSeparator();
    QAction *zin = menu.addAction(tr("Zoom in"));
    QAction *zout = menu.addAction(tr("Zoom out"));
    zout->setEnabled(zoomed());
    QAction *zfit = menu.addAction(tr("Zoom to fit"));
    zfit->setEnabled(zoomed());
    const int trackId = trackIndex >= 0 ? m_tracks[trackIndex].id : 0;
    QAction *chosen = menu.exec(e->globalPos());
    if (!chosen)
        return;
    if (chosen == addClip)
        emit addClipRequested(insertIndexAt(atMs));
    else if (chosen == addFile)
        emit addAudioFileRequested(onAudio ? trackId : 0, std::min(atMs, m_totalMs));
    else if (chosen == addVideo)
        emit addVideoFileRequested(onVideo ? trackId : 0, std::min(atMs, m_totalMs));
    else if (chosen == addTrack)
        emit addAudioTrackRequested();
    else if (chosen == addVideoTrack)
        emit addVideoTrackRequested();
    else if (chosen == rename)
        startRename(trackIndex);
    else if (chosen == up)
        emit trackMoved(trackId, trackIndex - 1);
    else if (chosen == down)
        emit trackMoved(trackId, trackIndex + 1);
    else if (chosen == removeTrack)
        emit trackRemoveRequested(trackId);
    else if (chosen == zin)
        zoomBy(kWheelZoom, e->pos().x() >= contentLeft() ? e->pos().x()
                                                         : contentLeft() + contentWidth() / 2);
    else if (chosen == zout)
        zoomBy(1.0 / kWheelZoom, e->pos().x() >= contentLeft() ? e->pos().x()
                                                               : contentLeft() + contentWidth() / 2);
    else if (chosen == zfit)
        zoomToFit();
}

#include "PlayerBar.h"
#include "Theme.h"

#include <QActionGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kHandleRadius = 5; // the playhead's dot on the progress strip
constexpr int kDragSlopPx = 3;   // a click's jitter isn't a drag

QString speedText(double speed)
{
    return QString::number(speed, 'g', 3) + QStringLiteral("×");
}

} // namespace

// ---------------------------------------------------------------------------
// PlayerProgress
// ---------------------------------------------------------------------------

PlayerProgress::PlayerProgress(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true); // the hover time
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize PlayerProgress::sizeHint() const
{
    return {400, 2 * kHandleRadius + 8};
}

QSize PlayerProgress::minimumSizeHint() const
{
    return {80, 2 * kHandleRadius + 8};
}

void PlayerProgress::setDuration(qint64 durationMs)
{
    m_durationMs = std::max<qint64>(0, durationMs);
    update();
}

void PlayerProgress::setPosition(qint64 positionMs)
{
    if (positionMs == m_positionMs)
        return;
    m_positionMs = positionMs;
    update();
}

void PlayerProgress::setCuts(const QList<qint64> &cutsMs)
{
    m_cutsMs = cutsMs;
    update();
}

QRect PlayerProgress::grooveRect() const
{
    const int h = m_hover || m_pressed ? 6 : 4;
    return {kHandleRadius, (height() - h) / 2, std::max(1, width() - 2 * kHandleRadius), h};
}

int PlayerProgress::xAt(qint64 ms) const
{
    const QRect g = grooveRect();
    if (m_durationMs <= 0)
        return g.left();
    const double f = std::clamp(double(ms) / double(m_durationMs), 0.0, 1.0);
    return g.left() + int(std::lround(f * g.width()));
}

qint64 PlayerProgress::timeAt(int x) const
{
    const QRect g = grooveRect();
    const double f = std::clamp(double(x - g.left()) / std::max(1, g.width()), 0.0, 1.0);
    return std::llround(f * double(m_durationMs));
}

void PlayerProgress::paintEvent(QPaintEvent *)
{
    const theme::Colors &tc = theme::colors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect g = grooveRect();
    const double radius = g.height() / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(tc.border);
    p.drawRoundedRect(g, radius, radius);
    const bool enabled = isEnabled() && m_durationMs > 0;
    const int px = xAt(m_positionMs);
    if (enabled && px > g.left()) {
        p.setBrush(tc.accent);
        p.drawRoundedRect(QRect(g.left(), g.top(), px - g.left(), g.height()), radius, radius);
    }
    // The cuts: short ticks across the groove.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(tc.textDim);
    for (const qint64 cut : std::as_const(m_cutsMs)) {
        if (cut <= 0 || cut >= m_durationMs)
            continue;
        p.drawRect(xAt(cut) - 1, g.top() - 3, 2, g.height() + 6);
    }
    if (enabled) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(m_hover || m_pressed ? tc.accent : tc.icon);
        const int r = m_hover || m_pressed ? kHandleRadius : kHandleRadius - 1;
        p.drawEllipse(QPoint(px, height() / 2), r, r);
    }
}

void PlayerProgress::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || m_durationMs <= 0) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_pressed = true;
    m_dragMoved = false;
    m_pressX = e->position().toPoint().x();
    // The exact frame at once, as on the ruler: a scrub would show a
    // keyframe first.
    emit seekRequested(timeAt(m_pressX));
    update();
}

void PlayerProgress::mouseMoveEvent(QMouseEvent *e)
{
    const int x = e->position().toPoint().x();
    if (!m_hover) {
        m_hover = true;
        update();
    }
    if (m_durationMs > 0)
        QToolTip::showText(e->globalPosition().toPoint(),
                           PlayerBar::formatTime(timeAt(x), m_fps),
                           this);
    if (!m_pressed)
        return;
    if (!m_dragMoved && std::abs(x - m_pressX) < kDragSlopPx)
        return;
    m_dragMoved = true;
    emit scrubRequested(timeAt(x));
}

void PlayerProgress::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_pressed || e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    m_pressed = false;
    // A drag may have shown keyframes; land on the exact frame. A click
    // already sought on press.
    if (m_dragMoved)
        emit seekRequested(timeAt(e->position().toPoint().x()));
    update();
}

void PlayerProgress::leaveEvent(QEvent *)
{
    m_hover = false;
    QToolTip::hideText();
    update();
}

// ---------------------------------------------------------------------------
// PlayerBar
// ---------------------------------------------------------------------------

const QList<double> &PlayerBar::speeds()
{
    static const QList<double> s{0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    return s;
}

QString PlayerBar::formatTime(qint64 ms, double fps)
{
    if (ms < 0)
        ms = 0;
    const qint64 totalSeconds = ms / 1000;
    const qint64 rest = ms % 1000;
    QString out = QStringLiteral("%1:%2:%3")
                      .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
                      .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
                      .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
    if (fps > 0.0) {
        const qint64 frame = qint64(std::floor(double(rest) * fps / 1000.0));
        out += QStringLiteral(":%1").arg(frame, fps > 100.0 ? 3 : 2, 10, QLatin1Char('0'));
    } else {
        out += QStringLiteral(".%1").arg(rest / 10, 2, 10, QLatin1Char('0'));
    }
    return out;
}

std::optional<qint64> PlayerBar::parseTime(const QString &text, double fps)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return std::nullopt;
    QStringList parts = t.split(QLatin1Char(':'));
    if (parts.size() > 4)
        return std::nullopt;
    const auto whole = [](const QString &s, bool *ok) {
        if (s.isEmpty()) {
            *ok = false;
            return qint64(0);
        }
        for (const QChar c : s)
            if (!c.isDigit()) {
                *ok = false;
                return qint64(0);
            }
        return s.toLongLong(ok);
    };

    bool ok = true;
    // hh:mm:ss:ff, as shown: the last field is frames.
    qint64 frames = 0;
    if (parts.size() == 4) {
        if (fps <= 0.0)
            return std::nullopt;
        frames = whole(parts.takeLast(), &ok);
        if (!ok || frames >= std::ceil(fps))
            return std::nullopt;
    }
    // Then [[h:]m:]s, the seconds with a decimal part (point or comma) if
    // there are no frames.
    QString last = parts.takeLast();
    last.replace(QLatin1Char(','), QLatin1Char('.'));
    double seconds = 0.0;
    const int dot = int(last.indexOf(QLatin1Char('.')));
    if (dot >= 0) {
        if (frames > 0 || t.count(QLatin1Char(':')) == 3)
            return std::nullopt;
        bool okFrac = true;
        const qint64 intPart = dot == 0 ? 0 : whole(last.left(dot), &ok);
        const QString frac = last.mid(dot + 1);
        if (!frac.isEmpty())
            whole(frac, &okFrac);
        if (!ok || !okFrac)
            return std::nullopt;
        seconds = double(intPart) + (frac.isEmpty() ? 0.0 : frac.toDouble() / std::pow(10.0, frac.size()));
    } else {
        seconds = double(whole(last, &ok));
    }
    qint64 h = 0;
    qint64 m = 0;
    if (ok && parts.size() == 2) {
        h = whole(parts[0], &ok);
        m = ok ? whole(parts[1], &ok) : 0;
        if (ok && m >= 60)
            return std::nullopt;
    } else if (ok && parts.size() == 1) {
        m = whole(parts[0], &ok);
    }
    // A field after the first stays under 60.
    if (!ok || (!parts.isEmpty() && seconds >= 60.0))
        return std::nullopt;

    qint64 ms = (h * 60 + m) * 60 * 1000 + std::llround(seconds * 1000.0);
    // The frame's start, rounded up so it reads back as the same frame.
    if (frames > 0)
        ms += qint64(std::ceil(double(frames) * 1000.0 / fps));
    return ms;
}

PlayerBar::PlayerBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("playerBar"));
    setAttribute(Qt::WA_StyledBackground); // the stylesheet's background
    const QSize kIconSize(18, 18);

    const auto makeButton = [this, kIconSize](const char *name, const QString &iconName,
                                              const QString &tip) {
        auto *b = new QToolButton(this);
        b->setObjectName(QLatin1String(name));
        b->setIcon(theme::icon(iconName));
        b->setIconSize(kIconSize);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };

    // --- top row: time, progress, speed ---
    m_timeButton = new QToolButton(this);
    m_timeButton->setObjectName(QStringLiteral("playerTime"));
    m_timeButton->setAutoRaise(true);
    m_timeButton->setToolTip(tr("Playhead / length, in hours:minutes:seconds:frames. Click to "
                                "type a time and jump there: as shown, or shorter -- 1:23 is "
                                "1 minute 23 seconds, 83.5 is seconds  ( Ctrl+G )"));
    QFont timeFont = m_timeButton->font();
    timeFont.setFeature(QFont::Tag("tnum"), 1); // digits that don't jitter
    m_timeButton->setFont(timeFont);
    connect(m_timeButton, &QToolButton::clicked, this, &PlayerBar::editTime);

    m_timeEdit = new QLineEdit(this);
    m_timeEdit->setObjectName(QStringLiteral("playerTimeEdit"));
    m_timeEdit->setFont(timeFont);
    m_timeEdit->setVisible(false);
    // Enter / Esc are its own (MainWindow's filter otherwise just drops focus).
    m_timeEdit->setProperty("ownsReturnEscape", true);
    m_timeEdit->installEventFilter(this);
    connect(m_timeEdit, &QLineEdit::returnPressed, this, [this] { finishTimeEdit(true); });

    m_progress = new PlayerProgress(this);
    m_progress->setObjectName(QStringLiteral("playerProgress"));
    connect(m_progress, &PlayerProgress::seekRequested, this, &PlayerBar::seekRequested);
    connect(m_progress, &PlayerProgress::scrubRequested, this, &PlayerBar::scrubRequested);

    m_speedButton = new QToolButton(this);
    m_speedButton->setObjectName(QStringLiteral("playerSpeed"));
    m_speedButton->setAutoRaise(true);
    m_speedButton->setPopupMode(QToolButton::InstantPopup);
    m_speedButton->setToolTip(tr("Playback speed  ( J / L: slower / faster ). Only for watching; "
                                 "the sound plays at 1× only."));
    auto *speedMenu = new QMenu(m_speedButton);
    speedMenu->setObjectName(QStringLiteral("playerSpeedMenu"));
    auto *speedGroup = new QActionGroup(speedMenu);
    for (const double s : speeds()) {
        QAction *a = speedMenu->addAction(speedText(s));
        a->setObjectName(QStringLiteral("playerSpeed%1Action").arg(int(std::lround(s * 100))));
        a->setData(s);
        a->setCheckable(true);
        a->setChecked(s == 1.0);
        speedGroup->addAction(a);
    }
    connect(speedGroup, &QActionGroup::triggered, this,
            [this](QAction *a) { setSpeed(a->data().toDouble()); });
    m_speedButton->setMenu(speedMenu);
    m_speedButton->setText(speedText(1.0));

    // --- bottom row: buffering, playback buttons, screenshot, volume, full screen ---
    m_bufferingLabel = new QLabel(tr("Buffering…"), this);
    m_bufferingLabel->setObjectName(QStringLiteral("playerBuffering"));
    m_bufferingLabel->setVisible(false);
    m_bufferingDelay = new QTimer(this);
    m_bufferingDelay->setSingleShot(true);
    m_bufferingDelay->setInterval(150);
    connect(m_bufferingDelay, &QTimer::timeout, this,
            [this] { m_bufferingLabel->setVisible(true); });

    QToolButton *toStart = makeButton("playerToStartButton", QStringLiteral("chevron-first"),
                                      tr("To the start  ( Home )"));
    QToolButton *prevCut = makeButton("playerPreviousCutButton", QStringLiteral("skip-back"),
                                      tr("Previous cut or clip edge  ( ↑ )"));
    QToolButton *frameBack = makeButton("playerFrameBackButton", QStringLiteral("chevron-left"),
                                        tr("One frame back  ( ← ; Shift+← one second )"));
    auto *play = makeButton("playerPlayButton", QStringLiteral("play"), tr("Play / pause  ( Space )"));
    play->setIcon(theme::icon(QStringLiteral("play"), theme::IconTone::Accent));
    play->setIconSize(QSize(22, 22));
    m_playButton = play;
    QToolButton *frameFwd = makeButton("playerFrameForwardButton", QStringLiteral("chevron-right"),
                                       tr("One frame forward  ( → ; Shift+→ one second )"));
    QToolButton *nextCut = makeButton("playerNextCutButton", QStringLiteral("skip-forward"),
                                      tr("Next cut or clip edge  ( ↓ )"));
    QToolButton *toEnd = makeButton("playerToEndButton", QStringLiteral("chevron-last"),
                                    tr("To the end  ( End )"));
    // Holding a step button repeats it, like holding its key.
    for (QToolButton *b : {frameBack, frameFwd}) {
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(400);
        b->setAutoRepeatInterval(60);
    }
    connect(toStart, &QToolButton::clicked, this, &PlayerBar::toStartClicked);
    connect(prevCut, &QToolButton::clicked, this, &PlayerBar::previousCutClicked);
    connect(frameBack, &QToolButton::clicked, this, &PlayerBar::frameBackClicked);
    connect(play, &QToolButton::clicked, this, &PlayerBar::playPauseClicked);
    connect(frameFwd, &QToolButton::clicked, this, &PlayerBar::frameForwardClicked);
    connect(nextCut, &QToolButton::clicked, this, &PlayerBar::nextCutClicked);
    connect(toEnd, &QToolButton::clicked, this, &PlayerBar::toEndClicked);

    m_screenshotButton = makeButton("playerScreenshotButton", QStringLiteral("camera"),
        tr("Screenshot  ( Ctrl+Shift+P ): save the frame at the playhead as a PNG, at the "
           "project's size"));
    m_screenshotButton->setEnabled(false);
    connect(m_screenshotButton, &QToolButton::clicked, this, &PlayerBar::screenshotRequested);

    m_muteButton = makeButton("playerMuteButton", QStringLiteral("volume-2"),
                              tr("Mute the preview  ( only what you hear here, never the export )"));
    connect(m_muteButton, &QToolButton::clicked, this, [this] { setMuted(!m_muted); });
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setObjectName(QStringLiteral("playerVolume"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(80);
    m_volumeSlider->setToolTip(tr("Preview volume: only what you hear here, never the export or "
                                  "the clips' levels"));
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_muted && v > 0)
            setMuted(false); // turning it up unmutes
        updateVolumeIcon();
        emit volumeChanged(v);
    });

    m_fullScreenButton = makeButton("playerFullScreenButton", QStringLiteral("maximize"),
                                    tr("Full screen  ( F11 or double-click the picture; Esc leaves )"));
    connect(m_fullScreenButton, &QToolButton::clicked, this, &PlayerBar::fullScreenClicked);

    auto *topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(6);
    topRow->addWidget(m_timeButton);
    topRow->addWidget(m_timeEdit);
    topRow->addWidget(m_progress, 1);
    topRow->addWidget(m_speedButton);

    // The playback buttons centred; what's beside them sits at either end,
    // each side as wide as the other so the centre stays the centre.
    auto *left = new QWidget(this);
    auto *leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_bufferingLabel);
    leftLayout->addStretch(1);
    auto *right = new QWidget(this);
    auto *rightLayout = new QHBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(2);
    rightLayout->addStretch(1);
    rightLayout->addWidget(m_screenshotButton);
    rightLayout->addSpacing(8);
    rightLayout->addWidget(m_muteButton);
    rightLayout->addWidget(m_volumeSlider);
    rightLayout->addSpacing(8);
    rightLayout->addWidget(m_fullScreenButton);
    left->setMinimumWidth(right->sizeHint().width());

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(2);
    bottomRow->addWidget(left, 1);
    for (QAbstractButton *b : {static_cast<QAbstractButton *>(toStart), static_cast<QAbstractButton *>(prevCut),
                               static_cast<QAbstractButton *>(frameBack), m_playButton,
                               static_cast<QAbstractButton *>(frameFwd),
                               static_cast<QAbstractButton *>(nextCut),
                               static_cast<QAbstractButton *>(toEnd)})
        bottomRow->addWidget(b);
    bottomRow->addWidget(right, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 2);
    layout->setSpacing(0);
    layout->addLayout(topRow);
    layout->addLayout(bottomRow);

    updateTime();
}

int PlayerBar::volume() const
{
    return m_volumeSlider->value();
}

void PlayerBar::setDuration(qint64 durationMs)
{
    m_durationMs = std::max<qint64>(0, durationMs);
    m_progress->setDuration(m_durationMs);
    updateTime();
}

void PlayerBar::setPosition(qint64 positionMs)
{
    m_positionMs = positionMs;
    m_progress->setPosition(positionMs);
    updateTime();
}

void PlayerBar::setPlaying(bool playing)
{
    m_playButton->setIcon(theme::icon(playing ? QStringLiteral("pause") : QStringLiteral("play"),
                                      theme::IconTone::Accent));
}

void PlayerBar::setBuffering(bool buffering)
{
    if (buffering) {
        if (!m_bufferingLabel->isVisible() && !m_bufferingDelay->isActive())
            m_bufferingDelay->start();
    } else {
        m_bufferingDelay->stop();
        m_bufferingLabel->setVisible(false);
    }
}

void PlayerBar::setFrameRate(double fps)
{
    m_fps = std::max(0.0, fps);
    m_progress->setFrameRate(m_fps);
    updateTime();
}

void PlayerBar::setCuts(const QList<qint64> &cutsMs)
{
    m_progress->setCuts(cutsMs);
}

void PlayerBar::setScreenshotEnabled(bool enabled)
{
    m_screenshotButton->setEnabled(enabled);
}

void PlayerBar::setSpeed(double speed)
{
    const QList<double> &s = speeds();
    double best = s.front();
    for (const double v : s)
        if (std::abs(v - speed) < std::abs(best - speed))
            best = v;
    for (QAction *a : m_speedButton->menu()->actions())
        a->setChecked(a->data().toDouble() == best);
    if (best == m_speed)
        return;
    m_speed = best;
    m_speedButton->setText(speedText(best));
    // Off 1× the speed stands out: the sound is off then.
    m_speedButton->setStyleSheet(best != 1.0 ? QStringLiteral("color: %1;").arg(
                                                   theme::cssColor(theme::colors().accent))
                                             : QString());
    emit speedChanged(best);
}

void PlayerBar::stepSpeed(int direction)
{
    const QList<double> &s = speeds();
    const int i = int(s.indexOf(m_speed));
    setSpeed(s[std::clamp(i + (direction < 0 ? -1 : 1), 0, int(s.size()) - 1)]);
}

void PlayerBar::setVolume(int percent)
{
    m_volumeSlider->setValue(std::clamp(percent, 0, 100)); // emits volumeChanged on a change
}

void PlayerBar::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    m_muteButton->setToolTip(muted ? tr("Unmute the preview")
                                   : tr("Mute the preview  ( only what you hear here, never the "
                                        "export )"));
    updateVolumeIcon();
    emit mutedChanged(muted);
}

void PlayerBar::setFullScreen(bool fullScreen)
{
    m_fullScreenButton->setIcon(
        theme::icon(fullScreen ? QStringLiteral("minimize") : QStringLiteral("maximize")));
    m_fullScreenButton->setToolTip(
        fullScreen ? tr("Leave full screen  ( Esc or F11 )")
                   : tr("Full screen  ( F11 or double-click the picture; Esc leaves )"));
}

void PlayerBar::updateVolumeIcon()
{
    const int v = m_volumeSlider->value();
    m_muteButton->setIcon(theme::icon(m_muted || v == 0 ? QStringLiteral("volume-x")
                                      : v < 50          ? QStringLiteral("volume-1")
                                                        : QStringLiteral("volume-2")));
}

void PlayerBar::updateTime()
{
    m_timeButton->setText(formatTime(m_positionMs, m_fps) + QStringLiteral(" / ") +
                          formatTime(m_durationMs, m_fps));
}

void PlayerBar::editTime()
{
    if (m_durationMs <= 0 || m_timeEdit->isVisible())
        return;
    m_timeEdit->setText(formatTime(m_positionMs, m_fps));
    m_timeEdit->setMinimumWidth(m_timeButton->width());
    m_timeEdit->setStyleSheet(QString());
    m_timeButton->setVisible(false);
    m_timeEdit->setVisible(true);
    m_timeEdit->setFocus(Qt::MouseFocusReason);
    m_timeEdit->selectAll();
}

void PlayerBar::finishTimeEdit(bool apply)
{
    if (!m_timeEdit->isVisible())
        return;
    if (apply) {
        const std::optional<qint64> ms = parseTime(m_timeEdit->text(), m_fps);
        if (!ms) { // keep it open to fix
            m_timeEdit->setStyleSheet(QStringLiteral("border-color: %1;")
                                          .arg(theme::cssColor(theme::colors().error)));
            return;
        }
        emit seekRequested(std::clamp<qint64>(*ms, 0, m_durationMs));
    }
    m_timeEdit->setVisible(false);
    m_timeButton->setVisible(true);
}

bool PlayerBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_timeEdit) {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            finishTimeEdit(false);
            return true;
        }
        if (event->type() == QEvent::FocusOut)
            finishTimeEdit(false);
    }
    return QWidget::eventFilter(watched, event);
}

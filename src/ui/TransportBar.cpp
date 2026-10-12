#include "TransportBar.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSize>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace {

QString formatTime(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

TransportBar::TransportBar(QWidget *parent)
    : QWidget(parent)
{
    const QSize kIconSize(18, 18);

    auto *playButton = new QToolButton(this);
    playButton->setObjectName(QStringLiteral("playButton"));
    playButton->setIcon(theme::icon(QStringLiteral("play"), theme::IconTone::Accent));
    playButton->setIconSize(kIconSize);
    playButton->setAutoRaise(true);
    playButton->setToolTip(tr("Play / pause  ( Space )"));
    connect(playButton, &QToolButton::clicked, this, &TransportBar::playPauseClicked);
    m_playButton = playButton;

    const auto makeButton = [this, kIconSize](const QString &iconName, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setIcon(theme::icon(iconName));
        b->setIconSize(kIconSize);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    m_setInButton = makeButton(QStringLiteral("arrow-right-to-line"),
                               tr("Set trim in  ( [ ): cut the start of the clip off at the "
                                  "playhead. Trims the selected audio clip, or else the video "
                                  "clip."));
    m_setOutButton = makeButton(QStringLiteral("arrow-left-to-line"),
                                tr("Set trim out  ( ] ): cut the end of the clip off at the "
                                   "playhead. Trims the selected audio clip, or else the video "
                                   "clip."));
    m_clearTrimButton = makeButton(QStringLiteral("rotate-ccw"),
                                   tr("Restore  ( R ): give the clip back its full length. The "
                                      "selected audio clip, or else the video clip under the "
                                      "playhead."));
    connect(m_setInButton, &QToolButton::clicked, this, &TransportBar::trimInRequested);
    connect(m_setOutButton, &QToolButton::clicked, this, &TransportBar::trimOutRequested);
    connect(m_clearTrimButton, &QToolButton::clicked, this, &TransportBar::trimCleared);

    m_armReframeButton = makeButton(QStringLiteral("move-3d"),
        tr("Arm reframe authoring  ( A ): while on, every drag or zoom in the viewport sets a "
           "keyframe at the playhead  ( K sets one without moving )"));
    m_armReframeButton->setCheckable(true);
    m_deleteKeyButton = makeButton(QStringLiteral("trash-2"),
        tr("Delete keyframe  ( Shift+K ): the reframe keyframe at the playhead"));
    m_deleteKeyButton->setEnabled(false);
    m_clearReframeButton = makeButton(QStringLiteral("x"),
                                      tr("Delete all keyframes  ( Ctrl+Shift+K ): every reframe "
                                         "keyframe of the clip under the playhead"));
    m_clearReframeButton->setEnabled(false);
    connect(m_armReframeButton, &QToolButton::toggled, this,
            &TransportBar::reframeArmedChanged);
    connect(m_deleteKeyButton, &QToolButton::clicked, this, &TransportBar::deleteKeyRequested);
    connect(m_clearReframeButton, &QToolButton::clicked, this,
            &TransportBar::clearReframeRequested);

    m_screenshotButton = makeButton(QStringLiteral("camera"),
        tr("Screenshot  ( Ctrl+Shift+P ): save the frame at the playhead as a PNG, at the "
           "project's size"));
    m_screenshotButton->setEnabled(false);
    connect(m_screenshotButton, &QToolButton::clicked, this, &TransportBar::screenshotRequested);

    m_timeLabel = new QLabel(this);

    m_noticeLabel = new QLabel(this);
    m_noticeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_noticeClear = new QTimer(this);
    m_noticeClear->setSingleShot(true);
    m_noticeClear->setInterval(6000);
    connect(m_noticeClear, &QTimer::timeout, m_noticeLabel, &QLabel::clear);

    m_bufferingLabel = new QLabel(tr("Buffering…"), this);
    m_bufferingLabel->setVisible(false);

    m_bufferingDelay = new QTimer(this);
    m_bufferingDelay->setSingleShot(true);
    m_bufferingDelay->setInterval(150);
    connect(m_bufferingDelay, &QTimer::timeout, this,
            [this] { m_bufferingLabel->setVisible(true); });

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->addWidget(m_playButton);
    layout->addSpacing(8);
    layout->addWidget(m_setInButton);
    layout->addWidget(m_setOutButton);
    layout->addWidget(m_clearTrimButton);
    layout->addSpacing(12);
    layout->addWidget(m_armReframeButton);
    layout->addWidget(m_deleteKeyButton);
    layout->addWidget(m_clearReframeButton);
    layout->addSpacing(12);
    layout->addWidget(m_screenshotButton);
    layout->addSpacing(8);
    layout->addWidget(m_noticeLabel);
    layout->addStretch(1);
    layout->addWidget(m_bufferingLabel);
    layout->addSpacing(8);
    layout->addWidget(m_timeLabel);

    updateTimeLabel();
}

void TransportBar::setDuration(qint64 durationMs)
{
    m_durationMs = std::max<qint64>(0, durationMs);
    updateTimeLabel();
}

void TransportBar::toggleReframeArmed()
{
    m_armReframeButton->toggle();
}

void TransportBar::setHasReframeKeys(bool hasKeys)
{
    m_deleteKeyButton->setEnabled(hasKeys);
    m_clearReframeButton->setEnabled(hasKeys);
}

void TransportBar::setScreenshotEnabled(bool enabled)
{
    m_screenshotButton->setEnabled(enabled);
}

void TransportBar::showNotice(const QString &text, bool sticky)
{
    m_noticeLabel->setText(text);
    if (sticky)
        m_noticeClear->stop();
    else
        m_noticeClear->start();
}

void TransportBar::setPosition(qint64 positionMs)
{
    m_positionMs = positionMs;
    updateTimeLabel();
}

void TransportBar::setPlaying(bool playing)
{
    m_playing = playing;
    m_playButton->setIcon(theme::icon(playing ? QStringLiteral("pause") : QStringLiteral("play"),
                                      theme::IconTone::Accent));
}

void TransportBar::setBuffering(bool buffering)
{
    if (buffering) {
        if (!m_bufferingLabel->isVisible() && !m_bufferingDelay->isActive())
            m_bufferingDelay->start();
    } else {
        m_bufferingDelay->stop();
        m_bufferingLabel->setVisible(false);
    }
}

void TransportBar::updateTimeLabel()
{
    m_timeLabel->setText(formatTime(m_positionMs) + QStringLiteral("  /  ")
                         + formatTime(m_durationMs));
}

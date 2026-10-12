#include "TransportBar.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSize>
#include <QTimer>
#include <QToolButton>

TransportBar::TransportBar(QWidget *parent)
    : QWidget(parent)
{
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
    m_setInButton = makeButton("trimInButton", QStringLiteral("arrow-right-to-line"),
                               tr("Set trim in  ( [ ): cut the start of the clip off at the "
                                  "playhead. Trims the selected audio clip, or else the video "
                                  "clip."));
    m_setOutButton = makeButton("trimOutButton", QStringLiteral("arrow-left-to-line"),
                                tr("Set trim out  ( ] ): cut the end of the clip off at the "
                                   "playhead. Trims the selected audio clip, or else the video "
                                   "clip."));
    m_clearTrimButton = makeButton("trimRestoreButton", QStringLiteral("rotate-ccw"),
                                   tr("Restore  ( R ): give the clip back its full length. The "
                                      "selected audio clip, or else the video clip under the "
                                      "playhead."));
    connect(m_setInButton, &QToolButton::clicked, this, &TransportBar::trimInRequested);
    connect(m_setOutButton, &QToolButton::clicked, this, &TransportBar::trimOutRequested);
    connect(m_clearTrimButton, &QToolButton::clicked, this, &TransportBar::trimCleared);

    m_armReframeButton = makeButton("reframeArmButton", QStringLiteral("move-3d"),
        tr("Arm reframe authoring  ( A ): while on, every drag or zoom in the viewport sets a "
           "keyframe at the playhead  ( K sets one without moving )"));
    m_armReframeButton->setCheckable(true);
    m_deleteKeyButton = makeButton("reframeDeleteKeyButton", QStringLiteral("trash-2"),
        tr("Delete keyframe  ( Shift+K ): the reframe keyframe at the playhead"));
    m_deleteKeyButton->setEnabled(false);
    m_clearReframeButton = makeButton("reframeDeleteAllButton", QStringLiteral("x"),
                                      tr("Delete all keyframes  ( Ctrl+Shift+K ): every reframe "
                                         "keyframe of the clip under the playhead"));
    m_clearReframeButton->setEnabled(false);
    connect(m_armReframeButton, &QToolButton::toggled, this,
            &TransportBar::reframeArmedChanged);
    connect(m_deleteKeyButton, &QToolButton::clicked, this, &TransportBar::deleteKeyRequested);
    connect(m_clearReframeButton, &QToolButton::clicked, this,
            &TransportBar::clearReframeRequested);

    m_noticeLabel = new QLabel(this);
    m_noticeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_noticeClear = new QTimer(this);
    m_noticeClear->setSingleShot(true);
    m_noticeClear->setInterval(6000);
    connect(m_noticeClear, &QTimer::timeout, m_noticeLabel, &QLabel::clear);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->addWidget(m_setInButton);
    layout->addWidget(m_setOutButton);
    layout->addWidget(m_clearTrimButton);
    layout->addSpacing(12);
    layout->addWidget(m_armReframeButton);
    layout->addWidget(m_deleteKeyButton);
    layout->addWidget(m_clearReframeButton);
    layout->addSpacing(12);
    layout->addWidget(m_noticeLabel);
    layout->addStretch(1);
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

void TransportBar::showNotice(const QString &text, bool sticky)
{
    m_noticeLabel->setText(text);
    if (sticky)
        m_noticeClear->stop();
    else
        m_noticeClear->start();
}

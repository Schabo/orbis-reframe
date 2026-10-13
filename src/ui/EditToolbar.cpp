#include "EditToolbar.h"
#include "Theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>

EditToolbar::EditToolbar(QWidget *parent)
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
    connect(m_setInButton, &QToolButton::clicked, this, &EditToolbar::trimInRequested);
    connect(m_setOutButton, &QToolButton::clicked, this, &EditToolbar::trimOutRequested);
    connect(m_clearTrimButton, &QToolButton::clicked, this, &EditToolbar::trimCleared);

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
            &EditToolbar::reframeArmedChanged);
    connect(m_deleteKeyButton, &QToolButton::clicked, this, &EditToolbar::deleteKeyRequested);
    connect(m_clearReframeButton, &QToolButton::clicked, this,
            &EditToolbar::clearReframeRequested);

    m_zoomOutButton = makeButton("zoomOutButton", QStringLiteral("zoom-out"),
                                 tr("Zoom out  ( Ctrl+- ): show more of the timeline"));
    m_zoomFitButton = makeButton("zoomFitButton", QStringLiteral("move-horizontal"),
                                 tr("Zoom to fit  ( Ctrl+0 ): the whole edit across the timeline"));
    m_zoomInButton = makeButton("zoomInButton", QStringLiteral("zoom-in"),
                                tr("Zoom in  ( Ctrl++ ): show less of the timeline, in more "
                                   "detail; Ctrl+wheel zooms around the pointer"));
    connect(m_zoomOutButton, &QToolButton::clicked, this, &EditToolbar::zoomOutRequested);
    connect(m_zoomFitButton, &QToolButton::clicked, this, &EditToolbar::zoomToFitRequested);
    connect(m_zoomInButton, &QToolButton::clicked, this, &EditToolbar::zoomInRequested);

    m_noticeLabel = new QLabel(this);
    m_noticeLabel->setObjectName(QStringLiteral("editNotice"));
    m_noticeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_noticeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // A long notice must not push the tools off centre: it's cut, and shown
    // whole in its tooltip.
    m_noticeLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_noticeClear = new QTimer(this);
    m_noticeClear->setSingleShot(true);
    m_noticeClear->setInterval(6000);
    connect(m_noticeClear, &QTimer::timeout, m_noticeLabel, &QLabel::clear);

    // Three columns: the two sides share what's left equally, so the tools
    // stay in the middle of the window whatever the notice says.
    auto *tools = new QHBoxLayout;
    tools->setSpacing(2);
    tools->addWidget(m_setInButton);
    tools->addWidget(m_setOutButton);
    tools->addWidget(m_clearTrimButton);
    tools->addSpacing(14);
    tools->addWidget(m_armReframeButton);
    tools->addWidget(m_deleteKeyButton);
    tools->addWidget(m_clearReframeButton);
    auto *right = new QHBoxLayout;
    right->setSpacing(2);
    right->addSpacing(12);
    right->addWidget(m_noticeLabel, 1);
    right->addSpacing(12);
    right->addWidget(m_zoomOutButton);
    right->addWidget(m_zoomFitButton);
    right->addWidget(m_zoomInButton);
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(0);
    layout->addLayout(tools, 0, 1);
    layout->addLayout(right, 0, 2);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(2, 1);
    // The empty left column as wide as the right one's least (the zoom
    // buttons), so equal stretch means equal widths.
    layout->setColumnMinimumWidth(0, right->minimumSize().width());
}

void EditToolbar::toggleReframeArmed()
{
    m_armReframeButton->toggle();
}

void EditToolbar::setHasReframeKeys(bool hasKeys)
{
    m_deleteKeyButton->setEnabled(hasKeys);
    m_clearReframeButton->setEnabled(hasKeys);
}

void EditToolbar::showNotice(const QString &text, bool sticky)
{
    m_noticeLabel->setText(text);
    m_noticeLabel->setToolTip(text);
    if (sticky)
        m_noticeClear->stop();
    else
        m_noticeClear->start();
}

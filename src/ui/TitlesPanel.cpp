#include "TitlesPanel.h"

#include "../core/Title.h"

#include <QAction>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QVBoxLayout>

namespace {

// A tile's picture: the title's text, white on dark, like it shows over
// the video.
QIcon titleTile(const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x30, 0x34, 0x3a));
    p.drawRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), 4, 4);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(size.height() / 3);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, QObject::tr("Title"));
    return QIcon(pm);
}

} // namespace

TitleList::TitleList(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("titleList"));
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setWrapping(true);
    setSpacing(6);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setFrameShape(QFrame::NoFrame);
}

QStringList TitleList::mimeTypes() const
{
    return {QString::fromLatin1(title::kMimeType)};
}

QMimeData *TitleList::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    // Which title; there's only the plain one so far.
    mime->setData(QString::fromLatin1(title::kMimeType), QByteArrayLiteral("plain"));
    return mime;
}

TitlesPanel::TitlesPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("titlesPanel"));
    setAttribute(Qt::WA_StyledBackground);

    auto *heading = new QLabel(tr("Titles"), this);
    heading->setObjectName(QStringLiteral("sidebarPageTitle"));

    m_list = new TitleList(this);
    const QSize tile(128, 72);
    m_list->setIconSize(tile);
    auto *plain = new QListWidgetItem(titleTile(tile, devicePixelRatioF()), tr("Text"), m_list);
    plain->setToolTip(tr("Plain text. Drag it onto the timeline, or double-click to add it at "
                         "the playhead; edit it in the Title tab on the right."));
    m_list->setCurrentItem(plain);
    connect(m_list, &QListWidget::itemActivated, this, &TitlesPanel::addTitleRequested);

    auto *add = new QAction(tr("&Add to timeline"), m_list);
    add->setObjectName(QStringLiteral("titleAddToTimelineAction"));
    connect(add, &QAction::triggered, this, &TitlesPanel::addTitleRequested);
    m_list->addAction(add);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *hint = new QLabel(tr("Drag a title onto the timeline, or double-click it to add it at "
                               "the playhead. Its text, font, colour, size and timing are in the "
                               "Title tab on the right."),
                            this);
    hint->setObjectName(QStringLiteral("titlesHint"));
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addWidget(m_list, 1);
}

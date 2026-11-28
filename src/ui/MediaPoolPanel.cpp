#include "MediaPoolPanel.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "DoubleClickToAddList.h"
#include "DurationText.h"
#include "Theme.h"

namespace {

constexpr int kPad = 6;
constexpr int kFolderRowH = 26;
constexpr int kRadius = 3;
constexpr int kListThumbW = 80;
constexpr int kListThumbH = 45;
constexpr int kGridThumbW = 160;
constexpr int kGridThumbH = 90;
constexpr int kGridLineH = 18;
constexpr double kZoomStep = 1.15;
constexpr double kMinThumbScale = 0.5;
constexpr double kMaxListScale = 3.0;
constexpr double kMaxGridScale = 2.5;

QString fpsText(double fps)
{
    QString s = QString::number(fps, 'f', 2);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return QObject::tr("%1 fps").arg(s);
}

QString kindText(MediaItem::Kind kind)
{
    switch (kind) {
    case MediaItem::Kind::Clip360:
        return QObject::tr("360°");
    case MediaItem::Kind::Video:
        return QObject::tr("Video");
    case MediaItem::Kind::Audio:
        break;
    }
    return QObject::tr("Audio");
}

// "5.7K · 30 fps" for a 360 clip whose camera names its mode (as the Media
// information panel does), "1920×1080 · 25 fps" for a flat video, or a 360
// clip whose mode isn't known (yet).
QString formatText(const MediaItem &m, const QString &mode)
{
    QStringList parts;
    if (!mode.isEmpty())
        parts << mode;
    else if (m.width > 0 && m.height > 0)
        parts << QStringLiteral("%1×%2").arg(m.width).arg(m.height);
    if (m.fps > 0)
        parts << fpsText(m.fps);
    return parts.join(QStringLiteral(" · "));
}

QString iconFor(MediaItem::Kind kind)
{
    switch (kind) {
    case MediaItem::Kind::Clip360:
        return QStringLiteral("clapperboard");
    case MediaItem::Kind::Video:
        return QStringLiteral("film");
    case MediaItem::Kind::Audio:
        break;
    }
    return QStringLiteral("music");
}

// Local files and folders among a drag's URLs; empty for a drag from the
// view itself.
QStringList mediaDropPaths(const QDropEvent *e, const QWidget *source)
{
    QStringList out;
    if (e->source() == source || !e->mimeData()->hasUrls())
        return out;
    for (const QUrl &u : e->mimeData()->urls()) {
        if (!u.isLocalFile())
            continue;
        const QString path = u.toLocalFile();
        if (QFileInfo(path).isDir() || media::kindForPath(path))
            out << path;
    }
    return out;
}

// The empty pool's hint, the same in both views.
void paintEmptyMediaHint(QWidget *viewport)
{
    QPainter p(viewport);
    p.setPen(theme::colors().textDim);
    p.drawText(viewport->rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
               MediaPoolPanel::tr("No media yet.\n\nImport files or a folder, or drop them here. "
                                  "Drag them from here onto the timeline."));
}

// A file's picture: the thumbnail, or the kind's icon on a plain area; the
// play button under the mouse (pause on the previewed file), and the
// previewed file's ring. The same on a list row and a grid cell.
void paintFileThumb(QPainter *p, const QRectF &thumbRect, const QModelIndex &index, bool hover,
                    const theme::Colors &tc)
{
    const bool missing = index.data(MediaPoolPanel::MissingRole).toBool();
    const auto kind = MediaItem::Kind(index.data(MediaPoolPanel::KindRole).toInt());
    const auto preview =
        MediaPoolPanel::Preview(index.data(MediaPoolPanel::PreviewRole).toInt());
    p->save();
    QPainterPath shape;
    shape.addRoundedRect(thumbRect, kRadius, kRadius);
    p->setClipPath(shape);
    const QImage thumb = index.data(MediaPoolPanel::ThumbnailRole).value<QImage>();
    if (!thumb.isNull() && !missing) {
        p->drawImage(thumbRect, thumb);
    } else {
        p->fillRect(thumbRect, tc.window);
        const QRect icon(0, 0, 20, 20);
        theme::icon(missing ? QStringLiteral("triangle-alert") : iconFor(kind))
            .paint(p, icon.translated(thumbRect.center().toPoint() - icon.center()),
                   Qt::AlignCenter, missing ? QIcon::Normal : QIcon::Disabled);
    }
    if (!missing && (hover || preview != MediaPoolPanel::Preview::None)) {
        constexpr qreal kButton = 26;
        const QRectF button(thumbRect.center() - QPointF(kButton / 2, kButton / 2),
                            QSizeF(kButton, kButton));
        QColor disc = tc.window;
        disc.setAlpha(preview != MediaPoolPanel::Preview::None ? 235 : 200);
        p->setPen(Qt::NoPen);
        p->setBrush(disc);
        p->drawEllipse(button);
        const QRect icon(0, 0, 16, 16);
        theme::icon(preview == MediaPoolPanel::Preview::Playing ? QStringLiteral("pause")
                                                                : QStringLiteral("play"),
                    theme::IconTone::Accent)
            .paint(p, icon.translated(button.center().toPoint() - icon.center()));
    }
    p->restore();
    if (preview != MediaPoolPanel::Preview::None) {
        p->setPen(QPen(tc.accent, 2));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(thumbRect.adjusted(-1, -1, 1, 1), kRadius + 1, kRadius + 1);
    }
}

class MediaRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(MediaPoolPanel::FolderRole).toBool())
            return {option.rect.width(), kFolderRowH};
        const auto *list = qobject_cast<const MediaPoolList *>(parent());
        const int thumbH = list ? list->thumbSize().height() : kListThumbH;
        // Small pictures still leave the three text lines their height.
        return {option.rect.width(),
                std::max(thumbH + 2 * kPad, 3 * QFontMetrics(option.font).height() + kPad)};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const theme::Colors &tc = theme::colors();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect;
        const QFontMetrics fm(option.font);

        if (index.data(MediaPoolPanel::FolderRole).toBool()) {
            // Folder header: open / closed chevron, name, count.
            const bool open = option.state & QStyle::State_Open;
            const QRect chevron(r.left() + 4, r.center().y() - 7, 14, 14);
            theme::icon(open ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"))
                .paint(p, chevron);
            QFont bold = option.font;
            bold.setWeight(QFont::DemiBold);
            const QFontMetrics bfm(bold);
            const QString count = QString::number(index.data(MediaPoolPanel::CountRole).toInt());
            const int countW = fm.horizontalAdvance(count);
            const QRect text = r.adjusted(chevron.right() + 6, 0, -kPad - countW - 8, 0);
            p->setFont(bold);
            p->setPen(tc.textDim);
            p->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                        bfm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                       text.width()));
            p->setFont(option.font);
            p->drawText(r.adjusted(0, 0, -kPad, 0), Qt::AlignRight | Qt::AlignVCenter, count);
            p->restore();
            return;
        }

        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (selected)
            p->fillRect(r, tc.accentSoft);
        else if (hover)
            p->fillRect(r, tc.button);

        const bool missing = index.data(MediaPoolPanel::MissingRole).toBool();
        const auto *list = qobject_cast<const MediaPoolList *>(parent());
        const QRectF thumbRect =
            list ? list->thumbRect(r)
                 : QRectF(r.left() + kPad, r.top() + kPad, kListThumbW, kListThumbH);
        paintFileThumb(p, thumbRect, index, hover, tc);

        // Name (and the in-use mark), kind and length, size and rate.
        const int left = int(thumbRect.right()) + kPad + 2;
        const int lineH = fm.height();
        const int top = r.top() + (r.height() - 3 * lineH) / 2;
        const bool inUse = index.data(MediaPoolPanel::InUseRole).toBool();
        const int markW = inUse ? 18 : 0;
        QRect line(left, top, r.right() - kPad - left - markW, lineH);
        p->setPen(missing ? tc.error : tc.text);
        p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                  line.width()));
        if (inUse)
            theme::icon(QStringLiteral("circle-check"), theme::IconTone::Accent)
                .paint(p, QRect(r.right() - kPad - 14, top + (lineH - 14) / 2, 14, 14));
        line.setWidth(r.right() - kPad - left);
        p->setPen(tc.textDim);
        line.translate(0, lineH);
        p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(MediaPoolPanel::DetailsRole).toString(),
                                  Qt::ElideRight, line.width()));
        line.translate(0, lineH);
        if (missing)
            p->setPen(tc.error);
        p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(missing ? QObject::tr("File not found")
                                          : index.data(MediaPoolPanel::FormatRole).toString(),
                                  Qt::ElideRight, line.width()));
        p->restore();
    }
};

// A grid cell: the file's picture on top, its name and its kind and length
// under it.
class MediaCellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        const auto *grid = qobject_cast<const MediaPoolGrid *>(parent());
        return grid ? grid->cellSize() : QSize(kGridThumbW + 2 * kPad, 130);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const theme::Colors &tc = theme::colors();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect;
        const QFontMetrics fm(option.font);

        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (selected)
            p->fillRect(r, tc.accentSoft);
        else if (hover)
            p->fillRect(r, tc.button);

        const bool missing = index.data(MediaPoolPanel::MissingRole).toBool();
        const auto *grid = qobject_cast<const MediaPoolGrid *>(parent());
        const QRectF thumbRect =
            grid ? grid->thumbRect(r)
                 : QRectF(r.left() + kPad, r.top() + kPad, kGridThumbW, kGridThumbH);
        paintFileThumb(p, thumbRect, index, hover, tc);

        // The in-use mark in the corner.
        if (index.data(MediaPoolPanel::InUseRole).toBool())
            theme::icon(QStringLiteral("circle-check"), theme::IconTone::Accent)
                .paint(p, QRect(r.right() - kPad - 14, r.top() + kPad - 2, 14, 14));

        // Name, then kind and length -- "File not found" when missing.
        const int left = r.left() + kPad + 2;
        QRect line(left, int(thumbRect.bottom()) + 4, r.right() - kPad - left, kGridLineH);
        p->setPen(missing ? tc.error : tc.text);
        p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                  line.width()));
        line.translate(0, kGridLineH);
        p->setPen(missing ? tc.error : tc.textDim);
        p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(missing ? QObject::tr("File not found")
                                          : index.data(MediaPoolPanel::DetailsRole).toString(),
                                  Qt::ElideRight, line.width()));
        p->restore();
    }
};

} // namespace

// ---------------------------------------------------------------------------

MediaPoolList::MediaPoolList(QWidget *parent) : QTreeWidget(parent)
{
    setObjectName(QStringLiteral("mediaPoolList"));
    setHeaderHidden(true);
    setColumnCount(1);
    setRootIsDecorated(false); // the folder rows paint their own chevron
    setIndentation(0);
    setUniformRowHeights(false);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setMouseTracking(true); // hover highlight
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setExpandsOnDoubleClick(false);
    setItemDelegate(new MediaRowDelegate(this));
}

QSize MediaPoolList::thumbSize() const
{
    return {int(std::lround(kListThumbW * m_thumbScale)),
            int(std::lround(kListThumbH * m_thumbScale))};
}

QRect MediaPoolList::thumbRect(const QRect &row) const
{
    const QSize ts = thumbSize();
    return {row.left() + kPad, row.top() + (row.height() - ts.height()) / 2, ts.width(),
            ts.height()};
}

void MediaPoolList::setThumbScale(double scale)
{
    scale = std::clamp(scale, kMinThumbScale, kMaxListScale);
    if (scale == m_thumbScale)
        return;
    m_thumbScale = scale;
    scheduleDelayedItemsLayout();
    viewport()->update();
    emit thumbScaleChanged(m_thumbScale);
}

void MediaPoolList::wheelEvent(QWheelEvent *e)
{
    // Ctrl + wheel zooms the files' pictures; the wheel alone scrolls.
    if (e->modifiers() & Qt::ControlModifier) {
        const int d = e->angleDelta().y();
        if (d != 0)
            setThumbScale(m_thumbScale * std::pow(kZoomStep, d / 120.0));
        e->accept();
        return;
    }
    QTreeWidget::wheelEvent(e);
}

QString MediaPoolList::previewPath() const
{
    const QTreeWidgetItem *item = currentItem();
    if (!item || item->data(0, MediaPoolPanel::FolderRole).toBool() ||
        item->data(0, MediaPoolPanel::MissingRole).toBool())
        return {};
    return item->data(0, MediaPoolPanel::PathRole).toString();
}

bool MediaPoolList::event(QEvent *e)
{
    // Space previews the current file here rather than play the timeline
    // (the window's shortcut).
    if (e->type() == QEvent::ShortcutOverride) {
        const auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Space && k->modifiers() == Qt::NoModifier &&
            !previewPath().isEmpty()) {
            e->accept();
            return true;
        }
    }
    return QTreeWidget::event(e);
}

void MediaPoolList::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space && e->modifiers() == Qt::NoModifier) {
        if (const QString path = previewPath(); !path.isEmpty()) {
            if (!e->isAutoRepeat())
                emit previewKeyPressed(path);
            e->accept();
            return;
        }
    }
    if (DoubleClickToAddList::isActivationKey(e) && currentItem()) {
        emit activationKeyPressed();
        e->accept();
        return;
    }
    QTreeWidget::keyPressEvent(e);
}

void MediaPoolList::mousePressEvent(QMouseEvent *e)
{
    // On a file's picture: its play button. The row is selected as by any
    // click, but the press doesn't start a drag.
    if (e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier) {
        const QModelIndex index = indexAt(e->position().toPoint());
        QTreeWidgetItem *item = itemFromIndex(index);
        if (item && !item->data(0, MediaPoolPanel::FolderRole).toBool() &&
            !item->data(0, MediaPoolPanel::MissingRole).toBool() &&
            thumbRect(visualRect(index)).contains(e->position().toPoint())) {
            setCurrentItem(item);
            emit previewClicked(item->data(0, MediaPoolPanel::PathRole).toString());
            e->accept();
            return;
        }
    }
    QTreeWidget::mousePressEvent(e);
}

QStringList MediaPoolList::mimeTypes() const { return {QStringLiteral("text/uri-list")}; }

QMimeData *MediaPoolList::mimeData(const QList<QTreeWidgetItem *> &items) const
{
    QList<QUrl> urls;
    for (const QTreeWidgetItem *item : items)
        if (!item->data(0, MediaPoolPanel::FolderRole).toBool() &&
            !item->data(0, MediaPoolPanel::MissingRole).toBool())
            urls << QUrl::fromLocalFile(item->data(0, MediaPoolPanel::PathRole).toString());
    if (urls.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

Qt::DropActions MediaPoolList::supportedDropActions() const { return Qt::CopyAction; }

void MediaPoolList::dragEnterEvent(QDragEnterEvent *e)
{
    if (mediaDropPaths(e, this).isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void MediaPoolList::dragMoveEvent(QDragMoveEvent *e)
{
    if (mediaDropPaths(e, this).isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void MediaPoolList::dropEvent(QDropEvent *e)
{
    const QStringList paths = mediaDropPaths(e, this);
    if (paths.isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    emit filesDropped(paths);
}

void MediaPoolList::paintEvent(QPaintEvent *e)
{
    QTreeWidget::paintEvent(e);
    if (topLevelItemCount() == 0)
        paintEmptyMediaHint(viewport());
}

// ---------------------------------------------------------------------------

MediaPoolGrid::MediaPoolGrid(QWidget *parent) : DoubleClickToAddList(parent)
{
    setObjectName(QStringLiteral("mediaPoolGrid"));
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setMouseTracking(true); // hover highlight
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setSpacing(4);
    setItemDelegate(new MediaCellDelegate(this));
    setGridSize(cellSize() + QSize(spacing(), spacing()));
}

QSize MediaPoolGrid::thumbSize() const
{
    return {int(std::lround(kGridThumbW * m_thumbScale)),
            int(std::lround(kGridThumbH * m_thumbScale))};
}

QSize MediaPoolGrid::cellSize() const
{
    const QSize ts = thumbSize();
    return {ts.width() + 2 * kPad, kPad + ts.height() + 4 + 2 * kGridLineH + kPad};
}

QRect MediaPoolGrid::thumbRect(const QRect &cell) const
{
    const QSize ts = thumbSize();
    return {cell.left() + (cell.width() - ts.width()) / 2, cell.top() + kPad, ts.width(),
            ts.height()};
}

void MediaPoolGrid::setThumbScale(double scale)
{
    scale = std::clamp(scale, kMinThumbScale, kMaxGridScale);
    if (scale == m_thumbScale)
        return;
    m_thumbScale = scale;
    setGridSize(cellSize() + QSize(spacing(), spacing()));
    scheduleDelayedItemsLayout();
    viewport()->update();
    emit thumbScaleChanged(m_thumbScale);
}

void MediaPoolGrid::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const int d = e->angleDelta().y();
        if (d != 0)
            setThumbScale(m_thumbScale * std::pow(kZoomStep, d / 120.0));
        e->accept();
        return;
    }
    QListWidget::wheelEvent(e);
}

QString MediaPoolGrid::previewPath() const
{
    const QListWidgetItem *item = currentItem();
    if (!item || item->data(MediaPoolPanel::MissingRole).toBool())
        return {};
    return item->data(MediaPoolPanel::PathRole).toString();
}

bool MediaPoolGrid::event(QEvent *e)
{
    // Space previews the current file here rather than play the timeline
    // (the window's shortcut), as in the list.
    if (e->type() == QEvent::ShortcutOverride) {
        const auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Space && k->modifiers() == Qt::NoModifier &&
            !previewPath().isEmpty()) {
            e->accept();
            return true;
        }
    }
    return QListWidget::event(e);
}

void MediaPoolGrid::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space && e->modifiers() == Qt::NoModifier) {
        if (const QString path = previewPath(); !path.isEmpty()) {
            if (!e->isAutoRepeat())
                emit previewKeyPressed(path);
            e->accept();
            return;
        }
    }
    // Return / Enter: activationKeyPressed, the grid's add.
    DoubleClickToAddList::keyPressEvent(e);
}

void MediaPoolGrid::mousePressEvent(QMouseEvent *e)
{
    // On a file's picture: its play button, as on a list row.
    if (e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier) {
        const QModelIndex index = indexAt(e->position().toPoint());
        QListWidgetItem *item = itemFromIndex(index);
        if (item && !item->data(MediaPoolPanel::MissingRole).toBool() &&
            thumbRect(visualRect(index)).contains(e->position().toPoint())) {
            setCurrentItem(item);
            emit previewClicked(item->data(MediaPoolPanel::PathRole).toString());
            e->accept();
            return;
        }
    }
    QListWidget::mousePressEvent(e);
}

QStringList MediaPoolGrid::mimeTypes() const { return {QStringLiteral("text/uri-list")}; }

QMimeData *MediaPoolGrid::mimeData(const QList<QListWidgetItem *> &items) const
{
    QList<QUrl> urls;
    for (const QListWidgetItem *item : items)
        if (!item->data(MediaPoolPanel::MissingRole).toBool())
            urls << QUrl::fromLocalFile(item->data(MediaPoolPanel::PathRole).toString());
    if (urls.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

Qt::DropActions MediaPoolGrid::supportedDropActions() const { return Qt::CopyAction; }

void MediaPoolGrid::dragEnterEvent(QDragEnterEvent *e)
{
    if (mediaDropPaths(e, this).isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void MediaPoolGrid::dragMoveEvent(QDragMoveEvent *e)
{
    if (mediaDropPaths(e, this).isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void MediaPoolGrid::dropEvent(QDropEvent *e)
{
    const QStringList paths = mediaDropPaths(e, this);
    if (paths.isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    emit filesDropped(paths);
}

void MediaPoolGrid::paintEvent(QPaintEvent *e)
{
    QListWidget::paintEvent(e);
    if (count() == 0)
        paintEmptyMediaHint(viewport());
}

// ---------------------------------------------------------------------------

MediaPoolPanel::MediaPoolPanel(MediaPool *pool, QWidget *parent) : QWidget(parent), m_pool(pool)
{
    setObjectName(QStringLiteral("mediaPoolPanel"));
    setAttribute(Qt::WA_StyledBackground);
    // Reading a file's picture is mostly waiting on the disk; two at a time
    // keeps a big import from swamping a card reader.
    m_thumbnailPool.setMaxThreadCount(2);

    auto *title = new QLabel(tr("Media"), this);
    title->setObjectName(QStringLiteral("sidebarPageTitle"));
    auto *importFiles = new QToolButton(this);
    importFiles->setObjectName(QStringLiteral("mediaImportFilesButton"));
    importFiles->setAutoRaise(true);
    importFiles->setIcon(theme::icon(QStringLiteral("file-plus-2")));
    importFiles->setToolTip(tr("Import files…"));
    importFiles->setFocusPolicy(Qt::NoFocus);
    connect(importFiles, &QToolButton::clicked, this, &MediaPoolPanel::importFilesRequested);
    auto *importFolder = new QToolButton(this);
    importFolder->setObjectName(QStringLiteral("mediaImportFolderButton"));
    importFolder->setAutoRaise(true);
    importFolder->setIcon(theme::icon(QStringLiteral("folder-plus")));
    importFolder->setToolTip(tr("Import folder…: every video and audio file in it"));
    importFolder->setFocusPolicy(Qt::NoFocus);
    connect(importFolder, &QToolButton::clicked, this, &MediaPoolPanel::importFolderRequested);
    // The two views; the checked one is the shown one.
    m_viewList = new QToolButton(this);
    m_viewList->setObjectName(QStringLiteral("mediaViewListButton"));
    m_viewList->setAutoRaise(true);
    m_viewList->setCheckable(true);
    m_viewList->setAutoExclusive(true);
    m_viewList->setChecked(true);
    m_viewList->setIcon(theme::icon(QStringLiteral("list")));
    m_viewList->setToolTip(tr("Show as a list, grouped by folder"));
    m_viewList->setFocusPolicy(Qt::NoFocus);
    connect(m_viewList, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            setViewMode(ViewMode::List);
    });
    m_viewGrid = new QToolButton(this);
    m_viewGrid->setObjectName(QStringLiteral("mediaViewGridButton"));
    m_viewGrid->setAutoRaise(true);
    m_viewGrid->setCheckable(true);
    m_viewGrid->setAutoExclusive(true);
    m_viewGrid->setIcon(theme::icon(QStringLiteral("layout-grid")));
    m_viewGrid->setToolTip(tr("Show as a grid of pictures"));
    m_viewGrid->setFocusPolicy(Qt::NoFocus);
    connect(m_viewGrid, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            setViewMode(ViewMode::Grid);
    });
    // The cameras' backup sound files (…_backup.wav) sit out of the way
    // until this shows them.
    m_backupAudio = new QToolButton(this);
    m_backupAudio->setObjectName(QStringLiteral("mediaBackupAudioButton"));
    m_backupAudio->setAutoRaise(true);
    m_backupAudio->setCheckable(true);
    m_backupAudio->setIcon(theme::icon(QStringLiteral("mic")));
    m_backupAudio->setToolTip(
        tr("Show the cameras' backup audio files (a clip's …_backup.wav). "
           "A backup file used on the timeline always shows."));
    m_backupAudio->setFocusPolicy(Qt::NoFocus);
    connect(m_backupAudio, &QToolButton::toggled, this,
            &MediaPoolPanel::setBackupAudioShown);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 6, 0);
    header->setSpacing(2);
    header->addWidget(title, 1);
    header->addWidget(importFiles);
    header->addWidget(importFolder);
    header->addWidget(m_backupAudio);
    header->addWidget(m_viewList);
    header->addWidget(m_viewGrid);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("mediaPoolStatus"));
    m_status->hide();

    // A zoom reads the pictures again once the wheel settles, not at every
    // notch: each read decodes every file and keeps a picture per size.
    m_thumbnailTimer = new QTimer(this);
    m_thumbnailTimer->setSingleShot(true);
    m_thumbnailTimer->setInterval(300);
    connect(m_thumbnailTimer, &QTimer::timeout, this, &MediaPoolPanel::requestThumbnails);

    m_list = new MediaPoolList(this);
    connect(m_list, &MediaPoolList::filesDropped, this, &MediaPoolPanel::filesDropped);
    connect(m_list, &MediaPoolList::previewClicked, this, &MediaPoolPanel::previewRequested);
    connect(m_list, &MediaPoolList::previewKeyPressed, this, &MediaPoolPanel::previewRequested);
    connect(m_list, &MediaPoolList::thumbScaleChanged, this, [this](double scale) {
        m_thumbnailTimer->start();
        emit listScaleChanged(scale);
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &MediaPoolPanel::updateActions);
    connect(m_list, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem *item) {
        if (item->data(0, FolderRole).toBool())
            item->setExpanded(!item->isExpanded());
    });
    // Adding is a double-click, or Return / Enter on the current item. Not a
    // click: on a desktop set to open things on one click Qt reports that
    // click as an activation, and it would add a file the user meant only to
    // select. A folder folds on its click (above); on a missing file the same
    // gestures ask where it is now. The grid's cells do the same (below).
    const auto activate = [this](const QString &path, bool missing) {
        if (missing) {
            emit locateRequested(path);
            return;
        }
        if (m_addAction->isEnabled())
            m_addAction->trigger();
    };
    const auto activateRow = [activate](const QTreeWidgetItem *item) {
        if (item && !item->data(0, FolderRole).toBool())
            activate(item->data(0, PathRole).toString(), item->data(0, MissingRole).toBool());
    };
    connect(m_list, &QTreeWidget::itemDoubleClicked, this,
            [activateRow](QTreeWidgetItem *item) { activateRow(item); });
    connect(m_list, &MediaPoolList::activationKeyPressed, this,
            [this, activateRow] { activateRow(m_list->currentItem()); });
    connect(m_list, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        m_collapsed.insert(item->data(0, PathRole).toString());
    });
    connect(m_list, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        m_collapsed.remove(item->data(0, PathRole).toString());
    });

    m_grid = new MediaPoolGrid(this);
    connect(m_grid, &MediaPoolGrid::filesDropped, this, &MediaPoolPanel::filesDropped);
    connect(m_grid, &MediaPoolGrid::previewClicked, this, &MediaPoolPanel::previewRequested);
    connect(m_grid, &MediaPoolGrid::previewKeyPressed, this, &MediaPoolPanel::previewRequested);
    connect(m_grid, &MediaPoolGrid::thumbScaleChanged, this, [this](double scale) {
        m_thumbnailTimer->start();
        emit gridScaleChanged(scale);
    });
    connect(m_grid, &QListWidget::itemSelectionChanged, this, &MediaPoolPanel::updateActions);
    const auto activateCell = [activate](const QListWidgetItem *item) {
        if (item)
            activate(item->data(PathRole).toString(), item->data(MissingRole).toBool());
    };
    connect(m_grid, &QListWidget::itemDoubleClicked, this,
            [activateCell](QListWidgetItem *item) { activateCell(item); });
    connect(m_grid, &DoubleClickToAddList::activationKeyPressed, this,
            [this, activateCell] { activateCell(m_grid->currentItem()); });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_list);
    m_stack->addWidget(m_grid);

    m_addAction = new QAction(tr("&Add to timeline"), this);
    m_addAction->setObjectName(QStringLiteral("mediaAddToTimelineAction"));
    connect(m_addAction, &QAction::triggered, this, [this] {
        QStringList paths;
        for (const QString &path : selectedPaths())
            if (const MediaItem *m = m_pool->item(path); m && !m->missing)
                paths << path;
        if (!paths.isEmpty())
            emit addToTimelineRequested(paths);
    });
    m_locateAction = new QAction(theme::icon(QStringLiteral("folder-open")), tr("&Locate file…"),
                                 this);
    m_locateAction->setObjectName(QStringLiteral("mediaLocateAction"));
    m_locateAction->setToolTip(tr("Find a moved or renamed file. Other missing files with "
                                  "the same names in that folder are found too."));
    connect(m_locateAction, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (paths.size() == 1)
            emit locateRequested(paths.first());
    });
    m_showAction = new QAction(tr("Show in &file manager"), this);
    m_showAction->setObjectName(QStringLiteral("mediaShowInFileManagerAction"));
    connect(m_showAction, &QAction::triggered, this, &MediaPoolPanel::showInFileManager);
    m_showTimelineAction = new QAction(tr("Show in &timeline"), this);
    m_showTimelineAction->setObjectName(QStringLiteral("mediaShowInTimelineAction"));
    connect(m_showTimelineAction, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (m_showTimelineAction->isEnabled() && paths.size() == 1)
            emit showInTimelineRequested(paths.first());
    });
    m_removeAction = new QAction(theme::icon(QStringLiteral("x")), tr("&Remove from project"),
                                 this);
    m_removeAction->setObjectName(QStringLiteral("mediaRemoveAction"));
    m_removeAction->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_removeAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_removeAction, &QAction::triggered, this, &MediaPoolPanel::removeSelected);
    // The files themselves: to the trash, or gone for good. MainWindow asks
    // first and deletes a take's other files too.
    m_trashAction =
        new QAction(theme::icon(QStringLiteral("trash-2")), tr("Move to &trash"), this);
    m_trashAction->setObjectName(QStringLiteral("mediaMoveToTrashAction"));
    m_trashAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    m_trashAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_trashAction, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (m_trashAction->isEnabled() && !paths.isEmpty())
            emit moveToTrashRequested(paths);
    });
    m_deleteAction = new QAction(tr("&Delete permanently"), this);
    m_deleteAction->setObjectName(QStringLiteral("mediaDeletePermanentlyAction"));
    m_deleteAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Delete));
    m_deleteAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_deleteAction, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (m_deleteAction->isEnabled() && !paths.isEmpty())
            emit deletePermanentlyRequested(paths);
    });
    auto *separator = new QAction(this);
    separator->setSeparator(true);
    auto *fileSeparator = new QAction(this);
    fileSeparator->setSeparator(true);
    // The same actions on both views: context menu and keys (Delete)
    // follow whichever is shown.
    const QList<QAction *> actions{m_addAction,     m_locateAction, m_showAction,
                                   m_showTimelineAction, separator,   m_removeAction,
                                   fileSeparator,  m_trashAction,  m_deleteAction};
    m_list->addActions(actions);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_grid->addActions(actions);
    m_grid->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_status);
    layout->addWidget(m_stack, 1);

    connect(m_pool, &MediaPool::changed, this, &MediaPoolPanel::rebuild);
    connect(m_pool, &MediaPool::missingChanged, this, &MediaPoolPanel::rebuild);
    rebuild();
}

MediaPoolPanel::~MediaPoolPanel()
{
    // Workers post back to this panel; let the running ones finish first.
    m_thumbnailPool.clear();
    m_thumbnailPool.waitForDone();
}

void MediaPoolPanel::setThumbnailProvider(ThumbnailProvider provider)
{
    m_thumbnailProvider = std::move(provider);
    m_thumbnails.clear();
    rebuild();
}

void MediaPoolPanel::setModeProvider(ModeProvider provider)
{
    m_modeProvider = std::move(provider);
    m_modes.clear();
    rebuild();
}

void MediaPoolPanel::forgetFiles(const QStringList &paths)
{
    bool forgot = false;
    for (const QString &path : paths) {
        ++m_fileReads[path];
        forgot |= m_thumbnails.remove(path) | m_modes.remove(path);
    }
    if (forgot)
        rebuild();
}

void MediaPoolPanel::setInUse(const QSet<QString> &paths)
{
    if (paths == m_inUse)
        return;
    // A hidden backup file entering or leaving the timeline's use joins or
    // leaves the shown files: rebuild for it.
    const QSet<QString> toggled = (paths - m_inUse) + (m_inUse - paths);
    m_inUse = paths;
    if (!m_showBackupAudio) {
        for (const QString &path : toggled)
            if (media::isBackupAudio(path)) {
                rebuild();
                return;
            }
    }
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j) {
            QTreeWidgetItem *item = folder->child(j);
            const bool inUse = m_inUse.contains(item->data(0, PathRole).toString());
            item->setData(0, InUseRole, inUse);
            item->setToolTip(0, item->data(0, PathRole).toString() +
                                    (inUse ? tr("\nUsed on the timeline") : QString()));
        }
    }
    for (int i = 0; i < m_grid->count(); ++i) {
        QListWidgetItem *item = m_grid->item(i);
        const bool inUse = m_inUse.contains(item->data(PathRole).toString());
        item->setData(InUseRole, inUse);
        item->setToolTip(item->data(PathRole).toString() +
                         (inUse ? tr("\nUsed on the timeline") : QString()));
    }
    updateActions();
}

void MediaPoolPanel::setPreview(const QString &path, bool playing)
{
    const Preview preview =
        path.isEmpty() ? Preview::None : playing ? Preview::Playing : Preview::Paused;
    if (path == m_previewPath && preview == m_preview)
        return;
    m_previewPath = path;
    m_preview = preview;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j) {
            QTreeWidgetItem *item = folder->child(j);
            const bool shown = item->data(0, PathRole).toString() == m_previewPath;
            item->setData(0, PreviewRole, int(shown ? m_preview : Preview::None));
        }
    }
    for (int i = 0; i < m_grid->count(); ++i) {
        QListWidgetItem *item = m_grid->item(i);
        const bool shown = item->data(PathRole).toString() == m_previewPath;
        item->setData(PreviewRole, int(shown ? m_preview : Preview::None));
    }
}

void MediaPoolPanel::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void MediaPoolPanel::setViewMode(ViewMode mode)
{
    if (mode == m_viewMode)
        return;
    const QStringList selected = selectedPaths();
    m_viewMode = mode;
    m_stack->setCurrentIndex(mode == ViewMode::Grid ? 1 : 0);
    m_viewList->setChecked(mode == ViewMode::List);
    m_viewGrid->setChecked(mode == ViewMode::Grid);
    if (!selected.isEmpty())
        selectPaths(selected);
    updateActions();
    emit viewModeChanged(mode);
}

void MediaPoolPanel::setBackupAudioShown(bool shown)
{
    if (shown == m_showBackupAudio)
        return;
    m_showBackupAudio = shown;
    m_backupAudio->setChecked(shown);
    rebuild();
    emit backupAudioShownChanged(shown);
}

double MediaPoolPanel::listScale() const { return m_list->thumbScale(); }

double MediaPoolPanel::gridScale() const { return m_grid->thumbScale(); }

void MediaPoolPanel::setListScale(double scale) { m_list->setThumbScale(scale); }

void MediaPoolPanel::setGridScale(double scale) { m_grid->setThumbScale(scale); }

QStringList MediaPoolPanel::selectedPaths() const
{
    QStringList out;
    if (m_viewMode == ViewMode::Grid) {
        for (int i = 0; i < m_grid->count(); ++i)
            if (m_grid->item(i)->isSelected())
                out << m_grid->item(i)->data(PathRole).toString();
        return out;
    }
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j)
            if (folder->child(j)->isSelected())
                out << folder->child(j)->data(0, PathRole).toString();
    }
    return out;
}

void MediaPoolPanel::selectPaths(const QStringList &paths)
{
    if (m_viewMode == ViewMode::Grid) {
        m_grid->clearSelection();
        QListWidgetItem *first = nullptr;
        for (int i = 0; i < m_grid->count(); ++i) {
            QListWidgetItem *item = m_grid->item(i);
            if (!paths.contains(item->data(PathRole).toString()))
                continue;
            item->setSelected(true);
            if (!first)
                first = item;
        }
        if (first) {
            m_grid->setCurrentItem(first, QItemSelectionModel::NoUpdate);
            m_grid->scrollToItem(first);
        }
        return;
    }
    m_list->clearSelection();
    QTreeWidgetItem *first = nullptr;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j) {
            QTreeWidgetItem *item = folder->child(j);
            if (!paths.contains(item->data(0, PathRole).toString()))
                continue;
            item->setSelected(true);
            if (!first) {
                first = item;
                folder->setExpanded(true);
            }
        }
    }
    if (first) {
        m_list->setCurrentItem(first, 0, QItemSelectionModel::NoUpdate);
        m_list->scrollToItem(first);
    }
}

void MediaPoolPanel::rebuild()
{
    const QStringList selected = selectedPaths();
    const int scroll = m_list->verticalScrollBar() ? m_list->verticalScrollBar()->value() : 0;
    const int gridScroll =
        m_grid->verticalScrollBar() ? m_grid->verticalScrollBar()->value() : 0;
    m_list->clear();
    m_grid->clear();

    // Folder -> its files, folders and files sorted by name.
    QList<MediaItem> items = m_pool->items();
    std::sort(items.begin(), items.end(), [](const MediaItem &a, const MediaItem &b) {
        const QFileInfo fa(a.path), fb(b.path);
        const int byFolder = QString::compare(fa.absolutePath(), fb.absolutePath(),
                                              Qt::CaseInsensitive);
        if (byFolder != 0)
            return byFolder < 0;
        return QString::compare(fa.fileName(), fb.fileName(), Qt::CaseInsensitive) < 0;
    });
    QTreeWidgetItem *folder = nullptr;
    QString folderPath;
    for (const MediaItem &m : items) {
        // Backup sound files stay out unless the user shows them or the
        // timeline uses them. A folder with nothing shown gets no header.
        const bool inUse = m_inUse.contains(m.path);
        if (!m_showBackupAudio && !inUse && media::isBackupAudio(m.path))
            continue;
        const QFileInfo fi(m.path);
        if (!folder || fi.absolutePath() != folderPath) {
            folderPath = fi.absolutePath();
            folder = new QTreeWidgetItem(m_list);
            const QString name = QFileInfo(folderPath).fileName();
            folder->setText(0, name.isEmpty() ? QDir::toNativeSeparators(folderPath) : name);
            folder->setToolTip(0, QDir::toNativeSeparators(folderPath));
            folder->setData(0, PathRole, folderPath);
            folder->setData(0, FolderRole, true);
            folder->setData(0, CountRole, 0);
            folder->setFlags(Qt::ItemIsEnabled);
        }
        auto *item = new QTreeWidgetItem(folder);
        item->setText(0, fi.fileName());
        item->setData(0, PathRole, m.path);
        item->setData(0, KindRole, int(m.kind));
        item->setData(0, DetailsRole,
                      QStringLiteral("%1 · %2").arg(kindText(m.kind), durationText(m.durationMs)));
        item->setData(0, FormatRole, formatText(m, m_modes.value(m.path)));
        item->setData(0, MissingRole, m.missing);
        item->setData(0, InUseRole, inUse);
        item->setData(0, PreviewRole, int(m.path == m_previewPath ? m_preview : Preview::None));
        item->setToolTip(0, QDir::toNativeSeparators(m.path) +
                                (m.missing ? tr("\nFile not found: double-click to locate it")
                                 : inUse   ? tr("\nUsed on the timeline")
                                           : QString()));
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!m.missing)
            flags |= Qt::ItemIsDragEnabled;
        item->setFlags(flags);
        folder->setData(0, CountRole, folder->data(0, CountRole).toInt() + 1);

        // The same file in the grid: all folders' files together.
        auto *cell = new QListWidgetItem(fi.fileName(), m_grid);
        cell->setData(PathRole, m.path);
        cell->setData(KindRole, int(m.kind));
        cell->setData(DetailsRole, item->data(0, DetailsRole));
        cell->setData(FormatRole, item->data(0, FormatRole));
        cell->setData(MissingRole, m.missing);
        cell->setData(InUseRole, inUse);
        cell->setData(PreviewRole, item->data(0, PreviewRole));
        cell->setToolTip(item->toolTip(0));
        cell->setFlags(flags);

        if (const auto it = m_thumbnails.constFind(m.path); it != m_thumbnails.constEnd()) {
            item->setData(0, ThumbnailRole, *it);
            cell->setData(ThumbnailRole, *it);
        } else if (!m.missing) {
            requestThumbnail(m);
        }
        if (m.kind == MediaItem::Kind::Clip360 && !m.missing && !m_modes.contains(m.path))
            requestMode(m);
    }
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *f = m_list->topLevelItem(i);
        f->setExpanded(!m_collapsed.contains(f->data(0, PathRole).toString()));
    }
    if (!selected.isEmpty())
        selectPaths(selected);
    if (m_list->verticalScrollBar())
        m_list->verticalScrollBar()->setValue(scroll);
    if (m_grid->verticalScrollBar())
        m_grid->verticalScrollBar()->setValue(gridScroll);
    updateActions();
}

void MediaPoolPanel::updateActions()
{
    const QStringList paths = selectedPaths();
    bool anyPresent = false, anyInUse = false, anyMissing = false;
    for (const QString &path : paths) {
        const MediaItem *m = m_pool->item(path);
        if (m && !m->missing)
            anyPresent = true;
        if (!m || m->missing)
            anyMissing = true;
        anyInUse |= m_inUse.contains(path);
    }
    m_addAction->setEnabled(anyPresent);
    const MediaItem *one = paths.size() == 1 ? m_pool->item(paths.first()) : nullptr;
    m_locateAction->setEnabled(one && one->missing);
    m_showAction->setEnabled(paths.size() == 1 && QFileInfo(paths.first()).absoluteDir().exists());
    const bool oneInUse =
        paths.size() == 1 && !anyMissing && m_inUse.contains(paths.first());
    m_showTimelineAction->setEnabled(oneInUse);
    m_showTimelineAction->setToolTip(
        oneInUse ? tr("Show this file's clip on the timeline.")
                 : tr("Only a file that is on the timeline and on the disk can be shown there."));
    m_removeAction->setEnabled(!paths.isEmpty() && !anyInUse);
    m_removeAction->setToolTip(anyInUse ? tr("Files used on the timeline can't be removed")
                                        : tr("Remove from the project's media. The files are "
                                             "not deleted."));
    // A file that's gone can't be removed from the file system. A file the
    // timeline uses can be asked for: MainWindow explains and offers to go to
    // its clip.
    const bool canRemoveFiles = !paths.isEmpty() && !anyMissing;
    QString fileReason;
    if (anyMissing)
        fileReason = tr("Files that aren't on the disk can't be removed.");
    const QString inUseReason =
        tr("The selected file is on the timeline. Click to see how to go to its clip.");
    m_trashAction->setEnabled(canRemoveFiles);
    m_trashAction->setToolTip(
        !canRemoveFiles ? fileReason
        : anyInUse      ? inUseReason
                        : tr("Move the selected files to the trash, where you can put them back."));
    m_deleteAction->setEnabled(canRemoveFiles);
    m_deleteAction->setToolTip(
        !canRemoveFiles ? fileReason
        : anyInUse      ? inUseReason
                        : tr("Delete the selected files for good. This can't be undone."));
}

QSize MediaPoolPanel::thumbnailRequestSize() const
{
    const QSize l = m_list->thumbSize(), g = m_grid->thumbSize();
    return (g.width() > l.width() ? g : l) * devicePixelRatioF();
}

void MediaPoolPanel::requestThumbnail(const MediaItem &m)
{
    if (!m_thumbnailProvider)
        return;
    // Asked for: don't ask again. A picture being read again at a bigger
    // size stays until the new one arrives.
    if (!m_thumbnails.contains(m.path))
        m_thumbnails.insert(m.path, QImage());
    if (m_thumbnailReadSize.isEmpty())
        m_thumbnailReadSize = thumbnailRequestSize();
    const QSize size = m_thumbnailReadSize;
    m_thumbnailPool.start([this, m, size, generation = m_thumbnailGeneration,
                           read = m_fileReads.value(m.path), provider = m_thumbnailProvider] {
        const QImage image = provider(m, size);
        QMetaObject::invokeMethod(
            this,
            [this, path = m.path, image, generation, read] {
                applyThumbnail(path, image, generation, read);
            },
            Qt::QueuedConnection);
    });
}

void MediaPoolPanel::requestThumbnails()
{
    const QSize want = thumbnailRequestSize();
    if (want.width() <= m_thumbnailReadSize.width())
        return;
    m_thumbnailReadSize = want;
    ++m_thumbnailGeneration;
    const QList<QString> paths = m_thumbnails.keys();
    for (const QString &path : paths)
        if (const MediaItem *m = m_pool->item(path);
            m && !m->missing && m->kind != MediaItem::Kind::Audio)
            requestThumbnail(*m);
}

void MediaPoolPanel::applyThumbnail(const QString &path, const QImage &image, int generation,
                                    int read)
{
    if (read != m_fileReads.value(path))
        return; // of the file before it was replaced
    // A picture read before the last zoom in: only while there's none yet,
    // so it can't replace the bigger one if it comes in after it.
    if (generation != m_thumbnailGeneration && !m_thumbnails.value(path).isNull())
        return;
    QImage t = image;
    t.setDevicePixelRatio(devicePixelRatioF());
    m_thumbnails.insert(path, t);
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j)
            if (folder->child(j)->data(0, PathRole).toString() == path)
                folder->child(j)->setData(0, ThumbnailRole, t);
    }
    for (int i = 0; i < m_grid->count(); ++i)
        if (m_grid->item(i)->data(PathRole).toString() == path)
            m_grid->item(i)->setData(ThumbnailRole, t);
}

void MediaPoolPanel::requestMode(const MediaItem &m)
{
    if (!m_modeProvider)
        return;
    m_modes.insert(m.path, QString()); // asked for: don't ask again
    m_thumbnailPool.start([this, m, read = m_fileReads.value(m.path), provider = m_modeProvider] {
        const QString mode = provider(m);
        QMetaObject::invokeMethod(
            this, [this, path = m.path, mode, read] { applyMode(path, mode, read); },
            Qt::QueuedConnection);
    });
}

void MediaPoolPanel::applyMode(const QString &path, const QString &mode, int read)
{
    if (read != m_fileReads.value(path))
        return; // of the file before it was replaced
    m_modes.insert(path, mode);
    const MediaItem *m = m_pool->item(path);
    if (!m)
        return;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j)
            if (folder->child(j)->data(0, PathRole).toString() == path)
                folder->child(j)->setData(0, FormatRole, formatText(*m, mode));
    }
    for (int i = 0; i < m_grid->count(); ++i)
        if (m_grid->item(i)->data(PathRole).toString() == path)
            m_grid->item(i)->setData(FormatRole, formatText(*m, mode));
}

void MediaPoolPanel::removeSelected()
{
    if (!m_removeAction->isEnabled())
        return;
    m_pool->remove(selectedPaths());
}

void MediaPoolPanel::showInFileManager()
{
    const QStringList paths = selectedPaths();
    if (paths.size() == 1)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first()).absolutePath()));
}

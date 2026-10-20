#include "MediaPoolPanel.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "Theme.h"

namespace {

constexpr int kPad = 6;
constexpr int kFolderRowH = 26;
constexpr int kRadius = 3;

QString durationText(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600)
            .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

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

// "5.7K · 30 fps" for a 360 clip (named by its square lens pictures, as the
// Media information panel does), "1920×1080 · 25 fps" for a flat video.
QString formatText(const MediaItem &m)
{
    QStringList parts;
    if (m.width > 0 && m.height > 0) {
        QString size = QStringLiteral("%1×%2").arg(m.width).arg(m.height);
        if (m.kind == MediaItem::Kind::Clip360 && m.width == m.height) {
            if (m.width == 3840)
                size = QStringLiteral("8K");
            else if (m.width == 3008)
                size = QStringLiteral("6K");
            else if (m.width == 2880)
                size = QStringLiteral("5.7K");
            else if (m.width == 1920)
                size = QStringLiteral("4K");
        }
        parts << size;
    }
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

class MediaRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(MediaPoolPanel::FolderRole).toBool())
            return {option.rect.width(), kFolderRowH};
        return {option.rect.width(), MediaPoolPanel::thumbnailSize().height() + 2 * kPad};
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
        const auto kind = MediaItem::Kind(index.data(MediaPoolPanel::KindRole).toInt());

        // Thumbnail, or the kind's icon on a plain area.
        const QSize ts = MediaPoolPanel::thumbnailSize();
        const QRectF thumbRect(r.left() + kPad, r.top() + kPad, ts.width(), ts.height());
        QPainterPath shape;
        shape.addRoundedRect(thumbRect, kRadius, kRadius);
        p->save();
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
        p->restore();

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

QStringList MediaPoolList::droppedPaths(const QDropEvent *e) const
{
    QStringList out;
    if (e->source() == this || !e->mimeData()->hasUrls())
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

void MediaPoolList::dragEnterEvent(QDragEnterEvent *e)
{
    if (droppedPaths(e).isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void MediaPoolList::dragMoveEvent(QDragMoveEvent *e)
{
    if (droppedPaths(e).isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void MediaPoolList::dropEvent(QDropEvent *e)
{
    const QStringList paths = droppedPaths(e);
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
    if (topLevelItemCount() > 0)
        return;
    QPainter p(viewport());
    p.setPen(theme::colors().textDim);
    p.drawText(viewport()->rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
               tr("No media yet.\n\nImport files or a folder, or drop them here. Drag them from "
                  "here onto the timeline."));
}

// ---------------------------------------------------------------------------

QSize MediaPoolPanel::thumbnailSize() { return {80, 45}; }

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
    importFiles->setToolTip(tr("Import files…: pick video and audio files"));
    importFiles->setFocusPolicy(Qt::NoFocus);
    connect(importFiles, &QToolButton::clicked, this, &MediaPoolPanel::importFilesRequested);
    auto *importFolder = new QToolButton(this);
    importFolder->setObjectName(QStringLiteral("mediaImportFolderButton"));
    importFolder->setAutoRaise(true);
    importFolder->setIcon(theme::icon(QStringLiteral("folder-plus")));
    importFolder->setToolTip(tr("Import folder…: every video and audio file in it"));
    importFolder->setFocusPolicy(Qt::NoFocus);
    connect(importFolder, &QToolButton::clicked, this, &MediaPoolPanel::importFolderRequested);
    auto *collect = new QToolButton(this);
    collect->setObjectName(QStringLiteral("mediaCollectButton"));
    collect->setAutoRaise(true);
    collect->setIcon(theme::icon(QStringLiteral("download")));
    collect->setToolTip(
        tr("Copy media into the project…: the files it uses, from a connected device"));
    collect->setFocusPolicy(Qt::NoFocus);
    connect(collect, &QToolButton::clicked, this,
            &MediaPoolPanel::collectIntoProjectRequested);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 6, 0);
    header->setSpacing(2);
    header->addWidget(title, 1);
    header->addWidget(importFiles);
    header->addWidget(importFolder);
    header->addWidget(collect);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("mediaPoolStatus"));
    m_status->hide();

    m_list = new MediaPoolList(this);
    connect(m_list, &MediaPoolList::filesDropped, this, &MediaPoolPanel::filesDropped);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &MediaPoolPanel::updateActions);
    connect(m_list, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem *item) {
        if (item->data(0, FolderRole).toBool())
            item->setExpanded(!item->isExpanded());
    });
    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, FolderRole).toBool())
            return;
        if (item->data(0, MissingRole).toBool()) {
            emit locateRequested(item->data(0, PathRole).toString());
            return;
        }
        if (m_addAction->isEnabled())
            m_addAction->trigger();
    });
    connect(m_list, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        m_collapsed.insert(item->data(0, PathRole).toString());
    });
    connect(m_list, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        m_collapsed.remove(item->data(0, PathRole).toString());
    });

    m_addAction = new QAction(tr("&Add to timeline"), m_list);
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
                                 m_list);
    m_locateAction->setObjectName(QStringLiteral("mediaLocateAction"));
    m_locateAction->setToolTip(tr("Find a moved or renamed file. Other missing files with "
                                  "the same names in that folder are found too."));
    connect(m_locateAction, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (paths.size() == 1)
            emit locateRequested(paths.first());
    });
    m_showAction = new QAction(tr("Show in &file manager"), m_list);
    m_showAction->setObjectName(QStringLiteral("mediaShowInFileManagerAction"));
    connect(m_showAction, &QAction::triggered, this, &MediaPoolPanel::showInFileManager);
    m_removeAction = new QAction(theme::icon(QStringLiteral("x")), tr("&Remove from project"),
                                 m_list);
    m_removeAction->setObjectName(QStringLiteral("mediaRemoveAction"));
    m_removeAction->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_removeAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_removeAction, &QAction::triggered, this, &MediaPoolPanel::removeSelected);
    auto *separator = new QAction(m_list);
    separator->setSeparator(true);
    m_list->addActions({m_addAction, m_locateAction, m_showAction, separator, m_removeAction});
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_status);
    layout->addWidget(m_list, 1);

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

void MediaPoolPanel::setInUse(const QSet<QString> &paths)
{
    if (paths == m_inUse)
        return;
    m_inUse = paths;
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
    updateActions();
}

void MediaPoolPanel::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

QStringList MediaPoolPanel::selectedPaths() const
{
    QStringList out;
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
    m_list->clear();

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
        item->setData(0, FormatRole, formatText(m));
        item->setData(0, MissingRole, m.missing);
        const bool inUse = m_inUse.contains(m.path);
        item->setData(0, InUseRole, inUse);
        item->setToolTip(0, QDir::toNativeSeparators(m.path) +
                                (m.missing ? tr("\nFile not found: double-click to locate it")
                                 : inUse   ? tr("\nUsed on the timeline")
                                           : QString()));
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!m.missing)
            flags |= Qt::ItemIsDragEnabled;
        item->setFlags(flags);
        folder->setData(0, CountRole, folder->data(0, CountRole).toInt() + 1);

        if (const auto it = m_thumbnails.constFind(m.path); it != m_thumbnails.constEnd())
            item->setData(0, ThumbnailRole, *it);
        else if (!m.missing)
            requestThumbnail(m);
    }
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *f = m_list->topLevelItem(i);
        f->setExpanded(!m_collapsed.contains(f->data(0, PathRole).toString()));
    }
    if (!selected.isEmpty())
        selectPaths(selected);
    if (m_list->verticalScrollBar())
        m_list->verticalScrollBar()->setValue(scroll);
    updateActions();
}

void MediaPoolPanel::updateActions()
{
    const QStringList paths = selectedPaths();
    bool anyPresent = false, anyInUse = false;
    for (const QString &path : paths) {
        if (const MediaItem *m = m_pool->item(path); m && !m->missing)
            anyPresent = true;
        anyInUse |= m_inUse.contains(path);
    }
    m_addAction->setEnabled(anyPresent);
    const MediaItem *one = paths.size() == 1 ? m_pool->item(paths.first()) : nullptr;
    m_locateAction->setEnabled(one && one->missing);
    m_showAction->setEnabled(paths.size() == 1 && QFileInfo(paths.first()).absoluteDir().exists());
    m_removeAction->setEnabled(!paths.isEmpty() && !anyInUse);
    m_removeAction->setToolTip(anyInUse ? tr("Files used on the timeline can't be removed")
                                        : tr("Remove from the project's media. The files are "
                                             "not deleted."));
}

void MediaPoolPanel::requestThumbnail(const MediaItem &m)
{
    if (!m_thumbnailProvider)
        return;
    m_thumbnails.insert(m.path, QImage()); // asked for: don't ask again
    const QSize size = thumbnailSize() * devicePixelRatioF();
    m_thumbnailPool.start([this, m, size, provider = m_thumbnailProvider] {
        const QImage image = provider(m, size);
        QMetaObject::invokeMethod(
            this, [this, path = m.path, image] { applyThumbnail(path, image); },
            Qt::QueuedConnection);
    });
}

void MediaPoolPanel::applyThumbnail(const QString &path, const QImage &image)
{
    QImage t = image;
    t.setDevicePixelRatio(devicePixelRatioF());
    m_thumbnails.insert(path, t);
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = m_list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j)
            if (folder->child(j)->data(0, PathRole).toString() == path)
                folder->child(j)->setData(0, ThumbnailRole, t);
    }
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

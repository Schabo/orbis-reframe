// The left sidebar and its Media tab, driven with QTest on the offscreen
// platform and found by objectName: the sidebar's close / reopen, the media
// pool's list (folders, sort order, in use, missing), drag out (file URLs),
// drops in, add to timeline, remove, thumbnails (and their disk cache). No GPU, no media decoding.
// Exit code is the failure count. ORBIS_MEDIA_SHOT=<path> saves a picture of
// the sidebar.

#include "MediaPoolPanel.h"
#include "TabSidebar.h"
#include "ThumbnailCache.h"
#include "Theme.h"

#include "../core/MediaPool.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMimeData>
#include <QMutex>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUrl>

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

template <typename T> T *find(QObject *root, const char *name)
{
    T *w = root->findChild<T *>(QLatin1String(name));
    if (!w)
        std::printf("FAIL  no %s named \"%s\"\n", T::staticMetaObject.className(), name);
    return w;
}

void writeFile(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write("x");
}

// The list's file rows, top to bottom: "folder/file".
QStringList rows(MediaPoolList *list)
{
    QStringList out;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *folder = list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j)
            out << folder->text(0) + QLatin1Char('/') + folder->child(j)->text(0);
    }
    return out;
}

QTreeWidgetItem *row(MediaPoolList *list, const QString &fileName)
{
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *folder = list->topLevelItem(i);
        for (int j = 0; j < folder->childCount(); ++j)
            if (folder->child(j)->text(0) == fileName)
                return folder->child(j);
    }
    return nullptr;
}

void testSidebar()
{
    TabSidebar bar(TabSidebar::Side::Left, QStringLiteral("sourcesSidebar"));
    auto *page = new QLabel(QStringLiteral("page"));
    page->setObjectName(QStringLiteral("mediaPoolPanel"));
    bar.addTab(page, QStringLiteral("film"), QStringLiteral("Media"));
    bar.setToggleKeys(QStringLiteral("Ctrl+B"));
    bar.resize(300, 400);
    bar.show();

    auto *tab = find<QToolButton>(&bar, "mediaPoolPanelTab");
    auto *close = find<QToolButton>(&bar, "sourcesSidebarClose");
    auto *strip = find<QWidget>(&bar, "sourcesSidebarStrip");
    auto *open = find<QToolButton>(&bar, "sourcesSidebarOpen");
    if (!tab || !close || !strip || !open)
        return;
    CHECK(tab->isChecked());
    CHECK(tab->toolTip() == QStringLiteral("Media"));
    CHECK(bar.currentIndex() == 0);
    CHECK(close->toolTip().contains(QStringLiteral("Ctrl+B")));
    // The close button sits left of the tab on the left side.
    CHECK(close->geometry().x() < tab->geometry().x());
    CHECK(page->isVisible() && !strip->isVisible());

    QSignalSpy openSpy(&bar, &TabSidebar::openChanged);
    close->click();
    CHECK(!bar.isOpen());
    CHECK(!page->isVisible() && strip->isVisible());
    CHECK(openSpy.size() == 1 && openSpy.first().first().toBool() == false);
    // Closed, it's a slim strip.
    CHECK(bar.sizeHint().width() < 40);
    open->click();
    CHECK(bar.isOpen() && page->isVisible() && !strip->isVisible());
    bar.toggle();
    CHECK(!bar.isOpen());
    CHECK(openSpy.size() == 3);
    bar.toggle();
}

void testSidebarResize()
{
    QWidget window;
    window.resize(1200, 600);
    auto *bar = new TabSidebar(TabSidebar::Side::Left, QStringLiteral("sourcesSidebar"), &window);
    auto *page = new QLabel(QStringLiteral("page"));
    page->setObjectName(QStringLiteral("mediaPoolPanel"));
    bar->addTab(page, QStringLiteral("film"), QStringLiteral("Media"));
    bar->setPageWidthRange(200, 300, 900);
    bar->setPageWidth(300);
    bar->resize(bar->sizeHint().width(), 600);
    window.show();
    CHECK(bar->pageWidth() == 300);
    bar->setPageWidth(50); // clamped to the range
    CHECK(bar->pageWidth() == 200);
    bar->setPageWidth(300);

    auto *handle = find<QWidget>(bar, "sourcesSidebarHandle");
    if (!handle)
        return;
    CHECK(handle->isVisible());
    QSignalSpy widthSpy(bar, &TabSidebar::pageWidthChanged);
    const QPoint at(2, 100);
    // Drag 120 px right: wider.
    QTest::mousePress(handle, Qt::LeftButton, {}, at);
    QMouseEvent move(QEvent::MouseMove, QPointF(at + QPoint(120, 0)),
                     QPointF(handle->mapToGlobal(at + QPoint(120, 0))), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, {}, at + QPoint(120, 0));
    CHECK(bar->pageWidth() == 420);
    CHECK(widthSpy.size() == 1 && widthSpy.first().first().toInt() == 420);
    // Far right: at most half the window.
    QTest::mousePress(handle, Qt::LeftButton, {}, at);
    QMouseEvent far(QEvent::MouseMove, QPointF(at + QPoint(2000, 0)),
                    QPointF(handle->mapToGlobal(at + QPoint(2000, 0))), Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &far);
    QTest::mouseRelease(handle, Qt::LeftButton, {}, at);
    CHECK(bar->pageWidth() == 600);
    // Double-click: the default width.
    QTest::mouseDClick(handle, Qt::LeftButton, {}, at);
    CHECK(bar->pageWidth() == 300);
    CHECK(widthSpy.size() == 3);
    // Closed: no handle.
    bar->setOpen(false);
    CHECK(!handle->isVisible());
}

void testMediaTab()
{
    QTemporaryDir tmp;
    const QString a = tmp.path() + QStringLiteral("/DCIM");
    const QString b = tmp.path() + QStringLiteral("/Music");
    QDir().mkpath(a);
    QDir().mkpath(b);
    const QString clip2 = a + QStringLiteral("/VID_2.insv");
    const QString clip1 = a + QStringLiteral("/VID_1.insv");
    const QString phone = a + QStringLiteral("/phone.mp4");
    const QString song = b + QStringLiteral("/song.mp3");
    const QString gone = b + QStringLiteral("/gone.wav");
    for (const QString &f : {clip1, clip2, phone, song})
        writeFile(f);

    MediaPool pool;
    // Added out of order: the list sorts by folder, then name.
    pool.add({song, MediaItem::Kind::Audio, 185000});
    pool.add({clip2, MediaItem::Kind::Clip360, 42000, 2880, 2880, 29.97});
    pool.add({gone, MediaItem::Kind::Audio, 1000});
    pool.add({clip1, MediaItem::Kind::Clip360, 61000, 3840, 3840, 29.97});
    pool.add({phone, MediaItem::Kind::Video, 9000, 1080, 1920, 30.0});
    CHECK(!pool.add({clip1, MediaItem::Kind::Clip360, 1}));  // already there
    CHECK(pool.item(gone) && pool.item(gone)->missing);
    CHECK(pool.item(song) && !pool.item(song)->missing);

    MediaPoolPanel panel(&pool);
    panel.resize(300, 500);
    panel.show();
    MediaPoolList *list = panel.list();
    CHECK(list->objectName() == QStringLiteral("mediaPoolList"));
    CHECK(find<QToolButton>(&panel, "mediaImportFilesButton"));
    CHECK(find<QToolButton>(&panel, "mediaImportFolderButton"));

    CHECK(list->topLevelItemCount() == 2);
    CHECK(rows(list) == (QStringList{QStringLiteral("DCIM/phone.mp4"),
                                     QStringLiteral("DCIM/VID_1.insv"),
                                     QStringLiteral("DCIM/VID_2.insv"),
                                     QStringLiteral("Music/gone.wav"),
                                     QStringLiteral("Music/song.mp3")}));
    CHECK(list->topLevelItem(0)->data(0, MediaPoolPanel::CountRole).toInt() == 3);
    CHECK(list->topLevelItem(0)->isExpanded());
    if (QTreeWidgetItem *r = row(list, QStringLiteral("VID_1.insv"))) {
        CHECK(r->data(0, MediaPoolPanel::DetailsRole).toString() == QStringLiteral("360° · 1:01"));
        CHECK(r->data(0, MediaPoolPanel::FormatRole).toString() ==
              QStringLiteral("3840×3840 · 29.97 fps")); // no mode provider: the lens size
    }
    if (QTreeWidgetItem *r = row(list, QStringLiteral("phone.mp4")))
        CHECK(r->data(0, MediaPoolPanel::FormatRole).toString() ==
              QStringLiteral("1080×1920 · 30 fps"));
    if (QTreeWidgetItem *r = row(list, QStringLiteral("song.mp3")))
        CHECK(r->data(0, MediaPoolPanel::DetailsRole).toString() == QStringLiteral("Audio · 3:05"));
    QTreeWidgetItem *goneRow = row(list, QStringLiteral("gone.wav"));
    CHECK(goneRow && goneRow->data(0, MediaPoolPanel::MissingRole).toBool());
    CHECK(goneRow && !(goneRow->flags() & Qt::ItemIsDragEnabled));

    // Folders open and close on a click, and stay so through a rebuild.
    QTreeWidgetItem *music = list->topLevelItem(1);
    emit list->itemClicked(music, 0);
    CHECK(!music->isExpanded());
    pool.add({b + QStringLiteral("/new.flac"), MediaItem::Kind::Audio, 1000});
    CHECK(!list->topLevelItem(1)->isExpanded());
    emit list->itemClicked(list->topLevelItem(1), 0);
    CHECK(list->topLevelItem(1)->isExpanded());

    // In use: marked, and can't be removed.
    panel.setInUse({clip1});
    CHECK(row(list, QStringLiteral("VID_1.insv"))->data(0, MediaPoolPanel::InUseRole).toBool());
    CHECK(!row(list, QStringLiteral("VID_2.insv"))->data(0, MediaPoolPanel::InUseRole).toBool());
    auto *remove = find<QAction>(&panel, "mediaRemoveAction");
    auto *add = find<QAction>(&panel, "mediaAddToTimelineAction");
    if (!remove || !add)
        return;
    panel.selectPaths({clip1, clip2});
    CHECK(panel.selectedPaths() == (QStringList{clip1, clip2}));
    CHECK(!remove->isEnabled());
    CHECK(add->isEnabled());

    // Locate file…: only for one missing file; double-clicking a missing
    // file asks for it too, instead of adding it.
    auto *locate = find<QAction>(&panel, "mediaLocateAction");
    if (!locate)
        return;
    QSignalSpy locateSpy(&panel, &MediaPoolPanel::locateRequested);
    panel.selectPaths({clip2});
    CHECK(!locate->isEnabled());
    panel.selectPaths({gone, clip2});
    CHECK(!locate->isEnabled());
    panel.selectPaths({gone});
    CHECK(locate->isEnabled());
    locate->trigger();
    CHECK(locateSpy.size() == 1 && locateSpy.first().first().toString() == gone);
    {
        QSignalSpy addSpy(&panel, &MediaPoolPanel::addToTimelineRequested);
        emit list->itemActivated(row(list, QStringLiteral("gone.wav")), 0);
        CHECK(locateSpy.size() == 2 && addSpy.isEmpty());
    }

    // Drag out: the files as URLs, missing ones left out.
    panel.selectPaths({clip2, gone});
    {
        QList<QTreeWidgetItem *> items = list->selectedItems();
        const QMimeData *mime = list->model()->mimeData(list->selectionModel()->selectedIndexes());
        CHECK(items.size() == 2);
        CHECK(mime && mime->urls() == QList<QUrl>{QUrl::fromLocalFile(clip2)});
        delete mime;
    }

    // Add to timeline: the selected files that are there.
    QSignalSpy addSpy(&panel, &MediaPoolPanel::addToTimelineRequested);
    add->trigger();
    CHECK(addSpy.size() == 1 && addSpy.first().first().toStringList() == QStringList{clip2});
    // Double-click / Enter on a row does the same.
    emit list->itemActivated(row(list, QStringLiteral("phone.mp4")), 0);
    CHECK(addSpy.size() == 2);

    // Remove: gone from the pool and the list; the selection stays on the rest.
    remove->trigger();
    CHECK(!pool.contains(clip2) && !pool.contains(gone));
    CHECK(!row(list, QStringLiteral("VID_2.insv")));

    // Drops from outside: known files and folders, nothing else.
    QSignalSpy dropSpy(&panel, &MediaPoolPanel::filesDropped);
    {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(song), QUrl::fromLocalFile(b),
                      QUrl::fromLocalFile(tmp.path() + QStringLiteral("/notes.txt"))});
        const QPoint at = list->viewport()->rect().center();
        QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &enter);
        CHECK(enter.isAccepted());
        QDropEvent drop(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &drop);
        CHECK(dropSpy.size() == 1 &&
              dropSpy.first().first().toStringList() == (QStringList{song, b}));
    }
    {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(tmp.path() + QStringLiteral("/notes.txt"))});
        QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &enter);
        CHECK(!enter.isAccepted());
    }

    // Thumbnails from the provider, on a worker thread.
    panel.setThumbnailProvider([](const MediaItem &m, const QSize &size) {
        if (m.kind == MediaItem::Kind::Audio)
            return QImage();
        QImage img(size, QImage::Format_RGB32);
        img.fill(Qt::red);
        return img;
    });
    CHECK(QTest::qWaitFor(
        [&] {
            const QTreeWidgetItem *r = row(list, QStringLiteral("VID_1.insv"));
            return r && !r->data(0, MediaPoolPanel::ThumbnailRole).value<QImage>().isNull();
        },
        5000));

    // The recording mode from the provider, on a worker thread; asked for 360
    // clips only (VID_2 was removed above, the phone video isn't one).
    QSet<QString> askedMode;
    QMutex askedMx;
    panel.setModeProvider([&](const MediaItem &m) {
        QMutexLocker lock(&askedMx);
        askedMode.insert(QFileInfo(m.path).fileName());
        return QStringLiteral("8K");
    });
    CHECK(QTest::qWaitFor(
        [&] {
            const QTreeWidgetItem *r = row(list, QStringLiteral("VID_1.insv"));
            return r && r->data(0, MediaPoolPanel::FormatRole).toString() ==
                            QStringLiteral("8K · 29.97 fps");
        },
        5000));
    QTest::qWait(50);
    {
        QMutexLocker lock(&askedMx);
        CHECK(askedMode == QSet<QString>{QStringLiteral("VID_1.insv")});
        askedMode.clear();
    }
    // A rebuild keeps the mode without asking again.
    pool.setItems(pool.items());
    if (QTreeWidgetItem *r = row(list, QStringLiteral("VID_1.insv")))
        CHECK(r->data(0, MediaPoolPanel::FormatRole).toString() == QStringLiteral("8K · 29.97 fps"));
    QTest::qWait(50);
    {
        QMutexLocker lock(&askedMx);
        CHECK(askedMode.isEmpty());
    }

    panel.setStatus(QStringLiteral("Adding 2 files…"));
    CHECK(find<QLabel>(&panel, "mediaPoolStatus")->isVisible());
    panel.setStatus(QString());
    CHECK(!find<QLabel>(&panel, "mediaPoolStatus")->isVisible());

    if (const QByteArray shot = qgetenv("ORBIS_MEDIA_SHOT"); !shot.isEmpty())
        panel.grab().save(QString::fromLocal8Bit(shot));

    // Empty: the hint is painted, nothing crashes.
    pool.clear();
    CHECK(list->topLevelItemCount() == 0);
    list->viewport()->repaint();
}

void testKinds()
{
    CHECK(media::kindForPath(QStringLiteral("/a/VID.INSV")) == MediaItem::Kind::Clip360);
    CHECK(media::kindForPath(QStringLiteral("x.mov")) == MediaItem::Kind::Video);
    CHECK(media::kindForPath(QStringLiteral("x.opus")) == MediaItem::Kind::Audio);
    CHECK(!media::kindForPath(QStringLiteral("LRV_1.lrv")));
    CHECK(!media::kindForPath(QStringLiteral("x.orp")));

    QTemporaryDir tmp;
    for (const char *name : {"b.mp4", "a.insv", "a.lrv", "c.txt", "d.wav"})
        writeFile(tmp.path() + QLatin1Char('/') + QLatin1String(name));
    QDir().mkpath(tmp.path() + QStringLiteral("/sub.mp4"));
    const QStringList files = media::filesInFolder(tmp.path());
    CHECK(files == (QStringList{tmp.path() + QStringLiteral("/a.insv"),
                                tmp.path() + QStringLiteral("/b.mp4"),
                                tmp.path() + QStringLiteral("/d.wav")}));
}

// Thumbnails kept on disk: made once, read back by a new cache (a reopened
// project), made again when the file changes; audio never asks.
void testThumbnailCache()
{
    QTemporaryDir tmp;
    const QString video = tmp.path() + QStringLiteral("/ride.mp4");
    writeFile(video);
    MediaItem item;
    item.path = video;
    item.kind = MediaItem::Kind::Video;
    const QSize size(80, 45);
    int made = 0;
    const ThumbnailCache::Provider make = [&made](const MediaItem &, const QSize &s) {
        ++made;
        QImage img(s, QImage::Format_RGB32);
        img.fill(Qt::red);
        return img;
    };

    ThumbnailCache cache;
    cache.setDirectory(tmp.path() + QStringLiteral("/cache"));
    CHECK(cache.get(item, size, make).size() == size);
    CHECK(made == 1);
    CHECK(QFileInfo::exists(cache.diskPath(video, size)));
    CHECK(cache.diskPath(video, size).startsWith(tmp.path() + QStringLiteral("/cache/thumbnails/")));

    ThumbnailCache reopened;
    reopened.setDirectory(tmp.path() + QStringLiteral("/cache"));
    const QImage again = reopened.get(item, size, make);
    CHECK(made == 1);
    CHECK(again.size() == size && again.pixelColor(10, 10) == QColor(Qt::red));

    // Another size is another picture.
    reopened.get(item, QSize(160, 90), make);
    CHECK(made == 2);

    // The file changed: made again.
    QFile f(video);
    if (f.open(QIODevice::ReadWrite)) {
        f.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime);
        f.close();
    }
    reopened.get(item, size, make);
    CHECK(made == 3);

    // Nothing made (a file that can't be read) isn't kept; audio isn't asked.
    const ThumbnailCache::Provider none = [&made](const MediaItem &, const QSize &) {
        ++made;
        return QImage();
    };
    MediaItem other = item;
    other.path = tmp.path() + QStringLiteral("/broken.mp4");
    writeFile(other.path);
    CHECK(reopened.get(other, size, none).isNull());
    CHECK(!QFileInfo::exists(reopened.diskPath(other.path, size)));
    MediaItem audio = item;
    audio.kind = MediaItem::Kind::Audio;
    const int before = made;
    CHECK(reopened.get(audio, size, make).isNull());
    CHECK(made == before);
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    theme::apply(app, theme::themes().first());
    testKinds();
    testThumbnailCache();
    testSidebar();
    testSidebarResize();
    testMediaTab();
    if (g_failures == 0)
        std::printf("media_pool_test: all checks passed\n");
    else
        std::printf("media_pool_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}

// The project library's widgets, driven with QTest on the offscreen platform
// and found by objectName: the start screen's cards (listing, summaries,
// thumbnails, open, rename, duplicate, remove from list, the empty state)
// and the folder operations under them, and the New project /
// Save project as dialog (name checks, the folder it makes, the output
// format). No GPU, no media decoding. Exit code is the failure count.
// ORBIS_LIBRARY_SHOT=<path> saves a picture of the start screen.

#include "NewProjectDialog.h"
#include "ProjectLibraryPage.h"

#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/ProjectLibrary.h"
#include "../core/Timeline.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>
#include <cstring>

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

void writeFile(const QString &path, const QByteArray &data = QByteArrayLiteral("x"))
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

// A project with `clips` main-track clips of 10 s each (the sources exist
// unless `missing`) saved at `file`.
void makeProject(const QString &file, const QString &mediaDir, int clips, bool missing = false)
{
    Project p;
    for (int i = 0; i < clips; ++i) {
        const QString src = mediaDir + QStringLiteral("/clip%1.insv").arg(i);
        if (!missing)
            writeFile(src);
        p.timeline()->appendClip(src, 10000, 30.0);
    }
    p.save(file);
}

bool waitLoaded(ProjectLibraryPage &page, QSignalSpy &spy)
{
    if (spy.isEmpty() && !spy.wait(5000))
        return false;
    spy.clear();
    return true;
}

QListWidgetItem *cardNamed(QListWidget *list, const QString &name)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == name)
            return list->item(i);
    return nullptr;
}

void testLibraryPage()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString media = tmp.path() + "/media";
    QDir().mkpath(media);
    const QString folder = tmp.path() + "/Projects";

    // Alpha: two clips; Beta: empty; Gamma: its media is gone; Loose: a
    // project file outside the projects folder, listed because it was opened.
    makeProject(projectfolder::create(folder, "Alpha"), media, 2);
    makeProject(projectfolder::create(folder, "Beta"), media, 0);
    QDir().mkpath(tmp.path() + "/gone");
    makeProject(projectfolder::create(folder, "Gamma"), tmp.path() + "/gone", 1, true);
    const QString loose = tmp.path() + "/Loose.orp";
    makeProject(loose, media, 1);

    ProjectLibrary library(tmp.path() + "/projects.json");
    library.load();
    CHECK(library.setProjectsFolder(folder));
    CHECK(library.noteOpened(loose));

    ProjectLibraryPage page(&library);
    page.resize(1000, 700);
    page.show();
    // Delete is a widget shortcut on the list, and Qt fires shortcuts only in
    // the active window (Enter and double-click don't need it).
    page.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&page));
    QSignalSpy loaded(&page, &ProjectLibraryPage::loaded);
    QSignalSpy open(&page, &ProjectLibraryPage::openRequested);
    QSignalSpy newProject(&page, &ProjectLibraryPage::newProjectRequested);
    QSignalSpy openOther(&page, &ProjectLibraryPage::openOtherRequested);
    page.refresh();
    CHECK(waitLoaded(page, loaded));

    auto *list = find<QListWidget>(&page, "projectList");
    if (!list)
        return;
    CHECK(list->count() == 4);
    // The one opened here first, then the found ones.
    CHECK(list->count() > 0 && list->item(0)->text() == "Loose");
    QListWidgetItem *alpha = cardNamed(list, "Alpha");
    QListWidgetItem *beta = cardNamed(list, "Beta");
    QListWidgetItem *gamma = cardNamed(list, "Gamma");
    CHECK(alpha && beta && gamma);
    if (alpha && beta && gamma) {
        CHECK(alpha->data(ProjectLibraryPage::SummaryReadRole).toBool());
        CHECK(alpha->data(ProjectLibraryPage::DetailsRole).toString().contains("2 clips"));
        CHECK(alpha->data(ProjectLibraryPage::DetailsRole).toString().contains("0:20"));
        CHECK(alpha->data(ProjectLibraryPage::ProblemRole).toString().isEmpty());
        CHECK(beta->data(ProjectLibraryPage::DetailsRole).toString() == "Empty");
        CHECK(gamma->data(ProjectLibraryPage::ProblemRole).toString().contains("missing"));
        // Not real .insv files: no thumbnail, the card draws a placeholder.
        CHECK(alpha->data(ProjectLibraryPage::ThumbnailRole).isNull());
    }
    auto *folderLabel = find<QLabel>(&page, "projectsFolderLabel");
    CHECK(folderLabel && folderLabel->text().contains("Projects"));

    // Buttons.
    if (auto *b = find<QPushButton>(&page, "newProjectButton"))
        QTest::mouseClick(b, Qt::LeftButton);
    CHECK(newProject.size() == 1);
    if (auto *b = find<QPushButton>(&page, "openOtherButton"))
        QTest::mouseClick(b, Qt::LeftButton);
    CHECK(openOther.size() == 1);

    // Open: Enter on the card, double-click, the context menu's action.
    if (alpha) {
        list->setCurrentItem(alpha);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Return);
        CHECK(open.size() == 1 && open.last().at(0).toString() == alpha->data(ProjectLibraryPage::PathRole));
        const QRect r = list->visualItemRect(alpha);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, r.center());
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, r.center());
        CHECK(open.size() == 2);
        if (auto *a = find<QAction>(&page, "openProjectCardAction")) {
            CHECK(a->isEnabled());
            a->trigger();
        }
        CHECK(open.size() == 3);
    }

    // Remove from list: Delete on the selected card. The files stay, and it
    // stays gone after a refresh (hidden, though it's in the projects folder).
    if (gamma) {
        const QString gammaPath = gamma->data(ProjectLibraryPage::PathRole).toString();
        list->setCurrentItem(gamma);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Delete);
        CHECK(list->count() == 3);
        CHECK(!cardNamed(list, "Gamma"));
        CHECK(QFileInfo::exists(gammaPath));
        page.refresh();
        CHECK(waitLoaded(page, loaded));
        CHECK(list->count() == 3);
        CHECK(!cardNamed(list, "Gamma"));
    }

    if (const char *shot = std::getenv("ORBIS_LIBRARY_SHOT"))
        page.grab().save(QString::fromLocal8Bit(shot));

    // Remove the rest: the empty state shows instead of the list.
    if (auto *a = find<QAction>(&page, "removeProjectAction")) {
        while (list->count() > 0) {
            list->setCurrentRow(0);
            a->trigger();
        }
    }
    auto *empty = find<QLabel>(&page, "projectLibraryEmpty");
    CHECK(empty && empty->isVisible());
    CHECK(!list->isVisible());
    if (auto *a = find<QAction>(&page, "removeProjectAction"))
        CHECK(!a->isEnabled());
}

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// Rename and duplicate, in projectfolder and from the cards.
void testRenameDuplicate()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString folder = tmp.path() + "/Projects";
    const QString outside = tmp.path() + "/media";
    QDir().mkpath(outside);

    // Ride: one clip outside its folder, one inside it (stored relative),
    // a scan in its cache, an export.
    const QString ride = projectfolder::create(folder, "Ride");
    const QString rideDir = folder + "/Ride";
    QDir().mkpath(rideDir + "/footage");
    writeFile(outside + "/out.insv");
    writeFile(rideDir + "/footage/in.insv");
    {
        Project p;
        p.timeline()->appendClip(outside + "/out.insv", 10000, 30.0);
        p.timeline()->appendClip(rideDir + "/footage/in.insv", 10000, 30.0);
        CHECK(p.save(ride));
    }
    CHECK(readAll(ride).contains("\"footage/in.insv\""));
    QDir().mkpath(rideDir + "/cache/plates");
    writeFile(rideDir + "/cache/plates/scan.bin");
    writeFile(rideDir + "/exports/ride.mp4");
    projectfolder::create(folder, "Taken");

    // Names: bad, taken, the copy name.
    QString error;
    CHECK(projectfolder::rename(ride, "Taken", &error).isEmpty() && error.contains("exists"));
    CHECK(projectfolder::rename(ride, "a/b", &error).isEmpty());
    CHECK(QFileInfo(ride).isFile());
    CHECK(projectfolder::copyName(ride) == "Ride copy");

    // Duplicate: new folder, project file + cache, not exports; the inside
    // clip now stored absolute (it stayed where it was).
    const QString copy = projectfolder::duplicate(ride, projectfolder::copyName(ride), &error);
    CHECK(copy == folder + "/Ride copy/Ride copy.orp");
    CHECK(QFileInfo(folder + "/Ride copy/cache/plates/scan.bin").isFile());
    CHECK(!QFileInfo(folder + "/Ride copy/exports/ride.mp4").exists());
    CHECK(QFileInfo(folder + "/Ride copy/exports").isDir());
    {
        Project p;
        CHECK(p.load(copy));
        CHECK(p.timeline()->count() == 2);
        if (p.timeline()->count() == 2) {
            CHECK(p.timeline()->clipAt(0).sourcePath == outside + "/out.insv");
            CHECK(p.timeline()->clipAt(1).sourcePath == rideDir + "/footage/in.insv");
            CHECK(!p.timeline()->clipAt(1).mediaMissing);
        }
    }
    CHECK(projectfolder::copyName(ride) == "Ride copy 2");
    CHECK(projectfolder::duplicate(ride, "Ride copy", &error).isEmpty());

    // Rename: folder and file; the inside clip moves with it and resolves.
    const QString renamed = projectfolder::rename(ride, "Trip", &error);
    CHECK(renamed == folder + "/Trip/Trip.orp");
    CHECK(QFileInfo(renamed).isFile() && !QFileInfo(rideDir).exists());
    {
        Project p;
        CHECK(p.load(renamed));
        if (p.timeline()->count() == 2)
            CHECK(p.timeline()->clipAt(1).sourcePath == folder + "/Trip/footage/in.insv");
    }

    // A loose project: the file alone.
    const QString loose = tmp.path() + "/Loose.orp";
    makeProject(loose, outside, 1);
    CHECK(projectfolder::rename(loose, "Free", &error) == tmp.path() + "/Free.orp");
    CHECK(projectfolder::duplicate(tmp.path() + "/Free.orp", "Free copy", &error) ==
          tmp.path() + "/Free copy.orp");

    // From the cards: F2 renames, Duplicate… copies, the list follows.
    ProjectLibrary library(tmp.path() + "/projects.json");
    library.load();
    CHECK(library.setProjectsFolder(folder));
    CHECK(library.noteOpened(renamed));
    ProjectLibraryPage page(&library);
    QStringList asked;
    QString answer;
    page.setNamePrompt([&](const QString &title, const QString &text) {
        asked << title + ":" + text;
        return answer;
    });
    page.resize(1000, 700);
    page.show();
    QSignalSpy loaded(&page, &ProjectLibraryPage::loaded);
    page.refresh();
    CHECK(waitLoaded(page, loaded));
    auto *list = find<QListWidget>(&page, "projectList");
    if (!list)
        return;
    page.selectPath(renamed);
    CHECK(page.selectedPath() == renamed);

    answer = "Summer";
    list->setFocus();
    QTest::keyClick(list, Qt::Key_F2);
    CHECK(asked.size() == 1 && asked.last() == "Rename project:Trip");
    CHECK(waitLoaded(page, loaded));
    const QString summer = folder + "/Summer/Summer.orp";
    CHECK(QFileInfo(summer).isFile());
    CHECK(page.selectedPath() == summer);
    CHECK(cardNamed(list, "Summer") && !cardNamed(list, "Trip"));
    // Still the one opened here (kept its place in the list file).
    library.load();
    const auto entries = library.entries();
    CHECK(!entries.isEmpty() && entries.first().path == summer &&
          entries.first().lastOpened.isValid());

    // Cancel does nothing.
    answer.clear();
    if (auto *a = find<QAction>(&page, "duplicateProjectAction")) {
        a->trigger();
        CHECK(asked.last() == "Duplicate project:Summer copy");
        CHECK(!QFileInfo(folder + "/Summer copy").exists());
        answer = "Summer B";
        a->trigger();
        CHECK(waitLoaded(page, loaded));
    }
    const QString b = folder + "/Summer B/Summer B.orp";
    CHECK(QFileInfo(b).isFile());
    CHECK(page.selectedPath() == b);
    CHECK(cardNamed(list, "Summer B"));
    library.load();
    bool listed = false;
    for (const auto &e : library.entries())
        if (e.path == b)
            listed = !e.lastOpened.isValid();
    CHECK(listed);
}

void testNewProjectDialog()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString parent = tmp.path() + "/Projects"; // doesn't exist yet: made on Create
    QDir().mkpath(tmp.path() + "/Projects/Taken");

    NewProjectDialog dlg(NewProjectDialog::Mode::NewProject, parent);
    dlg.show();
    auto *name = find<QLineEdit>(&dlg, "projectName");
    auto *location = find<QLineEdit>(&dlg, "projectLocation");
    auto *result = find<QLabel>(&dlg, "projectFolderResult");
    auto *buttons = dlg.findChild<QDialogButtonBox *>();
    CHECK(name && location && result && buttons);
    if (!name || !location || !result || !buttons)
        return;
    QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
    CHECK(location->text() == QDir::toNativeSeparators(parent));
    CHECK(!ok->isEnabled()); // no name yet

    QTest::keyClicks(name, "Taken");
    CHECK(!ok->isEnabled());
    CHECK(result->text().contains("already exists"));
    CHECK(result->property("problem").toBool());

    name->clear();
    QTest::keyClicks(name, "a/b");
    CHECK(!ok->isEnabled());

    name->clear();
    QTest::keyClicks(name, "Summer trip");
    CHECK(ok->isEnabled());
    CHECK(!result->property("problem").toBool());
    CHECK(result->text().contains("Summer trip.orp"));

    // The output format: 9:16 at its 1440-class size, 30 fps.
    auto *aspect = find<QComboBox>(&dlg, "outputAspect");
    auto *resolution = find<QComboBox>(&dlg, "outputResolution");
    auto *fps = find<QComboBox>(&dlg, "outputFps");
    if (aspect && resolution && fps) {
        aspect->setCurrentIndex(1);
        resolution->setCurrentIndex(2);
        fps->setCurrentIndex(fps->findData(30.0));
    }
    const OutputSettings out = dlg.output();
    CHECK(out.aspect == OutputSettings::Aspect::Portrait9x16);
    CHECK(out.width == 1440 && out.height == 2560);
    CHECK(out.fps == 30.0);

    QTest::mouseClick(ok, Qt::LeftButton);
    CHECK(dlg.result() == QDialog::Accepted);
    CHECK(dlg.projectFile() == parent + "/Summer trip/Summer trip.orp");
    CHECK(QFileInfo(parent + "/Summer trip/cache").isDir());

    // Save as: no output section; a folder that appeared meanwhile is
    // reported on OK and the dialog stays open.
    NewProjectDialog saveAs(NewProjectDialog::Mode::SaveAs, parent);
    saveAs.show();
    CHECK(!saveAs.findChild<QComboBox *>(QStringLiteral("outputAspect")));
    saveAs.setName("Later");
    QDir().mkpath(parent + "/Later");
    auto *saveButtons = saveAs.findChild<QDialogButtonBox *>();
    if (saveButtons) {
        saveButtons->button(QDialogButtonBox::Ok)->setEnabled(true);
        QTest::mouseClick(saveButtons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    }
    CHECK(saveAs.isVisible());
    CHECK(saveAs.projectFile().isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    // Offscreen like the other widget tests: on a real Wayland session the
    // page can't make itself the active window, which Delete needs.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testLibraryPage();
    testRenameDuplicate();
    testNewProjectDialog();
    if (g_failures == 0)
        std::printf("project_library_test: all checks passed\n");
    else
        std::printf("project_library_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}

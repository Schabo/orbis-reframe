// The edit toolbar above the timeline, driven with QTest on the offscreen
// platform and found by objectName: each button's signal, the reframe
// buttons' enabled state, that the tools stay centred whatever the notice
// says, and the notice's tooltip and timeout. No GPU, no media. Exit code is
// the failure count. ORBIS_TOOLBAR_SHOT=<path> saves a picture of the bar.

#include "EditToolbar.h"
#include "Theme.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include <cstdio>
#include <cstdlib>

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

// Clicking the button named `name` emits `signal` once.
template <typename Signal> void checkClick(EditToolbar &bar, const char *name, Signal signal)
{
    QSignalSpy spy(&bar, signal);
    if (auto *b = find<QToolButton>(&bar, name)) {
        QTest::mouseClick(b, Qt::LeftButton);
        if (spy.count() != 1) {
            std::printf("FAIL  %s: %lld signal(s)\n", name, (long long)spy.count());
            ++g_failures;
        }
    } else {
        ++g_failures;
    }
}

void testButtons()
{
    EditToolbar bar;
    bar.resize(1200, 34);
    bar.show();
    checkClick(bar, "trimInButton", &EditToolbar::trimInRequested);
    checkClick(bar, "trimOutButton", &EditToolbar::trimOutRequested);
    checkClick(bar, "trimRestoreButton", &EditToolbar::trimCleared);
    checkClick(bar, "zoomOutButton", &EditToolbar::zoomOutRequested);
    checkClick(bar, "zoomFitButton", &EditToolbar::zoomToFitRequested);
    checkClick(bar, "zoomInButton", &EditToolbar::zoomInRequested);

    // The keyframe buttons only work with keys to delete.
    auto *del = find<QToolButton>(&bar, "reframeDeleteKeyButton");
    auto *all = find<QToolButton>(&bar, "reframeDeleteAllButton");
    CHECK(del && all && !del->isEnabled() && !all->isEnabled());
    bar.setHasReframeKeys(true);
    CHECK(del && all && del->isEnabled() && all->isEnabled());
    checkClick(bar, "reframeDeleteKeyButton", &EditToolbar::deleteKeyRequested);
    checkClick(bar, "reframeDeleteAllButton", &EditToolbar::clearReframeRequested);

    // Arm: a toggle, by click or by the A key's slot.
    QSignalSpy armed(&bar, &EditToolbar::reframeArmedChanged);
    if (auto *arm = find<QToolButton>(&bar, "reframeArmButton")) {
        QTest::mouseClick(arm, Qt::LeftButton);
        CHECK(arm->isChecked() && armed.count() == 1 && armed.last().at(0).toBool());
        bar.toggleReframeArmed();
        CHECK(!arm->isChecked() && armed.count() == 2 && !armed.last().at(0).toBool());
    }
}

// The middle of the trim-in .. delete-all buttons, in the bar.
int toolsCentre(EditToolbar &bar)
{
    auto *first = find<QToolButton>(&bar, "trimInButton");
    auto *last = find<QToolButton>(&bar, "reframeDeleteAllButton");
    if (!first || !last)
        return -1;
    return (first->geometry().left() + last->geometry().right()) / 2;
}

void testCentred()
{
    EditToolbar bar;
    for (int width : {700, 1200, 1900}) {
        bar.resize(width, 34);
        bar.show();
        QApplication::processEvents();
        CHECK(std::abs(toolsCentre(bar) - width / 2) <= 3);
        // A long notice is cut, not pushing the tools aside.
        bar.showNotice(QStringLiteral("Saved screenshot to a folder with a very long name "
                                      "somewhere deep down in the user's home directory.png"));
        QApplication::processEvents();
        CHECK(std::abs(toolsCentre(bar) - width / 2) <= 3);
        bar.showNotice(QString());
    }
}

void testNotice()
{
    EditToolbar bar;
    bar.resize(1200, 34);
    bar.show();
    auto *notice = find<QLabel>(&bar, "editNotice");
    if (!notice)
        return;
    bar.showNotice(QStringLiteral("Too close to the clip's edge to split"));
    CHECK(notice->text() == QStringLiteral("Too close to the clip's edge to split"));
    CHECK(notice->toolTip() == notice->text());
    // Sticky stays; a plain one goes after a few seconds (6 s).
    bar.showNotice(QStringLiteral("Exporting…"), true);
    QTest::qWait(6500);
    CHECK(notice->text() == QStringLiteral("Exporting…"));
    bar.showNotice(QStringLiteral("Done"));
    QTest::qWait(6500);
    CHECK(notice->text().isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    theme::apply(app, theme::themes().first());
    testButtons();
    testCentred();
    testNotice();
    if (const QByteArray shot = qgetenv("ORBIS_TOOLBAR_SHOT"); !shot.isEmpty()) {
        EditToolbar bar;
        bar.resize(1400, 34);
        bar.setHasReframeKeys(true);
        bar.showNotice(QStringLiteral("Saved screenshot"));
        bar.show();
        bar.grab().save(QString::fromLocal8Bit(shot));
    }
    if (g_failures == 0)
        std::printf("edit_toolbar_test: all checks passed\n");
    else
        std::printf("edit_toolbar_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}

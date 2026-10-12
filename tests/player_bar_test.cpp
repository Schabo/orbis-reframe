// The player bar under the viewport, driven with QTest on the offscreen
// platform and found by objectName: the time display's format and the times
// it reads back, its click-to-type, the playback buttons' signals, the
// progress strip's seek / scrub, the speed steps and the volume / mute.
// No GPU, no media. Exit code is the failure count. ORBIS_PLAYER_SHOT=<path>
// saves a picture of the bar.

#include "PlayerBar.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QSignalSpy>
#include <QSlider>
#include <QTest>
#include <QToolButton>

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

void testFormat()
{
    using P = PlayerBar;
    CHECK(P::formatTime(0, 30.0) == QStringLiteral("00:00:00:00"));
    CHECK(P::formatTime(83'500, 30.0) == QStringLiteral("00:01:23:15"));
    CHECK(P::formatTime(83'999, 29.97) == QStringLiteral("00:01:23:29"));
    CHECK(P::formatTime(3'723'040, 25.0) == QStringLiteral("01:02:03:01"));
    CHECK(P::formatTime(3'723'040, 0.0) == QStringLiteral("01:02:03.04"));
    CHECK(P::formatTime(-5, 30.0) == QStringLiteral("00:00:00:00"));
    CHECK(P::formatTime(500, 120.0) == QStringLiteral("00:00:00:060"));

    // Read back as shown, and the shorter forms.
    CHECK(P::parseTime(QStringLiteral("00:01:23:15"), 30.0) == 83'500);
    CHECK(P::parseTime(QStringLiteral("01:02:03:01"), 25.0) == 3'723'040);
    CHECK(P::parseTime(QStringLiteral("01:02:03.04"), 0.0) == 3'723'040);
    CHECK(P::parseTime(QStringLiteral("1:02:03"), 30.0) == 3'723'000);
    CHECK(P::parseTime(QStringLiteral("1:23"), 30.0) == 83'000);
    CHECK(P::parseTime(QStringLiteral("83"), 30.0) == 83'000);
    CHECK(P::parseTime(QStringLiteral("90"), 30.0) == 90'000); // a lone field may pass 59
    CHECK(P::parseTime(QStringLiteral(" 1:23.5 "), 30.0) == 83'500);
    CHECK(P::parseTime(QStringLiteral("12,25"), 30.0) == 12'250);
    CHECK(P::parseTime(QStringLiteral(".5"), 30.0) == 500);
    CHECK(P::parseTime(QStringLiteral("0:0:1:5"), 0.0) == std::nullopt);    // frames need a rate
    CHECK(P::parseTime(QStringLiteral("0:1:2.5:03"), 30.0) == std::nullopt); // decimal only in s
    CHECK(P::parseTime(QStringLiteral("00:01:23:30"), 30.0) == std::nullopt); // no frame 30
    CHECK(P::parseTime(QStringLiteral("1:60"), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral("1:60:00"), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral("abc"), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral("1e3"), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral(""), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral("-3"), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral("1::3"), 30.0) == std::nullopt);
    CHECK(P::parseTime(QStringLiteral("1:2:3:4:5"), 30.0) == std::nullopt);
    // Every frame start reads back as the same frame, at awkward rates too.
    for (const double fps : {23.976, 29.97, 59.94, 30.0, 50.0}) {
        bool ok = true;
        for (qint64 ms = 0; ms < 3000; ms += 7) {
            const QString shown = P::formatTime(ms, fps);
            const std::optional<qint64> back = P::parseTime(shown, fps);
            ok = ok && back && P::formatTime(*back, fps) == shown;
        }
        CHECK(ok);
    }
}

void testButtons()
{
    PlayerBar bar;
    bar.resize(900, 70);
    bar.show();
    const struct {
        const char *name;
        void (PlayerBar::*signal)();
    } buttons[] = {
        {"playerToStartButton", &PlayerBar::toStartClicked},
        {"playerPreviousCutButton", &PlayerBar::previousCutClicked},
        {"playerFrameBackButton", &PlayerBar::frameBackClicked},
        {"playerPlayButton", &PlayerBar::playPauseClicked},
        {"playerFrameForwardButton", &PlayerBar::frameForwardClicked},
        {"playerNextCutButton", &PlayerBar::nextCutClicked},
        {"playerToEndButton", &PlayerBar::toEndClicked},
        {"playerFullScreenButton", &PlayerBar::fullScreenClicked},
    };
    int lastX = -1;
    for (const auto &b : buttons) {
        auto *button = find<QToolButton>(&bar, b.name);
        if (!button)
            continue;
        QSignalSpy spy(&bar, b.signal);
        button->click();
        CHECK(spy.size() == 1);
        // The tooltip names the key.
        CHECK(button->toolTip().contains(QLatin1Char('(')));
        // Left to right in this order (full screen at the far right).
        const int x = button->mapTo(&bar, QPoint(0, 0)).x();
        CHECK(x > lastX);
        lastX = x;
    }
    // The playback buttons are centred.
    auto *play = find<QToolButton>(&bar, "playerPlayButton");
    if (play)
        CHECK(std::abs(play->mapTo(&bar, play->rect().center()).x() - bar.width() / 2) < 20);

    auto *shot = find<QToolButton>(&bar, "playerScreenshotButton");
    if (shot) {
        CHECK(!shot->isEnabled());
        bar.setScreenshotEnabled(true);
        QSignalSpy spy(&bar, &PlayerBar::screenshotRequested);
        shot->click();
        CHECK(spy.size() == 1);
    }
}

void testProgress()
{
    PlayerBar bar;
    bar.resize(900, 70);
    bar.show();
    bar.setDuration(100'000);
    bar.setFrameRate(30.0);
    bar.setCuts({25'000, 60'000});
    auto *progress = find<PlayerProgress>(&bar, "playerProgress");
    if (!progress)
        return;
    QSignalSpy seeks(&bar, &PlayerBar::seekRequested);
    QSignalSpy scrubs(&bar, &PlayerBar::scrubRequested);
    const int y = progress->height() / 2;
    const auto xAt = [progress](qint64 ms) {
        for (int x = 0; x < progress->width(); ++x)
            if (progress->timeAt(x) >= ms)
                return x;
        return progress->width() - 1;
    };

    // A click: one exact seek on press, nothing on release.
    QTest::mouseClick(progress, Qt::LeftButton, {}, QPoint(xAt(50'000), y));
    CHECK(seeks.size() == 1 && scrubs.isEmpty());
    if (!seeks.isEmpty())
        CHECK(std::abs(seeks.first().first().toLongLong() - 50'000) < 500);

    // A drag: seek on press, scrubs while moving, an exact seek on release.
    seeks.clear();
    QTest::mousePress(progress, Qt::LeftButton, {}, QPoint(xAt(10'000), y));
    QTest::mouseMove(progress, QPoint(xAt(20'000), y));
    QTest::mouseMove(progress, QPoint(xAt(30'000), y));
    QTest::mouseRelease(progress, Qt::LeftButton, {}, QPoint(xAt(30'000), y));
    CHECK(seeks.size() == 2);
    CHECK(scrubs.size() == 2);
    if (seeks.size() == 2)
        CHECK(std::abs(seeks.last().first().toLongLong() - 30'000) < 500);

    // Outside the strip it clamps to the project.
    CHECK(progress->timeAt(-50) == 0);
    CHECK(progress->timeAt(progress->width() + 50) == 100'000);

    // Nothing to play: no seeks.
    seeks.clear();
    bar.setDuration(0);
    QTest::mouseClick(progress, Qt::LeftButton, {}, QPoint(xAt(0) + 100, y));
    CHECK(seeks.isEmpty());
}

void testTimeDisplay()
{
    PlayerBar bar;
    bar.resize(900, 70);
    bar.show();
    bar.setDuration(100'000);
    bar.setFrameRate(30.0);
    bar.setPosition(83'500);
    auto *time = find<QToolButton>(&bar, "playerTime");
    auto *edit = find<QLineEdit>(&bar, "playerTimeEdit");
    if (!time || !edit)
        return;
    CHECK(time->text() == QStringLiteral("00:01:23:15 / 00:01:40:00"));

    QSignalSpy seeks(&bar, &PlayerBar::seekRequested);
    time->click();
    CHECK(edit->isVisible() && !time->isVisible());
    CHECK(edit->text() == QStringLiteral("00:01:23:15"));
    edit->setText(QStringLiteral("0:10"));
    QTest::keyClick(edit, Qt::Key_Return);
    CHECK(seeks.size() == 1 && seeks.first().first().toLongLong() == 10'000);
    CHECK(!edit->isVisible() && time->isVisible());

    // Something it can't read stays open to fix; Esc gives up.
    seeks.clear();
    time->click();
    edit->setText(QStringLiteral("nope"));
    QTest::keyClick(edit, Qt::Key_Return);
    CHECK(edit->isVisible() && seeks.isEmpty());
    QTest::keyClick(edit, Qt::Key_Escape);
    CHECK(!edit->isVisible() && seeks.isEmpty());

    // Past the end: the end.
    time->click();
    edit->setText(QStringLiteral("5:00"));
    QTest::keyClick(edit, Qt::Key_Return);
    CHECK(seeks.size() == 1 && seeks.first().first().toLongLong() == 100'000);

    // Without a frame rate (nothing open yet): hundredths.
    bar.setFrameRate(0.0);
    CHECK(time->text() == QStringLiteral("00:01:23.50 / 00:01:40.00"));
}

void testSpeedAndVolume()
{
    PlayerBar bar;
    bar.resize(900, 70);
    bar.show();
    auto *speed = find<QToolButton>(&bar, "playerSpeed");
    if (!speed)
        return;
    CHECK(speed->text() == QStringLiteral("1×"));
    QSignalSpy speeds(&bar, &PlayerBar::speedChanged);
    bar.stepSpeed(1);
    CHECK(bar.speed() == 1.25 && speed->text() == QStringLiteral("1.25×"));
    bar.stepSpeed(1);
    bar.stepSpeed(1);
    bar.stepSpeed(1); // stops at the top
    CHECK(bar.speed() == 2.0);
    CHECK(speeds.size() == 3);
    bar.setSpeed(0.3); // the nearest offered
    CHECK(bar.speed() == 0.25);
    bar.stepSpeed(-1);
    CHECK(bar.speed() == 0.25);
    // The menu picks one too, and shows which is on.
    if (auto *a = find<QAction>(speed->menu(), "playerSpeed50Action")) {
        a->trigger();
        CHECK(bar.speed() == 0.5 && a->isChecked());
    }
    find<QAction>(speed->menu(), "playerSpeed100Action")->trigger();
    CHECK(bar.speed() == 1.0);

    auto *mute = find<QToolButton>(&bar, "playerMuteButton");
    auto *volume = find<QSlider>(&bar, "playerVolume");
    if (!mute || !volume)
        return;
    QSignalSpy volumes(&bar, &PlayerBar::volumeChanged);
    QSignalSpy mutes(&bar, &PlayerBar::mutedChanged);
    bar.setVolume(40);
    CHECK(volume->value() == 40 && bar.volume() == 40);
    CHECK(volumes.size() == 1 && volumes.first().first().toInt() == 40);
    mute->click();
    CHECK(bar.isMuted() && mutes.size() == 1);
    // Turning it up unmutes.
    volume->setValue(60);
    CHECK(!bar.isMuted() && mutes.size() == 2);
    bar.setVolume(400); // clamped
    CHECK(bar.volume() == 100);
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    theme::apply(app, theme::themes().first());
    testFormat();
    testButtons();
    testProgress();
    testTimeDisplay();
    testSpeedAndVolume();
    if (const QByteArray shot = qgetenv("ORBIS_PLAYER_SHOT"); !shot.isEmpty()) {
        PlayerBar bar;
        bar.resize(1100, 70);
        bar.setDuration(272'000);
        bar.setFrameRate(29.97);
        bar.setCuts({61'000, 140'500, 201'000});
        bar.setPosition(97'300);
        bar.setScreenshotEnabled(true);
        bar.show();
        bar.grab().save(QString::fromLocal8Bit(shot));
    }
    if (g_failures == 0)
        std::printf("player_bar_test: all checks passed\n");
    else
        std::printf("player_bar_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}

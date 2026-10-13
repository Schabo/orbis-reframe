// The right sidebar (settings) and File ▸ Project settings…, driven with
// QTest on the offscreen platform and found by objectName: the tabs and
// their sections in order, opening / closing sections (with a greyed-out
// body too), what each section says it applies to, the tab keys' toggle,
// the Stitching section keeping the View menu's part of the stitch config,
// and the Project settings dialog. No GPU, no media.
// Exit code is the failure count. ORBIS_SETTINGS_SHOT=<path> saves a
// picture of the sidebar.

#include "ProjectSettingsDialog.h"
#include "SettingsPanel.h"
#include "SettingsSidebar.h"
#include "Theme.h"
#include "panels/AudioPanel.h"
#include "panels/ColourPanel.h"
#include "panels/MediaInfoPanel.h"
#include "panels/OverlayPanel.h"
#include "panels/PrivacyPanel.h"
#include "panels/ReframePanel.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QSignalSpy>
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

// The sections on a page, top to bottom, by object name.
QStringList sectionNames(QScrollArea *page)
{
    QStringList out;
    QList<SettingsPanel *> sections =
        page->widget()->findChildren<SettingsPanel *>(Qt::FindDirectChildrenOnly);
    std::sort(sections.begin(), sections.end(),
              [](SettingsPanel *a, SettingsPanel *b) { return a->y() < b->y(); });
    for (SettingsPanel *s : sections)
        out << s->objectName();
    return out;
}

void testLayout(SettingsSidebar &bar)
{
    CHECK(bar.objectName() == QLatin1String("settingsSidebar"));
    CHECK(bar.count() == 4);
    const char *pages[] = {"videoSettings", "audioSettings", "overlaySettings", "privacySettings"};
    for (const char *name : pages) {
        find<QScrollArea>(&bar, name);
        find<QToolButton>(&bar, (QByteArray(name) + "Tab").constData());
    }
    // Icon tabs: no text, the tooltip names them.
    if (auto *tab = find<QToolButton>(&bar, "videoSettingsTab")) {
        CHECK(tab->text().isEmpty() || tab->toolButtonStyle() == Qt::ToolButtonIconOnly);
        CHECK(tab->toolTip() == QLatin1String("Video"));
    }
    if (auto *video = find<QScrollArea>(&bar, "videoSettings"))
        CHECK(sectionNames(video) == QStringList({"stitchingPanel", "stabilizationPanel",
                                                  "reframePanel", "colourPanel",
                                                  "mediaInfoPanel"}));
    if (auto *audio = find<QScrollArea>(&bar, "audioSettings"))
        CHECK(sectionNames(audio) == QStringList({"audioPanel"}));

    // Rarely touched sections start closed; the only section on a tab can't
    // close.
    CHECK(!bar.stitching()->isExpanded());
    CHECK(!bar.mediaInfo()->isExpanded());
    CHECK(bar.stabilization()->isExpanded() && bar.stabilization()->isCollapsible());
    CHECK(bar.colour()->isExpanded());
    CHECK(!bar.overlay()->isCollapsible() && bar.overlay()->isExpanded());
    CHECK(!bar.privacy()->isCollapsible());
    CHECK(bar.sections().size() == 8);

    // Wide enough for every section: the pages don't scroll sideways.
    for (const char *name : pages)
        if (auto *page = find<QScrollArea>(&bar, name))
            CHECK(page->widget()->minimumSizeHint().width() <= bar.pageWidth());
}

void testSections(SettingsSidebar &bar)
{
    SettingsPanel *colour = bar.colour();
    auto *header = find<QToolButton>(&bar, "colourPanelHeader");
    auto *body = colour->findChild<QWidget *>(QStringLiteral("settingsPanelBody"));
    if (!header || !body)
        return;
    QSignalSpy toggled(colour, &SettingsPanel::expandedToggled);
    QTest::mouseClick(header, Qt::LeftButton);
    CHECK(!colour->isExpanded() && body->isHidden());
    CHECK(toggled.size() == 1 && !toggled.first().first().toBool());
    QTest::mouseClick(header, Qt::LeftButton);
    CHECK(colour->isExpanded() && !body->isHidden());
    CHECK(toggled.size() == 2);
    colour->setExpanded(false); // restoring a remembered state isn't the user's toggle
    CHECK(toggled.size() == 2);
    colour->setExpanded(true);

    // No clip: the body greys out, the header still opens and closes it.
    bar.colour()->setNoClip();
    CHECK(!body->isEnabled() && header->isEnabled());
    QTest::mouseClick(header, Qt::LeftButton);
    CHECK(!colour->isExpanded());
    QTest::mouseClick(header, Qt::LeftButton);

    // A section that can't collapse ignores its header.
    if (auto *overlayHeader = find<QToolButton>(&bar, "overlayPanelHeader")) {
        QTest::mouseClick(overlayHeader, Qt::LeftButton);
        CHECK(bar.overlay()->isExpanded());
    }
}

void testScopes(SettingsSidebar &bar)
{
    CHECK(bar.stitching()->scope() == QLatin1String("Whole project"));
    CHECK(bar.stabilization()->scope() == QLatin1String("This clip"));
    CHECK(bar.colour()->scope() == QLatin1String("This clip"));
    CHECK(bar.reframe()->scope() == QLatin1String("Keys at the playhead"));
    CHECK(bar.overlay()->scope() == QLatin1String("Selected overlay clip"));
    CHECK(bar.privacy()->scope() == QLatin1String("This clip"));
    if (auto *label = bar.colour()->findChild<QLabel *>(QStringLiteral("settingsPanelScope")))
        CHECK(label->text() == QLatin1String("This clip") && !label->isHidden());
    // The Audio section follows what it shows.
    AudioPanel::AudioClipState s;
    s.label = QStringLiteral("music.mp3");
    s.spanMs = 10000;
    bar.audio()->setAudioClipState(s);
    CHECK(bar.audio()->scope() == QLatin1String("Selected audio clip"));
    s.label = QStringLiteral("clip.insv (camera mic)");
    s.selected = false; // the sound of the video clip under the playhead
    bar.audio()->setAudioClipState(s);
    CHECK(bar.audio()->scope() == QLatin1String("This clip"));
}

void testTabKeys(SettingsSidebar &bar)
{
    bar.setOpen(true);
    bar.setCurrentIndex(0);
    QSignalSpy current(&bar, &TabSidebar::currentChanged);
    bar.toggleTab(1); // Alt+2: Audio
    CHECK(bar.currentIndex() == 1 && bar.isOpen());
    CHECK(current.size() == 1 && current.first().first().toInt() == 1);
    bar.toggleTab(1); // again: closes
    CHECK(!bar.isOpen() && bar.currentIndex() == 1);
    bar.toggleTab(2); // another tab: opens on it
    CHECK(bar.isOpen() && bar.currentIndex() == 2);
    bar.toggleTab(7); // no such tab
    CHECK(bar.isOpen() && bar.currentIndex() == 2);
    bar.setCurrentIndex(0);
}

void testStitchingKeepsPreview(SettingsSidebar &bar)
{
    StitchingPanel *stitching = bar.stitching();
    stitch::PreviewStitchConfig cfg;
    cfg.width = 3840;
    cfg.height = 1920;
    cfg.lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
    stitching->setConfig(cfg);
    QSignalSpy changed(stitching, &StitchingPanel::configChanged);
    stitching->setExpanded(true);
    auto *guard = stitching->findChild<QComboBox *>();
    if (!guard) {
        CHECK(guard);
        return;
    }
    guard->setCurrentIndex(0); // None
    CHECK(changed.size() == 1);
    if (!changed.isEmpty()) {
        const auto out = changed.first().first().value<stitch::PreviewStitchConfig>();
        CHECK(out.lensGuard == stitch::PreviewStitchConfig::LensGuard::None);
        CHECK(out.width == 3840 && out.height == 1920); // the View menu's part, unchanged
    }
}

void testProjectSettingsDialog()
{
    OutputSettings out;
    out.aspect = OutputSettings::Aspect::Square1x1;
    out.width = out.height = 1080;
    out.fps = 30.0;
    ProjectSettingsDialog dialog(out, 59.94);
    CHECK(dialog.objectName() == QLatin1String("projectSettingsDialog"));
    CHECK(dialog.output() == out);
    auto *aspect = find<QComboBox>(&dialog, "outputAspect");
    auto *fps = find<QComboBox>(&dialog, "outputFps");
    if (!aspect || !fps)
        return;
    CHECK(aspect->currentIndex() == 2);
    CHECK(fps->itemText(0).contains(QLatin1String("59.94")));
    aspect->setCurrentIndex(0); // 16:9, same size class
    const OutputSettings edited = dialog.output();
    CHECK(edited.aspect == OutputSettings::Aspect::Landscape16x9);
    CHECK(edited.width == 1920 && edited.height == 1080 && edited.fps == 30.0);
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    theme::apply(app, theme::themes().first());
    qRegisterMetaType<stitch::PreviewStitchConfig>();

    SettingsSidebar bar;
    bar.resize(bar.sizeHint().width(), 900);
    bar.show();
    QApplication::processEvents();

    testLayout(bar);
    testSections(bar);
    testScopes(bar);
    testTabKeys(bar);
    testStitchingKeepsPreview(bar);
    testProjectSettingsDialog();

    if (const QByteArray shot = qgetenv("ORBIS_SETTINGS_SHOT"); !shot.isEmpty()) {
        bar.setOpen(true);
        bar.setCurrentIndex(0);
        bar.stitching()->setExpanded(true);
        QApplication::processEvents();
        bar.grab().save(QString::fromLocal8Bit(shot));
    }

    if (g_failures == 0)
        std::printf("settings_sidebar_test: all checks passed\n");
    else
        std::printf("settings_sidebar_test: %d check(s) FAILED\n", g_failures);
    return g_failures;
}

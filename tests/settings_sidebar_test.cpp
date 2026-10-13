// The right sidebar (settings) and File ▸ Project settings…, driven with
// QTest on the offscreen platform and found by objectName: the tabs and
// their sections in order, opening / closing sections (with a greyed-out
// body too), what each section says it applies to, the tab keys' toggle,
// the Stitching section keeping the View menu's part of the stitch config,
// the Overlay tab's layer row, and the Project settings dialog. No GPU, no media.
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
#include <QTemporaryDir>
#include <QSlider>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
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
    if (auto *audio = find<QScrollArea>(&bar, "audioSettings")) // nothing to show yet
        CHECK(sectionNames(audio) == QStringList({"audioPanel"}));

    // Rarely touched sections start closed; the only section on a tab can't
    // close.
    CHECK(!bar.stitching()->isExpanded());
    CHECK(!bar.mediaInfo()->isExpanded());
    CHECK(bar.stabilization()->isExpanded() && bar.stabilization()->isCollapsible());
    CHECK(bar.colour()->isExpanded());
    CHECK(!bar.overlay()->isCollapsible() && bar.overlay()->isExpanded());
    CHECK(!bar.privacy()->isCollapsible());
    CHECK(bar.sections().size() == 7); // the Audio tab's come and go (testAudioSections)

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
}

// The Audio tab: a section per audio clip, titled by its track.
void testAudioSections(SettingsSidebar &bar)
{
    auto *page = find<QScrollArea>(&bar, "audioSettings");
    if (!page)
        return;
    bar.setOpen(true);
    bar.setCurrentIndex(1);
    const auto settle = [] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    };
    AudioPanel *audio = bar.audio();
    CHECK(audio->scope().isEmpty()); // the placeholder

    // A video clip with two mics: both, in track order, "This clip".
    AudioClipState cam, ext;
    cam.clipId = 10;
    cam.trackId = 1;
    cam.kind = QStringLiteral("cameraMic");
    cam.trackName = QStringLiteral("Camera mic");
    cam.label = QStringLiteral("clip.insv");
    cam.spanMs = 10000;
    cam.linked = true;
    ext = cam;
    ext.clipId = 11;
    ext.trackId = 2;
    ext.kind = QStringLiteral("externalMic");
    ext.trackName = QStringLiteral("External mic");
    audio->setClips({cam, ext}, QStringLiteral("This clip"));
    settle();
    CHECK(sectionNames(page) == QStringList({"audioCameraMicPanel", "audioExternalMicPanel"}));
    CHECK(audio->scope() == QLatin1String("This clip"));
    const QList<SettingsPanel *> two = audio->sections();
    CHECK(two.size() == 2 && two[0]->title() == QLatin1String("Camera mic") &&
          two[1]->title() == QLatin1String("External mic") && two[1]->isCollapsible());
    if (two.size() != 2)
        return;

    // An edit says which clip / track; the same clips again update in place.
    QSignalSpy clipEdits(audio, &AudioPanel::audioClipEdited);
    QSignalSpy trackEdits(audio, &AudioPanel::trackLevelEdited);
    const QList<QSlider *> sliders = two[1]->findChildren<QSlider *>();
    // In the order made: noise strength, track level, level.
    CHECK(sliders.size() == 3);
    if (sliders.size() == 3) {
        sliders[2]->setValue(-6);
        CHECK(clipEdits.size() == 1 && clipEdits.last().at(0).toInt() == 11 &&
              clipEdits.last().at(1).toDouble() == -6.0);
        sliders[1]->setValue(-3);
        CHECK(trackEdits.size() == 1 && trackEdits.last().at(0).toInt() == 2);
    }
    ext.gainDb = -6.0;
    audio->setClips({cam, ext}, QStringLiteral("This clip"));
    settle();
    CHECK(audio->sections() == two); // not rebuilt
    CHECK(clipEdits.size() == 1);   // and setting doesn't emit

    // One selected audio clip: just it.
    AudioClipState music;
    music.clipId = 20;
    music.trackId = 3;
    music.trackName = QStringLiteral("Music");
    music.label = QStringLiteral("song.mp3");
    music.spanMs = 5000;
    audio->setClips({music}, QStringLiteral("Selected audio clip"));
    settle();
    CHECK(sectionNames(page) == QStringList({"audioClipPanel"}));
    CHECK(audio->scope() == QLatin1String("Selected audio clip"));

    audio->setNoClip();
    settle();
    CHECK(sectionNames(page) == QStringList({"audioPanel"}));
    bar.setCurrentIndex(0);
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

// The Overlay tab's layer row: which layer the selected clip's track is,
// and Bring forward / Send backward only where there's a layer to pass.
void testOverlayLayer(SettingsSidebar &bar)
{
    OverlayPanel *panel = bar.overlay();
    auto *label = find<QLabel>(panel, "overlayLayerLabel");
    auto *forward = find<QPushButton>(panel, "overlayBringForward");
    auto *backward = find<QPushButton>(panel, "overlaySendBackward");
    if (!label || !forward || !backward)
        return;
    QSignalSpy fwd(panel, &OverlayPanel::bringForwardRequested);
    QSignalSpy back(panel, &OverlayPanel::sendBackwardRequested);

    OverlayPanel::State st;
    st.trackName = "Cam";
    st.layer = 1;
    st.layerCount = 2;
    st.inFrontOf = "Overlay 2";
    st.spanMs = 5000;
    panel->setState(st);
    CHECK(label->text() == QLatin1String("1 of 2 (front)"));
    CHECK(label->toolTip().contains(QString::fromUtf8("In front of “Overlay 2”")));
    CHECK(!forward->isEnabled() && backward->isEnabled());
    QTest::mouseClick(backward, Qt::LeftButton);
    CHECK(back.count() == 1 && fwd.count() == 0);

    st.layer = 2;
    st.inFrontOf.clear();
    st.behind = "Overlay 2";
    panel->setState(st);
    CHECK(label->text() == QLatin1String("2 of 2 (back)"));
    CHECK(forward->isEnabled() && !backward->isEnabled());
    QTest::mouseClick(forward, Qt::LeftButton);
    CHECK(fwd.count() == 1);

    st.layer = st.layerCount = 1;
    st.behind.clear();
    panel->setState(st);
    CHECK(label->text() == QLatin1String("1 of 1"));
    CHECK(!forward->isEnabled() && !backward->isEnabled());

    panel->setNoClip();
    CHECK(label->text().isEmpty() && !forward->isEnabled());
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // The Audio tab's sections read their open / closed state from QSettings:
    // not the user's.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
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
    testAudioSections(bar);
    testTabKeys(bar);
    testStitchingKeepsPreview(bar);
    testOverlayLayer(bar);
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

// The right sidebar (settings) and File ▸ Project settings…, driven with
// QTest on the offscreen platform and found by objectName: the tabs and
// their sections in order, opening / closing sections (with a greyed-out
// body too), what each section says it applies to, the tab keys' toggle,
// the Stitching section keeping the View menu's part of the stitch config,
// the Overlay tab's layer row, the Easing field of the Reframe and Overlay
// sections, the Colour section's LUT picker, and the Project settings
// dialog. No GPU, no media.
// Exit code is the failure count. ORBIS_SETTINGS_SHOT=<path> saves a
// picture of the sidebar.

#include "ProjectSettingsDialog.h"
#include "SettingsPanel.h"
#include "SettingsSidebar.h"
#include "Theme.h"
#include "panels/AudioPanel.h"
#include "panels/ColourPanel.h"
#include "panels/MediaInfoPanel.h"

#include <MediaInfo.h>
#include "panels/OverlayPanel.h"
#include "panels/TitleAnimationPanel.h"
#include "panels/TitleLookPanel.h"
#include "panels/TitleTextPanel.h"
#include "panels/TransitionPanel.h"
#include "panels/PrivacyPanel.h"
#include "panels/ReframePanel.h"
#include "panels/SplitScreenPanel.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"

#include <QApplication>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
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
    CHECK(bar.count() == 6);
    const char *pages[] = {"videoSettings",   "audioSettings",      "overlaySettings",
                           "titleSettings",   "transitionSettings", "privacySettings"};
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
                                                  "reframePanel", "splitScreenPanel",
                                                  "colourPanel", "mediaInfoPanel"}));
    if (auto *title = find<QScrollArea>(&bar, "titleSettings"))
        CHECK(sectionNames(title) ==
              QStringList({"titleTextPanel", "titleLookPanel", "titleAnimationPanel",
                           "titlePlacementPanel"}));
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
    CHECK(!bar.transition()->isCollapsible());
    CHECK(bar.sections().size() == 13); // the Audio tab's come and go (testAudioSections)

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
    CHECK(bar.stitching()->scope() == QLatin1String("This clip"));
    CHECK(bar.stabilization()->scope() == QLatin1String("This clip"));
    CHECK(bar.colour()->scope() == QLatin1String("This clip"));
    CHECK(bar.reframe()->scope() == QLatin1String("Keys at the playhead"));
    CHECK(bar.splitScreen()->scope() == QLatin1String("This clip"));
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

void testStitchingPanel(SettingsSidebar &bar)
{
    using G = StitchingPanel::LensGuard;
    StitchingPanel *stitching = bar.stitching();
    CHECK(stitching->scope() == QLatin1String("This clip"));
    QSignalSpy edited(stitching, &StitchingPanel::stitchEdited);
    QSignalSpy all(stitching, &StitchingPanel::applyToAllRequested);
    stitching->setState(G::Standard, false); // shows a clip: no signal
    CHECK(edited.isEmpty());
    stitching->setExpanded(true);
    auto *guard = stitching->findChild<QComboBox *>(QStringLiteral("stitchLensGuard"));
    auto *depth = stitching->findChild<QCheckBox *>(QStringLiteral("stitchDepthSeam"));
    auto *apply = stitching->findChild<QPushButton *>(QStringLiteral("stitchApplyToAll"));
    if (!guard || !depth || !apply) {
        CHECK(guard && depth && apply);
        return;
    }
    CHECK(guard->currentIndex() == int(G::Standard) && !depth->isChecked());
    guard->setCurrentIndex(0); // None
    CHECK(edited.size() == 1);
    if (!edited.isEmpty()) {
        CHECK(edited.first().at(0).value<G>() == G::None);
        CHECK(!edited.first().at(1).toBool());
    }
    depth->setChecked(true);
    CHECK(edited.size() == 2 && edited.last().at(1).toBool());
    apply->click();
    CHECK(all.size() == 1);
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

// The Easing field: greyed out unless the playhead is between two keys, shows
// that segment's ease without emitting, and a pick emits it; the Reframe
// one hides on a flat clip and greys out while tracking.
void testEasing(SettingsSidebar &bar)
{
    OverlayPanel *overlay = bar.overlay();
    auto *oEase = find<QComboBox>(overlay, "overlayEaseCombo");
    ReframePanel *reframe = bar.reframe();
    auto *rEase = find<QComboBox>(reframe, "reframeEaseCombo");
    if (!oEase || !rEase)
        return;
    CHECK(oEase->count() == 3 && oEase->itemText(1) == QLatin1String("Fast to slow") &&
          oEase->itemText(2) == QLatin1String("Slow to fast"));

    QSignalSpy oSpy(overlay, &OverlayPanel::easeEdited);
    OverlayPanel::State st;
    st.spanMs = 5000;
    st.keyCount = 2;
    st.playheadInClip = true;
    overlay->setState(st);
    CHECK(!oEase->isEnabled());
    st.easeAvailable = true;
    st.ease = keytrack::Ease::Out;
    overlay->setState(st);
    CHECK(oEase->isEnabled() && oEase->currentIndex() == 1 && oSpy.isEmpty());
    oEase->setFocus();
    QTest::keyClick(oEase, Qt::Key_Down);
    CHECK(oSpy.count() == 1 &&
          oSpy.at(0).at(0).value<keytrack::Ease>() == keytrack::Ease::In);
    overlay->setNoClip();
    CHECK(!oEase->isEnabled());

    QSignalSpy rSpy(reframe, &ReframePanel::easeEdited);
    reframe->setEase(true, keytrack::Ease::In);
    CHECK(rEase->isEnabled() && rEase->currentIndex() == 2 && rSpy.isEmpty());
    rEase->setFocus();
    QTest::keyClick(rEase, Qt::Key_Up);
    CHECK(rSpy.count() == 1 &&
          rSpy.at(0).at(0).value<keytrack::Ease>() == keytrack::Ease::Out);
    reframe->setTrackRunning(true);
    CHECK(!rEase->isEnabled());
    reframe->setTrackRunning(false);
    CHECK(rEase->isEnabled());
    reframe->setEase(false, keytrack::Ease::Linear);
    CHECK(!rEase->isEnabled() && rEase->currentIndex() == 0);
    reframe->setClip(true);
    CHECK(!rEase->isVisibleTo(reframe));
    reframe->setClip(false);
    CHECK(rEase->isVisibleTo(reframe));

    // No clip: the fields grey out, except Stop tracking while a run is on.
    QWidget *stop = reframe->trackStopButton();
    reframe->setNoClip();
    CHECK(!find<QDoubleSpinBox>(reframe, "reframeFov")->isEnabled());
    reframe->setTrackRunning(true);
    CHECK(stop->isEnabled());
    reframe->setTrackRunning(false);
    CHECK(!stop->isEnabled());
    reframe->setClip(false);
    CHECK(find<QDoubleSpinBox>(reframe, "reframeFov")->isEnabled() && stop->isEnabled());
}

// The Title tab's Look section: reflects a title without emitting, greys out
// what is off, reports edits as the whole style.
void testTitleLook(SettingsSidebar &bar)
{
    TitleLookPanel *look = bar.titleLook();
    CHECK(look->title() == "Look");
    auto *outline = find<QCheckBox>(look, "titleOutline");
    auto *width = find<QDoubleSpinBox>(look, "titleOutlineWidth");
    auto *shadow = find<QCheckBox>(look, "titleShadow");
    auto *blur = find<QDoubleSpinBox>(look, "titleShadowBlur");
    auto *box = find<QCheckBox>(look, "titleBox");
    auto *corners = find<QSlider>(look, "titleBoxCorners");
    auto *lines = find<QDoubleSpinBox>(look, "titleLineSpacing");
    if (!outline || !width || !shadow || !blur || !box || !corners || !lines)
        return;
    CHECK(!outline->isEnabled()); // no title
    QSignalSpy spy(look, &TitleLookPanel::titleEdited);
    title::Style s;
    s.text = "Pass";
    s.align = title::Align::Right;
    s.shadow.on = true;
    s.shadow.blur = 0.1f;
    look->setTitle(s);
    CHECK(spy.isEmpty() && outline->isEnabled() && !outline->isChecked() && !width->isEnabled());
    CHECK(shadow->isChecked() && blur->isEnabled() && std::abs(blur->value() - 10.0) < 1e-9);
    CHECK(!box->isChecked() && !corners->isEnabled() && std::abs(lines->value() - 100.0) < 1e-9);
    outline->click();
    CHECK(spy.count() == 1 && width->isEnabled());
    title::Style got = spy.last().at(0).value<title::Style>();
    CHECK(got.outline.on && got.text == "Pass" && got.align == title::Align::Right &&
          got.shadow == s.shadow);
    lines->setValue(150.0);
    CHECK(spy.count() == 2 &&
          std::abs(spy.last().at(0).value<title::Style>().lineSpacing - 1.5f) < 1e-6f);
    box->click();
    corners->setValue(20);
    got = spy.last().at(0).value<title::Style>();
    CHECK(got.box.on && std::abs(got.box.cornerRadius - 0.2f) < 1e-6f && got.outline.on);
    look->setNoTitle(QString());
    CHECK(!outline->isEnabled() && !corners->isEnabled());
}

// The Title tab's Animation section: reflects a title's motion without
// emitting, greys out a length with nothing to time, reports edits.
void testTitleAnimation(SettingsSidebar &bar)
{
    TitleAnimationPanel *anim = bar.titleAnimation();
    CHECK(anim->title() == "Animation");
    auto *in = find<QComboBox>(anim, "titleMotionIn");
    auto *inLength = find<QDoubleSpinBox>(anim, "titleMotionInLength");
    auto *out = find<QComboBox>(anim, "titleMotionOut");
    auto *outLength = find<QDoubleSpinBox>(anim, "titleMotionOutLength");
    if (!in || !inLength || !out || !outLength)
        return;
    CHECK(!in->isEnabled() && !outLength->isEnabled()); // no title
    QSignalSpy spy(anim, &TitleAnimationPanel::motionEdited);
    inset::Motion m;
    m.in = {inset::Motion::Kind::Slide, inset::Motion::Side::Bottom, 1500};
    anim->setMotion(m);
    CHECK(spy.isEmpty() && in->isEnabled() && in->currentText().contains("bottom"));
    CHECK(inLength->isEnabled() && std::abs(inLength->value() - 1.5) < 1e-9);
    CHECK(out->currentIndex() == 0 && !outLength->isEnabled());
    out->setCurrentIndex(out->count() - 1); // shrink away
    CHECK(spy.count() == 1 && outLength->isEnabled());
    const inset::Motion got = spy.last().at(0).value<inset::Motion>();
    CHECK(got.in == m.in && got.out.kind == inset::Motion::Kind::Pop);
    inLength->setValue(0.3);
    CHECK(spy.count() == 2 && spy.last().at(0).value<inset::Motion>().in.ms == 300);
    anim->setNoTitle(QString());
    CHECK(!in->isEnabled() && !inLength->isEnabled());
}

// The Title tab: the Text section reflects a title without emitting and
// reports edits; Position and timing is the overlay panel without a frame,
// its size the text's.
void testTitleTab(SettingsSidebar &bar)
{
    TitleTextPanel *text = bar.titleText();
    OverlayPanel *place = bar.titlePlacement();
    CHECK(place->kind() == OverlayPanel::Kind::Title && place->title() == "Position and timing");
    auto *edit = find<QPlainTextEdit>(text, "titleText");
    auto *bold = find<QPushButton>(text, "titleBold");
    if (!edit || !bold)
        return;
    QSignalSpy spy(text, &TitleTextPanel::titleEdited);
    title::Style s;
    s.text = "Day 1";
    s.bold = false;
    text->setTitle(s);
    CHECK(spy.isEmpty() && edit->toPlainText() == "Day 1" && !bold->isChecked());
    bold->click();
    CHECK(spy.count() == 1 && spy.last().at(0).value<title::Style>().bold &&
          spy.last().at(0).value<title::Style>().text == "Day 1");

    // The font: a list to pick from, not a field; a pick is reported and goes
    // to the top; a font that isn't installed is kept, not replaced.
    auto *font = find<QComboBox>(text, "titleFont");
    QStringList families;
    for (const QString &f : QFontDatabase::families())
        if (!QFontDatabase::isPrivateFamily(f))
            families << f;
    if (font && families.size() >= 2) {
        CHECK(!font->isEditable());
        s.font = families.at(0);
        text->setTitle(s);
        CHECK(font->currentData().toString() == families.at(0) && spy.count() == 1);
        const int other = font->findData(families.at(1));
        CHECK(other >= 0);
        font->setCurrentIndex(other);
        emit font->activated(other);
        CHECK(spy.count() == 2 && spy.last().at(0).value<title::Style>().font == families.at(1));
        CHECK(font->itemData(0).toString() == families.at(1)); // recent, on top
        CHECK(font->currentData().toString() == families.at(1));
        s.font = "No Such Font 12";
        text->setTitle(s);
        CHECK(spy.count() == 2 && font->currentIndex() == 0 &&
              font->currentText().contains("not installed"));
        bold->click();
        CHECK(spy.count() == 3 && spy.last().at(0).value<title::Style>().font == "No Such Font 12");
        bold->click();
    } else {
        CHECK(font);
    }
    // Alignment: one of three, reported as the style's.
    auto *left = find<QPushButton>(text, "titleAlignLeft");
    auto *centre = find<QPushButton>(text, "titleAlignCentre");
    if (left && centre) {
        CHECK(centre->isChecked() && !left->isChecked());
        const qsizetype before = spy.count();
        left->click();
        CHECK(spy.count() == before + 1 && !centre->isChecked() &&
              spy.last().at(0).value<title::Style>().align == title::Align::Left);
    }
    // Each section reports the whole style, keeping what the other set.
    s.box.on = true;
    s.box.padding = 0.4f;
    text->setTitle(s);
    bold->click();
    CHECK(spy.last().at(0).value<title::Style>().box == s.box);
    text->setNoTitle(QString());
    CHECK(!edit->isEnabled() && !left->isEnabled());

    testTitleLook(bar);
    testTitleAnimation(bar);

    OverlayPanel::State st;
    st.layer = st.layerCount = 1;
    st.spanMs = st.maxSpanMs = 5000;
    place->setState(st);
    bar.setCurrentIndex(SettingsSidebar::TitleTab);
    QApplication::processEvents();
    auto *size = find<QDoubleSpinBox>(place, "titleDuration");
    CHECK(size && size->isVisibleTo(&bar));
    bool border = false, textSize = false;
    for (QLabel *l : place->findChildren<QLabel *>()) {
        border |= l->text() == "Border" && l->isVisibleTo(&bar);
        textSize |= l->text() == "Text size" && l->isVisibleTo(&bar);
    }
    CHECK(!border && textSize);
    place->setNoClip();
}

// The tracking status shows under the Track subject button, also after a
// clip has been shown (setClip hides the status row while no run is on).
void testTrackStatus(SettingsSidebar &bar)
{
    ReframePanel *reframe = bar.reframe();
    QWidget *button = reframe->trackStopButton();
    auto *status = find<QLabel>(reframe, "trackStatus");
    if (!status)
        return;
    bar.setCurrentIndex(SettingsSidebar::VideoTab);
    reframe->setExpanded(true);
    reframe->setClip(false);
    reframe->setTrackRunning(true);
    reframe->setTrackStatus(QStringLiteral("1.0 s tracked."));
    QApplication::processEvents();
    CHECK(status->isVisible());
    CHECK(status->mapTo(reframe, QPoint()).y() > button->mapTo(reframe, QPoint()).y());
    reframe->setTrackRunning(false);
    QApplication::processEvents();
    CHECK(!status->isVisible());
}

// The Transition tab: the selected transition's length, why it plays
// shorter, and removing it; greyed out with none selected.
void testTransitionTab(SettingsSidebar &bar)
{
    bar.setCurrentIndex(SettingsSidebar::TransitionTab);
    QApplication::processEvents();
    TransitionPanel *panel = bar.transition();
    auto *duration = find<QDoubleSpinBox>(&bar, "transitionDuration");
    auto *note = find<QLabel>(&bar, "transitionNote");
    auto *remove = find<QPushButton>(&bar, "transitionRemove");
    auto *cut = find<QLabel>(&bar, "transitionCut");
    if (!duration || !note || !remove || !cut)
        return;
    CHECK(!duration->isEnabled() && !remove->isEnabled() && note->isHidden());

    QSignalSpy edited(panel, &TransitionPanel::durationEdited);
    QSignalSpy removed(panel, &TransitionPanel::removeRequested);
    Transition t;
    t.durationMs = 1500;
    panel->setTransition(t, QStringLiteral("“a” and “b”"), QString());
    CHECK(duration->isEnabled() && remove->isEnabled() && note->isHidden());
    CHECK(duration->value() == 1.5 && cut->text().contains(QStringLiteral("“b”")));
    CHECK(edited.isEmpty()); // showing it isn't an edit
    panel->setTransition(t, QString(), QStringLiteral("Plays 0.6 s: …"));
    CHECK(note->isVisibleTo(panel) && note->text().startsWith(QStringLiteral("Plays")));

    duration->setValue(2.0);
    CHECK(edited.size() == 1 && edited.first().first().toLongLong() == 2000);
    QTest::mouseClick(remove, Qt::LeftButton);
    CHECK(removed.size() == 1);
    // The type: shown, and picked.
    auto *type = find<QComboBox>(&bar, "transitionType");
    QSignalSpy typed(panel, &TransitionPanel::typeEdited);
    t.type = Transition::Type::DipToBlack;
    panel->setTransition(t, QString(), QString());
    CHECK(type && type->isEnabled() && type->currentText() == QStringLiteral("Dip to black"));
    CHECK(typed.isEmpty());
    if (type) {
        type->setCurrentIndex(type->findData(int(Transition::Type::Crossfade)));
        emit type->activated(type->currentIndex()); // as a pick from the list
    }
    CHECK(typed.size() == 1 &&
          typed.first().first().value<Transition::Type>() == Transition::Type::Crossfade);
    panel->setNoTransition(QString());
    CHECK(!duration->isEnabled() && !remove->isEnabled() && note->isHidden());
    CHECK(type && !type->isEnabled());
}

// The Colour section's LUT: the picker lists None, the LUTs it's given and
// Load LUT...; picking one edits the grade, Load LUT... asks for a file and
// keeps the clip's; Strength only with a LUT; Reset drops it.
void testColourLut(SettingsSidebar &bar)
{
    ColourPanel *colour = bar.colour();
    auto *lut = find<QComboBox>(colour, "colourLut");
    auto *strength = find<QSlider>(colour, "colourLutStrength");
    auto *problem = find<QLabel>(colour, "colourLutProblem");
    if (!lut || !strength || !problem)
        return;
    colour->setLuts({"/p/luts/X5 Log.cube", "/p/luts/Teal.cube"});
    colour->setState(ColorGrade{});
    CHECK(lut->count() == 5); // None, two, separator, Load LUT...
    CHECK(lut->currentIndex() == 0 && !strength->isEnabled());
    CHECK(lut->itemText(1) == "X5 Log");

    QSignalSpy edited(colour, &ColourPanel::gradeEdited);
    QSignalSpy load(colour, &ColourPanel::loadLutRequested);
    lut->setCurrentIndex(1);
    emit lut->activated(1);
    CHECK(!edited.isEmpty());
    CHECK(edited.last().at(0).value<ColorGrade>().lut == "/p/luts/X5 Log.cube");
    CHECK(strength->isEnabled());

    // A clip's LUT not in the list is shown anyway; Load LUT... keeps it.
    ColorGrade g;
    g.lut = "/elsewhere/Film.cube";
    g.lutStrength = 60.0f;
    colour->setState(g);
    CHECK(lut->currentText() == "Film" && strength->value() == 60);
    edited.clear();
    lut->setCurrentIndex(lut->count() - 1);
    emit lut->activated(lut->count() - 1);
    CHECK(load.size() == 1 && edited.isEmpty());
    CHECK(lut->currentData().toString() == "/elsewhere/Film.cube");

    colour->setLutProblem("Film.cube isn't applied: the file is missing.");
    CHECK(!problem->isHidden());
    colour->setLutProblem({});
    CHECK(problem->isHidden());

    // A log clip without a LUT: a red frame on the picker, and why.
    auto *wanted = find<QLabel>(colour, "colourLutWanted");
    CHECK(wanted && wanted->isHidden() && !lut->property("attention").toBool());
    colour->setLutWanted("This clip was filmed in I-Log, a log profile.");
    CHECK(wanted && !wanted->isHidden() && lut->property("attention").toBool());
    colour->setLutWanted({});
    CHECK(wanted && wanted->isHidden() && !lut->property("attention").toBool());

    auto *reset = find<QPushButton>(colour, "colourReset");
    CHECK(reset && reset->isEnabled());
    edited.clear();
    if (reset)
        reset->click();
    CHECK(!edited.isEmpty() && edited.last().at(0).value<ColorGrade>().lut.isEmpty());
    colour->setState(ColorGrade{});
}

// Media information: the colour profile the trailer names, I-Log by its
// own name, an HDR mode with the format its stream is tagged with.
void testMediaInfoProfile(SettingsSidebar &bar)
{
    MediaInfoPanel *panel = bar.mediaInfo();
    auto shows = [panel](const QString &text) {
        for (const QLabel *l : panel->findChildren<QLabel *>())
            if (l->text() == text)
                return true;
        return false;
    };
    camera::MediaInfo m;
    m.path = "/c/VID_20261018_185907_00_006.insv";
    camera::MediaVideoStream v;
    v.width = v.height = 2880;
    v.fps = 29.97;
    m.video = {v, v};
    m.cameraModel = "Insta360 X5";
    m.colorProfile = "I_Log";
    m.logProfile = "I-Log";
    panel->setInfo(m);
    CHECK(shows("Colour profile") && shows("I-Log"));
    m.colorProfile = "standard";
    m.logProfile.clear();
    panel->setInfo(m);
    CHECK(shows("Colour profile") && shows("Standard"));
    m.colorProfile = "Dolby_Vision"; // HDR: the camera's name, the stream's format
    m.hdrFormat = "HLG";
    m.video[0].colorSpace = m.video[1].colorSpace = "bt2020nc";
    panel->setInfo(m);
    CHECK(shows("Dolby Vision (HLG)") && shows("BT.2020, HLG, limited range"));
    m.colorProfile.clear(); // an HDR file without a camera's name for it
    panel->setInfo(m);
    CHECK(shows("Colour profile") && shows("HLG"));
    m.hdrFormat.clear();
    m.video[0].colorSpace = m.video[1].colorSpace = "";

    // Recorded: by the camera's clock, not the container's (UTC) time; a
    // camera's file that doesn't say has no row.
    const QLocale locale;
    m.creationTime = "2026-10-18T16:59:07.000000Z";
    m.recordedLocal = "2026-10-18T18:59:07";
    panel->setInfo(m);
    CHECK(shows("Recorded") &&
          shows(locale.toString(QDateTime(QDate(2026, 10, 18), QTime(18, 59, 7)),
                                QLocale::ShortFormat)));
    m.recordedLocal.clear();
    panel->setInfo(m);
    CHECK(!shows("Recorded"));

    m.cameraModel.clear();
    panel->setInfo(m); // a phone video: nothing to say, the container's time
    CHECK(!shows("Colour profile"));
    CHECK(shows(locale.toString(
        QDateTime(QDate(2026, 10, 18), QTime(16, 59, 7), QTimeZone::UTC).toLocalTime(),
        QLocale::ShortFormat)));
    panel->setNoClip();
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
    testStitchingPanel(bar);
    testOverlayLayer(bar);
    testEasing(bar);
    testTitleTab(bar);
    testTrackStatus(bar);
    testTransitionTab(bar);
    testColourLut(bar);
    testMediaInfoProfile(bar);
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

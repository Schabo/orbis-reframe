#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QString>

class QApplication;

// The app's colours. Every colour the UI uses is a named token here; a theme
// is one set of values for all of them (adding a theme = adding a set in
// Theme.cpp and to themes(); the user picks one under View ▸ Theme). The stylesheet (resources/theme.qss) is a template that names
// the tokens as @token; the palette, custom painting (timeline, project
// cards, the viewport's surround and outlines) and the icons read them too.
//
// Rules the token sets follow: surfaces and the viewport's surround stay
// neutral grey (judging a grade needs a colour-free surround); the accent
// marks small things only -- play, playhead, toggles that are on,
// selection, focus, links, icons on hover or when on; clips are muted, cool
// colours so the accent stands out on them; errors and warnings keep clear
// of the accent.
namespace theme {

// X(token, what it colours)
#define ORBIS_THEME_TOKENS(X)                                                                      \
    X(window, "window background")                                                                 \
    X(panel, "panels, inputs, cards, menus")                                                       \
    X(button, "buttons; hover on panels and cards")                                                \
    X(pressed, "a button held down")                                                               \
    X(border, "borders, separators, scroll handles")                                               \
    X(borderHover, "an input's border under the mouse; scroll handle hover")                       \
    X(menuBar, "menu bar, the sidebar's icon rail, timeline rows")                                 \
    X(surround, "around the picture in the viewport")                                              \
    X(text, "text")                                                                                \
    X(textDim, "secondary text: hints, labels, ruler")                                             \
    X(textDisabled, "disabled text and icons")                                                     \
    X(icon, "icons at rest")                                                                       \
    X(iconActive, "icons under the mouse or when on")                                              \
    X(accent, "the accent: selection, focus, sliders, checked boxes, links")                       \
    X(accentSoft, "background of something toggled on or selected")                               \
    X(textOnAccent, "text on the accent colour")                                                   \
    X(playhead, "the playhead")                                                                    \
    X(clipVideo, "video clips")                                                                    \
    X(clipVideoAlt, "every other video clip")                                                      \
    X(clipAudio, "audio clips")                                                                    \
    X(clipOverlay, "overlay clips")                                                                \
    X(clipMuted, "muted audio clips and hidden overlays")                                          \
    X(clipMissing, "clips whose file is missing")                                                  \
    X(waveform, "waveforms and clip marks (fades, keyframes)")                                     \
    X(shade, "a translucent shade over the timeline past its end")                                 \
    X(error, "errors, missing files, a lost track")                                                \
    X(warning, "warnings")                                                                         \
    X(success, "something going well: a subject being tracked")                                    \
    X(plateDetected, "privacy blur outlines: a detected plate")                                    \
    X(plateTracked, "privacy blur outlines: a tracked plate")                                      \
    X(plateFilled, "privacy blur outlines: a filled-in plate")                                     \
    X(plateUnverified, "privacy blur outlines: tracked, not verified")                             \
    X(plateRemoved, "privacy blur outlines: a removed blur")                                       \
    X(plateAdded, "privacy blur outlines: blurred by hand")

struct Colors {
#define ORBIS_THEME_FIELD(name, what) QColor name;
    ORBIS_THEME_TOKENS(ORBIS_THEME_FIELD)
#undef ORBIS_THEME_FIELD
};

struct Theme {
    QString id;   // stable, for remembering the choice
    QString name; // shown to people
    Colors colors;
};

// The themes the app ships; the first is the default.
const QList<Theme> &themes();

// The theme in use (the default until apply() picks another).
const Theme &current();
inline const Colors &colors() { return current().colors; }

// The theme with id `id`; the default (the first) when there's none.
const Theme &byId(const QString &id);

// Make `theme` current: the palette and the stylesheet built from its tokens.
// Icons made by icon() and widgets that read colors() when they paint pick
// up the new colours by themselves; what keeps a colour it read once
// (the viewport's surround, a label's rich text) listens to changed().
void apply(QApplication &app, const Theme &theme);

// Says when apply() switched the theme.
class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier *notifier();

// resources/theme.qss with every @token replaced by `colors`' value. An
// unknown @token is left as it is (theme_test checks there are none).
// url(icon:NAME/@token) becomes the url of a file holding the bundled icon
// NAME drawn in that token's colour (an opaque one).
QString styleSheet(const Colors &colors);

// How an icon is tinted.
enum class IconTone {
    Normal, // icon at rest, iconActive under the mouse / when on
    Accent, // the accent at rest too (the play button)
};

// A bundled Lucide icon (resources/icons/<name>.svg, drawn with
// currentColor), tinted from the current theme when it's drawn: icon /
// iconActive (QIcon::Active, QIcon::Selected, or state On) / textDisabled.
QIcon icon(const QString &name, IconTone tone = IconTone::Normal);

// A colour as the stylesheet writes it: #rrggbb, or rgba() when translucent.
QString cssColor(const QColor &c);

} // namespace theme

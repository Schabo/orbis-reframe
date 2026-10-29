#pragma once

#include "../SettingsPanel.h"

#include "../../core/Title.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QWidget;

// The Overlay tab's Text section: the selected title's text (several lines),
// with an Insert menu for the clock's and the GPS's fields
// (title::withFields) and, when it shows a time, how the clock writes the
// hours, and when it shows a speed or an altitude, their units; its
// font, bold / italic, colour and alignment (title::Style). The font list is
// the installed families, each in its own face, with the last few picked
// (app-wide, not the project's) at the top. Where it sits,
// its size and timing are the tab's last section, an OverlayPanel of kind
// Title; the outline, shadow, box and spacing are TitleLookPanel's.
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class TitleTextPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit TitleTextPanel(QWidget *parent = nullptr);

    // Reflect a title. Does not emit. The text is only replaced when it
    // differs, so the cursor stays put while the user types. `clockNote`,
    // shown under the text when not empty: why its fields show nothing
    // somewhere.
    void setTitle(const title::Style &style, const QString &clockNote = QString());
    // No title selected: `hint` says why, the fields are greyed out.
    void setNoTitle(const QString &hint);

signals:
    // The whole style: the last one set with this section's fields changed.
    void titleEdited(const title::Style &style);

private:
    title::Style current() const;
    void setColor(uint32_t rgb);
    // Fill the font list: recent fonts, the installed ones, and `family` at
    // the top when it isn't installed (a project from another computer), so
    // it is kept until the user picks another. Selects `family`.
    void fillFonts(const QString &family);
    void fontPicked(int index);

    QPlainTextEdit *m_text = nullptr;
    QPushButton *m_insert = nullptr;
    QComboBox *m_hours = nullptr; // title::Hours order
    QComboBox *m_speedUnit = nullptr;    // title::SpeedUnit order
    QComboBox *m_altitudeUnit = nullptr; // title::AltitudeUnit order
    // Show the hours row while the text has a time field, a unit's while
    // it has a field in that unit.
    void showHours();
    QLabel *m_clockNote = nullptr;
    QComboBox *m_font = nullptr;
    QWidget *m_styleRow = nullptr;
    QPushButton *m_bold = nullptr;
    QPushButton *m_italic = nullptr;
    QPushButton *m_color = nullptr;
    QWidget *m_alignRow = nullptr;
    QPushButton *m_align[3] = {}; // title::Align order
    title::Style m_style; // as last set
    uint32_t m_rgb = 0xffffff;
    bool m_updating = false;
};

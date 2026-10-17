#pragma once

#include "../SettingsPanel.h"

#include "../../core/Title.h"

class QFontComboBox;
class QPlainTextEdit;
class QPushButton;
class QWidget;

// The Title tab's Text section: the selected title's text (several lines,
// centred), font, bold / italic and colour (title::Style). Where it sits,
// its size and timing are the tab's other section, an OverlayPanel of kind
// Title.
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class TitleTextPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit TitleTextPanel(QWidget *parent = nullptr);

    // Reflect a title. Does not emit. The text is only replaced when it
    // differs, so the cursor stays put while the user types.
    void setTitle(const title::Style &style);
    // No title selected: `hint` says why, the fields are greyed out.
    void setNoTitle(const QString &hint);

signals:
    void titleEdited(const title::Style &style);

private:
    title::Style current() const;
    void setColor(uint32_t rgb);

    QPlainTextEdit *m_text = nullptr;
    QFontComboBox *m_font = nullptr;
    QWidget *m_styleRow = nullptr;
    QPushButton *m_bold = nullptr;
    QPushButton *m_italic = nullptr;
    QPushButton *m_color = nullptr;
    uint32_t m_rgb = 0xffffff;
    bool m_updating = false;
};

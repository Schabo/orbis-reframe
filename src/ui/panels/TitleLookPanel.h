#pragma once

#include "../SettingsPanel.h"

#include "../../core/Title.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QWidget;

// The Overlay tab's Look section: how the selected title's text is drawn
// beyond its font -- line and letter spacing, an outline, a drop shadow and a
// box behind it (title::Style). Lengths are shown as a share of the text
// size, as the style keeps them.
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class TitleLookPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit TitleLookPanel(QWidget *parent = nullptr);

    // Reflect a title. Does not emit.
    void setTitle(const title::Style &style);
    // No title selected: `hint` says why, the fields are greyed out.
    void setNoTitle(const QString &hint);

signals:
    // The whole style: the last one set with this section's fields changed.
    void titleEdited(const title::Style &style);

private:
    title::Style current() const;
    // Grey out each part's fields while it's off.
    void updateEnabled();
    void pickColor(QPushButton *button, uint32_t *rgb, const QString &caption);

    QDoubleSpinBox *m_lineSpacing = nullptr;
    QDoubleSpinBox *m_letterSpacing = nullptr;
    QCheckBox *m_outline = nullptr;
    QPushButton *m_outlineColor = nullptr;
    QDoubleSpinBox *m_outlineWidth = nullptr;
    QCheckBox *m_shadow = nullptr;
    QPushButton *m_shadowColor = nullptr;
    QSlider *m_shadowOpacity = nullptr;
    QLabel *m_shadowOpacityValue = nullptr;
    QDoubleSpinBox *m_shadowDistance = nullptr;
    QDoubleSpinBox *m_shadowBlur = nullptr;
    QCheckBox *m_box = nullptr;
    QPushButton *m_boxColor = nullptr;
    QSlider *m_boxOpacity = nullptr;
    QLabel *m_boxOpacityValue = nullptr;
    QDoubleSpinBox *m_boxPadding = nullptr;
    QSlider *m_boxCorners = nullptr;
    QLabel *m_boxCornersValue = nullptr;
    QList<QWidget *> m_fields; // everything greyed out with no title
    title::Style m_style;      // as last set, with the colours picked since
    bool m_enabled = false;
    bool m_updating = false;
};

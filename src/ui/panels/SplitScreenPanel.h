#pragma once

#include "../SettingsPanel.h"

#include "../../core/PaneLayout.h" // pane::Split

class QComboBox;
class QDoubleSpinBox;
class QPushButton;

// Split screen for the clip: its layout (one picture, side by side, stacked,
// a 2 x 2 grid), the divider between the panes, and Fill panes, which points
// the panes front / back / left / right in one go. Which pane the view
// fields and the viewport steer is picked in the Reframe section, next to
// the fields it governs.
class SplitScreenPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit SplitScreenPanel(QWidget *parent = nullptr);

    // The clip's split. Does not emit.
    void setSplit(const pane::Split &split);
    // No clip to split: shows one picture, greyed out.
    void setNoClip();
    // While subject tracking runs the panes can't change.
    void setEditable(bool editable);

signals:
    // The layout or the divider changed.
    void splitEdited(const pane::Split &split);
    void fillPanesRequested();

private:
    pane::Split currentSplit() const;
    void showSplitRows();
    void setGapColor(uint32_t rgb);

    QComboBox *m_layout = nullptr;
    QDoubleSpinBox *m_gap = nullptr;
    QPushButton *m_gapColor = nullptr;
    QPushButton *m_fillPanes = nullptr;
    uint32_t m_gapRgb = 0;
    bool m_updating = false; // true while setSplit() writes the widgets
};

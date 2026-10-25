#pragma once

#include "../SettingsPanel.h"

#include "../../core/InsetGeometry.h"

class QComboBox;
class QDoubleSpinBox;

// The Title tab's Animation section: how the selected title comes on at
// its start and goes off at its end (inset::Motion) -- a slide from or to
// an edge of the frame, or a pop -- each with its length.
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class TitleAnimationPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit TitleAnimationPanel(QWidget *parent = nullptr);

    // Reflect a title's motion. Does not emit.
    void setMotion(const inset::Motion &motion);
    // No title selected: `hint` says why, the fields are greyed out.
    void setNoTitle(const QString &hint);

signals:
    void motionEdited(const inset::Motion &motion);

private:
    inset::Motion current() const;
    void updateEnabled();

    QComboBox *m_in = nullptr;
    QDoubleSpinBox *m_inLength = nullptr;
    QComboBox *m_out = nullptr;
    QDoubleSpinBox *m_outLength = nullptr;
    bool m_enabled = false;
    bool m_updating = false;
};

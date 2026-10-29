#pragma once

#include "../SettingsPanel.h"

#include "../../core/InsetGeometry.h"

class QComboBox;
class QDoubleSpinBox;

// The Animation section: how the selected overlay comes on at its start and
// goes off at its end (inset::Motion) -- a slide from or to an edge of the
// frame, a pop, or (text only) typing on / off -- each with its length.
// Two of them, like OverlayPanel: kind Video on the Picture in picture tab,
// for PiP videos, and kind Title on the Overlay tab, for titles and gauges.
// Their widgets' object names start with "overlay" / "title".
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class AnimationPanel : public SettingsPanel {
    Q_OBJECT

public:
    enum class Kind { Video, Title };
    explicit AnimationPanel(Kind kind = Kind::Title, QWidget *parent = nullptr);
    Kind kind() const { return m_kind; }

    // Reflect a clip's motion. `canType` false (a gauge's) leaves out typing
    // on or off, which only text does; kind Video never offers it. Does not
    // emit.
    void setMotion(const inset::Motion &motion, bool canType = true);
    // No clip of its kind selected: `hint` (else how to get one) explains,
    // the fields are greyed out.
    void setNoClip(const QString &hint = QString());

signals:
    void motionEdited(const inset::Motion &motion);

private:
    inset::Motion current() const;
    void updateEnabled();

    QComboBox *m_in = nullptr;
    QDoubleSpinBox *m_inLength = nullptr;
    QComboBox *m_out = nullptr;
    QDoubleSpinBox *m_outLength = nullptr;
    Kind m_kind = Kind::Title;
    bool m_enabled = false;
    bool m_updating = false;
};

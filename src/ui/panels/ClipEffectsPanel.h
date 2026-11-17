#pragma once

#include "../SettingsPanel.h"

#include "../../core/ClipEffects.h"

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;
class QWidget;

// The Video tab's Effects section: the video effects on the clip under the
// playhead (the selected one), each with its own rows -- on / off, Remove,
// and its settings -- in the order they run. Effects are put on from the
// left sidebar's Effects tab. An edit hands the clip's new fx::ClipEffects
// back to MainWindow, which writes it onto the clip (Timeline::setClipEffects).
class ClipEffectsPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit ClipEffectsPanel(QWidget *parent = nullptr);

    // Reflect a clip's effects. Does not emit effectsEdited.
    void setState(const fx::ClipEffects &effects);
    // No clip under the playhead: none, greyed out. Does not emit.
    void setNoClip();
    // The .cube looks the look list offers besides the built-in ones
    // (absolute paths: the project's looks/ folder and the clips' looks).
    void setLookFiles(const QStringList &paths);
    // Why the clip's look isn't applied (a missing file, a later version's
    // built-in look), or empty.
    void setLookProblem(const QString &problem);

signals:
    void effectsEdited(const fx::ClipEffects &effects);
    // The look list's Load LUT...: pick a .cube for the clip's look.
    void loadLookRequested();

private:
    // One effect's rows: its heading (the name to tick on / off, Remove),
    // then a percent slider per setting.
    struct Setting {
        QWidget *row = nullptr;
        QSlider *slider = nullptr;
        QLabel *value = nullptr;
    };
    struct Group {
        const char *name = nullptr; // fx::kVignette, ...
        QWidget *head = nullptr;
        QCheckBox *on = nullptr;
        QToolButton *remove = nullptr;
        std::vector<Setting> settings;
    };
    // A group's heading: `id` names its widgets ("vignette" -> vignetteOn,
    // vignetteRemove).
    Group addGroup(const char *name, const QString &label, const QString &removeTip,
                   const QString &id);
    // A setting's row in `g`, objectName `id`.
    void addSetting(Group &g, const QString &label, int lo, int hi, const QString &id,
                    const QString &tip);
    // Show `g` (present: the clip has it) with `values`, in its settings'
    // order.
    void showGroup(const Group &g, bool present, bool on, std::initializer_list<float> values);
    float value(const Group &g, size_t i) const;
    // The look list's entry for `look` (added when it isn't listed), current.
    void selectLook(const fx::Look &look);
    fx::ClipEffects currentEffects() const;
    void emitIfEdited();
    void enableSettings(const fx::ClipEffects &e);

    fx::ClipEffects m_shown;
    QLabel *m_none = nullptr; // "No effects": how to put one on
    Group m_look;
    QWidget *m_lookRow = nullptr;
    QComboBox *m_lookChoice = nullptr;
    QLabel *m_lookProblem = nullptr;
    QStringList m_lookFiles;
    Group m_vignette;
    Group m_grain;
    bool m_updating = false;
};

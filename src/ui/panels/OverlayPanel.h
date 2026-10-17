#pragma once

#include "../SettingsPanel.h"

#include "../../core/InsetGeometry.h"
#include "../../core/KeyTrack.h" // keytrack::Ease

class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;

// The Overlay (picture-in-picture) panel: the video-track clip selected on the
// timeline (or picked in the view) -- its track's layer (which overlays it
// draws in front of / behind), where its inset sits (position, size),
// how it looks (opacity, corner radius, border) and its fades and start.
// Keyframes: with none, an edit sets the clip's one look; once it has some, an
// edit of position / size / opacity sets a key at the playhead, and the fields
// follow the playhead.
// Two of them: kind Video on the Overlay tab, for overlay videos, and kind
// Title on the Title tab ("Position and timing", beside TitleTextPanel),
// for titles -- the same fields, but the size is the text's height and
// there are no corners or border. Their widgets' object names start with
// "overlay" / "title".
//
// Pure view like the other panels: edits go back to MainWindow as signals,
// which writes them to the Timeline.
class OverlayPanel : public SettingsPanel {
    Q_OBJECT

public:
    enum class Kind { Video, Title };
    explicit OverlayPanel(Kind kind = Kind::Video, QWidget *parent = nullptr);
    Kind kind() const { return m_kind; }

    struct State {
        QString label;
        QString trackName;
        // Its track's layer among the overlay tracks: 1 = in front.
        int layer = 0;
        int layerCount = 0;
        QString inFrontOf; // the overlay track just behind it, if any
        QString behind;    // the one just in front of it, if any
        inset::Look look; // at the playhead (keys sampled), before the fades
        inset::Frame frame;
        qint64 fadeInMs = 0;
        qint64 fadeOutMs = 0;
        qint64 spanMs = 0;    // its length on the timeline
        qint64 maxSpanMs = 0; // ... at most: to its source's end (a title's is hours)
        qint64 startMs = 0;
        int keyCount = 0;
        bool keyAtPlayhead = false;  // a key sits at the playhead
        bool playheadInClip = false; // keys can be set here
        // The playhead is between two keys: the move to the next one eases so.
        bool easeAvailable = false;
        keytrack::Ease ease = keytrack::Ease::Linear;
    };
    // Reflect a clip. Does not emit.
    void setState(const State &s);
    // No clip of its kind selected: `hint` (else how to get one) explains,
    // the rest is greyed out.
    void setNoClip(const QString &hint = QString());

signals:
    void lookEdited(const inset::Look &look);
    void frameEdited(const inset::Frame &frame);
    // Fades, start and length on the timeline (`spanMs`).
    void timingEdited(qint64 fadeInMs, qint64 fadeOutMs, qint64 startMs, qint64 spanMs);
    void addKeyRequested();
    void removeKeyRequested();
    void clearKeysRequested();
    void easeEdited(keytrack::Ease ease);
    // Move its track a layer to the front / back.
    void bringForwardRequested();
    void sendBackwardRequested();

private:
    inset::Look currentLook() const;
    inset::Frame currentFrame() const;
    void setBorderColor(uint32_t rgb);
    // "overlay<suffix>" or "title<suffix>", by kind.
    QString partName(const char *suffix) const;

    QLabel *m_clipName = nullptr;
    QLabel *m_trackName = nullptr;
    QLabel *m_layer = nullptr;
    QPushButton *m_forward = nullptr;
    QPushButton *m_backward = nullptr;
    QDoubleSpinBox *m_x = nullptr;     // centre, % of the output width
    QDoubleSpinBox *m_y = nullptr;     // % of the height
    QDoubleSpinBox *m_size = nullptr;  // width, % of the output width
    QSlider *m_opacity = nullptr;      // %
    QLabel *m_opacityValue = nullptr;
    QSlider *m_radius = nullptr;       // % of the shorter side, 0..50
    QLabel *m_radiusValue = nullptr;
    QDoubleSpinBox *m_border = nullptr; // % of the output height
    QPushButton *m_borderColor = nullptr;
    QDoubleSpinBox *m_fadeIn = nullptr;
    QDoubleSpinBox *m_fadeOut = nullptr;
    QDoubleSpinBox *m_start = nullptr;
    QDoubleSpinBox *m_duration = nullptr;
    QLabel *m_keys = nullptr;
    QPushButton *m_addKey = nullptr;
    QPushButton *m_removeKey = nullptr;
    QPushButton *m_clearKeys = nullptr;
    QComboBox *m_ease = nullptr;
    Kind m_kind = Kind::Video;
    QList<QWidget *> m_clipRows; // greyed out with no clip
    uint32_t m_borderRgb = 0xffffff;
    bool m_updating = false;
};

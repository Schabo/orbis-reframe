#include "OverlayPanel.h"

#include "EaseCombo.h"
#include "PanelFields.h"

#include "../../core/TrackClip.h" // TrackClip::kMinTrimMs

#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

#include <algorithm>
#include <cmath>

using panelfields::numberBox;
using panelfields::sliderRow;

OverlayPanel::OverlayPanel(Kind kind, QWidget *parent)
    : SettingsPanel(kind == Kind::Title ? tr("Position and timing") : tr("Picture in picture"), parent),
      m_kind(kind)
{
    const bool title = kind == Kind::Title;
    m_trackName = new QLabel(this);
    form()->addRow(tr("Track"), m_trackName);
    m_layer = new QLabel(this);
    m_layer->setObjectName(partName("LayerLabel"));
    form()->addRow(tr("Layer"), m_layer);
    auto *layerRow = new QWidget(this);
    auto *layerLayout = new QHBoxLayout(layerRow);
    layerLayout->setContentsMargins(0, 0, 0, 0);
    m_forward = new QPushButton(tr("Bring forward"), layerRow);
    m_forward->setObjectName(partName("BringForward"));
    m_forward->setToolTip(tr("Draw this track's overlays in front of the overlay track above "
                             "it (its track moves up)."));
    m_backward = new QPushButton(tr("Send backward"), layerRow);
    m_backward->setObjectName(partName("SendBackward"));
    m_backward->setToolTip(tr("Draw this track's overlays behind the overlay track below it "
                              "(its track moves down)."));
    layerLayout->addWidget(m_forward);
    layerLayout->addWidget(m_backward);
    form()->addRow(QString(), layerRow);
    m_clipName = new QLabel(this);
    m_clipName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_clipName->setMinimumWidth(1); // a long name mustn't widen the sidebar
    form()->addRow(tr("Clip"), m_clipName);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    form()->addRow(sep);

    m_x = numberBox(this, -50.0, 150.0, 1, 1.0, QStringLiteral(" %"));
    m_x->setToolTip(tr("The inset's centre, across the output frame (0% = left edge)."));
    form()->addRow(tr("Position X"), m_x);
    m_y = numberBox(this, -50.0, 150.0, 1, 1.0, QStringLiteral(" %"));
    m_y->setToolTip(tr("The inset's centre, down the output frame (0% = top edge)."));
    form()->addRow(tr("Position Y"), m_y);
    m_size = numberBox(this, inset::Look::kMinWidth * 100.0, inset::Look::kMaxWidth * 100.0, 1,
                       1.0, QStringLiteral(" %"));
    m_size->setToolTip(title ? tr("The text's size: its font's height, as a share of the "
                                  "video's height.")
                             : tr("The inset's width, as a share of the video's. Its height "
                                  "follows the inset video's shape."));
    form()->addRow(title ? tr("Text size") : tr("Size"), m_size);
    form()->addRow(tr("Opacity"), sliderRow(this, 0, 100, &m_opacity, &m_opacityValue));

    m_keys = new QLabel(this);
    m_keys->setObjectName(partName("KeyCount"));
    form()->addRow(tr("Keyframes"), m_keys);
    auto *keyRow = new QWidget(this);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    m_addKey = new QPushButton(tr("Set key"), keyRow);
    m_addKey->setToolTip(tr("Keyframe the position, size and opacity at the playhead (K). With "
                            "keys, the inset moves between them, and edits here set a key."));
    m_removeKey = new QPushButton(tr("Delete"), keyRow);
    m_removeKey->setToolTip(tr("Delete the key at the playhead (Shift+K). The Delete key "
                               "removes the whole clip."));
    m_clearKeys = new QPushButton(tr("Clear"), keyRow);
    m_clearKeys->setToolTip(tr("Delete every key (Ctrl+Shift+K); the inset stays where it is "
                               "at the playhead."));
    keyLayout->addWidget(m_addKey);
    keyLayout->addWidget(m_removeKey);
    keyLayout->addWidget(m_clearKeys);
    form()->addRow(QString(), keyRow);
    m_ease = easecombo::make(this);
    m_ease->setObjectName(partName("EaseCombo"));
    m_ease->setToolTip(tr("How the inset moves from the key at the playhead (or the last one "
                          "before it) to the next key: at an even pace, starting fast and "
                          "slowing into the next key, or starting slowly and speeding up."));
    form()->addRow(tr("Easing"), m_ease);

    auto *sep2 = new QFrame(this);
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    form()->addRow(sep2);

    form()->addRow(tr("Corners"), sliderRow(this, 0, 50, &m_radius, &m_radiusValue));
    m_radius->setToolTip(tr("Corner rounding: 50% makes the short sides fully round."));
    m_border = numberBox(this, 0.0, inset::Frame::kMaxBorder * 100.0, 1, 0.1, QStringLiteral(" %"));
    m_border->setToolTip(tr("Border width, as a share of the video's height (0.5 % is about 5 "
                            "pixels at 1080p)."));
    form()->addRow(tr("Border"), m_border);
    m_borderColor = new QPushButton(this);
    m_borderColor->setToolTip(tr("Border colour."));
    form()->addRow(tr("Border colour"), m_borderColor);

    m_fadeIn = numberBox(this, 0.0, 3600.0, 1, 0.5, QStringLiteral(" s"));
    m_fadeIn->setToolTip(tr("Fade the inset in over this long at the clip's start."));
    form()->addRow(tr("Fade in"), m_fadeIn);
    m_fadeOut = numberBox(this, 0.0, 3600.0, 1, 0.5, QStringLiteral(" s"));
    m_fadeOut->setToolTip(tr("Fade the inset out over this long at the clip's end."));
    form()->addRow(tr("Fade out"), m_fadeOut);
    m_start = numberBox(this, 0.0, 24 * 3600.0, 3, 0.04, QStringLiteral(" s"));
    m_start->setToolTip(tr("Where the clip starts on the timeline."));
    form()->addRow(tr("Start"), m_start);
    m_duration = numberBox(this, TrackClip::kMinTrimMs / 1000.0, 24 * 3600.0, 3, 0.5,
                           QStringLiteral(" s"));
    m_duration->setObjectName(partName("Duration"));
    m_duration->setToolTip(tr("How long the clip shows. Its start stays put; the end moves."));
    form()->addRow(tr("Duration"), m_duration);

    m_clipRows = {layerRow, m_x,      m_y,      m_size,        m_opacity, keyRow, m_ease,
                  m_radius, m_border, m_borderColor, m_fadeIn, m_fadeOut, m_start, m_duration};
    if (title) // a title has no frame
        for (QWidget *w : {static_cast<QWidget *>(m_radius->parentWidget()),
                           static_cast<QWidget *>(m_border), static_cast<QWidget *>(m_borderColor),
                           static_cast<QWidget *>(sep2)})
            form()->setRowVisible(w, false);

    setNoClip();

    auto look = [this] {
        if (!m_updating)
            emit lookEdited(currentLook());
    };
    connect(m_x, &QDoubleSpinBox::valueChanged, this, look);
    connect(m_y, &QDoubleSpinBox::valueChanged, this, look);
    connect(m_size, &QDoubleSpinBox::valueChanged, this, look);
    connect(m_opacity, &QSlider::valueChanged, this, [this, look](int v) {
        m_opacityValue->setText(tr("%1%").arg(v));
        look();
    });
    auto frame = [this] {
        if (!m_updating)
            emit frameEdited(currentFrame());
    };
    connect(m_radius, &QSlider::valueChanged, this, [this, frame](int v) {
        m_radiusValue->setText(tr("%1%").arg(v));
        frame();
    });
    connect(m_border, &QDoubleSpinBox::valueChanged, this, frame);
    connect(m_borderColor, &QPushButton::clicked, this, [this, frame] {
        const QColor c = QColorDialog::getColor(QColor::fromRgb(m_borderRgb), this,
                                                tr("Border colour"));
        if (!c.isValid())
            return;
        setBorderColor(c.rgb() & 0xffffff);
        frame();
    });
    auto timing = [this] {
        if (!m_updating)
            emit timingEdited(std::llround(m_fadeIn->value() * 1000.0),
                              std::llround(m_fadeOut->value() * 1000.0),
                              std::llround(m_start->value() * 1000.0),
                              std::llround(m_duration->value() * 1000.0));
    };
    connect(m_fadeIn, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_fadeOut, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_start, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_duration, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_addKey, &QPushButton::clicked, this, &OverlayPanel::addKeyRequested);
    connect(m_removeKey, &QPushButton::clicked, this, &OverlayPanel::removeKeyRequested);
    connect(m_clearKeys, &QPushButton::clicked, this, &OverlayPanel::clearKeysRequested);
    connect(m_ease, &QComboBox::activated, this,
            [this] { emit easeEdited(easecombo::ease(m_ease)); });
    connect(m_forward, &QPushButton::clicked, this, &OverlayPanel::bringForwardRequested);
    connect(m_backward, &QPushButton::clicked, this, &OverlayPanel::sendBackwardRequested);
}

inset::Look OverlayPanel::currentLook() const
{
    inset::Look l;
    l.centerX = float(m_x->value() / 100.0);
    l.centerY = float(m_y->value() / 100.0);
    l.width = float(m_size->value() / 100.0);
    l.opacity = float(m_opacity->value() / 100.0);
    return l;
}

inset::Frame OverlayPanel::currentFrame() const
{
    inset::Frame f;
    f.cornerRadius = float(m_radius->value() / 100.0);
    f.borderWidth = float(m_border->value() / 100.0);
    f.borderRgb = m_borderRgb;
    return f;
}

void OverlayPanel::setBorderColor(uint32_t rgb)
{
    m_borderRgb = rgb & 0xffffff;
    panelfields::showColor(m_borderColor, m_borderRgb);
}

QString OverlayPanel::partName(const char *suffix) const
{
    return (m_kind == Kind::Title ? QStringLiteral("title") : QStringLiteral("overlay")) +
           QLatin1String(suffix);
}

void OverlayPanel::setState(const State &s)
{
    m_updating = true;
    for (QWidget *w : m_clipRows)
        w->setEnabled(true);
    m_clipName->setText(s.label);
    m_clipName->setToolTip(s.label);
    if (m_kind == Kind::Title) {
        if (auto *label = qobject_cast<QLabel *>(form()->labelForField(m_size)))
            label->setText(s.gauge ? tr("Size") : tr("Text size"));
        m_size->setToolTip(s.gauge ? tr("The gauge's height, as a share of the video's height.")
                                   : tr("The text's size: its font's height, as a share of the "
                                        "video's height."));
    }
    m_trackName->setText(s.trackName);
    // Short: a wrapped label here gets clipped by the form layout; the
    // neighbours go in the tooltip.
    if (s.layerCount <= 1)
        m_layer->setText(tr("1 of 1"));
    else if (s.layer == 1)
        m_layer->setText(tr("1 of %1 (front)").arg(s.layerCount));
    else if (s.layer == s.layerCount)
        m_layer->setText(tr("%1 of %1 (back)").arg(s.layerCount));
    else
        m_layer->setText(tr("%1 of %2").arg(s.layer).arg(s.layerCount));
    QStringList order;
    if (!s.behind.isEmpty())
        order << tr("Behind “%1”.").arg(s.behind);
    if (!s.inFrontOf.isEmpty())
        order << tr("In front of “%1”.").arg(s.inFrontOf);
    m_layer->setToolTip(order.isEmpty()
                            ? tr("The only overlay track: drawn over the main video.")
                            : order.join(QLatin1Char(' ')) +
                                  tr(" Lower layers are drawn in front."));
    m_forward->setEnabled(s.layer > 1);
    m_backward->setEnabled(s.layer > 0 && s.layer < s.layerCount);
    m_x->setValue(s.look.centerX * 100.0);
    m_y->setValue(s.look.centerY * 100.0);
    m_size->setValue(s.look.width * 100.0);
    const int op = std::clamp(int(std::lround(s.look.opacity * 100.0f)), 0, 100);
    m_opacity->setValue(op);
    m_opacityValue->setText(tr("%1%").arg(op));
    const int rad = std::clamp(int(std::lround(s.frame.cornerRadius * 100.0f)), 0, 50);
    m_radius->setValue(rad);
    m_radiusValue->setText(tr("%1%").arg(rad));
    m_border->setValue(s.frame.borderWidth * 100.0);
    setBorderColor(s.frame.borderRgb);
    m_fadeIn->setMaximum(s.spanMs / 1000.0);
    m_fadeOut->setMaximum(s.spanMs / 1000.0);
    m_fadeIn->setValue(s.fadeInMs / 1000.0);
    m_fadeOut->setValue(s.fadeOutMs / 1000.0);
    m_start->setValue(s.startMs / 1000.0);
    m_duration->setMaximum(std::max(s.maxSpanMs, s.spanMs) / 1000.0);
    m_duration->setValue(s.spanMs / 1000.0);

    // Short: a wrapped label here gets clipped by the form layout.
    m_keys->setText(s.keyCount == 0 ? tr("None") : s.keyCount == 1 ? tr("1 key") : tr("%1 keys").arg(s.keyCount));
    m_keys->setToolTip(s.keyCount == 0
                           ? tr("The same position, size and opacity for the whole clip. \"Set "
                                "key\" starts animating it.")
                           : tr("Edits of position, size and opacity set a key at the playhead."));
    m_addKey->setEnabled(s.playheadInClip);
    m_removeKey->setEnabled(s.keyAtPlayhead);
    m_clearKeys->setEnabled(s.keyCount > 0);
    easecombo::show(m_ease, s.easeAvailable, s.ease);
    // Keyed values can only change where a key can go.
    const bool lookEditable = s.keyCount == 0 || s.playheadInClip;
    for (QWidget *w : {static_cast<QWidget *>(m_x), static_cast<QWidget *>(m_y),
                       static_cast<QWidget *>(m_size), static_cast<QWidget *>(m_opacity)})
        w->setEnabled(lookEditable);
    // (A title's Text section above says what's selected.)
    if (m_kind == Kind::Title)
        setHint(s.keyCount > 0 ? tr("It is keyframed: moving it, here or in the view, sets a "
                                    "key at the playhead.")
                : s.gauge      ? tr("Drag the gauge in the view to move it.")
                               : tr("Drag the title in the view to move it."));
    else
        setHint(s.keyCount == 0
                ? tr("The PiP clip selected on the timeline or in the view. Drag it in the "
                     "view to move it.")
                : tr("The PiP clip selected on the timeline or in the view. It is keyframed: "
                     "moving it, here or in the view, sets a key at the playhead."));
    m_updating = false;
}

void OverlayPanel::setNoClip(const QString &hint)
{
    m_updating = true;
    m_clipName->setText(QString());
    m_trackName->setText(QString());
    m_layer->setText(QString());
    m_layer->setToolTip(QString());
    m_keys->setText(QString());
    for (QWidget *w : m_clipRows)
        w->setEnabled(false);
    if (!hint.isEmpty())
        setHint(hint);
    else if (m_kind == Kind::Title)
        setHint(tr("Select a title on the timeline, or add one from the Overlays tab on the left."));
    else
        setHint(tr("Select a clip on an overlay track. Add one with Media > Add picture in "
                   "picture, or drop a video file on the timeline."));
    m_updating = false;
}

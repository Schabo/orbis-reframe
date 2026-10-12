#include "OverlayPanel.h"

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

namespace {

QDoubleSpinBox *numberBox(QWidget *parent, double lo, double hi, int decimals, double step,
                          const QString &suffix)
{
    auto *b = new QDoubleSpinBox(parent);
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setSingleStep(step);
    b->setSuffix(suffix);
    b->setKeyboardTracking(false); // commit on Enter / focus-out / arrows, not per keystroke
    return b;
}

QWidget *sliderRow(QWidget *parent, int lo, int hi, QSlider **slider, QLabel **value)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    *slider = new QSlider(Qt::Horizontal, row);
    (*slider)->setRange(lo, hi);
    (*slider)->setPageStep(10);
    *value = new QLabel(row);
    (*value)->setMinimumWidth(row->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    layout->addWidget(*slider, 1);
    layout->addWidget(*value);
    return row;
}

} // namespace

OverlayPanel::OverlayPanel(QWidget *parent)
    : SettingsPanel(tr("Overlay"), QStringLiteral("picture-in-picture-2"), parent)
{
    m_trackName = new QLabel(this);
    form()->addRow(tr("Track"), m_trackName);
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
    m_size->setToolTip(tr("The inset's width, as a share of the video's. Its height follows "
                          "the inset video's shape."));
    form()->addRow(tr("Size"), m_size);
    form()->addRow(tr("Opacity"), sliderRow(this, 0, 100, &m_opacity, &m_opacityValue));

    m_keys = new QLabel(this);
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

    m_clipRows = {m_x,       m_y,      m_size,        m_opacity, keyRow, m_radius,
                  m_border,  m_borderColor, m_fadeIn, m_fadeOut, m_start};

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
                              std::llround(m_start->value() * 1000.0));
    };
    connect(m_fadeIn, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_fadeOut, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_start, &QDoubleSpinBox::valueChanged, this, timing);
    connect(m_addKey, &QPushButton::clicked, this, &OverlayPanel::addKeyRequested);
    connect(m_removeKey, &QPushButton::clicked, this, &OverlayPanel::removeKeyRequested);
    connect(m_clearKeys, &QPushButton::clicked, this, &OverlayPanel::clearKeysRequested);
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
    const QColor c = QColor::fromRgb(m_borderRgb);
    m_borderColor->setText(c.name());
    m_borderColor->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; }")
            .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

void OverlayPanel::setState(const State &s)
{
    m_updating = true;
    for (QWidget *w : m_clipRows)
        w->setEnabled(true);
    m_clipName->setText(s.label);
    m_clipName->setToolTip(s.label);
    m_trackName->setText(s.trackName);
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

    // Short: a wrapped label here gets clipped by the form layout.
    m_keys->setText(s.keyCount == 0 ? tr("None") : tr("%n key(s)", "", s.keyCount));
    m_keys->setToolTip(s.keyCount == 0
                           ? tr("The same position, size and opacity for the whole clip. \"Set "
                                "key\" starts animating it.")
                           : tr("Edits of position, size and opacity set a key at the playhead."));
    m_addKey->setEnabled(s.playheadInClip);
    m_removeKey->setEnabled(s.keyAtPlayhead);
    m_clearKeys->setEnabled(s.keyCount > 0);
    // Keyed values can only change where a key can go.
    const bool lookEditable = s.keyCount == 0 || s.playheadInClip;
    for (QWidget *w : {static_cast<QWidget *>(m_x), static_cast<QWidget *>(m_y),
                       static_cast<QWidget *>(m_size), static_cast<QWidget *>(m_opacity)})
        w->setEnabled(lookEditable);
    setHint(s.keyCount == 0
                ? tr("The overlay clip selected on the timeline or in the view. Drag it in the "
                     "view to move it.")
                : tr("The overlay clip selected on the timeline or in the view. It is keyframed: "
                     "moving it, here or in the view, sets a key at the playhead."));
    m_updating = false;
}

void OverlayPanel::setNoClip()
{
    m_updating = true;
    m_clipName->setText(QString());
    m_trackName->setText(QString());
    m_keys->setText(QString());
    for (QWidget *w : m_clipRows)
        w->setEnabled(false);
    setHint(tr("Select a clip on an overlay track. Add one with File > Add video overlay, or "
               "drop a video file on the timeline."));
    m_updating = false;
}

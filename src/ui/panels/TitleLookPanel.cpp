#include "TitleLookPanel.h"

#include "PanelFields.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

#include <cmath>

using panelfields::numberBox;
using panelfields::sliderRow;

namespace {

QFrame *separator(QWidget *parent)
{
    auto *sep = new QFrame(parent);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    return sep;
}

QString percent(int v)
{
    return QStringLiteral("%1%").arg(v);
}

} // namespace

TitleLookPanel::TitleLookPanel(QWidget *parent) : SettingsPanel(tr("Look"), parent)
{
    const QString ofSize = QStringLiteral(" %");
    m_lineSpacing = numberBox(this, title::Style::kMinLineSpacing * 100.0,
                              title::Style::kMaxLineSpacing * 100.0, 0, 5.0, ofSize);
    m_lineSpacing->setObjectName(QStringLiteral("titleLineSpacing"));
    m_lineSpacing->setToolTip(tr("The space from one line to the next, as a share of the "
                                 "font's own: 100 % is the font's usual spacing."));
    form()->addRow(tr("Line spacing"), m_lineSpacing);
    m_letterSpacing = numberBox(this, title::Style::kMinLetterSpacing * 100.0,
                                title::Style::kMaxLetterSpacing * 100.0, 0, 1.0, ofSize);
    m_letterSpacing->setObjectName(QStringLiteral("titleLetterSpacing"));
    m_letterSpacing->setToolTip(tr("Space added between the letters, as a share of the text "
                                   "size. Below 0 brings them closer."));
    form()->addRow(tr("Letter spacing"), m_letterSpacing);

    form()->addRow(separator(this));
    m_outline = new QCheckBox(tr("Outline"), this);
    m_outline->setObjectName(QStringLiteral("titleOutline"));
    m_outline->setToolTip(tr("A line round each letter: keeps light text readable over a "
                             "bright sky."));
    m_outlineColor = new QPushButton(this);
    m_outlineColor->setObjectName(QStringLiteral("titleOutlineColor"));
    m_outlineColor->setToolTip(tr("The outline's colour."));
    form()->addRow(m_outline, m_outlineColor);
    m_outlineWidth = numberBox(this, 1.0, title::Outline::kMaxWidth * 100.0, 0, 1.0, ofSize);
    m_outlineWidth->setObjectName(QStringLiteral("titleOutlineWidth"));
    m_outlineWidth->setToolTip(tr("How thick the outline is, as a share of the text size."));
    form()->addRow(tr("Width"), m_outlineWidth);

    form()->addRow(separator(this));
    m_shadow = new QCheckBox(tr("Shadow"), this);
    m_shadow->setObjectName(QStringLiteral("titleShadow"));
    m_shadow->setToolTip(tr("A soft shadow below and to the right of the text."));
    m_shadowColor = new QPushButton(this);
    m_shadowColor->setObjectName(QStringLiteral("titleShadowColor"));
    m_shadowColor->setToolTip(tr("The shadow's colour."));
    form()->addRow(m_shadow, m_shadowColor);
    QWidget *shadowOpacityRow = sliderRow(this, 0, 100, &m_shadowOpacity, &m_shadowOpacityValue);
    m_shadowOpacity->setObjectName(QStringLiteral("titleShadowOpacity"));
    form()->addRow(tr("Opacity"), shadowOpacityRow);
    m_shadowDistance = numberBox(this, 0.0, title::Shadow::kMaxDistance * 100.0, 0, 1.0, ofSize);
    m_shadowDistance->setObjectName(QStringLiteral("titleShadowDistance"));
    m_shadowDistance->setToolTip(tr("How far the shadow falls from the text, as a share of the "
                                    "text size."));
    form()->addRow(tr("Distance"), m_shadowDistance);
    m_shadowBlur = numberBox(this, 0.0, title::Shadow::kMaxBlur * 100.0, 0, 1.0, ofSize);
    m_shadowBlur->setObjectName(QStringLiteral("titleShadowBlur"));
    m_shadowBlur->setToolTip(tr("How soft the shadow's edge is, as a share of the text size. "
                                "0 is sharp."));
    form()->addRow(tr("Blur"), m_shadowBlur);

    form()->addRow(separator(this));
    m_box = new QCheckBox(tr("Box"), this);
    m_box->setObjectName(QStringLiteral("titleBox"));
    m_box->setToolTip(tr("A filled box behind the text."));
    m_boxColor = new QPushButton(this);
    m_boxColor->setObjectName(QStringLiteral("titleBoxColor"));
    m_boxColor->setToolTip(tr("The box's colour."));
    form()->addRow(m_box, m_boxColor);
    QWidget *boxOpacityRow = sliderRow(this, 0, 100, &m_boxOpacity, &m_boxOpacityValue);
    m_boxOpacity->setObjectName(QStringLiteral("titleBoxOpacity"));
    form()->addRow(tr("Opacity"), boxOpacityRow);
    m_boxPadding = numberBox(this, 0.0, title::Box::kMaxPadding * 100.0, 0, 5.0, ofSize);
    m_boxPadding->setObjectName(QStringLiteral("titleBoxPadding"));
    m_boxPadding->setToolTip(tr("The room between the text and the box's edge, as a share of "
                                "the text size."));
    form()->addRow(tr("Padding"), m_boxPadding);
    QWidget *cornersRow = sliderRow(this, 0, 50, &m_boxCorners, &m_boxCornersValue);
    m_boxCorners->setObjectName(QStringLiteral("titleBoxCorners"));
    m_boxCorners->setToolTip(tr("How round the box's corners are: 50 % makes its ends round."));
    form()->addRow(tr("Corners"), cornersRow);

    m_fields = {m_lineSpacing,    m_letterSpacing, m_outline,        m_outlineColor,
                m_outlineWidth,   m_shadow,        m_shadowColor,    shadowOpacityRow,
                m_shadowDistance, m_shadowBlur,    m_box,            m_boxColor,
                boxOpacityRow,    m_boxPadding,    cornersRow};

    auto edited = [this] {
        updateEnabled();
        if (!m_updating)
            emit titleEdited(current());
    };
    for (QDoubleSpinBox *b : {m_lineSpacing, m_letterSpacing, m_outlineWidth, m_shadowDistance,
                              m_shadowBlur, m_boxPadding})
        connect(b, &QDoubleSpinBox::valueChanged, this, edited);
    for (QCheckBox *c : {m_outline, m_shadow, m_box})
        connect(c, &QCheckBox::toggled, this, edited);
    const std::pair<QSlider *, QLabel *> sliders[] = {{m_shadowOpacity, m_shadowOpacityValue},
                                                      {m_boxOpacity, m_boxOpacityValue},
                                                      {m_boxCorners, m_boxCornersValue}};
    for (const auto &[slider, value] : sliders)
        connect(slider, &QSlider::valueChanged, this, [value, edited](int v) {
            value->setText(percent(v));
            edited();
        });
    connect(m_outlineColor, &QPushButton::clicked, this, [this, edited] {
        pickColor(m_outlineColor, &m_style.outline.rgb, tr("Outline colour"));
        edited();
    });
    connect(m_shadowColor, &QPushButton::clicked, this, [this, edited] {
        pickColor(m_shadowColor, &m_style.shadow.rgb, tr("Shadow colour"));
        edited();
    });
    connect(m_boxColor, &QPushButton::clicked, this, [this, edited] {
        pickColor(m_boxColor, &m_style.box.rgb, tr("Box colour"));
        edited();
    });
    setNoTitle(QString());
}

void TitleLookPanel::pickColor(QPushButton *button, uint32_t *rgb, const QString &caption)
{
    const QColor c = QColorDialog::getColor(QColor::fromRgb(*rgb), this, caption);
    if (!c.isValid())
        return;
    *rgb = c.rgb() & 0xffffff;
    panelfields::showColor(button, *rgb);
}

title::Style TitleLookPanel::current() const
{
    title::Style t = m_style;
    t.lineSpacing = float(m_lineSpacing->value() / 100.0);
    t.letterSpacing = float(m_letterSpacing->value() / 100.0);
    t.outline.on = m_outline->isChecked();
    t.outline.width = float(m_outlineWidth->value() / 100.0);
    t.shadow.on = m_shadow->isChecked();
    t.shadow.opacity = float(m_shadowOpacity->value() / 100.0);
    t.shadow.distance = float(m_shadowDistance->value() / 100.0);
    t.shadow.blur = float(m_shadowBlur->value() / 100.0);
    t.box.on = m_box->isChecked();
    t.box.opacity = float(m_boxOpacity->value() / 100.0);
    t.box.padding = float(m_boxPadding->value() / 100.0);
    t.box.cornerRadius = float(m_boxCorners->value() / 100.0);
    return t;
}

void TitleLookPanel::updateEnabled()
{
    for (QWidget *w : m_fields)
        w->setEnabled(m_enabled);
    if (!m_enabled)
        return;
    for (QWidget *w : {static_cast<QWidget *>(m_outlineColor),
                       static_cast<QWidget *>(m_outlineWidth)})
        w->setEnabled(m_outline->isChecked());
    for (QWidget *w : {static_cast<QWidget *>(m_shadowColor), m_shadowOpacity->parentWidget(),
                       static_cast<QWidget *>(m_shadowDistance),
                       static_cast<QWidget *>(m_shadowBlur)})
        w->setEnabled(m_shadow->isChecked());
    for (QWidget *w : {static_cast<QWidget *>(m_boxColor), m_boxOpacity->parentWidget(),
                       static_cast<QWidget *>(m_boxPadding), m_boxCorners->parentWidget()})
        w->setEnabled(m_box->isChecked());
}

void TitleLookPanel::setTitle(const title::Style &s)
{
    m_updating = true;
    m_style = s;
    m_enabled = true;
    m_lineSpacing->setValue(s.lineSpacing * 100.0);
    m_letterSpacing->setValue(s.letterSpacing * 100.0);
    m_outline->setChecked(s.outline.on);
    panelfields::showColor(m_outlineColor, s.outline.rgb);
    m_outlineWidth->setValue(s.outline.width * 100.0);
    m_shadow->setChecked(s.shadow.on);
    panelfields::showColor(m_shadowColor, s.shadow.rgb);
    m_shadowOpacity->setValue(int(std::lround(s.shadow.opacity * 100.0)));
    m_shadowOpacityValue->setText(percent(m_shadowOpacity->value()));
    m_shadowDistance->setValue(s.shadow.distance * 100.0);
    m_shadowBlur->setValue(s.shadow.blur * 100.0);
    m_box->setChecked(s.box.on);
    panelfields::showColor(m_boxColor, s.box.rgb);
    m_boxOpacity->setValue(int(std::lround(s.box.opacity * 100.0)));
    m_boxOpacityValue->setText(percent(m_boxOpacity->value()));
    m_boxPadding->setValue(s.box.padding * 100.0);
    m_boxCorners->setValue(int(std::lround(s.box.cornerRadius * 100.0)));
    m_boxCornersValue->setText(percent(m_boxCorners->value()));
    updateEnabled();
    setHint(QString());
    m_updating = false;
}

void TitleLookPanel::setNoTitle(const QString &hint)
{
    m_updating = true;
    m_enabled = false;
    for (QPushButton *b : {m_outlineColor, m_shadowColor, m_boxColor})
        panelfields::clearColor(b);
    updateEnabled();
    setHint(hint.isEmpty() ? tr("Select a title to change how its text is drawn.") : hint);
    m_updating = false;
}

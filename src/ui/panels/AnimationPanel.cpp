#include "AnimationPanel.h"

#include "PanelFields.h"

#include <QComboBox>
#include <QListView>
#include <QDoubleSpinBox>
#include <QFormLayout>

#include <cmath>

using panelfields::numberBox;

namespace {

using Kind = inset::Motion::Kind;
using Side = inset::Motion::Side;

// The choices in each list, in order: none, a slide from each side, a pop,
// typing (a title's lists only).
struct Choice {
    Kind kind;
    Side side;
};
constexpr Choice kChoices[] = {{Kind::None, Side::Left},    {Kind::Slide, Side::Left},
                               {Kind::Slide, Side::Right},  {Kind::Slide, Side::Top},
                               {Kind::Slide, Side::Bottom}, {Kind::Pop, Side::Left},
                               {Kind::Type, Side::Left}};

int indexOf(const inset::Motion::Part &p, int count)
{
    for (int i = 0; i < count; ++i)
        if (kChoices[i].kind == p.kind && (p.kind != Kind::Slide || kChoices[i].side == p.side))
            return i;
    return 0;
}

} // namespace

AnimationPanel::AnimationPanel(Kind kind, QWidget *parent)
    : SettingsPanel(tr("Animation"), parent), m_kind(kind)
{
    const bool title = kind == Kind::Title;
    const QString prefix = title ? QStringLiteral("title") : QStringLiteral("overlay");
    const auto lengthBox = [this, &prefix](const char *suffix) {
        QDoubleSpinBox *b = numberBox(this, inset::Motion::kMinMs / 1000.0,
                                      inset::Motion::kMaxMs / 1000.0, 1, 0.1, QStringLiteral(" s"));
        b->setObjectName(prefix + QLatin1String(suffix));
        return b;
    };
    m_in = new QComboBox(this);
    m_in->setObjectName(prefix + QStringLiteral("MotionIn"));
    m_in->addItems({tr("None"), tr("Slide in from the left"), tr("Slide in from the right"),
                    tr("Slide in from the top"), tr("Slide in from the bottom"), tr("Pop up")});
    if (title) {
        m_in->addItem(tr("Type on"));
        m_in->setToolTip(tr("How the title comes on at its start: sliding in from just past an "
                            "edge of the frame, growing from nothing, or typed on letter by "
                            "letter over the length. Fades come on top."));
    } else {
        m_in->setToolTip(tr("How the picture in picture comes on at its start: sliding in from "
                            "just past an edge of the frame, or growing from nothing. Fades come "
                            "on top."));
    }
    form()->addRow(tr("In"), m_in);
    m_inLength = lengthBox("MotionInLength");
    m_inLength->setToolTip(tr("How long it takes to come on."));
    form()->addRow(tr("Length"), m_inLength);

    m_out = new QComboBox(this);
    m_out->setObjectName(prefix + QStringLiteral("MotionOut"));
    m_out->addItems({tr("None"), tr("Slide out to the left"), tr("Slide out to the right"),
                     tr("Slide out to the top"), tr("Slide out to the bottom"),
                     tr("Shrink away")});
    if (title) {
        m_out->addItem(tr("Type off"));
        m_out->setToolTip(tr("How the title goes off at its end: sliding out past an edge of "
                             "the frame, shrinking to nothing, or typed off from the end."));
    } else {
        m_out->setToolTip(tr("How the picture in picture goes off at its end: sliding out past "
                             "an edge of the frame, or shrinking to nothing."));
    }
    form()->addRow(tr("Out"), m_out);
    m_outLength = lengthBox("MotionOutLength");
    m_outLength->setToolTip(tr("How long it takes to go off."));
    form()->addRow(tr("Length"), m_outLength);

    auto edited = [this] {
        updateEnabled();
        if (!m_updating)
            emit motionEdited(current());
    };
    for (QComboBox *c : {m_in, m_out})
        connect(c, &QComboBox::currentIndexChanged, this, edited);
    for (QDoubleSpinBox *b : {m_inLength, m_outLength})
        connect(b, &QDoubleSpinBox::valueChanged, this, edited);
    setNoClip();
}

inset::Motion AnimationPanel::current() const
{
    inset::Motion m;
    const auto part = [](const QComboBox *c, const QDoubleSpinBox *length) {
        const Choice &ch = kChoices[std::clamp(c->currentIndex(), 0, c->count() - 1)];
        return inset::Motion::Part{ch.kind, ch.side, std::llround(length->value() * 1000.0)};
    };
    m.in = part(m_in, m_inLength);
    m.out = part(m_out, m_outLength);
    return m;
}

void AnimationPanel::updateEnabled()
{
    m_in->setEnabled(m_enabled);
    m_out->setEnabled(m_enabled);
    m_inLength->setEnabled(m_enabled && m_in->currentIndex() > 0);
    m_outLength->setEnabled(m_enabled && m_out->currentIndex() > 0);
}

void AnimationPanel::setMotion(const inset::Motion &m, bool canType)
{
    m_updating = true;
    m_enabled = true;
    if (m_kind == Kind::Title)
        for (QComboBox *c : {m_in, m_out})
            static_cast<QListView *>(c->view())->setRowHidden(c->count() - 1, !canType);
    m_in->setCurrentIndex(indexOf(m.in, m_in->count()));
    m_inLength->setValue(m.in.ms / 1000.0);
    m_out->setCurrentIndex(indexOf(m.out, m_out->count()));
    m_outLength->setValue(m.out.ms / 1000.0);
    updateEnabled();
    setHint(QString());
    m_updating = false;
}

void AnimationPanel::setNoClip(const QString &hint)
{
    m_updating = true;
    m_enabled = false;
    updateEnabled();
    if (!hint.isEmpty())
        setHint(hint);
    else if (m_kind == Kind::Title)
        setHint(tr("Select a title to animate how it comes and goes."));
    else
        setHint(tr("Select a picture in picture to animate how it comes and goes."));
    m_updating = false;
}

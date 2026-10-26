#include "TitleAnimationPanel.h"

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
// typing.
struct Choice {
    Kind kind;
    Side side;
};
constexpr Choice kChoices[] = {{Kind::None, Side::Left},    {Kind::Slide, Side::Left},
                               {Kind::Slide, Side::Right},  {Kind::Slide, Side::Top},
                               {Kind::Slide, Side::Bottom}, {Kind::Pop, Side::Left},
                               {Kind::Type, Side::Left}};

int indexOf(const inset::Motion::Part &p)
{
    for (int i = 0; i < int(std::size(kChoices)); ++i)
        if (kChoices[i].kind == p.kind && (p.kind != Kind::Slide || kChoices[i].side == p.side))
            return i;
    return 0;
}

} // namespace

TitleAnimationPanel::TitleAnimationPanel(QWidget *parent) : SettingsPanel(tr("Animation"), parent)
{
    const auto lengthBox = [this](const char *name) {
        QDoubleSpinBox *b = numberBox(this, inset::Motion::kMinMs / 1000.0,
                                      inset::Motion::kMaxMs / 1000.0, 1, 0.1, QStringLiteral(" s"));
        b->setObjectName(QLatin1String(name));
        return b;
    };
    m_in = new QComboBox(this);
    m_in->setObjectName(QStringLiteral("titleMotionIn"));
    m_in->addItems({tr("None"), tr("Slide in from the left"), tr("Slide in from the right"),
                    tr("Slide in from the top"), tr("Slide in from the bottom"), tr("Pop up"),
                    tr("Type on")});
    m_in->setToolTip(tr("How the title comes on at its start: sliding in from just past an edge "
                        "of the frame, growing from nothing, or typed on letter by letter over "
                        "the length. Fades come on top."));
    form()->addRow(tr("In"), m_in);
    m_inLength = lengthBox("titleMotionInLength");
    m_inLength->setToolTip(tr("How long it takes to come on."));
    form()->addRow(tr("Length"), m_inLength);

    m_out = new QComboBox(this);
    m_out->setObjectName(QStringLiteral("titleMotionOut"));
    m_out->addItems({tr("None"), tr("Slide out to the left"), tr("Slide out to the right"),
                     tr("Slide out to the top"), tr("Slide out to the bottom"), tr("Shrink away"),
                     tr("Type off")});
    m_out->setToolTip(tr("How the title goes off at its end: sliding out past an edge of the "
                         "frame, shrinking to nothing, or typed off from the end."));
    form()->addRow(tr("Out"), m_out);
    m_outLength = lengthBox("titleMotionOutLength");
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
    setNoTitle(QString());
}

inset::Motion TitleAnimationPanel::current() const
{
    inset::Motion m;
    const auto part = [](const QComboBox *c, const QDoubleSpinBox *length) {
        const Choice &ch = kChoices[std::clamp(c->currentIndex(), 0, int(std::size(kChoices)) - 1)];
        return inset::Motion::Part{ch.kind, ch.side, std::llround(length->value() * 1000.0)};
    };
    m.in = part(m_in, m_inLength);
    m.out = part(m_out, m_outLength);
    return m;
}

void TitleAnimationPanel::updateEnabled()
{
    m_in->setEnabled(m_enabled);
    m_out->setEnabled(m_enabled);
    m_inLength->setEnabled(m_enabled && m_in->currentIndex() > 0);
    m_outLength->setEnabled(m_enabled && m_out->currentIndex() > 0);
}

void TitleAnimationPanel::setMotion(const inset::Motion &m, bool canType)
{
    m_updating = true;
    m_enabled = true;
    for (QComboBox *c : {m_in, m_out})
        static_cast<QListView *>(c->view())->setRowHidden(c->count() - 1, !canType);
    m_in->setCurrentIndex(indexOf(m.in));
    m_inLength->setValue(m.in.ms / 1000.0);
    m_out->setCurrentIndex(indexOf(m.out));
    m_outLength->setValue(m.out.ms / 1000.0);
    updateEnabled();
    setHint(QString());
    m_updating = false;
}

void TitleAnimationPanel::setNoTitle(const QString &hint)
{
    m_updating = true;
    m_enabled = false;
    updateEnabled();
    setHint(hint.isEmpty() ? tr("Select a title to animate how it comes and goes.") : hint);
    m_updating = false;
}

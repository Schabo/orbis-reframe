#include "SettingsPanel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "Theme.h"

SettingsPanel::SettingsPanel(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The header: the whole row is the button; the scope label sits on it.
    m_header = new QToolButton(this);
    m_header->setProperty("settingsPart", "header");
    m_header->setText(title);
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setIconSize(QSize(14, 14));
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_header->setFocusPolicy(Qt::NoFocus); // the editor's keys stay with the editor
    m_header->setAccessibleName(title);
    connect(m_header, &QToolButton::clicked, this, [this] {
        if (!m_collapsible)
            return;
        setExpanded(!m_expanded);
        emit expandedToggled(m_expanded);
    });
    m_scope = new QLabel(m_header);
    m_scope->setObjectName(QStringLiteral("settingsPanelScope"));
    m_scope->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_scope->hide();
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(0, 0, 12, 0);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_scope, 0, Qt::AlignVCenter);
    outer->addWidget(m_header);

    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("settingsPanelBody"));
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_hintLabel = new QLabel(m_body);
    m_hintLabel->setObjectName(QStringLiteral("settingsPanelHint"));
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setVisible(false);
    bodyLayout->addWidget(m_hintLabel);

    auto *formWidget = new QWidget(m_body);
    m_form = new QFormLayout(formWidget);
    m_form->setContentsMargins(14, 4, 14, 14);
    m_form->setSpacing(8);
    m_form->setLabelAlignment(Qt::AlignLeft);
    m_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    bodyLayout->addWidget(formWidget);
    outer->addWidget(m_body);

    updateHeader();
}

void SettingsPanel::setScope(const QString &scope)
{
    m_scope->setText(scope);
    m_scope->setVisible(!scope.isEmpty());
    updateHeader();
}

QString SettingsPanel::scope() const { return m_scope->text(); }

void SettingsPanel::setTitle(const QString &title)
{
    m_title = title;
    m_header->setText(title);
    m_header->setAccessibleName(title);
}

void SettingsPanel::setCollapsible(bool collapsible)
{
    m_collapsible = collapsible;
    if (!collapsible)
        setExpanded(true);
    updateHeader();
}

void SettingsPanel::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_body->setVisible(expanded);
    updateHeader();
}

void SettingsPanel::nameParts()
{
    m_header->setObjectName(objectName() + QStringLiteral("Header"));
}

void SettingsPanel::setHint(const QString &text)
{
    m_hintLabel->setText(text);
    m_hintLabel->setVisible(!text.isEmpty());
}

void SettingsPanel::setBodyEnabled(bool enabled) { m_body->setEnabled(enabled); }

void SettingsPanel::updateHeader()
{
    if (m_collapsible) {
        m_header->setIcon(theme::icon(m_expanded ? QStringLiteral("chevron-down")
                                                 : QStringLiteral("chevron-right")));
    } else {
        // A blank icon: the title stays where the other sections' titles are
        // (a tool button with no icon centres its text).
        QPixmap blank(m_header->iconSize());
        blank.fill(Qt::transparent);
        m_header->setIcon(QIcon(blank));
    }
    m_header->setCursor(m_collapsible ? Qt::PointingHandCursor : Qt::ArrowCursor);
    m_header->setProperty("collapsible", m_collapsible);
    QString tip = m_collapsible ? (m_expanded ? tr("Click to close this section")
                                              : tr("Click to open this section"))
                                : QString();
    if (!m_scope->text().isEmpty())
        tip = tr("Applies to: %1").arg(m_scope->text()) +
              (tip.isEmpty() ? QString() : QStringLiteral("\n") + tip);
    m_header->setToolTip(tip);
}

QScrollArea *makeSettingsPage(const QString &objectName, const QList<SettingsPanel *> &sections,
                              QWidget *parent)
{
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("settingsPageContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (SettingsPanel *s : sections) {
        s->nameParts();
        s->setProperty("settingsPart", "section");
        layout->addWidget(s);
    }
    layout->addStretch(1);
    if (sections.size() == 1)
        sections.front()->setCollapsible(false);

    auto *page = new QScrollArea(parent);
    page->setObjectName(objectName);
    page->setWidgetResizable(true);
    page->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    page->setFrameShape(QFrame::NoFrame);
    page->setWidget(content);
    return page;
}

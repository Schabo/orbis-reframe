#include "SettingsPanel.h"

#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

SettingsPanel::SettingsPanel(const QString &title, const QString &iconName, QWidget *parent)
    : QWidget(parent), m_title(title), m_iconName(iconName)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(QStringLiteral("settingsPanelTitle"));
    outer->addWidget(m_titleLabel);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setObjectName(QStringLiteral("settingsPanelHint"));
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setVisible(false);
    outer->addWidget(m_hintLabel);

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("settingsPanelBody"));
    m_form = new QFormLayout(body);
    m_form->setContentsMargins(14, 4, 14, 14);
    m_form->setSpacing(8);
    m_form->setLabelAlignment(Qt::AlignLeft);
    m_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    outer->addWidget(body);

    outer->addStretch(1);
}

void SettingsPanel::setHint(const QString &text)
{
    m_hintLabel->setText(text);
    m_hintLabel->setVisible(!text.isEmpty());
}

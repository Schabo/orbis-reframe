#pragma once

#include <QString>
#include <QWidget>

class QFormLayout;
class QLabel;
class QVBoxLayout;

// Base class for a single settings-sidebar panel. Subclasses add rows via
// form(). The sidebar (SettingsSidebar) stacks these behind an icon rail; each
// panel supplies its own rail icon and title.
class SettingsPanel : public QWidget {
    Q_OBJECT

public:
    SettingsPanel(const QString &title, const QString &iconName, QWidget *parent = nullptr);

    QString title() const { return m_title; }
    QString iconName() const { return m_iconName; }

protected:
    // Add controls here: form()->addRow(tr("Quality"), combo);
    QFormLayout *form() const { return m_form; }

    // A muted line under the title (e.g. "Applies on the next stitch.").
    void setHint(const QString &text);

private:
    QString m_title;
    QString m_iconName;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_hintLabel = nullptr;
    QFormLayout *m_form = nullptr;
};

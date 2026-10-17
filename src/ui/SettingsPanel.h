#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QFormLayout;
class QLabel;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

// One section of a settings tab in the right sidebar (SettingsSidebar): a
// header row -- the title, and on its right what the section applies to
// ("This clip", "Keys at the playhead") -- over a body with an optional hint line
// and a form. Clicking the header collapses the body to the header row and
// back. Subclasses add rows via form().
//
// Object names: the section's own (given by the subclass) and
// <name>Header for the header button.
class SettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPanel(const QString &title, QWidget *parent = nullptr);

    QString title() const { return m_title; }
    void setTitle(const QString &title);

    // What the section's settings apply to, shown at the header's right.
    void setScope(const QString &scope);
    QString scope() const;

    // A section that can't collapse (the only one on its tab) shows no
    // chevron and ignores clicks on its header.
    void setCollapsible(bool collapsible);
    bool isCollapsible() const { return m_collapsible; }
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    // Gives the header the section's object name + "Header"; call after
    // setObjectName.
    void nameParts();
    // The section doesn't apply to what it shows (e.g. stabilization on a
    // flat video): `reason` replaces its hint and controls. Empty = it
    // applies again.
    void setNotApplicable(const QString &reason);
    bool isApplicable() const { return m_formWidget->isVisibleTo(m_body); }

signals:
    // The user opened or closed the section (not emitted by setExpanded).
    void expandedToggled(bool expanded);

protected:
    // Add controls here: form()->addRow(tr("Quality"), combo);
    QFormLayout *form() const { return m_form; }

    // A muted line under the header (e.g. "Applies on the next stitch.").
    void setHint(const QString &text);

    // Greys out the body (no clip to show); the header stays live, so the
    // section can still be opened or closed.
    void setBodyEnabled(bool enabled);

private:
    void updateHeader();

    QString m_title;
    QToolButton *m_header = nullptr;
    QLabel *m_scope = nullptr;
    QWidget *m_body = nullptr;
    QLabel *m_hintLabel = nullptr;
    QLabel *m_notApplicable = nullptr;
    QWidget *m_formWidget = nullptr;
    QFormLayout *m_form = nullptr;
    bool m_collapsible = true;
    bool m_expanded = true;
};

// A settings tab's page: `sections` top to bottom in a vertical scroll area
// (the tab scrolls when they don't fit), named `objectName`.
QScrollArea *makeSettingsPage(const QString &objectName, const QList<SettingsPanel *> &sections,
                              QWidget *parent = nullptr);

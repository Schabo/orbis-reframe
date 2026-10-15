#pragma once

#include <QDialog>

#include "../core/OutputSettings.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class OutputSettingsEditor;

// Asks for a project's name and location -- and, for a new project, its
// output format -- then creates the project folder (projectfolder::create)
// on OK. Used by File ▸ New project… and by Save project as…, which saves the
// open project into a new folder (no output section there: the project
// already has one).
class NewProjectDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { NewProject, SaveAs };

    NewProjectDialog(Mode mode, const QString &location, QWidget *parent = nullptr);

    void setName(const QString &name);

    QString name() const;
    QString location() const;
    OutputSettings output() const;
    // After OK: the project file to save, inside the folder just made.
    QString projectFile() const { return m_projectFile; }

public slots:
    // Makes the folder; stays open with the reason shown if it can't.
    void accept() override;

private:
    void validate();
    void browse();

    Mode m_mode;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_location = nullptr;
    QLabel *m_result = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    OutputSettingsEditor *m_output = nullptr;
    QString m_projectFile;
};

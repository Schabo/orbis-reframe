#include "NewProjectDialog.h"

#include "OutputSettingsEditor.h"

#include "../core/ProjectFolder.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

NewProjectDialog::NewProjectDialog(Mode mode, const QString &location, QWidget *parent)
    : QDialog(parent), m_mode(mode)
{
    setObjectName(QStringLiteral("newProjectDialog"));
    setWindowTitle(mode == Mode::NewProject ? tr("New project") : tr("Save project as"));

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("projectName"));
    m_name->setPlaceholderText(tr("e.g. Summer trip"));
    form->addRow(tr("&Name"), m_name);

    m_location = new QLineEdit(QDir::toNativeSeparators(location), this);
    m_location->setObjectName(QStringLiteral("projectLocation"));
    auto *browse = new QPushButton(tr("&Browse…"), this);
    browse->setObjectName(QStringLiteral("browseLocationButton"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, &NewProjectDialog::browse);
    auto *locationRow = new QHBoxLayout;
    locationRow->addWidget(m_location, 1);
    locationRow->addWidget(browse);
    // A row whose field is a layout gets no buddy, so no Alt+L, from addRow.
    auto *locationLabel = new QLabel(tr("&Location"), this);
    locationLabel->setBuddy(m_location);
    form->addRow(locationLabel, locationRow);

    m_result = new QLabel(this);
    m_result->setObjectName(QStringLiteral("projectFolderResult"));
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::PlainText);
    form->addRow(QString(), m_result);

    if (mode == Mode::NewProject) {
        auto *heading = new QLabel(tr("Output"), this);
        heading->setObjectName(QStringLiteral("dialogSection"));
        form->addRow(heading);
        m_output = new OutputSettingsEditor(this, form);
        auto *hint = new QLabel(tr("Can be changed later in the Project panel."), this);
        hint->setObjectName(QStringLiteral("dialogHint"));
        form->addRow(QString(), hint);
    }

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(mode == Mode::NewProject ? tr("&Create")
                                                                             : tr("&Save"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &NewProjectDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &NewProjectDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_buttons);

    connect(m_name, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    connect(m_location, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    setMinimumWidth(520);
    m_name->setFocus();
    validate();
}

void NewProjectDialog::setName(const QString &name) { m_name->setText(name); }

void NewProjectDialog::setOutput(const OutputSettings &output)
{
    if (m_output)
        m_output->setOutput(output);
}

QString NewProjectDialog::name() const { return m_name->text(); }

QString NewProjectDialog::location() const
{
    return QDir::fromNativeSeparators(m_location->text().trimmed());
}

OutputSettings NewProjectDialog::output() const
{
    return m_output ? m_output->output() : OutputSettings{};
}

void NewProjectDialog::validate()
{
    QString problem = name().isEmpty() ? QString() : projectfolder::nameProblem(name());
    if (problem.isEmpty() && location().isEmpty())
        problem = tr("Choose where to put the project.");
    const QString folder = QDir::cleanPath(location() + QLatin1Char('/') + name());
    if (problem.isEmpty() && !name().isEmpty() && QFileInfo::exists(folder))
        problem = tr("%1 already exists.").arg(QDir::toNativeSeparators(folder));

    if (!problem.isEmpty())
        m_result->setText(problem);
    else if (name().isEmpty())
        m_result->setText(tr("The project gets a folder of its own, with the project file, its "
                             "cache, exports and screenshots. Clips stay where they are."));
    else
        m_result->setText(tr("Creates %1").arg(QDir::toNativeSeparators(
            projectfolder::projectFilePath(location(), name()))));
    m_result->setProperty("problem", !problem.isEmpty());
    m_result->style()->unpolish(m_result);
    m_result->style()->polish(m_result);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty() && !name().isEmpty());
}

void NewProjectDialog::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Location"), location());
    if (!dir.isEmpty())
        m_location->setText(QDir::toNativeSeparators(dir));
}

void NewProjectDialog::accept()
{
    QString error;
    m_projectFile = projectfolder::create(location(), name(), &error);
    if (m_projectFile.isEmpty()) {
        m_result->setText(error);
        m_result->setProperty("problem", true);
        m_result->style()->unpolish(m_result);
        m_result->style()->polish(m_result);
        return;
    }
    QDialog::accept();
}

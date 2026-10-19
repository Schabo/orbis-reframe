#include "ProjectSettingsDialog.h"

#include "OutputSettingsEditor.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

ProjectSettingsDialog::ProjectSettingsDialog(const OutputSettings &output, double firstClipFps,
                                             QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("projectSettingsDialog"));
    setWindowTitle(tr("Project settings"));

    auto *form = new QFormLayout;
    auto *heading = new QLabel(tr("Output"), this);
    heading->setObjectName(QStringLiteral("dialogSection"));
    form->addRow(heading);
    m_output = new OutputSettingsEditor(this, form);
    m_output->setFirstClipFps(firstClipFps);
    m_output->setOutput(output);
    auto *hint = new QLabel(
        tr("The shape, size, frame rate and bit depth of the video you export. The viewport "
           "shows this shape."),
        this);
    hint->setObjectName(QStringLiteral("dialogHint"));
    hint->setWordWrap(true);
    form->addRow(QString(), hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("projectSettingsButtons"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    setMinimumWidth(440);
}

OutputSettings ProjectSettingsDialog::output() const { return m_output->output(); }

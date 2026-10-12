#include "ProjectPanel.h"

#include "../OutputSettingsEditor.h"

ProjectPanel::ProjectPanel(QWidget *parent)
    : SettingsPanel(tr("Project"), QStringLiteral(":/icons/clapperboard.svg"), parent)
{
    m_editor = new OutputSettingsEditor(this, form());
    setHint(tr("The shape, size and frame rate of the video you export. The viewport shows "
               "this shape."));
    connect(m_editor, &OutputSettingsEditor::outputChanged, this, &ProjectPanel::outputChanged);
}

void ProjectPanel::setOutput(const OutputSettings &output) { m_editor->setOutput(output); }

void ProjectPanel::setFirstClipFps(double fps) { m_editor->setFirstClipFps(fps); }

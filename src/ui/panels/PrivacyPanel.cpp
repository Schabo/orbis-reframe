#include "PrivacyPanel.h"

#include "../Theme.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

PrivacyPanel::PrivacyPanel(QWidget *parent)
    : SettingsPanel(tr("Privacy"), parent)
{
    setHint(tr("Blurs the number plates a scan of the clip finds, in playback and export. "
               "Right-click a blur in the viewport to remove a false hit."));

    m_blur = new QCheckBox(tr("Blur number plates"), this);
    m_blur->setToolTip(tr("Blur every plate the scan found in this clip. The export won't take "
                          "the clip until its whole trimmed part has been scanned."));
    form()->addRow(m_blur);

    m_scan = new QPushButton(tr("Scan clip"), this);
    m_scan->setToolTip(tr("Look for number plates in the clip's trimmed part. Takes a little "
                          "less time than playing it; the result is kept for this file."));
    form()->addRow(m_scan);

    m_addPlate = new QPushButton(tr("Blur a plate…"), this);
    m_addPlate->setToolTip(tr("Blur a plate  ( B ): for a plate the scan missed, drag a box "
                              "around it in the viewport, where it's clearest. It's followed "
                              "forwards and backwards from that frame and blurred for as long "
                              "as it can be followed. Works without a scan, and with \"Blur "
                              "number plates\" off."));
    form()->addRow(m_addPlate);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    form()->addRow(m_status);
    m_status->setTextFormat(Qt::RichText); // for the warning's colour (showStatus)

    m_restore = new QPushButton(this);
    m_restore->setToolTip(tr("Bring back every blur removed from this clip by hand. To remove a "
                             "blur (a false hit), right-click it in the viewport."));
    form()->addRow(m_restore);
    form()->setRowVisible(m_restore, false);

    m_showRegions = new QCheckBox(tr("Show regions"), this);
    m_showRegions->setToolTip(tr("Outline what the scan found in the viewport, coloured by how "
                                 "each region was found, to check the scan. Shown whether the "
                                 "blur is on or not, and never exported. Right-click a region "
                                 "to remove or restore it."));
    form()->addRow(m_showRegions);

    // The theme's plate colours, as the viewport draws them
    // (MainWindow::pushRegionOverlay).
    m_legend = new QLabel(this);
    updateLegend();
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        updateLegend();
        showStatus(); // a warning is in the theme's colour
    });
    form()->addRow(m_legend);
    form()->setRowVisible(m_legend, false);

    connect(m_blur, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit blurToggled(on);
    });
    connect(m_showRegions, &QCheckBox::toggled, this, [this](bool on) {
        form()->setRowVisible(m_legend, on);
        if (!m_updating)
            emit showRegionsToggled(on);
    });
    connect(m_restore, &QPushButton::clicked, this, &PrivacyPanel::restoreRemovedRequested);
    connect(m_addPlate, &QPushButton::clicked, this, &PrivacyPanel::plateButtonClicked);
    connect(m_scan, &QPushButton::clicked, this, [this] {
        if (m_running)
            emit scanStopRequested();
        else
            emit scanRequested();
    });
    setNoClip();
}

void PrivacyPanel::setState(bool blurPlates, const QString &scanStatus, bool canScan, int removed)
{
    m_restore->setText(tr("Restore removed blurs (%1)").arg(removed));
    form()->setRowVisible(m_restore, removed > 0);
    m_updating = true;
    m_blur->setChecked(blurPlates);
    m_updating = false;
    m_blur->setEnabled(true);
    m_canScan = canScan;
    m_scan->setEnabled(m_running || canScan);
    m_addPlate->setEnabled(m_plateState != PlateState::Idle || canScan);
    if (!m_running && m_plateState != PlateState::Running) {
        m_statusText = scanStatus;
        showStatus();
    }
}

void PrivacyPanel::setWarning(const QString &text)
{
    m_warningText = text;
    showStatus();
}

// One label for both: a second word-wrapped label in the form kept a stale
// height after the texts changed, clipping both.
void PrivacyPanel::showStatus()
{
    QString html = m_statusText.toHtmlEscaped();
    if (!m_warningText.isEmpty())
        html += QStringLiteral("<p style='color:%1; margin-top:6px'>&#9888; %2</p>")
                    .arg(theme::colors().warning.name(), m_warningText.toHtmlEscaped());
    m_status->setText(html);
}

void PrivacyPanel::setNoClip()
{
    m_warningText.clear();
    m_updating = true;
    m_blur->setChecked(false);
    m_updating = false;
    m_blur->setEnabled(false);
    form()->setRowVisible(m_restore, false);
    m_canScan = false;
    m_scan->setEnabled(m_running);
    m_addPlate->setEnabled(m_plateState != PlateState::Idle);
    if (!m_running && m_plateState != PlateState::Running) {
        m_statusText.clear();
        showStatus();
    }
}

void PrivacyPanel::setScanRunning(bool running)
{
    m_running = running;
    m_scan->setText(running ? tr("Stop scan") : tr("Scan clip"));
    m_scan->setEnabled(running || m_canScan);
    m_addPlate->setEnabled(!running && (m_plateState != PlateState::Idle || m_canScan));
}

void PrivacyPanel::setPlateState(PlateState state)
{
    m_plateState = state;
    m_addPlate->setText(state == PlateState::Idle        ? tr("Blur a plate…")
                        : state == PlateState::Selecting ? tr("Cancel")
                                                         : tr("Stop following"));
    m_addPlate->setEnabled(state != PlateState::Idle || m_canScan);
    m_scan->setEnabled(state == PlateState::Idle && (m_running || m_canScan));
}

void PrivacyPanel::setScanStatus(const QString &text)
{
    m_statusText = text;
    showStatus();
}

void PrivacyPanel::updateLegend()
{
    const theme::Colors &tc = theme::colors();
    const auto swatch = [](const QColor &c) {
        return QStringLiteral("<span style='color:%1'>&#9632;</span>").arg(c.name());
    };
    m_legend->setText(tr("%1 detected &nbsp; %2 tracked<br>"
                         "%3 filled in &nbsp; %4 tracked, not verified<br>"
                         "%5 removed (right-click to restore)<br>"
                         "%6 blurred by hand")
                          .arg(swatch(tc.plateDetected), swatch(tc.plateTracked),
                               swatch(tc.plateFilled), swatch(tc.plateUnverified),
                               swatch(tc.plateRemoved), swatch(tc.plateAdded)));
}

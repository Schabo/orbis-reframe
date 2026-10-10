#pragma once

#include "../SettingsPanel.h"

class QCheckBox;
class QLabel;
class QPushButton;

// Per-clip privacy blur: "Blur number
// plates" for the clip under the playhead, and "Scan clip" to find them --
// the plate scan (privacy::PlateScanner) over the clip's trimmed range, cached
// per source file. "Show regions" outlines the scan's regions in the viewport
// (a view setting, not the clip's). "Blur a plate…" blurs one plate the user
// boxes in the viewport, followed from there (a hand-added blur). A right click on a region in the viewport
// removes its track (MainWindow::showPictureMenu); "Restore removed" brings
// back every track removed from the clip. MainWindow owns the scan, the cache and
// the flags; this panel only shows them and hands the clicks back.
class PrivacyPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit PrivacyPanel(QWidget *parent = nullptr);

    // Reflect a clip: its flag and a line about its scan. `canScan` = the
    // build has the detector and the clip's media is there; `removed` = blur
    // tracks removed from it by hand. Does not emit.
    void setState(bool blurPlates, const QString &scanStatus, bool canScan, int removed);
    // A warning under the status (the blur may be out of date); empty hides
    // it.
    void setWarning(const QString &text);
    // No clip under the playhead -- greys the panel out.
    void setNoClip();
    // While a scan runs the button is its Stop, and the status line shows
    // its progress (setScanStatus).
    void setScanRunning(bool running);
    void setScanStatus(const QString &text);
    // "Blur a plate…": idle, waiting for the box to be drawn (the button
    // cancels), or following the plate (the button stops; the status line
    // shows its progress via setScanStatus).
    enum class PlateState { Idle, Selecting, Running };
    void setPlateState(PlateState state);

signals:
    void blurToggled(bool on);
    void scanRequested();
    void scanStopRequested();
    void restoreRemovedRequested();
    // The "Blur a plate…" button, in whatever state (setPlateState).
    void plateButtonClicked();
    void showRegionsToggled(bool on);

private:
    QCheckBox *m_blur = nullptr;
    QPushButton *m_scan = nullptr;
    QLabel *m_status = nullptr;
    QString m_statusText;  // the status line's plain text...
    QString m_warningText; // ... and the warning under it (both in m_status)
    void showStatus();
    QPushButton *m_restore = nullptr;
    QPushButton *m_addPlate = nullptr;
    PlateState m_plateState = PlateState::Idle;
    QCheckBox *m_showRegions = nullptr;
    QLabel *m_legend = nullptr;
    bool m_running = false;
    bool m_canScan = false;
    bool m_updating = false;
};

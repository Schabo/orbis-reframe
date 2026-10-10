#pragma once

#include <QDialog>
#include <QStringList>

#include "../core/OutputSettings.h"

#include <cstdint>

class QComboBox;
class QLabel;
class QSpinBox;

// Small modal dialog shown before a flat reframe export: output resolution and
// codec. The resolution defaults to the project's (Project Settings) and
// only offers sizes with the project's aspect -- a different aspect would be
// a different crop than the one framed in the viewport. The video bitrate
// comes from a quality level (worked out from resolution, fps and codec) or
// is typed in; the level and a typed rate are remembered between exports.
// The caller fills in the clip list / trims / pose arrays separately.
class ExportOptionsDialog : public QDialog {
    Q_OBJECT

public:
    // clipCount / rangeMs / keyCount / fps are shown as a read-only summary so
    // the user can see what will be exported (totals across the whole
    // timeline; fps is the rate the export will actually run at).
    // `keylessClips` names the clips without reframe keys (they export a
    // static forward view); listed with a warning when only some are.
    ExportOptionsDialog(int clipCount, qint64 rangeMs, int keyCount, double fps,
                        const QStringList &keylessClips, const OutputSettings &output,
                        QWidget *parent = nullptr);

    int outWidth() const;
    int outHeight() const;
    bool h265() const;
    qint64 bitrateKbps() const;

    // Saves the quality choice for the next export; call once accepted.
    void rememberQuality() const;

private:
    OutputSettings::Quality quality() const;
    void updateRate();

    double m_fps = 0.0;
    qint64 m_rangeMs = 0;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_codec = nullptr;
    QComboBox *m_quality = nullptr;
    QSpinBox *m_customMbps = nullptr; // the rate; editable for Custom only
    QLabel *m_estimate = nullptr;
};

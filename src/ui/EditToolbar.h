#pragma once

#include <QWidget>

class QAbstractButton;
class QLabel;
class QTimer;

// The edit toolbar above the timeline: the tools that act on clips at the
// playhead -- trim in / out / restore and the reframe-authoring buttons --
// centred, with a transient notice line and the timeline's zoom buttons at
// its right end. Playback (and the screenshot) is the player bar's
// (PlayerBar), under the viewport.
//
// Pure view: it emits the user's intent and renders whatever state is pushed
// in through the setters.
class EditToolbar : public QWidget {
    Q_OBJECT

public:
    explicit EditToolbar(QWidget *parent = nullptr);

public slots:
    // Flips the arm toggle (the A key); emits reframeArmedChanged.
    void toggleReframeArmed();
    // Enables/disables the Del Key / Clr Path buttons.
    void setHasReframeKeys(bool hasKeys);
    // A short status line beside the buttons ("Saved …"), cleared after a
    // few seconds; `sticky` keeps it until the next notice.
    void showNotice(const QString &text, bool sticky = false);

signals:
    // "Set in/out point at the playhead": no payload -- MainWindow uses the
    // current clip's source time (the transport bar only knows timeline time).
    void trimInRequested();
    void trimOutRequested();
    void trimCleared();
    // Reframe authoring: the arm toggle flipped; delete the key at the playhead;
    // clear the whole track.
    void reframeArmedChanged(bool armed);
    void deleteKeyRequested(); // at the playhead -- see trimInRequested()
    void clearReframeRequested();
    // The timeline's zoom (TimelineWidget's zoomOut / zoomToFit / zoomIn).
    void zoomOutRequested();
    void zoomToFitRequested();
    void zoomInRequested();

private:
    QAbstractButton *m_setInButton = nullptr;
    QAbstractButton *m_setOutButton = nullptr;
    QAbstractButton *m_clearTrimButton = nullptr;
    QAbstractButton *m_armReframeButton = nullptr;
    QAbstractButton *m_deleteKeyButton = nullptr;
    QAbstractButton *m_clearReframeButton = nullptr;
    QAbstractButton *m_zoomOutButton = nullptr;
    QAbstractButton *m_zoomFitButton = nullptr;
    QAbstractButton *m_zoomInButton = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QTimer *m_noticeClear = nullptr;
};

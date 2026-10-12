#pragma once

#include <QWidget>

class QAbstractButton;
class QLabel;
class QTimer;

// The edit strip with the timeline: trim in/out buttons, the
// reframe-authoring buttons and a transient notice line. Playback (and the
// screenshot) is the player bar's (PlayerBar), under the viewport.
//
// Pure view: it emits the user's intent and renders whatever state is pushed
// in through the setters.
class TransportBar : public QWidget {
    Q_OBJECT

public:
    explicit TransportBar(QWidget *parent = nullptr);

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

private:
    QAbstractButton *m_setInButton = nullptr;
    QAbstractButton *m_setOutButton = nullptr;
    QAbstractButton *m_clearTrimButton = nullptr;
    QAbstractButton *m_armReframeButton = nullptr;
    QAbstractButton *m_deleteKeyButton = nullptr;
    QAbstractButton *m_clearReframeButton = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QTimer *m_noticeClear = nullptr;
};

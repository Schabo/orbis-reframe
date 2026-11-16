#pragma once

#include <QFrame>

class QLabel;

// The strip under the viewport that offers to play from the camera's preview
// files when the full files look too slow for this machine: why (a clip
// decodes on the processor, or slower than it plays) and three answers. Not
// a dialog: it comes while the clip plays, and a dialog would take the keys
// (Space would answer it instead of pausing). Its buttons take no focus for
// the same reason.
//
// Pure view: MainWindow decides when to offer and what an answer does.
class ProxyOfferBar : public QFrame {
    Q_OBJECT

public:
    explicit ProxyOfferBar(QWidget *parent = nullptr);

    // Shows the bar, `reason` first ("Clip 2 plays slowly: …").
    void offer(const QString &reason);

signals:
    void accepted();  // Play from preview files
    void dismissed(); // Not now
    void declined();  // Don't ask again

private:
    QLabel *m_text = nullptr;
};

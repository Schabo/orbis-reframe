#pragma once

#include <QListWidget>
#include <QWidget>

// The effects to pick from, as tiles. Dragging one out hands it over as an
// fx::kMimeType drag (the timeline takes it onto a clip).
class EffectList : public QListWidget {
    Q_OBJECT

public:
    explicit EffectList(QWidget *parent = nullptr);

protected:
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
};

// The left sidebar's Effects tab: the video effects to put on a clip. Drag
// one onto a clip on the video track, or double-click (Enter) to put it on
// the clip at the playhead (the selected one). Its settings are in the
// right sidebar's Video tab, in the clip's Effects section.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);

signals:
    // Put the effect named `name` (fx::kVignette, ...) on the clip at the
    // playhead.
    void addEffectRequested(const QString &name);

private:
    EffectList *m_list = nullptr;
};

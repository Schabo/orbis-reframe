#pragma once

#include <QListWidget>
#include <QWidget>

class QScrollArea;

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

// The left sidebar's Effects tab: the video effects to put on a clip, and
// below them the colour looks. Drag one onto a clip on the video track, or
// double-click (Enter) to put it on the clip at the playhead (the selected
// one). Its settings are in the right sidebar's Video tab, in the clip's
// Effects section.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);

    // A colour look tile's effect name: lookTile(id) for a built-in look
    // ("look:bw"), kCustomLook for the one that asks for a .cube file.
    static QString lookTile(const QString &presetId);
    static constexpr const char *kCustomLook = "look:custom";

signals:
    // Put the effect named `name` (fx::kVignette, ..., or a look tile's
    // name) on the clip at the playhead.
    void addEffectRequested(const QString &name);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Each list as tall as its tiles, in the page's width.
    void fitLists();

    EffectList *m_list = nullptr;
    EffectList *m_looks = nullptr;
    QScrollArea *m_scroll = nullptr;
};

#pragma once

#include <QElapsedTimer>
#include <QListWidget>
#include <QTimer>
#include <QWidget>

class QScrollArea;

// The titles to pick from, as tiles. Dragging one out hands it over as a
// title::kMimeType drag carrying its preset's id (the timeline takes it onto
// an overlay track).
class TitleList : public QListWidget {
    Q_OBJECT

public:
    explicit TitleList(QWidget *parent = nullptr);

protected:
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
};

// The left sidebar's Overlays tab: text to put over the output, as tiles of
// ready-made titles (titlepresets), and gauges (titlepresets::gauges) under
// them; drag one onto the timeline, or double-click (Enter) to add it at the
// playhead. It is edited in the right sidebar's Title tab. An animated tile
// plays its animation while the mouse is over it.
class TitlesPanel : public QWidget {
    Q_OBJECT

public:
    explicit TitlesPanel(QWidget *parent = nullptr);

signals:
    // Add the title or gauge preset `presetId` names at the playhead.
    void addTitleRequested(const QString &presetId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static constexpr QSize kTileSize{128, 72};
    // Play `item`'s animation (nothing for null or a still tile), the one
    // playing before back at rest.
    void preview(QListWidgetItem *item);
    void stepPreview();
    // Is the pointer still over the tile playing?
    bool pointerOnPreview() const;

    // Each list as tall as its tiles, at the page's width.
    void fitLists();

    TitleList *m_list = nullptr;
    TitleList *m_gauges = nullptr;
    QScrollArea *m_scroll = nullptr;
    QListWidgetItem *m_previewItem = nullptr;
    QTimer m_previewTimer;
    QElapsedTimer m_previewClock;
};

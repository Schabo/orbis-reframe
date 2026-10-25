#pragma once

#include <QListWidget>
#include <QWidget>

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

// The left sidebar's Titles tab: text to put over the output, as tiles of
// ready-made titles (titlepresets); drag one onto the timeline, or
// double-click (Enter) to add it at the playhead. It is edited in the right
// sidebar's Title tab.
class TitlesPanel : public QWidget {
    Q_OBJECT

public:
    explicit TitlesPanel(QWidget *parent = nullptr);

signals:
    // Add the title preset `presetId` names at the playhead.
    void addTitleRequested(const QString &presetId);

private:
    TitleList *m_list = nullptr;
};

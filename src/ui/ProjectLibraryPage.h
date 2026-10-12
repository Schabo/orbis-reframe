#pragma once

#include <QListWidget>
#include <QWidget>

class QAction;
class QLabel;
class QPushButton;
class QStackedWidget;
class ProjectLibrary;
struct ProjectSummary;

// The cards of the project library, painted in the theme's colours.
class ProjectCardList : public QListWidget {
    Q_OBJECT

public:
    explicit ProjectCardList(QWidget *parent = nullptr);

    // Card and thumbnail size, in device-independent pixels.
    static QSize cardSize();
    static QSize thumbnailSize();
};

// The start screen: every project in the library as a card (thumbnail, name,
// when it was last opened, clip count and length, missing media), with New
// project, Open other, and per project Open / Show in file manager / Remove
// from list. Shown when no project is open. Reading the projects and their
// thumbnails happens on worker threads; `loaded` fires when the last arrives.
class ProjectLibraryPage : public QWidget {
    Q_OBJECT

public:
    // `library` is the app's list (owned by the caller), read on refresh().
    explicit ProjectLibraryPage(ProjectLibrary *library, QWidget *parent = nullptr);
    ~ProjectLibraryPage() override;

    // Re-read the library and every project in it.
    void refresh();

    // The project file of the selected card, empty if none.
    QString selectedPath() const;

    // Item data roles of a card.
    enum Role {
        PathRole = Qt::UserRole + 1,
        SummaryReadRole,  // bool: the summary below has arrived
        DetailsRole,      // QString: "3 clips · 4:12"
        WhenRole,         // QString: "Opened today 14:32"
        ProblemRole,      // QString: missing media / file / unreadable, empty if fine
        ThumbnailRole,    // QImage
    };

signals:
    void openRequested(const QString &path);
    void newProjectRequested();
    void openOtherRequested();
    // The projects folder changed (Change… on the page).
    void projectsFolderChanged();
    void loaded();

private:
    void applySummary(int generation, const ProjectSummary &summary, const QImage &thumbnail);
    void removeSelected();
    void showSelectedInFileManager();
    void chooseProjectsFolder();
    void updateActions();

    ProjectLibrary *m_library;
    ProjectCardList *m_list = nullptr;
    QStackedWidget *m_body = nullptr;
    QLabel *m_folderLabel = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_removeAction = nullptr;
    int m_generation = 0; // results from an older refresh() are dropped
    int m_pending = 0;
};

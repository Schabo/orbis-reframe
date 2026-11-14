#pragma once

#include <QListWidget>
#include <QWidget>

#include <functional>

class QAction;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class ProjectLibrary;
struct ProjectSummary;

// The cards of the project library, painted in the theme's colours.
// Ctrl + wheel grows and shrinks them (scale).
class ProjectCardList : public QListWidget {
    Q_OBJECT

public:
    explicit ProjectCardList(QWidget *parent = nullptr);

    // Card and thumbnail size at the wheel zoom, in device-independent
    // pixels; 1.0 is the default card.
    QSize cardSize() const;
    QSize thumbnailSize() const;
    double scale() const { return m_scale; }
    void setScale(double scale);

signals:
    void scaleChanged(double scale);

protected:
    void wheelEvent(QWheelEvent *e) override;

private:
    void applyGridSize();

    double m_scale = 1.0;
};

// The start screen: every project in the library as a card (thumbnail, name,
// when it was last opened, clip count and length, missing media), with New
// project, Open other, and per project Open / Rename / Duplicate / Show in
// file manager / Remove from list. Shown when no project is open. Reading the projects and their
// thumbnails happens on worker threads; `loaded` fires when the last arrives.
// Ctrl + wheel on the cards changes their size, and re-reads their
// thumbnails at it.
class ProjectLibraryPage : public QWidget {
    Q_OBJECT

public:
    // `library` is the app's list (owned by the caller), read on refresh().
    explicit ProjectLibraryPage(ProjectLibrary *library, QWidget *parent = nullptr);
    ~ProjectLibraryPage() override;

    // What draws a card's picture when the project starts with a flat video
    // (an .insv's comes from its trailer): a frame of `path` at `size`. Set
    // before refresh(); called on worker threads. Unset = no picture.
    using FlatThumbnailFn = std::function<QImage(const QString &path, const QSize &size)>;
    void setFlatThumbnail(FlatThumbnailFn fn) { m_flatThumbnail = std::move(fn); }

    // Re-read the library and every project in it.
    void refresh();

    // The project file of the selected card, empty if none.
    QString selectedPath() const;
    // Select the card of project file `path`, if there is one.
    void selectPath(const QString &path);

    // The cards' wheel zoom, 1.0 the default.
    double cardScale() const;
    void setCardScale(double scale);

    // Asks for a project name (Rename, Duplicate): (title, current text) ->
    // the name, empty if cancelled. A dialog unless a test sets another.
    using NamePrompt = std::function<QString(const QString &title, const QString &text)>;
    void setNamePrompt(NamePrompt prompt) { m_namePrompt = std::move(prompt); }

    // Item data roles of a card.
    enum Role {
        PathRole = Qt::UserRole + 1,
        SummaryReadRole,  // bool: the summary below has arrived
        DetailsRole,      // QString: "3 clips · 4:12"
        WhenRole,         // QString: "Opened today 14:32"
        ProblemRole,      // QString: missing media / file / unreadable, empty if fine
        ThumbnailRole,    // QImage
        FirstClipRole,    // QString: the first clip's file (re-reads the thumbnail)
        FlatClipRole,     // bool: the first clip is a flat video
    };

signals:
    void openRequested(const QString &path);
    void newProjectRequested();
    void openOtherRequested();
    // The projects folder changed (Change… on the page).
    void projectsFolderChanged();
    void loaded();
    // The cards' zoom changed (remembered per user).
    void cardScaleChanged(double scale);

private:
    FlatThumbnailFn m_flatThumbnail;
    void applySummary(int generation, const ProjectSummary &summary, const QImage &thumbnail);
    // The card pictures again at the current zoom, after the wheel settles.
    void rethumbnails();
    void removeSelected();
    void renameSelected();
    void duplicateSelected();
    QString askName(const QString &title, const QString &text);
    void showSelectedInFileManager();
    void chooseProjectsFolder();
    void updateActions();

    ProjectLibrary *m_library;
    ProjectCardList *m_list = nullptr;
    QStackedWidget *m_body = nullptr;
    QLabel *m_folderLabel = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_duplicateAction = nullptr;
    QAction *m_removeAction = nullptr;
    QTimer *m_thumbTimer = nullptr;
    NamePrompt m_namePrompt;
    int m_generation = 0; // results from an older refresh() are dropped
    int m_pending = 0;
};

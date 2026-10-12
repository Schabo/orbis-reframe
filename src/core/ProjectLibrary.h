#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// The projects the start screen lists: the ones opened or saved before (kept
// in a small JSON file in the app's data location) plus every project folder
// found directly under the projects folder (where New project puts them by
// default). Remove from list forgets a project without touching its files; one
// found in the projects folder is then hidden until it's opened again.
//
// Plain data, no widgets. Every change reads the file first and rewrites it
// (it's a few lines), so a change never drops what's there -- also when
// load() hasn't been called, or another window changed it meanwhile.
class ProjectLibrary {
public:
    struct Entry {
        QString path;          // the project file
        QDateTime lastOpened;  // invalid: never opened here (found in the folder)
        QDateTime sortTime;    // lastOpened, else the file's modification time
    };

    // `listFile`: where the list is kept (tests pass a temporary file).
    explicit ProjectLibrary(const QString &listFile = defaultListFile());

    // <data location>/projects.json and <Videos>/orbis-reframe.
    static QString defaultListFile();
    static QString standardProjectsFolder();

    // Read the list file; a missing or unreadable one is an empty list.
    void load();

    // Where new projects go and which folder is scanned.
    QString projectsFolder() const { return m_projectsFolder; }
    bool setProjectsFolder(const QString &folder, QString *error = nullptr);

    // Listed and found projects, most recent first. A listed project whose
    // file is gone stays (the card says so) until removed.
    QList<Entry> entries() const;

    // A project was opened or saved: put it at the top (and unhide it).
    bool noteOpened(const QString &path, const QDateTime &when = QDateTime::currentDateTime(),
                    QString *error = nullptr);
    // Forget a project; its files stay.
    bool remove(const QString &path, QString *error = nullptr);

private:
    bool save(QString *error) const;
    // Project folders directly under the projects folder.
    QStringList scanProjectsFolder() const;

    QString m_listFile;
    QString m_projectsFolder;
    QList<Entry> m_listed; // newest first
    QStringList m_hidden;  // found in the projects folder but removed from the list
};

// What a project card shows, read from the project file (no media is opened;
// a source's presence is checked on disk).
struct ProjectSummary {
    QString path;
    QString name;            // the file name without .orp
    bool fileMissing = false;
    QString error;           // the file exists but can't be read
    int clipCount = 0;       // clips on the main track
    qint64 durationMs = 0;   // the main track's length
    int missingMedia = 0;    // source files (any track) not found
    QString firstClip;       // the main track's first source, for the thumbnail

    // Reads `path`. Safe to call from a worker thread.
    static ProjectSummary read(const QString &path);
};

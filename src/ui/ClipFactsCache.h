#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <IStitchSource.h> // stitch::LensGuardProfiles

#include "MediaProbe.h" // ClipProbeFn

// What a 360 clip's file says about itself that the editor shows next to
// the clip: which lens-guard profiles it carries (the Stitching panel,
// "Apply to all clips") and the highlight marks set while recording (the
// timeline's dots, Ctrl+Up / Down).
struct ClipFileFacts {
    stitch::LensGuardProfiles lensGuards = stitch::LensGuardProfiles::all();
    // Source ms from the file's first frame, in time order.
    QList<qint64> highlightMs;

    static ClipFileFacts fromProbe(const stitch::ClipProbe &probe);
};

// Each 360 clip's ClipFileFacts. Facts of the file, not settings: not saved,
// read through the clip probe -- off the GUI thread, so opening a project
// doesn't read every clip's trailer before the window shows. facts() never
// blocks: until a file has been read it answers the defaults (every guard
// choice open, as before the file is known; no marks) and starts the read;
// ready() fires when it's in.
class ClipFactsCache : public QObject {
    Q_OBJECT

public:
    explicit ClipFactsCache(ClipProbeFn probe, QObject *parent = nullptr);
    ~ClipFactsCache() override; // waits for running reads

    ClipFileFacts facts(const QString &path);
    stitch::LensGuardProfiles profiles(const QString &path) { return facts(path).lensGuards; }
    QList<qint64> highlights(const QString &path) { return facts(path).highlightMs; }
    // A file's facts, as a probe on the GUI thread already read them.
    void set(const QString &path, const stitch::ClipProbe &probe);
    // Read these files again on the next facts(): replaced in place, or
    // relinked.
    void forget(const QStringList &paths);

signals:
    void ready(const QString &path);

private:
    ClipProbeFn m_probe;
    QHash<QString, ClipFileFacts> m_done;
    QSet<QString> m_pending;
    QThreadPool m_pool;
};

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <IStitchSource.h> // stitch::LensGuardProfiles

#include "MediaProbe.h" // ClipProbeFn

// Which lens-guard profiles each 360 clip's file carries, for the Stitching
// panel and "Apply to all clips". Facts of the file, not settings: not saved,
// read through the clip probe -- off the GUI thread, so opening a project
// doesn't read every clip's trailer before the window shows. profiles() never
// blocks: until a file has been read it answers "all" (every choice open, as
// before the file is known) and starts the read; ready() fires when it's in.
class LensGuardCache : public QObject {
    Q_OBJECT

public:
    explicit LensGuardCache(ClipProbeFn probe, QObject *parent = nullptr);
    ~LensGuardCache() override; // waits for running reads

    stitch::LensGuardProfiles profiles(const QString &path);
    // A file's profiles, as a probe on the GUI thread already read them.
    void set(const QString &path, stitch::LensGuardProfiles profiles);
    // Read these files again on the next profiles(): replaced in place, or
    // relinked.
    void forget(const QStringList &paths);

signals:
    void ready(const QString &path);

private:
    ClipProbeFn m_probe;
    QHash<QString, stitch::LensGuardProfiles> m_done;
    QSet<QString> m_pending;
    QThreadPool m_pool;
};

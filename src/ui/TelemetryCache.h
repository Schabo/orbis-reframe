#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <ClipMetadata.h> // camera::ClipTelemetry

#include "MotionTelemetry.h"
#include "Telemetry.h"

#include <functional>
#include <future>
#include <memory>
#include <string>

// What each file gives the overlays besides its picture: the GPS track and
// the vehicle's lean and G-forces (camera::ReadClipTelemetry). Facts of the
// file, not settings: not saved, read once per file on a background thread
// -- the IMU of a 20 min clip takes seconds to read and fuse. readings()
// never blocks: until a file has been read it gives none and starts the
// read, and ready() fires when it's in. await() waits for it, for an export
// or a screenshot, which can't go without.
class TelemetryCache : public QObject {
    Q_OBJECT

public:
    // Null where the file has none.
    struct Readings {
        std::shared_ptr<const Telemetry> gps;
        std::shared_ptr<const MotionTelemetry> motion;
    };
    using ReadFn = std::function<camera::ClipTelemetry(const std::string &path)>;

    // `read` for tests; by default camera::ReadClipTelemetry.
    explicit TelemetryCache(ReadFn read = {}, QObject *parent = nullptr);
    ~TelemetryCache() override; // waits for running reads

    Readings readings(const QString &path);
    Readings await(const QString &path);
    // Is `path` being read (readings() gives none for now)?
    bool pending(const QString &path) const;
    // Read these files again on the next readings(): replaced in place, or
    // relinked.
    void forget(const QStringList &paths);

signals:
    void ready(const QString &path);

private:
    struct Entry {
        std::shared_future<Readings> result;
        quint64 id = 0; // which read, so one forgotten meanwhile isn't announced
    };
    // The entry for `path`, its read started if it wasn't.
    const Entry &entry(const QString &path);

    ReadFn m_read;
    QHash<QString, Entry> m_entries;
    quint64 m_nextId = 0;
    QThreadPool m_pool;
};

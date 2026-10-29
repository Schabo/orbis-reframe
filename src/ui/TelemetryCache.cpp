#include "TelemetryCache.h"

#include <QPointer>

#include <chrono>

TelemetryCache::TelemetryCache(ReadFn read, QObject *parent)
    : QObject(parent), m_read(read ? std::move(read) : ReadFn(camera::ReadClipTelemetry))
{
    m_pool.setMaxThreadCount(1); // a trailer read each; the files share a disk
}

TelemetryCache::~TelemetryCache()
{
    m_pool.clear();
    m_pool.waitForDone();
}

const TelemetryCache::Entry &TelemetryCache::entry(const QString &path)
{
    if (const auto it = m_entries.constFind(path); it != m_entries.constEnd())
        return *it;
    auto promise = std::make_shared<std::promise<Readings>>();
    const quint64 id = ++m_nextId;
    const Entry &e = *m_entries.insert(path, {promise->get_future().share(), id});

    QPointer<TelemetryCache> self(this);
    m_pool.start([self, path, id, promise, read = m_read] {
        camera::ClipTelemetry t = read(path.toStdString());
        Readings r;
        if (t.gps) {
            auto gps = std::make_shared<const Telemetry>(*t.gps);
            if (!gps->isEmpty())
                r.gps = std::move(gps);
        }
        if (!t.motion.empty())
            r.motion = std::make_shared<const MotionTelemetry>(std::move(t.motion));
        promise->set_value(std::move(r));
        QMetaObject::invokeMethod(
            self.data(),
            [self, path, id] {
                if (!self)
                    return;
                const auto it = self->m_entries.constFind(path);
                if (it != self->m_entries.constEnd() && it->id == id) // not forgotten meanwhile
                    emit self->ready(path);
            },
            Qt::QueuedConnection);
    });
    return e;
}

TelemetryCache::Readings TelemetryCache::readings(const QString &path)
{
    if (path.isEmpty())
        return {};
    const std::shared_future<Readings> &f = entry(path).result;
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? f.get() : Readings{};
}

TelemetryCache::Readings TelemetryCache::await(const QString &path)
{
    return path.isEmpty() ? Readings{} : entry(path).result.get();
}

bool TelemetryCache::pending(const QString &path) const
{
    const auto it = m_entries.constFind(path);
    return it != m_entries.constEnd() &&
           it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void TelemetryCache::forget(const QStringList &paths)
{
    for (const QString &p : paths)
        m_entries.remove(p);
}

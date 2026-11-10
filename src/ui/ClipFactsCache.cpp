#include "ClipFactsCache.h"

#include <QPointer>

#include <ProbeClip.h> // stitch::ProbeClip

ClipFileFacts ClipFileFacts::fromProbe(const stitch::ClipProbe &probe)
{
    ClipFileFacts f;
    f.lensGuards = probe.lens_guards;
    for (const int64_t ms : probe.highlight_ms)
        f.highlightMs.push_back(ms);
    return f;
}

ClipFactsCache::ClipFactsCache(ClipProbeFn probe, QObject *parent)
    : QObject(parent), m_probe(std::move(probe))
{
    m_pool.setMaxThreadCount(1); // a trailer read each; the files share a disk
}

ClipFactsCache::~ClipFactsCache()
{
    m_pool.clear();
    m_pool.waitForDone();
}

ClipFileFacts ClipFactsCache::facts(const QString &path)
{
    if (const auto it = m_done.constFind(path); it != m_done.constEnd())
        return *it;
    if (path.isEmpty() || m_pending.contains(path))
        return {};
    m_pending.insert(path);

    QPointer<ClipFactsCache> self(this);
    m_pool.start([self, path, probe = m_probe] {
        stitch::ClipProbe p;
        std::string err;
        const bool ok = probe ? probe(path.toStdString(), p, err)
                              : stitch::ProbeClip(path.toStdString(), p, err);
        // A file that can't be read doesn't play either; leave its choices
        // open rather than read it again on every look.
        const ClipFileFacts found = ok ? ClipFileFacts::fromProbe(p) : ClipFileFacts{};
        QMetaObject::invokeMethod(
            self.data(),
            [self, path, found] {
                if (!self || !self->m_pending.remove(path))
                    return; // forgotten meanwhile
                self->m_done.insert(path, found);
                emit self->ready(path);
            },
            Qt::QueuedConnection);
    });
    return {};
}

void ClipFactsCache::set(const QString &path, const stitch::ClipProbe &probe)
{
    m_pending.remove(path);
    m_done.insert(path, ClipFileFacts::fromProbe(probe));
}

void ClipFactsCache::forget(const QStringList &paths)
{
    for (const QString &p : paths) {
        m_done.remove(p);
        m_pending.remove(p);
    }
}

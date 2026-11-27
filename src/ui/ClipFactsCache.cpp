#include "ClipFactsCache.h"

#include <QPointer>

#include <ProbeClip.h> // stitch::ProbeClip

ClipFileFacts ClipFileFacts::fromProbe(const stitch::ClipProbe &probe)
{
    ClipFileFacts f;
    f.lensGuards = probe.lens_guards;
    for (const int64_t ms : probe.highlight_ms)
        f.highlightMs.push_back(ms);
    f.cameraStitched = probe.camera_stitched;
    return f;
}

namespace {

ClipFileFacts readFacts(const ClipProbeFn &probe, const QString &path)
{
    stitch::ClipProbe p;
    std::string err;
    const bool ok = probe ? probe(path.toStdString(), p, err)
                          : stitch::ProbeClip(path.toStdString(), p, err);
    // A file that can't be read doesn't play either; leave its choices
    // open rather than read it again on every look.
    return ok ? ClipFileFacts::fromProbe(p) : ClipFileFacts{};
}

} // namespace

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
    const quint64 read = ++m_nextRead;
    m_pending.insert(path, read);

    QPointer<ClipFactsCache> self(this);
    m_pool.start([self, path, read, probe = m_probe] {
        const ClipFileFacts found = readFacts(probe, path);
        QMetaObject::invokeMethod(
            self.data(),
            [self, path, read, found] {
                if (!self || self->m_pending.value(path) != read)
                    return; // forgotten meanwhile
                self->m_pending.remove(path);
                self->m_done.insert(path, found);
                emit self->ready(path);
            },
            Qt::QueuedConnection);
    });
    return {};
}

ClipFileFacts ClipFactsCache::factsNow(const QString &path)
{
    if (const auto it = m_done.constFind(path); it != m_done.constEnd())
        return *it;
    if (path.isEmpty())
        return {};
    // A read in the background under way is dropped when it's in (no
    // longer pending): this one is the file's.
    m_pending.remove(path);
    const ClipFileFacts found = readFacts(m_probe, path);
    m_done.insert(path, found);
    QPointer<ClipFactsCache> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, path] {
            if (self && self->m_done.contains(path))
                emit self->ready(path);
        },
        Qt::QueuedConnection);
    return found;
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

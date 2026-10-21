#include "LensGuardCache.h"

#include <QPointer>

#include <ProbeClip.h> // stitch::ProbeClip

LensGuardCache::LensGuardCache(ClipProbeFn probe, QObject *parent)
    : QObject(parent), m_probe(std::move(probe))
{
    m_pool.setMaxThreadCount(1); // a trailer read each; the files share a disk
}

LensGuardCache::~LensGuardCache()
{
    m_pool.clear();
    m_pool.waitForDone();
}

stitch::LensGuardProfiles LensGuardCache::profiles(const QString &path)
{
    if (const auto it = m_done.constFind(path); it != m_done.constEnd())
        return *it;
    if (path.isEmpty() || m_pending.contains(path))
        return stitch::LensGuardProfiles::all();
    m_pending.insert(path);

    QPointer<LensGuardCache> self(this);
    m_pool.start([self, path, probe = m_probe] {
        stitch::ClipProbe p;
        std::string err;
        const bool ok = probe ? probe(path.toStdString(), p, err)
                              : stitch::ProbeClip(path.toStdString(), p, err);
        // A file that can't be read doesn't play either; leave its choices
        // open rather than read it again on every look.
        const stitch::LensGuardProfiles found = ok ? p.lens_guards
                                                   : stitch::LensGuardProfiles::all();
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
    return stitch::LensGuardProfiles::all();
}

void LensGuardCache::set(const QString &path, stitch::LensGuardProfiles profiles)
{
    m_pending.remove(path);
    m_done.insert(path, profiles);
}

void LensGuardCache::forget(const QStringList &paths)
{
    for (const QString &p : paths) {
        m_done.remove(p);
        m_pending.remove(p);
    }
}

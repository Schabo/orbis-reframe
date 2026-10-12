#include "WaveformCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr quint32 kMagic = 0x57465031; // "WFP1": bump if the peak format changes

insv::AudioPeaks load(const QString &file, bool *ok)
{
    insv::AudioPeaks p;
    *ok = false;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return p;
    quint32 magic = 0, perSec = 0;
    quint64 n = 0;
    if (f.read(reinterpret_cast<char *>(&magic), 4) != 4 || magic != kMagic ||
        f.read(reinterpret_cast<char *>(&perSec), 4) != 4 ||
        perSec != quint32(insv::AudioPeaks::kPerSec) ||
        f.read(reinterpret_cast<char *>(&n), 8) != 8 || n > (1u << 28))
        return p;
    p.peaks.resize(n);
    const qint64 bytes = qint64(n * sizeof(float));
    if (f.read(reinterpret_cast<char *>(p.peaks.data()), bytes) != bytes) {
        p.peaks.clear();
        return p;
    }
    *ok = true;
    return p;
}

void save(const QString &file, const insv::AudioPeaks &p)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return;
    const quint32 magic = kMagic, perSec = insv::AudioPeaks::kPerSec;
    const quint64 n = p.peaks.size();
    f.write(reinterpret_cast<const char *>(&magic), 4);
    f.write(reinterpret_cast<const char *>(&perSec), 4);
    f.write(reinterpret_cast<const char *>(&n), 8);
    f.write(reinterpret_cast<const char *>(p.peaks.data()), qint64(n * sizeof(float)));
    f.commit();
}

} // namespace

WaveformCache::WaveformCache(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(2); // I/O-bound (an .insv's audio is spread through GBs of video)
}

WaveformCache::~WaveformCache()
{
    m_cancel->store(true);
    m_pool.clear();
    m_pool.waitForDone();
}

QString WaveformCache::key(const QString &path, int stream)
{
    return path + QLatin1Char('#') + QString::number(stream);
}

QString WaveformCache::diskPath(const QString &path, int stream) const
{
    const QFileInfo fi(path);
    const QByteArray id = (fi.absoluteFilePath() + QLatin1Char('|') + QString::number(fi.size()) +
                           QLatin1Char('|') +
                           QString::number(fi.lastModified().toMSecsSinceEpoch()) +
                           QLatin1Char('|') + QString::number(stream))
                              .toUtf8();
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex());
    const QString base =
        m_dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : m_dir;
    return base + QStringLiteral("/waveforms/") + hash + QStringLiteral(".peaks");
}

WaveformCache::Peaks WaveformCache::peaks(const QString &path, int stream)
{
    const QString k = key(path, stream);
    if (const auto it = m_done.constFind(k); it != m_done.constEnd())
        return *it;
    if (m_pending.contains(k) || path.isEmpty())
        return nullptr;
    m_pending.insert(k);

    const QString disk = diskPath(path, stream);
    QPointer<WaveformCache> self(this);
    auto cancel = m_cancel;
    m_pool.start([self, k, path, stream, disk, cancel] {
        bool cached = false;
        insv::AudioPeaks p = load(disk, &cached);
        if (!cached) {
            p = insv::ComputeAudioPeaks(path.toStdString(), stream, cancel.get());
            if (cancel->load())
                return;
            save(disk, p);
        }
        auto result = std::make_shared<const insv::AudioPeaks>(std::move(p));
        QMetaObject::invokeMethod(
            self.data(),
            [self, k, result] {
                if (!self)
                    return;
                self->m_pending.remove(k);
                self->m_done.insert(k, result);
                emit self->ready();
            },
            Qt::QueuedConnection);
    });
    return nullptr;
}

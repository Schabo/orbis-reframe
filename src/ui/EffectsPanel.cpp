#include "EffectsPanel.h"

#include "../core/ClipEffects.h"

#include <QAction>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QRadialGradient>
#include <QVBoxLayout>

namespace {

constexpr int kNameRole = Qt::UserRole;

// The picture the tiles show an effect on: a sky over a field.
void paintScene(QPainter &p, const QRectF &r)
{
    QLinearGradient sky(r.topLeft(), r.bottomLeft());
    sky.setColorAt(0.0, QColor(0x7a, 0xa6, 0xd0));
    sky.setColorAt(0.6, QColor(0xc8, 0xb0, 0x80));
    sky.setColorAt(1.0, QColor(0x6a, 0x7a, 0x4a));
    p.setPen(Qt::NoPen);
    p.setBrush(sky);
    p.drawRoundedRect(r, 4, 4);
}

// A vignette's tile: the picture, darker toward the corners.
QIcon vignetteTile(const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    paintScene(p, r);
    QRadialGradient dark(r.center(), r.width() * 0.62);
    dark.setColorAt(0.45, QColor(0, 0, 0, 0));
    dark.setColorAt(1.0, QColor(0, 0, 0, 220));
    p.setBrush(dark);
    p.drawRoundedRect(r, 4, 4);
    return QIcon(pm);
}

// Grain's tile: the picture with fx::applyEffects()' own grain on it, big
// enough to see at the tile's size.
QIcon grainTile(const QSize &size, qreal dpr)
{
    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        paintScene(p, QRectF(QPointF(0, 0), QSizeF(size)));
    }
    fx::ClipEffects e;
    e.grain = fx::Grain{true, 70.0f, 100.0f, 0.0f};
    fx::Render r = e.render();
    r.grain[1] = float(img.height()) / 2.0f; // grains two pixels wide
    for (int y = 0; y < img.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = line[x];
            const int a = qAlpha(c);
            if (a == 0)
                continue;
            float rgb[3] = {float(qRed(c)) * 255 / a, float(qGreen(c)) * 255 / a,
                            float(qBlue(c)) * 255 / a};
            fx::applyEffects(rgb, x + 0.5f, y + 0.5f, float(img.width()), float(img.height()),
                             r, 255.0f);
            line[x] = qPremultiply(qRgba(int(rgb[0] + 0.5f), int(rgb[1] + 0.5f),
                                         int(rgb[2] + 0.5f), a));
        }
    }
    return QIcon(QPixmap::fromImage(img));
}

} // namespace

EffectList::EffectList(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("effectList"));
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setWrapping(true);
    setSpacing(6);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setFrameShape(QFrame::NoFrame);
}

QStringList EffectList::mimeTypes() const
{
    return {QString::fromLatin1(fx::kMimeType)};
}

QMimeData *EffectList::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(fx::kMimeType),
                  items.first()->data(kNameRole).toString().toLatin1());
    return mime;
}

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("effectsPanel"));
    setAttribute(Qt::WA_StyledBackground);

    auto *heading = new QLabel(tr("Effects"), this);
    heading->setObjectName(QStringLiteral("sidebarPageTitle"));

    m_list = new EffectList(this);
    const QSize tile(128, 72);
    m_list->setIconSize(tile);
    auto *vignette =
        new QListWidgetItem(vignetteTile(tile, devicePixelRatioF()), tr("Vignette"), m_list);
    vignette->setData(kNameRole, QString::fromLatin1(fx::kVignette));
    vignette->setToolTip(tr("Darker (or lighter) toward the edges of the picture. Drag it onto "
                            "a clip, or double-click to put it on the clip at the playhead."));
    auto *grain = new QListWidgetItem(grainTile(tile, devicePixelRatioF()), tr("Grain"), m_list);
    grain->setData(kNameRole, QString::fromLatin1(fx::kGrain));
    grain->setToolTip(tr("Film grain over the picture, new on every frame. Drag it onto a "
                         "clip, or double-click to put it on the clip at the playhead."));
    m_list->setCurrentItem(vignette);
    const auto add = [this] {
        if (const QListWidgetItem *item = m_list->currentItem())
            emit addEffectRequested(item->data(kNameRole).toString());
    };
    connect(m_list, &QListWidget::itemActivated, this, add);

    auto *addAction = new QAction(tr("&Add to the clip at the playhead"), m_list);
    addAction->setObjectName(QStringLiteral("effectAddAction"));
    connect(addAction, &QAction::triggered, this, add);
    m_list->addAction(addAction);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *hint = new QLabel(tr("Drag an effect onto a clip on the video track, or double-click "
                               "it to put it on the clip at the playhead. Its settings are in "
                               "the Video tab on the right, under Effects."),
                            this);
    hint->setObjectName(QStringLiteral("effectsHint"));
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addWidget(m_list, 1);
}

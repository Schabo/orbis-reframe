#include "EffectsPanel.h"

#include "../core/ClipEffects.h"
#include "../core/ColorLut.h"
#include "../core/LookPresets.h"
#include "LookNames.h"

#include <QAction>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QRadialGradient>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

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

// The picture through `lut` (in the tile's right part, from `fromX` of its
// width on; 0 = all of it).
QIcon lutTile(const QSize &size, qreal dpr, const color::Lut3D &lut, double fromX = 0.0)
{
    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        paintScene(p, QRectF(QPointF(0, 0), QSizeF(size)));
    }
    const int x0 = int(fromX * img.width());
    for (int y = 0; y < img.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = x0; x < img.width(); ++x) {
            const QRgb c = line[x];
            const int a = qAlpha(c);
            if (a == 0)
                continue;
            const float in[3] = {float(qRed(c)) / a, float(qGreen(c)) / a, float(qBlue(c)) / a};
            float out[3];
            lut.apply(in, out);
            const auto level = [](float v) { return std::clamp(int(v * 255.0f + 0.5f), 0, 255); };
            line[x] = qPremultiply(qRgba(level(out[0]), level(out[1]), level(out[2]), a));
        }
    }
    if (fromX > 0.0) { // where the look starts
        QPainter p(&img);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
        p.drawLine(QPointF(x0 / dpr, 0), QPointF(x0 / dpr, size.height()));
    }
    return QIcon(QPixmap::fromImage(img));
}

// Custom LUT's tile: half the picture as it is, half through a warm look
// (shadows cooler, highlights warmer) -- what a LUT of your own might do.
QIcon customLookTile(const QSize &size, qreal dpr)
{
    color::Lut3D lut;
    lut.size = 2;
    lut.rgb = {0.05f, 0.10f, 0.20f, 1.00f, 0.15f, 0.10f, 0.05f, 0.95f, 0.30f, 0.90f, 1.00f, 0.25f,
               0.05f, 0.10f, 0.70f, 1.00f, 0.25f, 0.75f, 0.10f, 0.95f, 0.85f, 1.00f, 0.92f, 0.80f};
    return lutTile(size, dpr, lut, 0.5);
}

} // namespace

QString EffectsPanel::lookTile(const QString &presetId)
{
    return QString::fromLatin1(fx::kLook) + QLatin1Char(':') + presetId;
}

EffectList::EffectList(QWidget *parent) : DoubleClickToAddList(parent)
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

    // The colour looks: a creative LUT after the Colour section's grade, one
    // per clip -- a built-in one, or a .cube of your own.
    auto *looksHeading = new QLabel(tr("Colour looks"), this);
    looksHeading->setObjectName(QStringLiteral("sidebarPageTitle"));
    m_looks = new EffectList(this);
    m_looks->setObjectName(QStringLiteral("lookList"));
    m_looks->setIconSize(tile);
    for (const char *id : color::look::kPresets) {
        const QString presetId = QString::fromLatin1(id);
        auto *item = new QListWidgetItem(
            lutTile(tile, devicePixelRatioF(), *color::look::presetLut(id)), looknames::name(presetId),
            m_looks);
        item->setData(kNameRole, lookTile(presetId));
        item->setToolTip(tr("A colour look: %1. Drag it onto a clip, or double-click to put it "
                            "on the clip at the playhead; it replaces the clip's look.")
                             .arg(looknames::name(presetId)));
    }
    auto *custom = new QListWidgetItem(customLookTile(tile, devicePixelRatioF()),
                                       tr("Custom LUT"), m_looks);
    custom->setData(kNameRole, QString::fromLatin1(kCustomLook));
    custom->setToolTip(tr("A colour look from a 3D LUT file (.cube) of your own, after the "
                          "clip's colour grade. Drag it onto a clip, or double-click, and pick "
                          "the file."));

    for (EffectList *l : {m_list, m_looks}) {
        const auto add = [this, l] {
            if (const QListWidgetItem *item = l->currentItem())
                emit addEffectRequested(item->data(kNameRole).toString());
        };
        connect(l, &QListWidget::itemDoubleClicked, this, [add] { add(); });
        connect(l, &DoubleClickToAddList::activationKeyPressed, this, add);
        auto *addAction = new QAction(tr("&Add to the clip at the playhead"), l);
        addAction->setObjectName(l == m_list ? QStringLiteral("effectAddAction")
                                             : QStringLiteral("lookAddAction"));
        connect(addAction, &QAction::triggered, this, add);
        l->addAction(addAction);
        l->setContextMenuPolicy(Qt::ActionsContextMenu);
        // Both groups in one scrolling page: each list as tall as its tiles.
        l->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        l->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    auto *hint = new QLabel(tr("Drag an effect onto a clip on the video track, or double-click "
                               "it to put it on the clip at the playhead. Its settings are in "
                               "the Video tab on the right, under Effects."),
                            this);
    hint->setObjectName(QStringLiteral("effectsHint"));
    hint->setWordWrap(true);

    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("effectsPage"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(m_list);
    pageLayout->addWidget(looksHeading);
    pageLayout->addWidget(m_looks);
    pageLayout->addStretch(1);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("effectsScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidget(page);
    m_scroll->viewport()->installEventFilter(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addWidget(m_scroll, 1);
}

void EffectsPanel::fitLists()
{
    const int width = m_scroll->viewport()->width();
    for (EffectList *l : {m_list, m_looks}) {
        l->setFixedWidth(width);
        l->doItemsLayout();
        int bottom = 0;
        for (int i = 0; i < l->count(); ++i)
            bottom = std::max(bottom, l->visualItemRect(l->item(i)).bottom());
        l->setFixedHeight(bottom + l->spacing() + 2 * l->frameWidth() + 1);
    }
}

bool EffectsPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_scroll && watched == m_scroll->viewport() && event->type() == QEvent::Resize)
        fitLists();
    return QWidget::eventFilter(watched, event);
}

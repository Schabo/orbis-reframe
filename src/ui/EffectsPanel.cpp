#include "EffectsPanel.h"

#include "../core/ClipEffects.h"

#include <QAction>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QRadialGradient>
#include <QVBoxLayout>

namespace {

constexpr int kNameRole = Qt::UserRole;

// A vignette's tile: a picture's colours, darker toward the corners.
QIcon vignetteTile(const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    QLinearGradient sky(r.topLeft(), r.bottomLeft());
    sky.setColorAt(0.0, QColor(0x7a, 0xa6, 0xd0));
    sky.setColorAt(0.6, QColor(0xc8, 0xb0, 0x80));
    sky.setColorAt(1.0, QColor(0x6a, 0x7a, 0x4a));
    p.setPen(Qt::NoPen);
    p.setBrush(sky);
    p.drawRoundedRect(r, 4, 4);
    QRadialGradient dark(r.center(), r.width() * 0.62);
    dark.setColorAt(0.45, QColor(0, 0, 0, 0));
    dark.setColorAt(1.0, QColor(0, 0, 0, 220));
    p.setBrush(dark);
    p.drawRoundedRect(r, 4, 4);
    return QIcon(pm);
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

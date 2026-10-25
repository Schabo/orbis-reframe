#include "TitlesPanel.h"

#include "../core/Title.h"
#include "TitlePresets.h"
#include "TitleRender.h"

#include <QAction>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kPresetRole = Qt::UserRole;

// A tile's picture: the preset's title over a made-up landscape (sky, then
// ground), roughly where and how it lands. Its text is drawn larger than
// in a real frame -- at the true size a caption would be a few pixels high
// -- and kept inside the tile.
QIcon presetTile(const titlepresets::Preset &preset, const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0.0, QColor(0x5a, 0x7e, 0xa6));
    g.setColorAt(0.55, QColor(0x9a, 0xb4, 0xc8));
    g.setColorAt(0.56, QColor(0x4a, 0x4e, 0x44));
    g.setColorAt(1.0, QColor(0x2a, 0x2c, 0x28));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(r, 4, 4);

    constexpr double kScale = 1.5;  // the text, larger than in a frame
    constexpr double kMinFontPx = 7; // logical pixels
    const double W = size.width(), H = size.height();
    const title::Style style = titlepresets::styleFor(preset, QDateTime::currentDateTime());
    const double fontPx = std::max(preset.look.width * H * kScale, kMinFontPx);
    const titlerender::Picture pic = titlerender::render(style, qRound(fontPx * dpr));
    if (pic.empty())
        return QIcon(pm);
    inset::Look look = preset.look;
    look.width = static_cast<float>(fontPx / H);
    const inset::Look d = title::drawnLook(look, pic.widthPerFontPx * H / W, pic.anchorX);
    const double w = d.width * W, h = w * pic.height / pic.width;
    constexpr double kMargin = 3;
    const double x = std::clamp(d.centerX * W - w / 2, kMargin, std::max(kMargin, W - kMargin - w));
    const double y = std::clamp(d.centerY * H - h / 2, kMargin, std::max(kMargin, H - kMargin - h));
    const QImage img(pic.rgba->data(), pic.width, pic.height, pic.width * 4,
                     QImage::Format_RGBA8888_Premultiplied);
    p.drawImage(QRectF(x, y, w, h), img);
    return QIcon(pm);
}

} // namespace

TitleList::TitleList(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("titleList"));
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

QStringList TitleList::mimeTypes() const
{
    return {QString::fromLatin1(title::kMimeType)};
}

QMimeData *TitleList::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(title::kMimeType),
                  items.first()->data(kPresetRole).toString().toLatin1());
    return mime;
}

TitlesPanel::TitlesPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("titlesPanel"));
    setAttribute(Qt::WA_StyledBackground);

    auto *heading = new QLabel(tr("Titles"), this);
    heading->setObjectName(QStringLiteral("sidebarPageTitle"));

    m_list = new TitleList(this);
    const QSize tile(128, 72);
    m_list->setIconSize(tile);
    for (const titlepresets::Preset &preset : titlepresets::all()) {
        auto *item =
            new QListWidgetItem(presetTile(preset, tile, devicePixelRatioF()), preset.name, m_list);
        item->setData(kPresetRole, preset.id);
        item->setToolTip(preset.toolTip);
    }
    m_list->setCurrentRow(0);
    const auto addCurrent = [this] {
        if (const QListWidgetItem *item = m_list->currentItem())
            emit addTitleRequested(item->data(kPresetRole).toString());
    };
    connect(m_list, &QListWidget::itemActivated, this, addCurrent);

    auto *add = new QAction(tr("&Add to timeline"), m_list);
    add->setObjectName(QStringLiteral("titleAddToTimelineAction"));
    connect(add, &QAction::triggered, this, addCurrent);
    m_list->addAction(add);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *hint = new QLabel(tr("Drag a title onto the timeline, or double-click it to add it at "
                               "the playhead. Its text, font, colour, size and timing are in the "
                               "Title tab on the right."),
                            this);
    hint->setObjectName(QStringLiteral("titlesHint"));
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addWidget(m_list, 1);
}

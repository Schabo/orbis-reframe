#include "TitlesPanel.h"

#include "../core/Title.h"
#include "../core/TitleClock.h"
#include "TitlePresets.h"
#include "TitleRender.h"
#include "GaugeRender.h"

#include <QAction>
#include <QCursor>
#include <QLabel>
#include <QLinearGradient>
#include <QEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kPresetRole = Qt::UserRole;

// How long an animated tile's title rests between its In and its Out, and
// how long it stays off before the next round, while the mouse is over it.
constexpr qint64 kPreviewHoldMs = 1200;
constexpr qint64 kPreviewGapMs = 500;

qint64 previewSpanMs(const titlepresets::Preset &p)
{
    using Kind = inset::Motion::Kind;
    return (p.motion.in.kind == Kind::None ? 0 : p.motion.in.ms) + kPreviewHoldMs +
           (p.motion.out.kind == Kind::None ? 0 : p.motion.out.ms);
}

// A tile's made-up landscape (sky, then ground), its painter clipped to
// the tile's rounded corners: what a preset is shown over.
void tileBackground(QPainter &p, const QSize &size)
{
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
    QPainterPath clip;
    clip.addRoundedRect(r, 4, 4);
    p.setClipPath(clip); // a slide comes in past the tile's edge
}

// A ride's values, for a tile that shows GPS values.
Telemetry::Values sampleRide()
{
    Telemetry::Values ride;
    ride.speedMps = 20.0; // 72 km/h
    ride.headingDeg = 45.0;
    ride.altitudeM = 120.0;
    return ride;
}

// A tile's picture: the preset's title over a made-up landscape (sky, then
// ground), roughly where and how it lands. Its text is drawn larger than
// in a real frame -- at the true size a caption would be a few pixels high
// -- and kept inside the tile. At `localMs` into a title `spanMs` long
// (spanMs > 0), moved by the preset's motion from there: an animated tile
// playing, its text typed as far as it is then.
QIcon presetTile(const titlepresets::Preset &preset, const QSize &size, qreal dpr,
                 qint64 localMs = 0, qint64 spanMs = 0)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    tileBackground(p, size);

    constexpr double kScale = 1.5;  // the text, larger than in a frame
    constexpr double kMinFontPx = 7; // logical pixels
    const double W = size.width(), H = size.height();
    const QDateTime now = QDateTime::currentDateTime();
    title::Style style = titlepresets::styleFor(preset);
    // A date or clock tile shows the time now, a GPS one a ride's values.
    const bool clock = title::hasFields(style.text);
    style.text =
        title::withFields(style.text, now, QLocale(), style.hours, sampleRide(), style.units);
    const double fontPx = std::max(preset.look.width * H * kScale, kMinFontPx);
    // Typing on or off: the picture changes as the frame's would.
    const int visible =
        spanMs > 0
            ? titlerender::visibleAt(titlerender::graphemeCount(style.text), preset.motion,
                                     localMs, spanMs)
            : -1;
    const titlerender::Picture pic =
        titlerender::render(style, qRound(fontPx * dpr), visible, clock);
    if (pic.empty())
        return QIcon(pm);
    inset::Look look = preset.look;
    look.width = static_cast<float>(fontPx / H);
    const inset::Look d = title::drawnLook(look, pic.widthPerFontPx * H / W, pic.anchorX);
    const double w = d.width * W, h = w * pic.height / pic.width;
    constexpr double kMargin = 3;
    const double x = std::clamp(d.centerX * W - w / 2, kMargin, std::max(kMargin, W - kMargin - w));
    const double y = std::clamp(d.centerY * H - h / 2, kMargin, std::max(kMargin, H - kMargin - h));
    // Moved from where it rests in the tile, as the frame would move it.
    inset::Look rest = d;
    rest.centerX = static_cast<float>((x + w / 2) / W);
    rest.centerY = static_cast<float>((y + h / 2) / H);
    const inset::Look m = spanMs > 0 ? inset::moved(rest, preset.motion, localMs, spanMs,
                                                    double(pic.width) / pic.height, W / H)
                                     : rest;
    const double mw = m.width * W, mh = mw * pic.height / pic.width;
    if (mw < 0.5)
        return QIcon(pm); // popped down to nothing
    const QImage img(pic.rgba->data(), pic.width, pic.height, pic.width * 4,
                     QImage::Format_RGBA8888_Premultiplied);
    p.drawImage(QRectF(m.centerX * W - mw / 2, m.centerY * H - mh / 2, mw, mh), img);
    return QIcon(pm);
}

// A gauge tile: the gauge showing a ride's values over the landscape,
// roughly where it lands, larger than in a frame and kept inside the tile.
QIcon gaugeTile(const titlepresets::GaugePreset &preset, const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    tileBackground(p, size);
    constexpr double kScale = 1.4;
    const gauge::Style style = titlepresets::gaugeStyleFor(preset);
    const double W = size.width(), H = size.height();
    const double aspect = gauge::aspect(style.kind);
    // Big enough to read on a tile, and inside it.
    const double h = std::min({std::max(preset.look.width * H * kScale, 0.45 * H), H - 6.0,
                               (W - 6.0) / aspect});
    const double w = h * aspect;
    const int px = std::max(8, int(std::lround(h * dpr)));
    const gaugerender::Picture pic =
        gaugerender::render(style, px, gaugerender::readingOf(style, sampleRide(), px));
    if (pic.empty())
        return QIcon(pm);
    constexpr double kMargin = 3;
    const double x =
        std::clamp(preset.look.centerX * W - w / 2, kMargin, std::max(kMargin, W - kMargin - w));
    const double y =
        std::clamp(preset.look.centerY * H - h / 2, kMargin, std::max(kMargin, H - kMargin - h));
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
    m_list->setIconSize(kTileSize);
    for (const titlepresets::Preset &preset : titlepresets::all()) {
        auto *item = new QListWidgetItem(presetTile(preset, kTileSize, devicePixelRatioF()),
                                         preset.name, m_list);
        item->setData(kPresetRole, preset.id);
        item->setToolTip(preset.toolTip);
    }
    m_list->setCurrentRow(0);
    const auto addCurrent = [this] {
        if (const QListWidgetItem *item = m_list->currentItem())
            emit addTitleRequested(item->data(kPresetRole).toString());
    };
    connect(m_list, &QListWidget::itemActivated, this, addCurrent);

    // An animated tile plays its In and Out while the mouse is over it.
    m_list->viewport()->setMouseTracking(true);
    m_list->viewport()->installEventFilter(this);
    connect(m_list, &QListWidget::itemEntered, this, &TitlesPanel::preview);
    m_previewTimer.setInterval(33);
    connect(&m_previewTimer, &QTimer::timeout, this, &TitlesPanel::stepPreview);

    auto *add = new QAction(tr("&Add to timeline"), m_list);
    add->setObjectName(QStringLiteral("titleAddToTimelineAction"));
    connect(add, &QAction::triggered, this, addCurrent);
    m_list->addAction(add);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *hint = new QLabel(tr("Drag a title or a gauge onto the timeline, or double-click it to "
                               "add it at the playhead. Its settings, size and timing are in the "
                               "Title tab on the right."),
                            this);
    hint->setObjectName(QStringLiteral("titlesHint"));
    hint->setWordWrap(true);

    // The gauges: a GPS value drawn as an instrument, added and edited the
    // same way.
    auto *gaugeHeading = new QLabel(tr("Gauges"), this);
    gaugeHeading->setObjectName(QStringLiteral("sidebarPageTitle"));
    m_gauges = new TitleList(this);
    m_gauges->setObjectName(QStringLiteral("gaugeList"));
    m_gauges->setIconSize(kTileSize);
    for (const titlepresets::GaugePreset &preset : titlepresets::gauges()) {
        auto *item = new QListWidgetItem(gaugeTile(preset, kTileSize, devicePixelRatioF()),
                                         preset.name, m_gauges);
        item->setData(kPresetRole, preset.id);
        item->setToolTip(preset.toolTip);
    }
    connect(m_gauges, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        emit addTitleRequested(item->data(kPresetRole).toString());
    });
    auto *addGauge = new QAction(tr("&Add to timeline"), m_gauges);
    addGauge->setObjectName(QStringLiteral("gaugeAddToTimelineAction"));
    connect(addGauge, &QAction::triggered, this, [this] {
        if (const QListWidgetItem *item = m_gauges->currentItem())
            emit addTitleRequested(item->data(kPresetRole).toString());
    });
    m_gauges->addAction(addGauge);
    m_gauges->setContextMenuPolicy(Qt::ActionsContextMenu);

    // Both groups in one scrolling page: each list as tall as its tiles.
    for (TitleList *l : {m_list, m_gauges}) {
        l->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        l->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("titlesPage"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(m_list);
    pageLayout->addWidget(gaugeHeading);
    pageLayout->addWidget(m_gauges);
    pageLayout->addStretch(1);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("titlesScroll"));
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

void TitlesPanel::fitLists()
{
    const int width = m_scroll->viewport()->width();
    for (TitleList *l : {m_list, m_gauges}) {
        l->setFixedWidth(width);
        l->doItemsLayout();
        int bottom = 0;
        for (int i = 0; i < l->count(); ++i)
            bottom = std::max(bottom, l->visualItemRect(l->item(i)).bottom());
        l->setFixedHeight(bottom + l->spacing() + 2 * l->frameWidth() + 1);
    }
}

bool TitlesPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_scroll && watched == m_scroll->viewport() && event->type() == QEvent::Resize)
        fitLists();
    // A Leave can come while the pointer is still on the tile (seen on
    // Sway): only stop if it really is elsewhere.
    if (watched == m_list->viewport() && event->type() == QEvent::Leave && !pointerOnPreview())
        preview(nullptr);
    return QWidget::eventFilter(watched, event);
}

bool TitlesPanel::pointerOnPreview() const
{
    return m_previewItem && isVisible() &&
           m_list->itemAt(m_list->viewport()->mapFromGlobal(QCursor::pos())) == m_previewItem;
}

void TitlesPanel::preview(QListWidgetItem *item)
{
    if (item == m_previewItem)
        return;
    const auto presetOf = [](const QListWidgetItem *it) {
        return it ? titlepresets::find(it->data(kPresetRole).toString()) : nullptr;
    };
    if (const titlepresets::Preset *p = presetOf(m_previewItem)) // back to rest
        m_previewItem->setIcon(presetTile(*p, kTileSize, devicePixelRatioF()));
    const titlepresets::Preset *p = presetOf(item);
    m_previewItem = p && !p->motion.isNone() ? item : nullptr;
    if (!m_previewItem) {
        m_previewTimer.stop();
        return;
    }
    m_previewClock.start();
    stepPreview();
    m_previewTimer.start();
}

void TitlesPanel::stepPreview()
{
    const titlepresets::Preset *p =
        m_previewItem ? titlepresets::find(m_previewItem->data(kPresetRole).toString()) : nullptr;
    if (!p || !pointerOnPreview()) {
        preview(nullptr);
        return;
    }
    const qint64 span = previewSpanMs(*p);
    const qint64 t = m_previewClock.elapsed() % (span + kPreviewGapMs);
    m_previewItem->setIcon(
        presetTile(*p, kTileSize, devicePixelRatioF(), std::min(t, span), span));
}

#include "ProjectLibraryPage.h"
#include "Theme.h"

#include "ClipThumbnail.h"

#include "../core/ProjectLibrary.h"

#include <QAction>
#include <QDate>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kPad = 10;
constexpr int kRadius = 6;
constexpr int kLineH = 18;

QThreadPool &summaryPool()
{
    // Reading a project and its thumbnail is mostly waiting on the disk; a
    // few at a time keeps a slow drive from being swamped.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(4);
        return p;
    }();
    return *pool;
}

QString durationText(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600)
            .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString whenText(const QDateTime &t)
{
    const QLocale locale;
    const QDate today = QDate::currentDate();
    const QString time = locale.toString(t.time(), QLocale::ShortFormat);
    if (t.date() == today)
        return QObject::tr("today, %1").arg(time);
    if (t.date() == today.addDays(-1))
        return QObject::tr("yesterday, %1").arg(time);
    if (t.date() > today.addDays(-7))
        return locale.dayName(t.date().dayOfWeek());
    return locale.toString(t.date(), QLocale::ShortFormat);
}

class ProjectCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return ProjectCardList::cardSize();
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const theme::Colors &tc = theme::colors();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        // The card, inset so neighbours don't touch.
        const QRectF card = QRectF(option.rect).adjusted(6, 6, -6, -6);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        QPainterPath shape;
        shape.addRoundedRect(card, kRadius, kRadius);
        p->fillPath(shape, hover || selected ? tc.button : tc.panel);

        // Thumbnail across the top; a plain area with the project icon when
        // there's none (empty project, flat video first, file missing).
        const QSize ts = ProjectCardList::thumbnailSize();
        const QRectF thumbRect(card.left(), card.top(), card.width(), ts.height());
        p->save();
        p->setClipPath(shape);
        const QImage thumb = index.data(ProjectLibraryPage::ThumbnailRole).value<QImage>();
        if (!thumb.isNull()) {
            p->drawImage(thumbRect, thumb);
        } else {
            p->fillRect(thumbRect, tc.window);
            const QRect icon(0, 0, 40, 40);
            theme::icon(QStringLiteral("clapperboard"))
                .paint(p, icon.translated(thumbRect.center().toPoint() - icon.center()),
                       Qt::AlignCenter, QIcon::Disabled);
        }
        p->restore();

        // Name, when, details / problem.
        const QRectF text = card.adjusted(kPad, ts.height() + kPad - 2, -kPad, -kPad);
        QFont bold = option.font;
        bold.setWeight(QFont::DemiBold);
        p->setFont(bold);
        p->setPen(tc.text);
        QRectF line(text.left(), text.top(), text.width(), kLineH);
        p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(bold).elidedText(index.data(Qt::DisplayRole).toString(),
                                                  Qt::ElideRight, int(line.width())));
        p->setFont(option.font);
        const QFontMetrics fm(option.font);
        auto drawLine = [&](const QString &s, const QColor &color) {
            line.translate(0, kLineH);
            p->setPen(color);
            p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                        fm.elidedText(s, Qt::ElideRight, int(line.width())));
        };
        drawLine(index.data(ProjectLibraryPage::WhenRole).toString(), tc.textDim);
        const QString problem = index.data(ProjectLibraryPage::ProblemRole).toString();
        if (!problem.isEmpty())
            drawLine(problem, tc.error);
        else
            drawLine(index.data(ProjectLibraryPage::DetailsRole).toString(), tc.textDim);

        if (selected) {
            QPen pen(tc.accent, 2);
            p->setPen(pen);
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(card.adjusted(1, 1, -1, -1), kRadius - 1, kRadius - 1);
        }
        p->restore();
    }
};

} // namespace

ProjectCardList::ProjectCardList(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("projectList"));
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSpacing(4);
    setMouseTracking(true); // hover highlight
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setFrameShape(QFrame::NoFrame);
    setItemDelegate(new ProjectCardDelegate(this));
}

QSize ProjectCardList::cardSize() { return {268, 144 + 12 + 3 * kLineH + 2 * kPad}; }

QSize ProjectCardList::thumbnailSize() { return {256, 144}; }

ProjectLibraryPage::ProjectLibraryPage(ProjectLibrary *library, QWidget *parent)
    : QWidget(parent), m_library(library)
{
    setObjectName(QStringLiteral("projectLibrary"));
    // A QWidget subclass paints its stylesheet background only when asked.
    setAttribute(Qt::WA_StyledBackground);

    auto *title = new QLabel(tr("Projects"), this);
    title->setObjectName(QStringLiteral("projectLibraryTitle"));
    auto *openOther = new QPushButton(theme::icon(QStringLiteral("folder-open")),
                                      tr("&Open other…"), this);
    openOther->setObjectName(QStringLiteral("openOtherButton"));
    openOther->setToolTip(tr("Open a project file that isn't in this list"));
    auto *newProject = new QPushButton(theme::icon(QStringLiteral("file-plus-2")),
                                       tr("&New project…"), this);
    newProject->setObjectName(QStringLiteral("newProjectButton"));
    newProject->setDefault(true);
    connect(openOther, &QPushButton::clicked, this, &ProjectLibraryPage::openOtherRequested);
    connect(newProject, &QPushButton::clicked, this, &ProjectLibraryPage::newProjectRequested);

    auto *header = new QHBoxLayout;
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(openOther);
    header->addWidget(newProject);

    m_list = new ProjectCardList(this);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        emit openRequested(item->data(PathRole).toString());
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, &ProjectLibraryPage::updateActions);

    // Per card: the context menu, and the keys while the list has focus.
    m_openAction = new QAction(tr("&Open"), m_list);
    m_openAction->setObjectName(QStringLiteral("openProjectCardAction"));
    connect(m_openAction, &QAction::triggered, this, [this] {
        if (const QString path = selectedPath(); !path.isEmpty())
            emit openRequested(path);
    });
    m_showAction = new QAction(tr("Show in &file manager"), m_list);
    m_showAction->setObjectName(QStringLiteral("showProjectFolderAction"));
    connect(m_showAction, &QAction::triggered, this, &ProjectLibraryPage::showSelectedInFileManager);
    m_removeAction = new QAction(theme::icon(QStringLiteral("x")), tr("&Remove from list"),
                                 m_list);
    m_removeAction->setObjectName(QStringLiteral("removeProjectAction"));
    m_removeAction->setToolTip(tr("Remove the project from this list. Its files are not deleted."));
    m_removeAction->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_removeAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_removeAction, &QAction::triggered, this, &ProjectLibraryPage::removeSelected);
    m_list->addActions({m_openAction, m_showAction, m_removeAction});
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *empty = new QLabel(tr("No projects yet.\n\nNew project… starts one in its own folder. "
                                "Open other… opens a project file from anywhere."),
                             this);
    empty->setObjectName(QStringLiteral("projectLibraryEmpty"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);

    m_body = new QStackedWidget(this);
    m_body->addWidget(m_list);
    m_body->addWidget(empty);

    m_folderLabel = new QLabel(this);
    m_folderLabel->setObjectName(QStringLiteral("projectsFolderLabel"));
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *changeFolder = new QPushButton(tr("&Change…"), this);
    changeFolder->setObjectName(QStringLiteral("changeProjectsFolderButton"));
    changeFolder->setToolTip(tr("Where new projects go by default. Every project folder in it "
                                "is listed here."));
    connect(changeFolder, &QPushButton::clicked, this, &ProjectLibraryPage::chooseProjectsFolder);
    auto *footer = new QHBoxLayout;
    footer->addWidget(m_folderLabel, 1);
    footer->addWidget(changeFolder);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 18, 24, 14);
    layout->setSpacing(12);
    layout->addLayout(header);
    layout->addWidget(m_body, 1);
    layout->addLayout(footer);

    updateActions();
}

ProjectLibraryPage::~ProjectLibraryPage()
{
    // Workers post back to this page; let the running ones finish first.
    ++m_generation;
    summaryPool().clear();
    summaryPool().waitForDone();
}

void ProjectLibraryPage::refresh()
{
    const int generation = ++m_generation;
    summaryPool().clear(); // an older refresh's queued reads
    m_list->clear();
    m_library->load();
    m_folderLabel->setText(tr("Projects folder: %1")
                               .arg(QDir::toNativeSeparators(m_library->projectsFolder())));

    const auto entries = m_library->entries();
    m_pending = int(entries.size());
    for (const auto &e : entries) {
        auto *item = new QListWidgetItem(QFileInfo(e.path).completeBaseName(), m_list);
        item->setData(PathRole, e.path);
        item->setData(SummaryReadRole, false);
        item->setData(WhenRole, e.lastOpened.isValid()
                                    ? tr("Opened %1").arg(whenText(e.lastOpened))
                                    : tr("Saved %1").arg(whenText(e.sortTime)));
        item->setData(DetailsRole, tr("Reading…"));
        item->setToolTip(QDir::toNativeSeparators(e.path));

        summaryPool().start([this, generation, path = e.path] {
            const ProjectSummary s = ProjectSummary::read(path);
            const QImage thumb = s.firstClip.isEmpty()
                                     ? QImage()
                                     : clipThumbnail(s.firstClip, ProjectCardList::thumbnailSize() *
                                                                      devicePixelRatio());
            QMetaObject::invokeMethod(
                this, [this, generation, s, thumb] { applySummary(generation, s, thumb); },
                Qt::QueuedConnection);
        });
    }
    m_body->setCurrentIndex(entries.isEmpty() ? 1 : 0);
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateActions();
    if (m_pending == 0)
        emit loaded();
}

void ProjectLibraryPage::applySummary(int generation, const ProjectSummary &s, const QImage &thumb)
{
    if (generation != m_generation)
        return;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(PathRole).toString() != s.path)
            continue;
        item->setData(SummaryReadRole, true);
        QString details = s.clipCount == 0 ? tr("Empty")
                          : s.clipCount == 1
                              ? tr("1 clip · %1").arg(durationText(s.durationMs))
                              : tr("%1 clips · %2").arg(s.clipCount).arg(durationText(s.durationMs));
        QString problem;
        if (s.fileMissing)
            problem = tr("Project file not found");
        else if (!s.error.isEmpty())
            problem = tr("Can't be read");
        else if (s.missingMedia > 0)
            problem = s.missingMedia == 1 ? tr("1 media file missing")
                                          : tr("%1 media files missing").arg(s.missingMedia);
        item->setData(DetailsRole, details);
        item->setData(ProblemRole, problem);
        if (!thumb.isNull()) {
            QImage t = thumb;
            t.setDevicePixelRatio(devicePixelRatio());
            item->setData(ThumbnailRole, t);
        }
        QString tip = QDir::toNativeSeparators(s.path);
        if (!s.error.isEmpty())
            tip += QStringLiteral("\n") + s.error;
        item->setToolTip(tip);
        break;
    }
    if (--m_pending == 0)
        emit loaded();
}

QString ProjectLibraryPage::selectedPath() const
{
    const auto items = m_list->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(PathRole).toString();
}

void ProjectLibraryPage::updateActions()
{
    const QString path = selectedPath();
    m_openAction->setEnabled(!path.isEmpty() && QFileInfo(path).isFile());
    m_showAction->setEnabled(!path.isEmpty() && QFileInfo(path).absoluteDir().exists());
    m_removeAction->setEnabled(!path.isEmpty());
}

void ProjectLibraryPage::removeSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    QString error;
    if (!m_library->remove(path, &error))
        QMessageBox::warning(this, tr("Could not update the project list"), error);
    const int row = m_list->currentRow();
    delete m_list->takeItem(row);
    if (m_list->count() > 0)
        m_list->setCurrentRow(std::min(row, m_list->count() - 1));
    m_body->setCurrentIndex(m_list->count() == 0 ? 1 : 0);
    updateActions();
}

void ProjectLibraryPage::showSelectedInFileManager()
{
    const QString path = selectedPath();
    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void ProjectLibraryPage::chooseProjectsFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Projects folder"),
                                                          m_library->projectsFolder());
    if (dir.isEmpty())
        return;
    QString error;
    if (!m_library->setProjectsFolder(dir, &error))
        QMessageBox::warning(this, tr("Could not update the project list"), error);
    emit projectsFolderChanged();
    refresh();
}

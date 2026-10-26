#include "TitleTextPanel.h"

#include "PanelFields.h"

#include <QButtonGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>

#include "../../core/TitleClock.h"

#include <algorithm>

namespace {

constexpr int kRecentFonts = 3;
const QString kRecentKey = QStringLiteral("titles/recentFonts");

QSettings settings()
{
    return QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
}

// The installed families a title can use, sorted (private system families
// aren't fonts to pick).
QStringList installedFamilies()
{
    QStringList out;
    for (const QString &f : QFontDatabase::families())
        if (!QFontDatabase::isPrivateFamily(f))
            out << f;
    return out;
}

// The family's name drawn in its own face, when the face can draw it: symbol
// and maths fonts that claim Latin may have no letters or empty ones.
QFont previewFont(const QString &family, const QFont &base)
{
    static QHash<QString, bool> drawsName;
    auto it = drawsName.find(family);
    if (it == drawsName.end()) {
        bool ok = QFontDatabase::writingSystems(family).contains(QFontDatabase::Latin);
        const QFontMetrics m{QFont(family)};
        for (const QChar ch : family)
            ok = ok && m.inFont(ch);
        it = drawsName.insert(family, ok && m.horizontalAdvance(family) > 0);
    }
    if (!*it)
        return base;
    // The list's own size, whether the theme gives it in points or pixels.
    QFont f = base;
    f.setFamilies({family});
    return f;
}

} // namespace

TitleTextPanel::TitleTextPanel(QWidget *parent) : SettingsPanel(tr("Text"), parent)
{
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("titleText"));
    m_text->setTabChangesFocus(true);
    m_text->setProperty("ownsReturnEscape", true); // Enter is a new line (MainWindow)
    m_text->setFixedHeight(m_text->fontMetrics().lineSpacing() * 3 + 12);
    m_text->setToolTip(tr("The title's text. Enter starts a new line."));
    form()->addRow(tr("Text"), m_text);
    // The clock's fields, put in where the cursor is.
    m_insert = new QPushButton(tr("Insert"), this);
    m_insert->setObjectName(QStringLiteral("titleInsert"));
    m_insert->setToolTip(tr("Put the time or the date the picture under the title was "
                            "recorded, or the speed, altitude or heading from its GPS, into "
                            "the text, where the cursor is. The text shows it as a field in "
                            "braces; the picture shows its value, which follows the clip "
                            "under the title as it plays."));
    auto *insertMenu = new QMenu(m_insert);
    const struct {
        const char *name;
        QString text;
        QString field;
    } fields[] = {
        {"titleInsertTime", tr("Time"), title::kTimeField},
        {"titleInsertTimeSeconds", tr("Time with seconds"), title::kTimeSecondsField},
        {"titleInsertDate", tr("Date"), title::kDateField},
        {nullptr, {}, {}},
        {"titleInsertSpeed", tr("Speed"), title::kSpeedField},
        {"titleInsertAltitude", tr("Altitude"), title::kAltitudeField},
        {"titleInsertHeading", tr("Heading in degrees"), title::kHeadingField},
        {"titleInsertCompass", tr("Heading as a compass point"), title::kCompassField},
    };
    for (const auto &f : fields) {
        if (!f.name) {
            insertMenu->addSeparator();
            continue;
        }
        QAction *a = insertMenu->addAction(f.text + QStringLiteral("\t") + f.field);
        a->setObjectName(QLatin1String(f.name));
        connect(a, &QAction::triggered, this, [this, field = f.field] {
            m_text->insertPlainText(field);
            m_text->setFocus();
        });
    }
    m_insert->setMenu(insertMenu);
    auto *insertRow = new QWidget(this);
    auto *insertLayout = new QHBoxLayout(insertRow);
    insertLayout->setContentsMargins(0, 0, 0, 0);
    insertLayout->addWidget(m_insert);
    insertLayout->addStretch(1);
    form()->addRow(QString(), insertRow);
    m_hours = new QComboBox(this);
    m_hours->setObjectName(QStringLiteral("titleHours"));
    m_hours->addItem(tr("As the system writes it"));
    m_hours->addItem(tr("24-hour"));
    m_hours->addItem(tr("12-hour (AM / PM)"));
    m_hours->setToolTip(tr("How the time in the text writes the hours: as this computer's "
                           "language and region settings write a time, or always 24-hour or "
                           "12-hour. Saved with the title, so the export looks the same on "
                           "any computer when it isn't the system's."));
    form()->addRow(tr("Clock"), m_hours);
    m_speedUnit = new QComboBox(this);
    m_speedUnit->setObjectName(QStringLiteral("titleSpeedUnit"));
    m_speedUnit->addItem(tr("km/h"));
    m_speedUnit->addItem(tr("mph"));
    m_speedUnit->setToolTip(tr("The unit the speed in the text is written in. Saved with the "
                               "title; a new title takes the one this computer's region "
                               "uses."));
    form()->addRow(tr("Speed in"), m_speedUnit);
    m_altitudeUnit = new QComboBox(this);
    m_altitudeUnit->setObjectName(QStringLiteral("titleAltitudeUnit"));
    m_altitudeUnit->addItem(tr("metres"));
    m_altitudeUnit->addItem(tr("feet"));
    m_altitudeUnit->setToolTip(tr("The unit the altitude in the text is written in. Saved with "
                                  "the title; a new title takes the one this computer's "
                                  "region uses."));
    form()->addRow(tr("Altitude in"), m_altitudeUnit);
    m_clockNote = new QLabel(this);
    m_clockNote->setObjectName(QStringLiteral("titleClockNote"));
    m_clockNote->setWordWrap(true);
    m_clockNote->setVisible(false);
    form()->addRow(m_clockNote);
    m_font = new QComboBox(this);
    m_font->setObjectName(QStringLiteral("titleFont"));
    m_font->setToolTip(tr("The title's font: the fonts installed on this computer, the last "
                          "ones you picked at the top. Type a name's first letters to jump "
                          "to it."));
    m_font->setMinimumWidth(1); // long font names mustn't widen the sidebar
    fillFonts(QString());
    form()->addRow(tr("Font"), m_font);
    m_styleRow = new QWidget(this);
    auto *styleLayout = new QHBoxLayout(m_styleRow);
    styleLayout->setContentsMargins(0, 0, 0, 0);
    m_bold = new QPushButton(tr("Bold"), m_styleRow);
    m_bold->setObjectName(QStringLiteral("titleBold"));
    m_bold->setCheckable(true);
    m_italic = new QPushButton(tr("Italic"), m_styleRow);
    m_italic->setObjectName(QStringLiteral("titleItalic"));
    m_italic->setCheckable(true);
    styleLayout->addWidget(m_bold);
    styleLayout->addWidget(m_italic);
    styleLayout->addStretch(1);
    form()->addRow(tr("Style"), m_styleRow);
    m_color = new QPushButton(this);
    m_color->setObjectName(QStringLiteral("titleColor"));
    m_color->setToolTip(tr("The text's colour."));
    form()->addRow(tr("Colour"), m_color);
    m_alignRow = new QWidget(this);
    auto *alignLayout = new QHBoxLayout(m_alignRow);
    alignLayout->setContentsMargins(0, 0, 0, 0);
    auto *alignGroup = new QButtonGroup(this);
    const struct {
        const char *name;
        QString text;
        QString tip;
    } aligns[] = {
        {"titleAlignLeft", tr("Left"),
         tr("Lines start under each other. The title's position is its left edge: it "
            "stays put as the text grows.")},
        {"titleAlignCentre", tr("Centre"),
         tr("Lines centred under each other. The title's position is its middle.")},
        {"titleAlignRight", tr("Right"),
         tr("Lines end under each other. The title's position is its right edge: it "
            "stays put as the text grows.")},
    };
    for (int i = 0; i < 3; ++i) {
        m_align[i] = new QPushButton(aligns[i].text, m_alignRow);
        m_align[i]->setObjectName(QLatin1String(aligns[i].name));
        m_align[i]->setCheckable(true);
        m_align[i]->setToolTip(aligns[i].tip);
        alignGroup->addButton(m_align[i], i);
        alignLayout->addWidget(m_align[i]);
    }
    alignLayout->addStretch(1);
    form()->addRow(tr("Align"), m_alignRow);

    auto edited = [this] {
        if (!m_updating)
            emit titleEdited(current());
    };
    // Every keystroke: undo gathers a burst of edits into one step.
    connect(m_text, &QPlainTextEdit::textChanged, this, edited);
    connect(m_text, &QPlainTextEdit::textChanged, this, &TitleTextPanel::showHours);
    connect(m_hours, &QComboBox::activated, this, edited);
    connect(m_speedUnit, &QComboBox::activated, this, edited);
    connect(m_altitudeUnit, &QComboBox::activated, this, edited);
    connect(m_font, &QComboBox::activated, this, [this, edited](int index) {
        fontPicked(index);
        edited();
    });
    connect(m_bold, &QPushButton::toggled, this, edited);
    connect(m_italic, &QPushButton::toggled, this, edited);
    connect(alignGroup, &QButtonGroup::idToggled, this, [edited](int, bool on) {
        if (on)
            edited();
    });
    connect(m_color, &QPushButton::clicked, this, [this, edited] {
        const QColor c = QColorDialog::getColor(QColor::fromRgb(m_rgb), this, tr("Text colour"));
        if (!c.isValid())
            return;
        setColor(c.rgb() & 0xffffff);
        edited();
    });
    setNoTitle(QString());
}

title::Style TitleTextPanel::current() const
{
    title::Style t = m_style;
    t.text = m_text->toPlainText();
    t.font = m_font->currentData().toString();
    t.bold = m_bold->isChecked();
    t.italic = m_italic->isChecked();
    t.rgb = m_rgb;
    t.hours = static_cast<title::Hours>(std::max(0, m_hours->currentIndex()));
    t.units.speed = static_cast<title::SpeedUnit>(std::max(0, m_speedUnit->currentIndex()));
    t.units.altitude =
        static_cast<title::AltitudeUnit>(std::max(0, m_altitudeUnit->currentIndex()));
    for (int i = 0; i < 3; ++i)
        if (m_align[i]->isChecked())
            t.align = static_cast<title::Align>(i);
    return t;
}

void TitleTextPanel::setColor(uint32_t rgb)
{
    m_rgb = rgb & 0xffffff;
    panelfields::showColor(m_color, m_rgb);
}

void TitleTextPanel::setTitle(const title::Style &s, const QString &clockNote)
{
    m_updating = true;
    m_style = s;
    for (QWidget *w : {static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_insert),
                       static_cast<QWidget *>(m_font), m_styleRow,
                       static_cast<QWidget *>(m_color), m_alignRow})
        w->setEnabled(true);
    m_clockNote->setText(clockNote);
    m_clockNote->setVisible(!clockNote.isEmpty());
    if (m_text->toPlainText() != s.text)
        m_text->setPlainText(s.text);
    if (m_font->currentData().toString() != s.font)
        fillFonts(s.font);
    m_bold->setChecked(s.bold);
    m_italic->setChecked(s.italic);
    m_hours->setCurrentIndex(static_cast<int>(s.hours));
    m_hours->setEnabled(true);
    m_speedUnit->setCurrentIndex(static_cast<int>(s.units.speed));
    m_speedUnit->setEnabled(true);
    m_altitudeUnit->setCurrentIndex(static_cast<int>(s.units.altitude));
    m_altitudeUnit->setEnabled(true);
    showHours();
    m_align[static_cast<int>(s.align)]->setChecked(true);
    setColor(s.rgb);
    setHint(tr("The title selected on the timeline or in the view. The title changes as you "
               "type."));
    m_updating = false;
}

void TitleTextPanel::setNoTitle(const QString &hint)
{
    m_updating = true;
    m_text->clear();
    panelfields::clearColor(m_color);
    for (QWidget *w : {static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_insert),
                       static_cast<QWidget *>(m_font), m_styleRow,
                       static_cast<QWidget *>(m_color), m_alignRow})
        w->setEnabled(false);
    m_clockNote->clear();
    m_clockNote->setVisible(false);
    m_hours->setEnabled(false);
    m_speedUnit->setEnabled(false);
    m_altitudeUnit->setEnabled(false);
    showHours();
    setHint(hint.isEmpty() ? tr("Select a title on the timeline, or add one from the Overlays tab "
                                "on the left: drag it onto the timeline or double-click it.")
                           : hint);
    m_updating = false;
}

void TitleTextPanel::showHours()
{
    const QString text = m_text->toPlainText();
    form()->setRowVisible(m_hours, title::hasTimeField(text));
    form()->setRowVisible(m_speedUnit, text.contains(title::kSpeedField));
    form()->setRowVisible(m_altitudeUnit, text.contains(title::kAltitudeField));
}

void TitleTextPanel::fillFonts(const QString &family)
{
    const QStringList installed = installedFamilies();
    QStringList recent;
    for (const QString &f : settings().value(kRecentKey).toStringList())
        if (installed.contains(f) && recent.size() < kRecentFonts)
            recent << f;
    const QFont base = m_font->font();
    const auto add = [&](const QString &text, const QString &f, const QFont &look) {
        m_font->addItem(text, f);
        m_font->setItemData(m_font->count() - 1, look, Qt::FontRole);
    };
    const QSignalBlocker block(m_font);
    m_font->clear();
    if (!family.isEmpty() && !installed.contains(family))
        add(tr("%1 (not installed)").arg(family), family, base);
    for (const QString &f : recent)
        add(f, f, previewFont(f, base));
    if (m_font->count() > 0)
        m_font->insertSeparator(m_font->count());
    for (const QString &f : installed)
        add(f, f, previewFont(f, base));
    m_font->setCurrentIndex(family.isEmpty() ? -1 : m_font->findData(family));
}

void TitleTextPanel::fontPicked(int index)
{
    const QString family = m_font->itemData(index).toString();
    if (family.isEmpty() || !installedFamilies().contains(family))
        return;
    QSettings s = settings();
    QStringList recent = s.value(kRecentKey).toStringList();
    recent.removeAll(family);
    recent.prepend(family);
    s.setValue(kRecentKey, recent.mid(0, kRecentFonts));
    fillFonts(family);
}

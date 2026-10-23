#include "TitleTextPanel.h"

#include <QColorDialog>
#include <QComboBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>

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
    QFont f(family);
    f.setPointSizeF(base.pointSizeF());
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

    auto edited = [this] {
        if (!m_updating)
            emit titleEdited(current());
    };
    // Every keystroke: undo gathers a burst of edits into one step.
    connect(m_text, &QPlainTextEdit::textChanged, this, edited);
    connect(m_font, &QComboBox::activated, this, [this, edited](int index) {
        fontPicked(index);
        edited();
    });
    connect(m_bold, &QPushButton::toggled, this, edited);
    connect(m_italic, &QPushButton::toggled, this, edited);
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
    title::Style t;
    t.text = m_text->toPlainText();
    t.font = m_font->currentData().toString();
    t.bold = m_bold->isChecked();
    t.italic = m_italic->isChecked();
    t.rgb = m_rgb;
    return t;
}

void TitleTextPanel::setColor(uint32_t rgb)
{
    m_rgb = rgb & 0xffffff;
    const QColor c = QColor::fromRgb(m_rgb);
    m_color->setText(c.name());
    m_color->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; }")
            .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

void TitleTextPanel::setTitle(const title::Style &s)
{
    m_updating = true;
    for (QWidget *w : {static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_font),
                       m_styleRow, static_cast<QWidget *>(m_color)})
        w->setEnabled(true);
    if (m_text->toPlainText() != s.text)
        m_text->setPlainText(s.text);
    if (m_font->currentData().toString() != s.font)
        fillFonts(s.font);
    m_bold->setChecked(s.bold);
    m_italic->setChecked(s.italic);
    setColor(s.rgb);
    setHint(tr("The title selected on the timeline or in the view. The title changes as you "
               "type."));
    m_updating = false;
}

void TitleTextPanel::setNoTitle(const QString &hint)
{
    m_updating = true;
    m_text->clear();
    m_color->setText(QString());
    m_color->setStyleSheet(QString());
    for (QWidget *w : {static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_font),
                       m_styleRow, static_cast<QWidget *>(m_color)})
        w->setEnabled(false);
    setHint(hint.isEmpty() ? tr("Select a title on the timeline, or add one from the Titles tab "
                                "on the left: drag it onto the timeline or double-click it.")
                           : hint);
    m_updating = false;
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

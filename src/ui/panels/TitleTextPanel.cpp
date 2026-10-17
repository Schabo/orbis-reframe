#include "TitleTextPanel.h"

#include <QColorDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>

TitleTextPanel::TitleTextPanel(QWidget *parent) : SettingsPanel(tr("Text"), parent)
{
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("titleText"));
    m_text->setTabChangesFocus(true);
    m_text->setProperty("ownsReturnEscape", true); // Enter is a new line (MainWindow)
    m_text->setFixedHeight(m_text->fontMetrics().lineSpacing() * 3 + 12);
    m_text->setToolTip(tr("The title's text. Enter starts a new line."));
    form()->addRow(tr("Text"), m_text);
    m_font = new QFontComboBox(this);
    m_font->setObjectName(QStringLiteral("titleFont"));
    m_font->setToolTip(tr("The title's font."));
    m_font->setMinimumWidth(1); // long font names mustn't widen the sidebar
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
    connect(m_font, &QFontComboBox::currentFontChanged, this, edited);
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
    t.font = m_font->currentFont().family();
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
    if (m_font->currentFont().family() != s.font)
        m_font->setCurrentFont(QFont(s.font));
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

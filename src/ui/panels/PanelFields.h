#pragma once

#include <QColor>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QWidget>

#include <cstdint>

// Fields several settings panels share: a number box, a slider with its value
// beside it, and a button that shows a colour.
namespace panelfields {

inline QDoubleSpinBox *numberBox(QWidget *parent, double lo, double hi, int decimals, double step,
                                 const QString &suffix)
{
    auto *b = new QDoubleSpinBox(parent);
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setSingleStep(step);
    b->setSuffix(suffix);
    b->setKeyboardTracking(false); // commit on Enter / focus-out / arrows, not per keystroke
    return b;
}

inline QWidget *sliderRow(QWidget *parent, int lo, int hi, QSlider **slider, QLabel **value)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    *slider = new QSlider(Qt::Horizontal, row);
    (*slider)->setRange(lo, hi);
    (*slider)->setPageStep(10);
    *value = new QLabel(row);
    (*value)->setMinimumWidth(row->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    layout->addWidget(*slider, 1);
    layout->addWidget(*value);
    return row;
}

// The button filled with the colour, its name on it in black or white.
inline void showColor(QPushButton *button, uint32_t rgb)
{
    const QColor c = QColor::fromRgb(rgb & 0xffffff);
    button->setText(c.name());
    button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; }")
            .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

// An empty button (nothing selected).
inline void clearColor(QPushButton *button)
{
    button->setText(QString());
    button->setStyleSheet(QString());
}

} // namespace panelfields

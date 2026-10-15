#include "ShortcutHelp.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

void showShortcutHelp(QWidget *parent, const QList<KeyHelp> &entries, const QMenuBar *menus)
{
{
    const auto keysText = [](const QList<QKeySequence> &keys) {
        QStringList parts;
        for (const QKeySequence &k : keys)
            parts << k.toString(QKeySequence::NativeText).toHtmlEscaped();
        return parts.join(QStringLiteral(" / "));
    };
    // Sections of (keys, what) rows, laid out in two columns below.
    struct Section {
        QString title;
        QList<std::pair<QString, QString>> rows;
    };
    QList<Section> sections;
    const auto row = [&sections](const QString &group, const QString &keys, const QString &what) {
        if (sections.isEmpty() || sections.back().title != group)
            sections.append({group, {}});
        sections.back().rows.append({keys, what});
    };

    // Shortcuts made by addKey(); an entry with no text pairs with the one
    // before it ("Previous / next frame": Left / Right).
    for (int i = 0; i < entries.size(); ++i) {
        const KeyHelp &k = entries[i];
        if (k.what.isEmpty())
            continue;
        QList<QKeySequence> keys = k.keys;
        for (int j = i + 1; j < entries.size() && entries[j].what.isEmpty(); ++j)
            keys += entries[j].keys;
        row(k.group, k.keysLabel.isEmpty() ? keysText(keys) : k.keysLabel.toHtmlEscaped(), k.what);
    }
    // Menu actions, from the menus themselves.
    for (QAction *menuAction : menus->actions()) {
        QMenu *menu = menuAction->menu();
        if (!menu)
            continue;
        const QString title =
            QObject::tr("%1 menu").arg(QString(menuAction->text()).remove(QLatin1Char('&')));
        for (QAction *a : menu->actions())
            if (!a->shortcut().isEmpty())
                row(title, keysText(a->shortcuts()),
                    QString(a->text()).remove(QLatin1Char('&')).remove(QStringLiteral("…")));
    }

    // Two columns, split between sections near half the rows.
    int total = 0;
    for (const Section &s : sections)
        total += s.rows.size() + 1;
    QString columns[2];
    int done = 0;
    for (const Section &s : sections) {
        QString &html = columns[done + (s.rows.size() + 1) / 2 > total / 2 ? 1 : 0];
        html += QStringLiteral("<tr><td colspan='2' style='padding-top:10px'><b>%1</b></td></tr>")
                    .arg(s.title.toHtmlEscaped());
        for (const auto &[keys, what] : s.rows)
            html += QStringLiteral("<tr><td style='padding-right:18px'><tt>%1</tt></td>"
                                   "<td>%2</td></tr>")
                        .arg(keys, what.toHtmlEscaped());
        done += s.rows.size() + 1;
    }
    const QString html =
        QStringLiteral("<table><tr><td valign='top' style='padding-right:36px'>"
                       "<table cellspacing='0' cellpadding='3'>%1</table></td>"
                       "<td valign='top'><table cellspacing='0' cellpadding='3'>%2</table>"
                       "</td></tr></table>")
            .arg(columns[0], columns[1]);

    // Scrolls when the screen is too small for it.
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Keyboard shortcuts"));
    auto *label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setContentsMargins(12, 0, 12, 12);
    auto *scroll = new QScrollArea(&dlg);
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    const QSize want =
        label->sizeHint() + QSize(scroll->verticalScrollBar()->sizeHint().width(), 0);
    const QRect avail = dlg.screen()->availableGeometry();
    scroll->setMinimumSize(std::min(want.width(), avail.width() * 9 / 10),
                           std::min(want.height(), avail.height() * 8 / 10));
    dlg.exec();
}
}

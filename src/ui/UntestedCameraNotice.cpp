#include "UntestedCameraNotice.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>

namespace untestedcamera {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("UntestedCameraNotice", text);
}

QSettings settings()
{
    // Not the default QSettings(): that needs an organization name, which
    // would also move the cache folder (~/.cache/orbis-reframe).
    return QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
}

const QString kHiddenKey = QStringLiteral("untestedCameras/hidden");

} // namespace

QUrl sampleClipsUrl()
{
    return QUrl(QStringLiteral(
        "https://github.com/Schabo/orbis-reframe/blob/main/docs/formats/README.md"
        "#sample-clips-wanted"));
}

QStringList toTell(const QStringList &models, const QStringList &hidden, QSet<QString> &told)
{
    QStringList out;
    for (const QString &m : models) {
        if (m.isEmpty() || hidden.contains(m) || told.contains(m))
            continue;
        told.insert(m);
        out << m;
    }
    return out;
}

QString noticeText(const QStringList &models)
{
    QStringList names;
    for (const QString &m : models)
        names << QStringLiteral("<b>%1</b>").arg(m.toHtmlEscaped());
    const bool one = models.size() == 1;
    const QString link = QStringLiteral("<a href=\"%1\">%2</a>")
                             .arg(sampleClipsUrl().toString(), tr("sample clips wanted"));
    return QStringLiteral("<p>%1</p><p>%2</p><p>%3</p>")
        .arg((one ? tr("The %1 isn't supported yet: orbis-reframe hasn't been tested with "
                       "this camera.")
                  : tr("The %1 aren't supported yet: orbis-reframe hasn't been tested with "
                       "these cameras."))
                 .arg(QLocale().createSeparatedList(names)),
             one ? tr("Its clips open, but with general values instead of ones measured on "
                      "this camera, so they may not show correctly: the stitch seam, the "
                      "horizon and stabilization can be off.")
                 : tr("Their clips open, but with general values instead of ones measured on "
                      "these cameras, so they may not show correctly: the stitch seam, the "
                      "horizon and stabilization can be off."),
             (one ? tr("Short sample clips from it help add it: %1.")
                  : tr("Short sample clips from them help add them: %1."))
                 .arg(link));
}

void tell(QWidget *parent, const QStringList &models)
{
    static QSet<QString> told;
    const QStringList hidden = settings().value(kHiddenKey).toStringList();
    const QStringList now = toTell(models, hidden, told);
    if (now.isEmpty())
        return;
    QMessageBox box(QMessageBox::Information, tr("Untested camera"), noticeText(now),
                    QMessageBox::Ok, parent);
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction); // the link opens the browser
    box.setCheckBox(new QCheckBox(now.size() == 1 ? tr("Don't show this again for this camera")
                                                  : tr("Don't show this again for these cameras")));
    box.exec();
    if (box.checkBox()->isChecked())
        settings().setValue(kHiddenKey, QStringList(hidden) << now);
}

} // namespace untestedcamera

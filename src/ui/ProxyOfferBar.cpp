#include "ProxyOfferBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

ProxyOfferBar::ProxyOfferBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("proxyOfferBar"));
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("proxyOfferText"));
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto button = [this](const char *name, const QString &text) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoDefault(false);
        return b;
    };
    QPushButton *accept = button("proxyOfferAccept", tr("Play from preview files"));
    QPushButton *dismiss = button("proxyOfferDismiss", tr("Not now"));
    QPushButton *decline = button("proxyOfferDecline", tr("Don't ask again"));
    connect(accept, &QPushButton::clicked, this, [this] {
        hide();
        emit accepted();
    });
    connect(dismiss, &QPushButton::clicked, this, [this] {
        hide();
        emit dismissed();
    });
    connect(decline, &QPushButton::clicked, this, [this] {
        hide();
        emit declined();
    });

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(8);
    row->addWidget(m_text, 1);
    row->addWidget(accept);
    row->addWidget(dismiss);
    row->addWidget(decline);
    hide();
}

void ProxyOfferBar::offer(const QString &reason)
{
    m_text->setText(tr("%1. The camera's low-resolution preview files are much lighter: play from "
                       "them instead? The picture is softer; the export still uses the full "
                       "files.")
                        .arg(reason));
    show();
}

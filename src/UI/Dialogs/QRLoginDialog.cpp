#include "QRLoginDialog.hpp"

#include "Discord/RemoteAuthClient.hpp"
#include "Discord/CdnUrls.hpp"
#include "Core/Session.hpp"
#include "Core/Snowflake.hpp"

#include "qrcodegen.hpp"

#include <QDialogButtonBox>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

QRLoginDialog::QRLoginDialog(Core::Session *session, const Core::ProxyConfig &proxy, QWidget *parent)
    : QDialog(parent), session(session), proxy(proxy)
{
    setWindowTitle(tr("Log in with QR Code"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    avatarLabel = new QLabel(this);
    avatarLabel->setFixedSize(96, 96);
    avatarLabel->setScaledContents(true);
    avatarLabel->hide();
    layout->addWidget(avatarLabel, 0, Qt::AlignCenter);

    qrLabel = new QLabel(this);
    qrLabel->setFixedSize(256, 256);
    qrLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(qrLabel, 0, Qt::AlignCenter);

    statusLabel = new QLabel(tr("Generating QR code..."), this);
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(statusLabel);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    client = new Discord::RemoteAuthClient(proxy, session->getCaptchaResolver(), this);
    connect(client, &Discord::RemoteAuthClient::fingerprintReady, this, &QRLoginDialog::renderQr);
    connect(client, &Discord::RemoteAuthClient::pendingTicket, this, &QRLoginDialog::onPendingTicket);
    connect(client, &Discord::RemoteAuthClient::authenticated, this, &QRLoginDialog::onAuthenticated);
    connect(client, &Discord::RemoteAuthClient::failed, this, &QRLoginDialog::onFailed);
    connect(client, &Discord::RemoteAuthClient::canceled, this, &QRLoginDialog::onCanceled);
}

void QRLoginDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!started) {
        started = true;
        client->start();
    }
}

void QRLoginDialog::renderQr(const QString &fingerprint)
{
    const QString url = QStringLiteral("https://discord.com/ra/") + fingerprint;

    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.toUtf8().constData(), qrcodegen::QrCode::Ecc::QUARTILE);
        const int n = qr.getSize();
        const int border = 4;
        const int dim = n + border * 2;

        QImage image(dim, dim, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (qr.getModule(x, y))
                    image.setPixel(x + border, y + border, qRgb(0, 0, 0));

        qrLabel->setPixmap(QPixmap::fromImage(image.scaled(256, 256, Qt::KeepAspectRatio, Qt::FastTransformation)));
        statusLabel->setText(tr("Open Discord on your phone, go to Settings, tap \"Scan QR Code\", and scan this code."));
    } catch (const std::exception &) {
        statusLabel->setText(tr("Failed to generate the QR code."));
    }
}

void QRLoginDialog::onPendingTicket(const QString &userId, const QString &name, const QString &avatarHash)
{
    username = name;
    avatar = avatarHash;

    qrLabel->hide();

    const QString hash = (avatarHash == QLatin1String("0")) ? QString() : avatarHash;
    const Core::Snowflake pendingAccount(userId.toULongLong());
    const QUrl url = Discord::Cdn::userAvatar(pendingAccount, hash, 96);
    if (!url.isEmpty()) {
        avatarLabel->show();
        session->getImageManager()->setAccountProxy(pendingAccount, proxy);
        session->getImageManager()->assign(avatarLabel, url, QSize(96, 96), pendingAccount);
    }

    statusLabel->setText(tr("Logging in as %1...").arg(name));
}

void QRLoginDialog::onAuthenticated(const QString &t)
{
    token = t;
    accept();
}

void QRLoginDialog::onFailed(Discord::RemoteAuthError error)
{
    qrLabel->hide();
    avatarLabel->hide();
    statusLabel->setStyleSheet(QStringLiteral("color: red;"));

    QString message;
    switch (error) {
    case Discord::RemoteAuthError::KeyGenerationFailed:
        message = tr("Couldn't start a secure session. Please try again.");
        break;
    case Discord::RemoteAuthError::ConnectionFailed:
        message = tr("Couldn't reach Discord. Check your connection and try again.");
        break;
    case Discord::RemoteAuthError::HandshakeFailed:
        message = tr("The secure handshake with Discord failed. Please try again.");
        break;
    case Discord::RemoteAuthError::TimedOut:
        message = tr("The login request timed out. Please try again.");
        break;
    case Discord::RemoteAuthError::CaptchaRequired:
        message = tr("Discord is requiring a captcha. Log in with a token or browser instead.");
        break;
    case Discord::RemoteAuthError::LoginFailed:
        message = tr("Login failed. Please try again.");
        break;
    }
    statusLabel->setText(message);
}

void QRLoginDialog::onCanceled()
{
    statusLabel->setText(tr("Login was canceled on your phone."));
}

} // namespace UI
} // namespace Acheron

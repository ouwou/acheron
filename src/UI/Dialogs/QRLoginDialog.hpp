#pragma once

#include <QDialog>
#include <QString>

#include "Core/ProxyConfig.hpp"

class QLabel;

namespace Acheron {

namespace Core {
class Session;
}

namespace Discord {
class RemoteAuthClient;
enum class RemoteAuthError;
} // namespace Discord

namespace UI {

class QRLoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit QRLoginDialog(Core::Session *session, const Core::ProxyConfig &proxy, QWidget *parent = nullptr);

    QString getToken() const { return token; }
    QString getUsername() const { return username; }
    QString getAvatar() const { return avatar; }

protected:
    void showEvent(QShowEvent *event) override;

private:
    void renderQr(const QString &fingerprint);
    void onPendingTicket(const QString &userId, const QString &name, const QString &avatarHash);
    void onAuthenticated(const QString &token);
    void onFailed(Discord::RemoteAuthError error);
    void onCanceled();

    Core::Session *session = nullptr;
    Core::ProxyConfig proxy;
    Discord::RemoteAuthClient *client = nullptr;
    bool started = false;

    QLabel *qrLabel = nullptr;
    QLabel *avatarLabel = nullptr;
    QLabel *statusLabel = nullptr;

    QString token;
    QString username;
    QString avatar;
};

} // namespace UI
} // namespace Acheron

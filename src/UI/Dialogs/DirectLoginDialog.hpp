#pragma once

#include <QtWidgets>

#include "Core/ProxyConfig.hpp"
#include "UI/ProxyLineEdit.hpp"

namespace Acheron {

namespace Core {
class Session;
}

namespace Discord {
class LoginClient;
struct MfaChallenge;
enum class LoginError;
}

namespace UI {

class DirectLoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DirectLoginDialog(Core::Session *session, QWidget *parent = nullptr);

    QString getToken() const { return token; }
    QString getUserId() const { return userId; }
    QString getUsername() const { return username; }
    QString getDisplayName() const { return displayName; }
    QString getAvatar() const { return avatar; }
    Core::ProxyConfig getProxy() const { return proxy; }

private:
    void onLoginClicked();
    void onMfaRequired(const Discord::MfaChallenge &challenge);
    void onSmsClicked();
    void onVerifyClicked();
    void onAuthenticated(const QString &token, const QString &userId, const QString &username,
                         const QString &displayName, const QString &avatar);
    void onFailed(Discord::LoginError error);
    void onSmsSent(bool ok);

    void setBusy(bool busy);

    Core::Session *session = nullptr;
    Discord::LoginClient *client = nullptr;

    QStackedWidget *stack = nullptr;
    QWidget *loginPage = nullptr;
    QWidget *mfaPage = nullptr;

    QLineEdit *loginEdit = nullptr;
    QLineEdit *passwordEdit = nullptr;
    ProxyLineEdit *proxyEdit = nullptr;
    QLabel *loginStatus = nullptr;
    QPushButton *loginButton = nullptr;

    QLabel *mfaTitle = nullptr;
    QComboBox *mfaMethodCombo = nullptr;
    QLineEdit *codeEdit = nullptr;
    QPushButton *smsButton = nullptr;
    QPushButton *verifyButton = nullptr;
    QLabel *mfaStatus = nullptr;

    bool mfaSmsSent = false;

    QString token;
    QString userId;
    QString username;
    QString displayName;
    QString avatar;
    Core::ProxyConfig proxy;
};

} // namespace UI
} // namespace Acheron
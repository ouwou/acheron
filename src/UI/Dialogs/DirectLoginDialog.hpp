#pragma once

#include <QDialog>
#include <QString>

#include "Core/ProxyConfig.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace Acheron {

namespace Core {
class Session;
}

namespace Discord {
class CaptchaResolver;
class LoginClient;
struct MfaChallenge;
enum class LoginError;
enum class MfaMethod;
} // namespace Discord

namespace UI {

class ProxyLineEdit;

class DirectLoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DirectLoginDialog(Core::Session *session, QWidget *parent = nullptr);

    [[nodiscard]] QString getToken() const { return token; }
    [[nodiscard]] Core::ProxyConfig getProxy() const { return proxy; }

private:
    QWidget *buildLoginPage();
    QWidget *buildMfaPage();

    void ensureClient(const Core::ProxyConfig &wanted);
    void onLoginClicked();
    void onBackClicked();
    void onMfaRequired(const Discord::MfaChallenge &challenge);
    void onMfaMethodChanged();
    void onSmsClicked();
    void onSmsSent(bool ok);
    void onVerifyClicked();
    void onAuthenticated(const QString &token);
    void onFailed(Discord::LoginError error);

    [[nodiscard]] Discord::MfaMethod currentMfaMethod() const;
    void showStatus(QLabel *label, const QString &text, bool error = true);
    void setBusy(bool busy);

    Discord::CaptchaResolver *captchaResolver = nullptr;
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
    QLabel *mfaStatus = nullptr;
    QPushButton *backButton = nullptr;
    QPushButton *verifyButton = nullptr;

    QString token;
    Core::ProxyConfig proxy;
};

} // namespace UI
} // namespace Acheron

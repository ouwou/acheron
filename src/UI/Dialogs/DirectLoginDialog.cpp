#include "DirectLoginDialog.hpp"

#include "Core/Session.hpp"
#include "Discord/LoginClient.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

DirectLoginDialog::DirectLoginDialog(Core::Session *session, QWidget *parent)
    : QDialog(parent), session(session)
{
    setWindowTitle(tr("Log in"));
    resize(400, 300);

    stack = new QStackedWidget(this);

    loginPage = new QWidget(stack);
    QVBoxLayout *loginLayout = new QVBoxLayout(loginPage);

    QLabel *title = new QLabel(tr("Log into discord"), loginPage);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    loginLayout->addWidget(title);

    loginLayout->addSpacing(8);

    QFormLayout *form = new QFormLayout();
    loginEdit = new QLineEdit(loginPage);
    loginEdit->setPlaceholderText(tr("example@gmail.com or phone number"));
    passwordEdit = new QLineEdit(loginPage);
    passwordEdit->setEchoMode(QLineEdit::Password);
    proxyEdit = new ProxyLineEdit(loginPage);
    form->addRow(tr("Email or phone number"), loginEdit);
    form->addRow(tr("Password"), passwordEdit);
    form->addRow(tr("Proxy (optional)"), proxyEdit);
    loginLayout->addLayout(form);

    loginStatus = new QLabel(loginPage);
    loginStatus->setWordWrap(true);
    loginStatus->setAlignment(Qt::AlignCenter);
    loginStatus->setStyleSheet(QStringLiteral("color: red;"));
    loginStatus->hide();
    loginLayout->addWidget(loginStatus);

    loginLayout->addStretch();

    QHBoxLayout *loginButtons = new QHBoxLayout();
    QPushButton *cancelButton = new QPushButton(tr("Cancel"), loginPage);
    loginButton = new QPushButton(tr("Log in"), loginPage);
    loginButton->setDefault(true);
    loginButtons->addStretch();
    loginButtons->addWidget(cancelButton);
    loginButtons->addWidget(loginButton);
    loginLayout->addLayout(loginButtons);

    mfaPage = new QWidget(stack);
    QVBoxLayout *mfaLayout = new QVBoxLayout(mfaPage);

    mfaTitle = new QLabel(mfaPage);
    mfaTitle->setWordWrap(true);
    QFont mfaTitleFont = mfaTitle->font();
    mfaTitleFont.setPointSize(mfaTitleFont.pointSize() + 2);
    mfaTitleFont.setBold(true);
    mfaTitle->setFont(mfaTitleFont);
    mfaLayout->addWidget(mfaTitle);

    mfaLayout->addSpacing(8);

    mfaMethodCombo = new QComboBox(mfaPage);
    mfaLayout->addWidget(mfaMethodCombo);

    QHBoxLayout *codeLayout = new QHBoxLayout();
    codeEdit = new QLineEdit(mfaPage);
    codeEdit->setPlaceholderText(tr("6 digit code or backup code"));
    smsButton = new QPushButton(tr("send code"), mfaPage);
    codeLayout->addWidget(codeEdit);
    codeLayout->addWidget(smsButton);
    mfaLayout->addLayout(codeLayout);

    mfaStatus = new QLabel(mfaPage);
    mfaStatus->setWordWrap(true);
    mfaStatus->setAlignment(Qt::AlignCenter);
    mfaStatus->setStyleSheet(QStringLiteral("color: red;"));
    mfaStatus->hide();
    mfaLayout->addWidget(mfaStatus);

    mfaLayout->addStretch();

    QHBoxLayout *mfaButtons = new QHBoxLayout();
    QPushButton *backButton = new QPushButton(tr("back"), mfaPage);
    verifyButton = new QPushButton(tr("Verify"), mfaPage);
    verifyButton->setDefault(true);
    mfaButtons->addWidget(backButton);
    mfaButtons->addStretch();
    mfaButtons->addWidget(verifyButton);
    mfaLayout->addLayout(mfaButtons);

    stack->addWidget(loginPage);
    stack->addWidget(mfaPage);
    stack->setCurrentWidget(loginPage);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(stack);

    client = new Discord::LoginClient(session->getCaptchaResolver(), this);
    connect(client, &Discord::LoginClient::mfaRequired, this, &DirectLoginDialog::onMfaRequired);
    connect(client, &Discord::LoginClient::authenticated, this, &DirectLoginDialog::onAuthenticated);
    connect(client, &Discord::LoginClient::failed, this, &DirectLoginDialog::onFailed);
    connect(client, &Discord::LoginClient::smsSent, this, &DirectLoginDialog::onSmsSent);

    connect(loginButton, &QPushButton::clicked, this, &DirectLoginDialog::onLoginClicked);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(backButton, &QPushButton::clicked, this, [this]() {
        stack->setCurrentWidget(loginPage);
        loginStatus->hide();
        mfaStatus->hide();
        mfaSmsSent = false;
        setBusy(false);
        loginEdit->setFocus();
    });
    connect(smsButton, &QPushButton::clicked, this, &DirectLoginDialog::onSmsClicked);
    connect(verifyButton, &QPushButton::clicked, this, &DirectLoginDialog::onVerifyClicked);
    connect(loginEdit, &QLineEdit::returnPressed, this, &DirectLoginDialog::onLoginClicked);
    connect(passwordEdit, &QLineEdit::returnPressed, this, &DirectLoginDialog::onLoginClicked);
    connect(codeEdit, &QLineEdit::returnPressed, this, &DirectLoginDialog::onVerifyClicked);
}

void DirectLoginDialog::onLoginClicked()
{
    const QString login = loginEdit->text().trimmed();
    if (login.isEmpty() || passwordEdit->text().isEmpty()) {
        loginStatus->setText(tr("enter your email or phone number, and your password."));
        loginStatus->show();
        return;
    }

    std::optional<Core::ProxyConfig> parsed = proxyEdit->parseOrWarn();
    if (!parsed) {
        loginStatus->setText(tr("That proxy address isn't valid."));
        loginStatus->show();
        return;
    }
    proxy = *parsed;

    loginStatus->hide();
    mfaStatus->hide();
    mfaSmsSent = false;

    setBusy(true);
    loginButton->setText(tr("Logging in..."));
    client->startLogin(login, passwordEdit->text(), proxy);
}

void DirectLoginDialog::onMfaRequired(const Discord::MfaChallenge &challenge)
{
    mfaSmsSent = false;

    mfaMethodCombo->clear();
    if (challenge.totp)
        mfaMethodCombo->addItem(tr("authenticator app"), QStringLiteral("totp"));
    if (challenge.sms)
        mfaMethodCombo->addItem(tr("text message"), QStringLiteral("sms"));
    if (challenge.backup)
        mfaMethodCombo->addItem(tr("backup code"), QStringLiteral("backup"));

    mfaMethodCombo->setVisible(mfaMethodCombo->count() > 1);
    smsButton->setVisible(challenge.sms);

    if (challenge.sms && !challenge.totp && !challenge.backup)
        mfaTitle->setText(tr("enter the verification code sent to your phone."));
    else
        mfaTitle->setText(tr("enter your two-factor authentication code."));

    mfaStatus->hide();
    codeEdit->clear();
    verifyButton->setEnabled(true);
    smsButton->setEnabled(true);

    stack->setCurrentWidget(mfaPage);
    loginButton->setText(tr("Log in"));
    loginButton->setEnabled(true);
    codeEdit->setFocus();
}

void DirectLoginDialog::onSmsClicked()
{
    mfaStatus->hide();
    smsButton->setEnabled(false);
    mfaStatus->setText(tr("Requesting a code."));
    mfaStatus->show();
    client->sendSmsCode();
}

void DirectLoginDialog::onSmsSent(bool ok)
{
    smsButton->setEnabled(true);
    if (ok) {
        mfaSmsSent = true;
        mfaStatus->setText(
                tr("Code sent to your phone. Enter it below; it expires within a few minutes."));
        mfaStatus->setStyleSheet(QStringLiteral("color: inherit;"));
    } else {
        mfaStatus->setText(tr("Could not request a code/failed to request a code, check your connection and try again."));
        mfaStatus->setStyleSheet(QStringLiteral("color: red;"));
    }
    mfaStatus->show();
}

void DirectLoginDialog::onVerifyClicked()
{
    if (mfaMethodCombo->currentData().toString() == QLatin1String("sms") && !mfaSmsSent) {
        mfaStatus->setText(tr("Request a code first — click \"Send code\"."));
        mfaStatus->setStyleSheet(QStringLiteral("color: red;"));
        mfaStatus->show();
        return;
    }

    const QString code = codeEdit->text().trimmed();
    if (code.isEmpty()) {
        mfaStatus->setText(tr("Enter your verification code."));
        mfaStatus->setStyleSheet(QStringLiteral("color: red;"));
        mfaStatus->show();
        return;
    }

    mfaStatus->hide();
    setBusy(true);
    verifyButton->setText(tr("Verifying..."));
    client->submitMfa(code, mfaMethodCombo->currentData().toString());
}

void DirectLoginDialog::onAuthenticated(const QString &token, const QString &userId,
                                        const QString &username, const QString &displayName,
                                        const QString &avatar)
{
    this->token = token;
    this->userId = userId;
    this->username = username;
    this->displayName = displayName;
    this->avatar = avatar;
    accept();
}

void DirectLoginDialog::onFailed(Discord::LoginError error)
{
    QString message;
    switch (error) {
    case Discord::LoginError::Network:
        message = tr("Couldn't reach Discord. Check your connection and try again.");
        break;
    case Discord::LoginError::InvalidCredentials:
        message = tr("The email or password you entered is incorrect.");
        break;
    case Discord::LoginError::CaptchaRequired:
        message = tr("Discord asked for a captcha but it wasn't completed. Try again.");
        break;
    case Discord::LoginError::InvalidMfaCode:
        message = tr("That code didn't work. Double-check it and try again.");
        break;
    case Discord::LoginError::TooManyRequests:
        message = tr("Too many login attempts. Wait a bit and try again.");
        break;
    case Discord::LoginError::AccountLocked:
        message = tr("This account is disabled. Enable it on discord.com and try again.");
        break;
    case Discord::LoginError::Suspended:
        message = tr("This account has been suspended."); 
        break;
    case Discord::LoginError::RequiresVerification:
    message = tr("Discord wants to verify your login. Check your email or messages, follow the link, and try logging in again.");
        break;
    case Discord::LoginError::Unknown:
        message = tr("Login failed. Please try again. (This login error has not been properly handled)");
        break;
    }

    loginButton->setText(tr("Log in"));
    verifyButton->setText(tr("Verify"));

    if (error == Discord::LoginError::InvalidMfaCode) {
        setBusy(false);
        mfaStatus->setText(message);
        mfaStatus->show();
        codeEdit->setFocus();
        codeEdit->selectAll();
        return;
    }

    if (error == Discord::LoginError::InvalidCredentials) {
        setBusy(false);
        loginStatus->setText(message);
        loginStatus->show();
        passwordEdit->setFocus();
        passwordEdit->selectAll();
        return;
    }

    QLabel *status = (stack->currentWidget() == mfaPage) ? mfaStatus : loginStatus;
    status->setText(message);
    status->show();
}

void DirectLoginDialog::setBusy(bool busy)
{
    loginButton->setEnabled(!busy);
    verifyButton->setEnabled(!busy);
    smsButton->setEnabled(!busy);
    loginEdit->setEnabled(!busy);
    passwordEdit->setEnabled(!busy);
    codeEdit->setEnabled(!busy);
}

} // namespace UI
} // namespace Acheron
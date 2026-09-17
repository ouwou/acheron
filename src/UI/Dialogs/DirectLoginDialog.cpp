#include "DirectLoginDialog.hpp"

#include "Core/Session.hpp"
#include "Core/Theme/Manager.hpp"
#include "Discord/LoginClient.hpp"
#include "UI/ProxyLineEdit.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

static QLabel *makeStatusLabel(QWidget *parent)
{
    QLabel *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->hide();
    return label;
}

static QLabel *makeTitleLabel(int pointSizeDelta, QWidget *parent)
{
    QLabel *label = new QLabel(parent);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPointSize(font.pointSize() + pointSizeDelta);
    font.setBold(true);
    label->setFont(font);
    return label;
}

DirectLoginDialog::DirectLoginDialog(Core::Session *session, QWidget *parent)
    : QDialog(parent), captchaResolver(session->getCaptchaResolver())
{
    setWindowTitle(tr("Log in"));
    resize(400, 300);

    stack = new QStackedWidget(this);
    loginPage = buildLoginPage();
    mfaPage = buildMfaPage();
    stack->addWidget(loginPage);
    stack->addWidget(mfaPage);
    stack->setCurrentWidget(loginPage);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(stack);
}

QWidget *DirectLoginDialog::buildLoginPage()
{
    QWidget *page = new QWidget(stack);
    QVBoxLayout *layout = new QVBoxLayout(page);

    QLabel *title = makeTitleLabel(4, page);
    title->setText(tr("Log into Discord"));
    layout->addWidget(title);
    layout->addSpacing(8);

    QFormLayout *form = new QFormLayout();
    loginEdit = new QLineEdit(page);
    loginEdit->setPlaceholderText(tr("example@gmail.com or phone number"));
    passwordEdit = new QLineEdit(page);
    passwordEdit->setEchoMode(QLineEdit::Password);
    proxyEdit = new ProxyLineEdit(page);
    form->addRow(tr("Email or phone number"), loginEdit);
    form->addRow(tr("Password"), passwordEdit);
    form->addRow(tr("Proxy (optional)"), proxyEdit);
    layout->addLayout(form);

    loginStatus = makeStatusLabel(page);
    layout->addWidget(loginStatus);
    layout->addStretch();

    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *cancelButton = new QPushButton(tr("Cancel"), page);
    loginButton = new QPushButton(tr("Log in"), page);
    loginButton->setDefault(true);
    buttons->addStretch();
    buttons->addWidget(cancelButton);
    buttons->addWidget(loginButton);
    layout->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(loginButton, &QPushButton::clicked, this, &DirectLoginDialog::onLoginClicked);

    return page;
}

QWidget *DirectLoginDialog::buildMfaPage()
{
    QWidget *page = new QWidget(stack);
    QVBoxLayout *layout = new QVBoxLayout(page);

    mfaTitle = makeTitleLabel(2, page);
    layout->addWidget(mfaTitle);
    layout->addSpacing(8);

    mfaMethodCombo = new QComboBox(page);
    layout->addWidget(mfaMethodCombo);

    QHBoxLayout *codeLayout = new QHBoxLayout();
    codeEdit = new QLineEdit(page);
    codeEdit->setPlaceholderText(tr("6 digit code or backup code"));
    smsButton = new QPushButton(tr("Send code"), page);
    codeLayout->addWidget(codeEdit);
    codeLayout->addWidget(smsButton);
    layout->addLayout(codeLayout);

    mfaStatus = makeStatusLabel(page);
    layout->addWidget(mfaStatus);
    layout->addStretch();

    QHBoxLayout *buttons = new QHBoxLayout();
    backButton = new QPushButton(tr("Back"), page);
    verifyButton = new QPushButton(tr("Verify"), page);
    verifyButton->setDefault(true);
    buttons->addWidget(backButton);
    buttons->addStretch();
    buttons->addWidget(verifyButton);
    layout->addLayout(buttons);

    connect(backButton, &QPushButton::clicked, this, &DirectLoginDialog::onBackClicked);
    connect(verifyButton, &QPushButton::clicked, this, &DirectLoginDialog::onVerifyClicked);
    connect(smsButton, &QPushButton::clicked, this, &DirectLoginDialog::onSmsClicked);
    connect(mfaMethodCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &DirectLoginDialog::onMfaMethodChanged);

    return page;
}

void DirectLoginDialog::onLoginClicked()
{
    const QString login = loginEdit->text().trimmed();
    if (login.isEmpty() || passwordEdit->text().isEmpty()) {
        showStatus(loginStatus, tr("Enter your email or phone number, and your password."));
        return;
    }

    std::optional<Core::ProxyConfig> parsed = proxyEdit->parseOrWarn();
    if (!parsed)
        return;

    ensureClient(*parsed);

    loginStatus->hide();
    setBusy(true);
    loginButton->setText(tr("Logging in..."));
    client->login(login, passwordEdit->text());
}

// a new client would lose the cookies and fingerprint from the previous attempt
void DirectLoginDialog::ensureClient(const Core::ProxyConfig &wanted)
{
    if (client && proxy == wanted)
        return;

    delete client;
    proxy = wanted;
    client = new Discord::LoginClient(proxy, captchaResolver, this);
    connect(client, &Discord::LoginClient::mfaRequired, this, &DirectLoginDialog::onMfaRequired);
    connect(client, &Discord::LoginClient::authenticated, this, &DirectLoginDialog::onAuthenticated);
    connect(client, &Discord::LoginClient::failed, this, &DirectLoginDialog::onFailed);
    connect(client, &Discord::LoginClient::smsSent, this, &DirectLoginDialog::onSmsSent);
}

void DirectLoginDialog::onBackClicked()
{
    stack->setCurrentWidget(loginPage);
    mfaStatus->hide();
    loginEdit->setFocus();
}

void DirectLoginDialog::onMfaRequired(const Discord::MfaChallenge &challenge)
{
    setBusy(false);

    codeEdit->clear();
    mfaStatus->hide();

    if (challenge.sms && !challenge.totp && !challenge.backup)
        mfaTitle->setText(tr("Enter the verification code sent to your phone."));
    else
        mfaTitle->setText(tr("Enter your two-factor authentication code."));

    {
        QSignalBlocker blocker(mfaMethodCombo);
        mfaMethodCombo->clear();
        if (challenge.totp)
            mfaMethodCombo->addItem(tr("Authenticator app"), static_cast<int>(Discord::MfaMethod::Totp));
        if (challenge.backup)
            mfaMethodCombo->addItem(tr("Backup code"), static_cast<int>(Discord::MfaMethod::Backup));
        if (challenge.sms)
            mfaMethodCombo->addItem(tr("Text message"), static_cast<int>(Discord::MfaMethod::Sms));
    }
    mfaMethodCombo->setVisible(mfaMethodCombo->count() > 1);
    onMfaMethodChanged();

    stack->setCurrentWidget(mfaPage);
    codeEdit->setFocus();
}

void DirectLoginDialog::onMfaMethodChanged()
{
    if (mfaMethodCombo->count() == 0)
        return;

    smsButton->setVisible(currentMfaMethod() == Discord::MfaMethod::Sms);
}

void DirectLoginDialog::onSmsClicked()
{
    setBusy(true);
    showStatus(mfaStatus, tr("Requesting a code."), false);
    client->requestSmsCode();
}

void DirectLoginDialog::onSmsSent(bool ok)
{
    setBusy(false);
    if (ok)
        showStatus(mfaStatus, tr("Code sent to your phone. Enter it below; it expires within a few minutes."), false);
    else
        showStatus(mfaStatus, tr("Failed to request a code, check your connection and try again."));
}

void DirectLoginDialog::onVerifyClicked()
{
    const QString code = codeEdit->text().trimmed();
    if (code.isEmpty()) {
        showStatus(mfaStatus, tr("Enter your verification code."));
        return;
    }

    mfaStatus->hide();
    setBusy(true);
    verifyButton->setText(tr("Verifying..."));
    client->submitMfa(currentMfaMethod(), code);
}

void DirectLoginDialog::onAuthenticated(const QString &t)
{
    token = t;
    accept();
}

void DirectLoginDialog::onFailed(Discord::LoginError error)
{
    setBusy(false);

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
    case Discord::LoginError::PasskeyOnly:
        message = tr("This account only allows passkeys or security keys for two-factor authentication, which aren't supported here. Log in with a token or QR code instead.");
        break;
    case Discord::LoginError::Unknown:
        message = tr("Login failed. Please try again. (This login error has not been properly handled)");
        break;
    }

    if (stack->currentWidget() == mfaPage && client->canRetryMfa()) {
        showStatus(mfaStatus, message);
        if (error == Discord::LoginError::InvalidMfaCode) {
            codeEdit->setFocus();
            codeEdit->selectAll();
        }
        return;
    }

    stack->setCurrentWidget(loginPage);
    showStatus(loginStatus, message);
    if (error == Discord::LoginError::InvalidCredentials) {
        passwordEdit->setFocus();
        passwordEdit->selectAll();
    }
}

Discord::MfaMethod DirectLoginDialog::currentMfaMethod() const
{
    return static_cast<Discord::MfaMethod>(mfaMethodCombo->currentData().toInt());
}

void DirectLoginDialog::showStatus(QLabel *label, const QString &text, bool error)
{
    const QColor errorColor = Core::Theme::Manager::instance().color(Core::Theme::Token::ChatError);
    label->setStyleSheet(error ? QStringLiteral("color: %1;").arg(errorColor.name()) : QString());
    label->setText(text);
    label->show();
}

void DirectLoginDialog::setBusy(bool busy)
{
    if (!busy) {
        loginButton->setText(tr("Log in"));
        verifyButton->setText(tr("Verify"));
    }

    loginEdit->setEnabled(!busy);
    passwordEdit->setEnabled(!busy);
    proxyEdit->setEnabled(!busy);
    loginButton->setEnabled(!busy);

    mfaMethodCombo->setEnabled(!busy);
    codeEdit->setEnabled(!busy);
    smsButton->setEnabled(!busy);
    backButton->setEnabled(!busy);
    verifyButton->setEnabled(!busy);
}

} // namespace UI
} // namespace Acheron

#include "BrowserCaptchaResolver.hpp"

#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include "Core/Logging.hpp"
#include "UI/Dialogs/ConfirmPopup.hpp"

namespace Acheron {
namespace UI {

static const char *kHarnessTemplate = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Acheron Captcha Challenge</title>
<script src="https://js.hcaptcha.com/1/api.js?render=explicit" async defer></script>
<script id="cfg" type="application/json">__CONFIG_JSON__</script>
<style>
  body { font-family: sans-serif; background: #313338; color: #dbdee1;
         display: flex; flex-direction: column; align-items: center;
         justify-content: center; min-height: 90vh; }
</style>
</head>
<body>
<p>Complete the captcha below to make Discord happy</p>
<div id="hcaptcha-container"></div>
<script>
function cfg() { return JSON.parse(document.getElementById('cfg').textContent); }
function postBack(payload) {
  fetch(cfg().tokenUrl, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  }).then(function () {
    document.body.innerHTML = '<p>You can close this tab and return to Acheron.</p>';
  });
}
function onSolved(token) { postBack({ token: token }); }
function onCaptchaError(err) { postBack({ error: String(err) }); }
function renderWidget() {
  var c = cfg();
  var params = { sitekey: c.sitekey, callback: onSolved, 'error-callback': onCaptchaError };
  if (c.rqdata || c.invisible) params.size = 'invisible';
  var id = hcaptcha.render('hcaptcha-container', params);
  if (c.rqdata)
    hcaptcha.execute(id, { rqdata: c.rqdata });
  else if (c.invisible)
    hcaptcha.execute(id);
}
var _poll = setInterval(function () {
  if (window.hcaptcha && window.hcaptcha.render) {
    clearInterval(_poll);
    renderWidget();
  }
}, 100);
</script>
</body>
</html>
)HTML";

static void sendHttp(QTcpSocket *socket, int code, const QByteArray &contentType,
                     const QByteArray &body)
{
    QByteArray status;
    switch (code) {
    case 200:
        status = "200 OK";
        break;
    case 403:
        status = "403 Forbidden";
        break;
    case 404:
        status = "404 Not Found";
        break;
    default:
        status = "400 Bad Request";
        break;
    }

    QByteArray resp = "HTTP/1.1 " + status + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Cache-Control: no-store\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;

    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

BrowserCaptchaResolver::BrowserCaptchaResolver(QWidget *parentWindow, QObject *parent)
    : QObject(parent), parentWindow(parentWindow)
{
}

BrowserCaptchaResolver::~BrowserCaptchaResolver()
{
    if (modal)
        modal->deleteLater();
}

void BrowserCaptchaResolver::resolve(const Discord::CaptchaChallenge &challenge,
                                     const Core::ProxyConfig &proxy,
                                     Discord::CaptchaResolver::Callback cb)
{
    QMetaObject::invokeMethod(
            this,
            [this, challenge, proxy, cb]() {
                // reuse since it got rejected
                if (state == State::Verifying) {
                    current = challenge;
                    currentProxy = proxy;
                    currentCb = cb;
                    beginChallenge(true);
                    return;
                }

                queue.push_back({ challenge, proxy, cb });
                if (state == State::Idle)
                    startNext();
            },
            Qt::QueuedConnection);
}

void BrowserCaptchaResolver::notifyConcluded()
{
    QMetaObject::invokeMethod(
            this,
            [this]() {
                if (state != State::Verifying)
                    return;
                state = State::Idle;
                currentCb = nullptr;
                stopTtl();
                teardownServer();
                closeModal();
                if (!queue.empty())
                    startNext();
            },
            Qt::QueuedConnection);
}

void BrowserCaptchaResolver::startNext()
{
    if (state != State::Idle || queue.empty())
        return;

    PendingJob job = std::move(queue.front());
    queue.pop_front();

    current = job.challenge;
    currentProxy = job.proxy;
    currentCb = job.cb;

    // hcaptcha only for now
    if (current.service != QLatin1String("hcaptcha")) {
        qCWarning(LogUI) << "Unsupported captcha service:" << current.service;
        Discord::CaptchaResolver::Callback cb = std::move(currentCb);
        currentCb = nullptr;
        if (cb)
            cb(std::nullopt);
        startNext();
        return;
    }

    QDialog *dlg = new QDialog(parentWindow);
    modal = dlg;
    dlg->setWindowTitle(tr("Verification required"));
    dlg->setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(dlg);
    statusLabel = new QLabel(dlg);
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(statusLabel);

    openButton = new QPushButton(tr("Open in browser"), dlg);
    layout->addWidget(openButton);
    connect(openButton, &QPushButton::clicked, this, [this]() {
        if (!confirmBrowserBypassesProxy())
            return;

        QDesktopServices::openUrl(QUrl(harnessUrl()));
        if (statusLabel)
            statusLabel->setText(tr("Waiting for you to finish in your browser..."));
    });

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(dlg, &QDialog::rejected, this, [this]() { cancelCurrent(); });

    dlg->show();

    beginChallenge(false);
}

bool BrowserCaptchaResolver::confirmBrowserBypassesProxy()
{
    if (!currentProxy.enabled())
        return true;

    ConfirmPopup dialog(tr("Proxy will be bypassed"),
                        tr("This account connects through <b>%1</b>, but the captcha opens in your "
                           "normal browser, which ignores that proxy.<br><br>"
                           "The captcha provider will see your real IP address. Continue?")
                                .arg(currentProxy.toDisplayString().toHtmlEscaped()),
                        tr("Open Anyway"),
                        modal ? static_cast<QWidget *>(modal.data()) : parentWindow);

    return dialog.exec() == QDialog::Accepted;
}

void BrowserCaptchaResolver::beginChallenge(bool isRetry)
{
    teardownServer();

    state = State::Waiting;
    nonce = QUuid::createUuid().toString(QUuid::WithoutBraces) + QString::number(QRandomGenerator::global()->generate64(), 16);

    server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(LogUI) << "Failed to start captcha loopback server:" << server->errorString();
        cancelCurrent();
        return;
    }
    port = server->serverPort();
    connect(server, &QTcpServer::newConnection, this, &BrowserCaptchaResolver::onNewConnection);

    if (statusLabel) {
        if (isRetry)
            statusLabel->setText(tr("Failed. Click below to open your browser and try again."));
        else
            statusLabel->setText(tr("Discord wants you to complete a captcha to continue.\n"
                                    "Click below to open it in your browser."));
    }
    if (openButton)
        openButton->setEnabled(true);

    stopTtl();
    ttlTimer = new QTimer(this);
    ttlTimer->setSingleShot(true);
    connect(ttlTimer, &QTimer::timeout, this, [this]() {
        qCWarning(LogUI) << "Captcha timed out";
        cancelCurrent();
    });
    ttlTimer->start(120000);
}

void BrowserCaptchaResolver::deliverSolution(const Discord::CaptchaSolution &solution)
{
    if (state != State::Waiting)
        return;

    state = State::Verifying;

    teardownServer();
    stopTtl();

    if (statusLabel)
        statusLabel->setText(tr("Verifying..."));
    if (openButton)
        openButton->setEnabled(false);

    Discord::CaptchaResolver::Callback cb = std::move(currentCb);
    currentCb = nullptr;
    if (cb)
        cb(solution);
}

void BrowserCaptchaResolver::cancelCurrent()
{
    if (state == State::Idle)
        return;

    state = State::Idle;

    Discord::CaptchaResolver::Callback cb = std::move(currentCb);
    currentCb = nullptr;

    stopTtl();
    teardownServer();
    closeModal();

    if (cb)
        cb(std::nullopt);

    if (!queue.empty())
        startNext();
}

void BrowserCaptchaResolver::onNewConnection()
{
    while (server && server->hasPendingConnections()) {
        QTcpSocket *socket = server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleSocket(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void BrowserCaptchaResolver::handleSocket(QTcpSocket *socket)
{
    QByteArray &buf = buffers[socket];
    buf += socket->readAll();

    // juuuust in case
    if (buf.size() > 64 * 1024) {
        buffers.remove(socket);
        sendHttp(socket, 400, "text/plain", "too large");
        return;
    }

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return; // headers not complete yet

    const QByteArray header = buf.left(headerEnd);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty())
        return;

    const QList<QByteArray> reqLine = lines.first().trimmed().split(' ');
    if (reqLine.size() < 2) {
        buffers.remove(socket);
        sendHttp(socket, 400, "text/plain", "bad request");
        return;
    }
    const QByteArray method = reqLine[0];
    const QByteArray path = reqLine[1];

    int contentLength = 0;
    QByteArray origin;
    for (const QByteArray &line : lines) {
        const QByteArray l = line.trimmed();
        if (l.toLower().startsWith("content-length:"))
            contentLength = l.mid(15).trimmed().toInt();
        else if (l.toLower().startsWith("origin:"))
            origin = l.mid(7).trimmed();
    }

    const int bodyStart = headerEnd + 4;
    if (buf.size() - bodyStart < contentLength)
        return; // body not fully received yet

    const QByteArray body = buf.mid(bodyStart, contentLength);
    buffers.remove(socket);

    const QByteArray expectedPath = "/" + nonce.toUtf8();
    if (nonce.isEmpty() || !path.startsWith(expectedPath)) {
        sendHttp(socket, 404, "text/plain", "not found");
        return;
    }

    if (method == "GET" && path == expectedPath) {
        sendHttp(socket, 200, "text/html; charset=utf-8", buildHarnessHtml());
        return;
    }

    if (method == "POST" && path == expectedPath + "/token") {
        const QByteArray expectedOrigin = "http://127.0.0.1:" + QByteArray::number(port);
        if (!origin.isEmpty() && origin != expectedOrigin) {
            sendHttp(socket, 403, "text/plain", "bad origin");
            return;
        }

        sendHttp(socket, 200, "application/json", "{\"ok\":true}");

        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const QString token = obj.value("token").toString();
        if (token.isEmpty()) {
            qCWarning(LogUI) << "Captcha harness reported an error:"
                             << obj.value("error").toString();
            return;
        }

        Discord::CaptchaSolution sol;
        sol.token = token;
        sol.sessionId = current.sessionId;
        sol.rqtoken = current.rqtoken;
        deliverSolution(sol);
        return;
    }

    sendHttp(socket, 404, "text/plain", "not found");
}

void BrowserCaptchaResolver::teardownServer()
{
    buffers.clear();
    if (server) {
        server->close();
        server->deleteLater();
        server = nullptr;
    }
    port = 0;
    nonce.clear();
}

void BrowserCaptchaResolver::stopTtl()
{
    if (ttlTimer) {
        ttlTimer->stop();
        ttlTimer->deleteLater();
        ttlTimer = nullptr;
    }
}

void BrowserCaptchaResolver::closeModal()
{
    statusLabel = nullptr;
    openButton = nullptr;
    if (modal) {
        modal->close();
        modal->deleteLater();
        modal = nullptr;
    }
}

QString BrowserCaptchaResolver::harnessUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1/%2").arg(port).arg(nonce);
}

QByteArray BrowserCaptchaResolver::buildHarnessHtml() const
{
    QJsonObject cfg;
    cfg["sitekey"] = current.sitekey;
    cfg["rqdata"] = current.rqdata;
    cfg["invisible"] = current.shouldServeInvisible;
    cfg["tokenUrl"] = QStringLiteral("http://127.0.0.1:%1/%2/token").arg(port).arg(nonce);

    const QByteArray cfgJson = QJsonDocument(cfg).toJson(QJsonDocument::Compact);

    QByteArray html = kHarnessTemplate;
    html.replace(QByteArrayLiteral("__CONFIG_JSON__"), cfgJson);
    return html;
}

} // namespace UI
} // namespace Acheron

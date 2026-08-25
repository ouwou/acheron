#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <deque>
#include <optional>

#include "Discord/CaptchaResolver.hpp"

class QDialog;
class QLabel;
class QPushButton;
class QTcpServer;
class QTcpSocket;
class QTimer;
class QWidget;

namespace Acheron {
namespace UI {

class BrowserCaptchaResolver : public QObject, public Discord::CaptchaResolver
{
    Q_OBJECT
public:
    explicit BrowserCaptchaResolver(QWidget *parentWindow, QObject *parent = nullptr);
    ~BrowserCaptchaResolver() override;

    void resolve(const Discord::CaptchaChallenge &challenge,
                 const Core::ProxyConfig &proxy,
                 Discord::CaptchaResolver::Callback cb) override;
    void notifyConcluded() override;

private:
    enum class State {
        Idle, // nothing is happening
        Waiting, // modal open, waiting for a solution
        Verifying, // token delivered, waiting for replay of original request
    };

    struct PendingJob
    {
        Discord::CaptchaChallenge challenge;
        Core::ProxyConfig proxy;
        Discord::CaptchaResolver::Callback cb;
    };

    void startNext();
    void beginChallenge(bool isRetry);
    void deliverSolution(const Discord::CaptchaSolution &solution);
    void cancelCurrent();
    void onNewConnection();
    void handleSocket(QTcpSocket *socket);

    void teardownServer();
    void stopTtl();
    void closeModal();

    QByteArray buildHarnessHtml() const;
    QString harnessUrl() const;
    [[nodiscard]] bool confirmBrowserBypassesProxy();

    QWidget *parentWindow = nullptr;

    std::deque<PendingJob> queue;
    State state = State::Idle;

    QTcpServer *server = nullptr;
    quint16 port = 0;
    QString nonce;
    Discord::CaptchaChallenge current;
    Core::ProxyConfig currentProxy;
    Discord::CaptchaResolver::Callback currentCb;

    QPointer<QDialog> modal;
    QLabel *statusLabel = nullptr;
    QPushButton *openButton = nullptr;
    QTimer *ttlTimer = nullptr;

    QHash<QTcpSocket *, QByteArray> buffers;
};

} // namespace UI
} // namespace Acheron

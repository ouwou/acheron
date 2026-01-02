#pragma once

#include <QObject>

namespace Acheron {
namespace Discord {

struct HttpResponse
{
    int statusCode = 0;
    QByteArray body;
    QString error;
    bool success = false;
};

using HttpCallback = std::function<void(const HttpResponse &)>;

class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(const QString &baseUrl, const QString &token, QObject *parent = nullptr);

    void get(const QString &endpoint, const QUrlQuery &query, HttpCallback callback);

private:
    enum class Method { GET, POST, PUT, PATCH, DELETE_ }; // thanks windows.h for the DELETE macro

private:
    void executeRequest(Method method, const QString &endpoint, const QByteArray &data,
                        HttpCallback callback);

    QString baseUrl;
    QString token;
};

} // namespace Discord
} // namespace Acheron

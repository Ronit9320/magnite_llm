#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QJsonDocument;
class QJsonObject;

// Thin JSON-over-HTTP client for the local Python backend.
// All methods are asynchronous; results come back through std::function callbacks.
// 204 responses yield an empty QJsonDocument.
class BackendClient : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(int statusCode, const QJsonDocument &doc)>;
    using SseEvent = std::function<void(const QJsonObject &eventJson)>;
    using SseDone = std::function<void(int statusCode)>;

    explicit BackendClient(QObject *parent = nullptr);

    void setBaseUrl(const QString &url) { m_baseUrl = url; }
    QString baseUrl() const { return m_baseUrl; }
    bool isReady() const { return !m_baseUrl.isEmpty(); }

    void get(const QString &path, Callback onDone);
    void post(const QString &path, const QJsonObject &body, Callback onDone);
    void remove(const QString &path, Callback onDone);

    // Streaming POST that parses Server-Sent Events ("data: {...}" blocks).
    void streamPost(const QString &path, const QJsonObject &body,
                    SseEvent onEvent, SseDone onDone);

    // Stream an export into an atomic save file; an error leaves any existing file intact.
    void download(const QString &path, const QString &destination,
                  std::function<void(const QString &error)> onDone);

    void abortAll();
    void abortStream();

private:
    void send(const QString &method, const QString &path,
              const QJsonObject &body, Callback onDone);

    QNetworkAccessManager *m_nam;
    QString m_baseUrl;
    QNetworkReply *m_streamReply = nullptr;
};
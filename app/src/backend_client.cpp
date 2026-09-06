#include "backend_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

BackendClient::BackendClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void BackendClient::abortAll() {
    for (auto *reply : m_nam->findChildren<QNetworkReply*>()) {
        reply->abort();
    }
}

void BackendClient::abortStream() {
    if (m_streamReply) {
        m_streamReply->abort();
        m_streamReply = nullptr;
    }
}

void BackendClient::get(const QString &path, Callback onDone) {
    send("GET", path, QJsonObject(), onDone);
}

void BackendClient::post(const QString &path, const QJsonObject &body, Callback onDone) {
    send("POST", path, body, onDone);
}

void BackendClient::remove(const QString &path, Callback onDone) {
    send("DELETE", path, QJsonObject(), onDone);
}

void BackendClient::send(const QString &method, const QString &path,
                         const QJsonObject &body, Callback onDone) {
    QNetworkRequest req(QUrl(m_baseUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(60000);

    QNetworkReply *reply = nullptr;
    if (method == "GET") {
        reply = m_nam->get(req);
    } else if (method == "POST") {
        reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    } else if (method == "DELETE") {
        reply = m_nam->deleteResource(req);
    } else {
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [reply, onDone]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray data = reply->readAll();
        QJsonDocument doc;
        if (!data.isEmpty()) {
            QJsonParseError err;
            doc = QJsonDocument::fromJson(data, &err);
            if (err.error != QJsonParseError::NoError) {
                doc = QJsonDocument(QJsonObject{{"error", QString::fromUtf8(data)}});
            }
        }
        if (onDone) {
            onDone(status, doc);
        }
    });
}

void BackendClient::streamPost(const QString &path, const QJsonObject &body,
                               SseEvent onEvent, SseDone onDone) {
    QNetworkRequest req(QUrl(m_baseUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(600000);

    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_streamReply = reply;

    auto *buf = new QByteArray;
    const auto flushEvents = [buf, onEvent]() {
        while (true) {
            const int sep = buf->indexOf("\n\n");
            if (sep < 0) break;
            const QByteArray rawEvent = buf->left(sep);
            buf->remove(0, sep + 2);
            QString data;
            for (const QByteArray &line : rawEvent.split('\n')) {
                if (line.startsWith("data:")) {
                    const QByteArray payload = line.mid(5).trimmed();
                    if (!data.isEmpty()) data += '\n';
                    data += QString::fromUtf8(payload);
                }
            }
            if (data.isEmpty()) continue;
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject() && onEvent)
                onEvent(doc.object());
        }
    };

    connect(reply, &QNetworkReply::readyRead, this, [reply, buf, flushEvents]() {
        buf->append(reply->readAll());
        flushEvents();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, buf, flushEvents, onDone]() {
        buf->append(reply->readAll());
        flushEvents();
        delete buf;
        if (m_streamReply == reply) m_streamReply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (onDone) onDone(status);
    });
}
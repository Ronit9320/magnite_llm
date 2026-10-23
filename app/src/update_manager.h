#pragma once

#include <QObject>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <functional>
#include <memory>

class QNetworkReply;
class QSaveFile;
class QCryptographicHash;

class UpdateManager : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Checking, Available, Downloading, Ready, Installing, Failed };
    explicit UpdateManager(QObject *parent = nullptr);
    ~UpdateManager() override;
    State state() const { return m_state; }
    QString status() const { return m_status; }
    QString availableVersion() const { return m_manifest.value("version").toString(); }
    bool canInstall() const;
    static bool verifyManifest(const QByteArray &payload, const QByteArray &signature,
                               const QByteArray &publicKey, const QString &repository,
                               QJsonObject *result, QString *error);

public slots:
    void check();
    void download();
    void cancel();
    // Called only after the desktop has quiesced and stopped the backend.
    void install();

signals:
    void changed();
    void installRequested();
    void restartReady();
    void installFailed(const QString &error);

private:
    void setState(State state, const QString &message);
    void fetchSmall(const QUrl &url, qsizetype limit,
                    std::function<void(QByteArray, QString)> done);
    void fetchManifest(const QJsonObject &release);
    QString imagePath() const;

    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    State m_state = Idle;
    QString m_status;
    QJsonObject m_manifest;
    std::unique_ptr<QTemporaryDir> m_staging;
    std::unique_ptr<QSaveFile> m_output;
    std::unique_ptr<QCryptographicHash> m_hash;
    qint64 m_received = 0;
    QString m_downloadError;
    bool m_cancelled = false;
};

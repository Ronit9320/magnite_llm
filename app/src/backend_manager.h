#pragma once

#include <QObject>
#include <QProcess>
#include <QNetworkAccessManager>

class BackendManager : public QObject {
    Q_OBJECT

public:
    explicit BackendManager(QObject *parent = nullptr);
    ~BackendManager() override;

    void start();
    void stop();
    bool stopForUpdate();
    bool isRunning() const;
    QString baseUrl() const;

signals:
    void started();
    void errorOccurred(QProcess::ProcessError error);

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void launchBackend();
    void processLine(const QString &line);
    void waitForBackend();

    QProcess *m_process;
    quint16 m_port;
    QNetworkAccessManager *m_nam;
    QByteArray m_stderrBuffer;
    bool m_started;
    bool m_stopping;
};

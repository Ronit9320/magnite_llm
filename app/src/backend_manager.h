#pragma once

#include <QObject>
#include <QProcess>

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
    // Per-launch secret the backend requires on every request (adr/0009).
    QString apiToken() const { return m_apiToken; }

signals:
    void started();
    void errorOccurred(QProcess::ProcessError error);
    // Another Magnite backend holds the data folder (backend exit code 75).
    void dataDirInUse();

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void launchBackend();
    void processLine(const QString &line);
    void waitForBackend();
    void checkHealth();

    QProcess *m_process;
    quint16 m_port;
    QByteArray m_stderrBuffer;
    QString m_apiToken;
    bool m_started;
    bool m_stopping;
};

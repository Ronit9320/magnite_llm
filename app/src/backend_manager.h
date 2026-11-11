#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>

#include "restart_policy.h"

class BackendManager : public QObject {
    Q_OBJECT

public:
    explicit BackendManager(QObject *parent = nullptr);
    ~BackendManager() override;

    void start();
    void stop();
    bool stopForUpdate();
    bool isRunning() const;
    // A backend that dies on its own is relaunched with backoff (off for self-tests, which must fail loudly).
    void setAutoRestart(bool enabled) { m_autoRestart = enabled; }
    QString baseUrl() const;
    QString recentLogs() const;
    // Per-launch secret the backend requires on every request (adr/0009).
    QString apiToken() const { return m_apiToken; }

signals:
    void started();
    void errorOccurred(QProcess::ProcessError error);
    // Another Magnite backend holds the data folder (backend exit code 75).
    void dataDirInUse();
    // The backend died unexpectedly; `attempt` (1-based) will be relaunched in `delayMs`.
    void restarting(int attempt, int delayMs);

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void launchBackend();
    void requestStop();
#ifdef Q_OS_WIN
    bool attachProcessJob();
    void closeProcessJob();
    void *m_processJob = nullptr;
#endif
    void processLine(const QString &line);
    void waitForBackend();
    void checkHealth();

    QProcess *m_process;
    quint16 m_port;
    QByteArray m_stderrBuffer;
    QByteArray m_recentLogs;
    QString m_apiToken;
    bool m_started;
    bool m_stopping;
    bool m_autoRestart = true;
    RestartPolicy m_policy;
    QTimer *m_restartTimer;   // fires start() after the backoff delay
    QTimer *m_healthyTimer;   // a backend healthy this long earns back its restart budget
};

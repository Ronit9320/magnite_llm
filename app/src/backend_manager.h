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
    void checkHealth();

    QProcess *m_process;
    quint16 m_port;
    QByteArray m_stderrBuffer;
    bool m_started;
    bool m_stopping;
};

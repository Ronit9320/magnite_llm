#include "backend_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

BackendManager::BackendManager(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_port(0)
    , m_started(false)
    , m_stopping(false)
{
    connect(m_process, &QProcess::readyReadStandardError, this, &BackendManager::onProcessReadyRead);
    connect(m_process, &QProcess::finished, this, &BackendManager::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_stopping && error == QProcess::FailedToStart) emit errorOccurred(error);
    });
}

BackendManager::~BackendManager() {
    // Never emit signals from within a destructor. Disconnect the process so a
    // graceful terminate during teardown cannot re-enter a slot and emit
    // `errorOccurred` while the parent (MainWindow) is being destroyed.
    m_process->disconnect(this);
    m_process->blockSignals(true);
    stop();
#ifdef Q_OS_WIN
    closeProcessJob();
#endif
}

void BackendManager::start() {
    m_stopping = false;
    m_started = false;
    m_port = 0;
    m_stderrBuffer.clear();
    launchBackend();
}

void BackendManager::stop() {
    m_stopping = true;
    if (m_process->state() != QProcess::NotRunning) {
        requestStop();
        if (!m_process->waitForFinished(15000)) {
            m_process->kill();
            m_process->waitForFinished(3000);
        }
    }
}

bool BackendManager::isRunning() const {
    return m_process->state() == QProcess::Running;
}

bool BackendManager::stopForUpdate() {
    m_stopping = true;
    if (m_process->state() == QProcess::NotRunning) return true;
    requestStop();
    // Update installation must never force-kill a live ML operation.
    if (m_process->waitForFinished(15000)) return true;
    m_stopping = false;
    return false;
}

QString BackendManager::baseUrl() const {
    return QString("http://127.0.0.1:%1").arg(m_port);
}

void BackendManager::launchBackend() {
    const QString appDir = QCoreApplication::applicationDirPath();
    QString runtimeDir = appDir + "/runtime";
#ifdef Q_OS_MAC
    const QString bundleRuntime = QDir::cleanPath(appDir + "/../Resources/runtime");
    if (QDir(bundleRuntime).exists()) runtimeDir = bundleRuntime;
#endif
    const bool packaged = QDir(runtimeDir).exists();
    QStringList args = {"-m", "backend.main", "--port", "0"};
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QSettings settings;
    const QString dataDir = settings.value("storage/dataDir").toString().trimmed();
    if (!env.contains("MAGNITE_HOME") && !dataDir.isEmpty()) env.insert("MAGNITE_HOME", dataDir);
    env.insert("MAGNITE_ENGINE_OVERRIDE", QString::number(qBound(0, settings.value("engine/override", 0).toInt(), 2)));
    QString python;
    if (packaged) {
#ifdef Q_OS_WIN
        python = runtimeDir + "/python/python.exe";
#else
        python = runtimeDir + "/python/bin/python3";
#ifndef Q_OS_MAC
        QString libraryPath = QDir(appDir + "/../lib").absolutePath();
        const QString existingLibraries = env.value("LD_LIBRARY_PATH");
        if (!existingLibraries.isEmpty()) libraryPath += ":" + existingLibraries;
        env.insert("LD_LIBRARY_PATH", libraryPath);
#endif
#endif
        args.prepend("-I");
        args.prepend("-B");
        env.remove("PYTHONPATH");
        env.remove("PYTHONHOME");
        env.insert("MAGNITE_PACKAGED", "1");
    } else {
        // Multi-config Windows builds place the binary in build/Release. Require
        // backend/src as the repository marker; a nearby README is insufficient.
        QDir directory(appDir);
        QString repoRoot;
        for (int depth = 0; depth < 8; ++depth) {
            if (QDir(directory.filePath("backend/src/backend")).exists()) {
                repoRoot = directory.canonicalPath();
                break;
            }
            if (!directory.cdUp()) break;
        }
        if (!repoRoot.isEmpty()) {
            const QString modulePath = repoRoot + "/backend/src";
            const QString previous = env.value("PYTHONPATH");
            env.insert("PYTHONPATH", modulePath + (previous.isEmpty() ? QString() : QDir::listSeparator() + previous));
        }
#ifdef Q_OS_WIN
        const QStringList candidates = {repoRoot + "/.venv312/Scripts/python.exe",
                                        repoRoot + "/.venv/Scripts/python.exe"};
        for (const auto &candidate : candidates) {
            if (repoRoot.isEmpty() || !QFileInfo::exists(candidate)) continue;
            QProcess probe;
            probe.start(candidate, {"-I", "-c", "import sys; assert sys.version_info >= (3, 10)"});
            if (probe.waitForFinished(5000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0) {
                python = candidate;
                break;
            }
            probe.kill();
            probe.waitForFinished(1000);
        }
        if (python.isEmpty()) python = "python";
#else
        const QString venv = repoRoot + "/.venv/bin/python";
        python = !repoRoot.isEmpty() && QFileInfo::exists(venv) ? venv : QString("python3");
#endif
    }

    // A fresh secret per launch, so only this client can drive the backend (adr/0009). It goes
    // in the environment, never on the command line, which other users can read with ps.
    quint32 secret[8];
    QRandomGenerator::system()->fillRange(secret);
    m_apiToken = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(secret), sizeof secret).toHex());
    env.insert("MAGNITE_API_TOKEN", m_apiToken);

    m_process->setProcessEnvironment(env);
    m_process->setWorkingDirectory(appDir);
    m_process->start(python, args);

    if (!m_process->waitForStarted(5000) && !packaged) {
#ifdef Q_OS_WIN
        m_process->start("py", args);
#else
        if (python != "python3") m_process->start("python3", args);
#endif
        m_process->waitForStarted(5000);
    }
#ifdef Q_OS_WIN
    if (m_process->state() == QProcess::Running && !attachProcessJob()) {
        qWarning() << "Cannot supervise backend process tree:" << GetLastError();
        m_process->kill();
        m_process->waitForFinished(3000);
    }
#endif
}

void BackendManager::onProcessReadyRead() {
    QByteArray chunk = m_process->readAllStandardError();
    m_stderrBuffer += chunk;
    fprintf(stderr, "%s", chunk.constData());
    fflush(stderr);

    // Process complete lines (line-based parsing must be resilient to chunks
    // arriving split across multiple readyRead signals).
    int idx;
    while ((idx = m_stderrBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_stderrBuffer.left(idx);
        m_stderrBuffer.remove(0, idx + 1);
        processLine(QString::fromUtf8(line).trimmed());
    }
}

void BackendManager::processLine(const QString &line) {
    if (line.contains("starting on http://127.0.0.1:")) {
        QRegularExpression re(R"(http://127\.0\.0\.1:(\d+))");
        QRegularExpressionMatch match = re.match(line);
        if (match.hasMatch()) {
            m_port = match.captured(1).toUShort();
            waitForBackend();
        }
    }
}

void BackendManager::waitForBackend() {
    if (m_started || !isRunning()) {
        return;
    }
    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        timer->deleteLater();
        if (m_started || !isRunning()) {
            return;
        }
        checkHealth();
    });
    timer->start(500);
}

// Probes /health over a raw loopback socket. QNetworkAccessManager never sent
// this request on macOS (the app's GET stalled before reaching the backend),
// so readiness no longer depends on it.
void BackendManager::checkHealth() {
    auto *socket = new QTcpSocket(this);
    auto response = std::make_shared<QByteArray>();
    auto settled = std::make_shared<bool>(false);
    auto settle = [this, socket, settled](bool ok) {
        if (*settled) {
            return;
        }
        *settled = true;
        socket->abort();
        socket->deleteLater();
        if (m_started) {
            return;
        }
        if (ok) {
            m_started = true;
            fprintf(stderr, "[magnite] backend health OK (%s)\n", qUtf8Printable(baseUrl()));
            emit started();
        } else if (isRunning()) {
            qWarning() << "Backend health check failed - retrying";
            QTimer::singleShot(500, this, &BackendManager::waitForBackend);
        }
    };

    connect(socket, &QTcpSocket::connected, socket, [socket]() {
        socket->write("GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    });
    connect(socket, &QTcpSocket::readyRead, socket, [socket, response, settle]() {
        response->append(socket->readAll());
        if (response->size() > 65536) settle(false);
    });
    connect(socket, &QTcpSocket::disconnected, socket, [response, settle]() {
        const auto bodyOffset = response->indexOf("\r\n\r\n");
        const auto health = QJsonDocument::fromJson(response->mid(bodyOffset + 4)).object();
        settle(response->startsWith("HTTP/1.1 200 ") && bodyOffset >= 0
               && health.value("version").toString() == QCoreApplication::applicationVersion());
    });
    connect(socket, &QTcpSocket::errorOccurred, socket,
            [settle](QAbstractSocket::SocketError error) {
        // A graceful close also reports RemoteHostClosedError; the disconnected
        // handler decides the result for that case.
        if (error != QAbstractSocket::RemoteHostClosedError) {
            settle(false);
        }
    });
    QTimer::singleShot(3000, socket, [settle]() { settle(false); });

    socket->connectToHost(QStringLiteral("127.0.0.1"), m_port);
}

void BackendManager::onProcessFinished(int exitCode, QProcess::ExitStatus) {
#ifdef Q_OS_WIN
    closeProcessJob();
#endif
    m_started = false;
    m_port = 0;
    // Ignore failures caused by our own deliberate termination.
    if (m_stopping)
        return;
    constexpr int kExitDataDirInUse = 75;   // backend/local_access.py EXIT_DATA_DIR_IN_USE
    if (exitCode == kExitDataDirInUse) {
        emit dataDirInUse();
        return;
    }
    emit errorOccurred(QProcess::Crashed);
}

void BackendManager::requestStop() {
#ifdef Q_OS_WIN
    // QProcess::terminate sends WM_CLOSE on Windows, which console Python does
    // not handle. Ask the authenticated local server to drain and exit instead.
    if (m_port && !m_apiToken.isEmpty()) {
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), m_port);
        if (socket.waitForConnected(2000)) {
            socket.write("POST /_desktop/shutdown HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                         + m_apiToken.toUtf8() + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket.waitForBytesWritten(2000);
            socket.waitForReadyRead(2000);
        }
    }
#else
    m_process->terminate();
#endif
}

#ifdef Q_OS_WIN
bool BackendManager::attachProcessJob() {
    closeProcessJob();
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(m_process->processId()));
    const bool ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))
                    && child && AssignProcessToJobObject(job, child);
    if (child) CloseHandle(child);
    if (!ok) { CloseHandle(job); return false; }
    m_processJob = job;
    return true;
}

void BackendManager::closeProcessJob() {
    if (m_processJob) { CloseHandle(m_processJob); m_processJob = nullptr; }
}
#endif

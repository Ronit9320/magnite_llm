#include "backend_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QSettings>

#include <memory>

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
        m_process->terminate();
        if (!m_process->waitForFinished(15000)) {
            m_process->kill();
            m_process->waitForFinished(3000);
        }
    }
}

bool BackendManager::isRunning() const {
    return m_process->state() == QProcess::Running;
}

QString BackendManager::baseUrl() const {
    return QString("http://127.0.0.1:%1").arg(m_port);
}

void BackendManager::launchBackend() {
    QString appDir = QCoreApplication::applicationDirPath();
    QString repoRoot = QDir(appDir + "/../").canonicalPath();

    // Resolve paths in dev layout: <repo>/build/ -> <repo>/backend/src, <repo>/.venv
    QString modulePath;
    QDir devBackend(repoRoot + "/backend/src");
    if (devBackend.exists()) {
        modulePath = devBackend.absolutePath();
    } else {
        // Fall back to the location of this binary: <repo>/build/../backend/src
        QDir inlineBackend(QDir(appDir).absoluteFilePath("../backend/src"));
        if (inlineBackend.exists()) {
            modulePath = inlineBackend.absolutePath();
        }
    }

    QStringList args;
    args << "-m" << "backend.main" << "--port" << "0";

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QSettings settings;
    const QString dataDir = settings.value("storage/dataDir").toString().trimmed();
    if (!env.contains("MAGNITE_HOME") && !dataDir.isEmpty()) env.insert("MAGNITE_HOME", dataDir);
    env.insert("MAGNITE_ENGINE_OVERRIDE", QString::number(qBound(0, settings.value("engine/override", 0).toInt(), 2)));
    if (!modulePath.isEmpty()) {
        QString existing = env.value("PYTHONPATH");
        QString merged = modulePath;
        if (!existing.isEmpty()) {
            merged += QDir::listSeparator() + existing;
        }
        env.insert("PYTHONPATH", merged);
    }

    // Prefer the repo venv python if present
    QString venvPython = repoRoot + "/.venv/bin/python";
#ifdef Q_OS_WIN
    venvPython = repoRoot + "/.venv/Scripts/python.exe";
#endif
    QString python = QFileInfo::exists(venvPython) ? venvPython : QString("python3");

    // Test packages carry a relocatable Python installation beside the client.
    // Use isolated mode so a user's PYTHONPATH/site-packages cannot replace the
    // packaged backend. Never silently fall back when a package is incomplete.
    QString runtimeDir = appDir + "/runtime";
#ifdef Q_OS_MAC
    // A macOS .app keeps the runtime in Contents/Resources, beside Contents/MacOS.
    const QString bundleRuntimeDir = QDir::cleanPath(appDir + "/../Resources/runtime");
    if (QDir(bundleRuntimeDir).exists()) runtimeDir = bundleRuntimeDir;
#endif
    const bool packaged = QDir(runtimeDir).exists();
    if (packaged) {
#ifdef Q_OS_WIN
        python = runtimeDir + "/python/python.exe";
#else
        python = runtimeDir + "/python/bin/python3";
#ifndef Q_OS_MAC
        // Native Python extensions also need the bundled C++/OpenMP libraries.
        // (macOS wheels resolve their dylibs through @loader_path instead.)
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

    if (!m_process->waitForStarted(5000) && !packaged && python != "python3") {
        m_process->start("python3", args);
        m_process->waitForStarted(5000);
    }
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
    connect(socket, &QTcpSocket::readyRead, socket, [socket, response]() {
        response->append(socket->readAll());
    });
    connect(socket, &QTcpSocket::disconnected, socket, [response, settle]() {
        settle(response->startsWith("HTTP/1.1 200"));
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

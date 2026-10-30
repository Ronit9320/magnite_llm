#include "backend_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QSettings>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

BackendManager::BackendManager(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_port(0)
    , m_nam(new QNetworkAccessManager(this))
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

bool BackendManager::stopForUpdate() {
    m_stopping = true;
    if (m_process->state() == QProcess::NotRunning) return true;
    m_process->terminate();
    // Update installation must never force-kill a live ML operation.
    if (m_process->waitForFinished(15000)) return true;
    m_stopping = false;
    return false;
}

QString BackendManager::baseUrl() const {
    return QString("http://127.0.0.1:%1").arg(m_port);
}

void BackendManager::launchBackend() {
    QString appDir = QCoreApplication::applicationDirPath();
    QString repoRoot;

    // Try to find repo root by walking up from appDir looking for markers
    QDir cur(appDir);
    for (int i = 0; i < 8; ++i) {
        if (QDir(cur.absolutePath() + "/backend/src").exists() || 
            QFileInfo::exists(cur.absolutePath() + "/app/CMakeLists.txt") ||
            QFileInfo::exists(cur.absolutePath() + "/README.md")) {
            repoRoot = cur.canonicalPath();
            break;
        }
        if (!cur.cdUp()) break;
    }
    if (repoRoot.isEmpty()) {
        repoRoot = QDir(appDir + "/../").canonicalPath();
    }

    // Resolve paths in dev layout: <repo>/build/ -> <repo>/backend/src, <repo>/.venv
    QString modulePath;
    QDir devBackend(repoRoot + "/backend/src");
    if (devBackend.exists()) {
        modulePath = devBackend.absolutePath();
    } else {
        // Fall back to the location of this binary: look relative to appDir
        QDir inlineBackend(QDir(appDir).absoluteFilePath("../backend/src"));
        if (inlineBackend.exists()) {
            modulePath = inlineBackend.absolutePath();
        } else {
            // Try going up further
            QDir inlineBackend2(QDir(appDir).absoluteFilePath("../../backend/src"));
            if (inlineBackend2.exists()) {
                modulePath = inlineBackend2.absolutePath();
            }
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

#ifdef Q_OS_WIN
    QList<QString> venvPythons = {
        repoRoot + "/.venv312/Scripts/python.exe",
        repoRoot + "/.venv/Scripts/python.exe",
    };
#else
    QList<QString> venvPythons = {
        repoRoot + "/.venv312/bin/python",
        repoRoot + "/.venv/bin/python",
    };
#endif
    QString python;
    for (const QString &vp : venvPythons) {
        if (python.isEmpty()) {
            if (QFileInfo::exists(vp)) python = vp;
        }
        if (!python.isEmpty()) {
            // Skip venvs that cannot run: wrong architecture or empty python.exe
            QProcess probe;
            probe.setProgram(python);
            QStringList probeArgs; probeArgs << "-c" << "print('ok')";
            probe.start(probe.program(), probeArgs);
            if (probe.waitForFinished(8000) &&
                probe.exitStatus() == QProcess::NormalExit &&
                probe.exitCode() == 0) {
                break;
            }
            probe.kill();
            python.clear();
        }
    }
    if (python.isEmpty()) {
#ifdef Q_OS_WIN
        // Try common Windows Python executables
        QStringList winCandidates;
        winCandidates << repoRoot + "/.venv/Scripts/python.exe";
        winCandidates << repoRoot + "/venv/Scripts/python.exe";
        winCandidates << "C:/Python313/python.exe";
        winCandidates << "C:/Python312/python.exe";
        winCandidates << "C:/Python311/python.exe";
        winCandidates << "C:/Python310/python.exe";
        bool found = false;
        for (const auto &cand : winCandidates) {
            if (QFileInfo::exists(cand) && cand.endsWith(".exe")) {
                // sanity-check candidate runs (architecture match)
                QProcess probe;
                probe.start(cand, {"-c", "print('ok')"});
                if (probe.waitForFinished(8000) &&
                    probe.exitStatus() == QProcess::NormalExit &&
                    probe.exitCode() == 0) {
                    python = cand;
                    found = true;
                    break;
                }
                probe.kill();
            }
        }
        if (!found) python = "py";
#else
        python = "python3";
#endif
    }

    // Test packages carry a relocatable Python installation beside the client.
    // Use isolated mode so a user's PYTHONPATH/site-packages cannot replace the
    // packaged backend. Never silently fall back when a package is incomplete.
    const QString runtimeDir = appDir + "/runtime";
    const bool packaged = QDir(runtimeDir).exists();
    if (packaged) {
#ifdef Q_OS_WIN
        python = runtimeDir + "/python/python.exe";
#else
        python = runtimeDir + "/python/bin/python3";
        // Native Python extensions also need the bundled C++/OpenMP libraries.
        QString libraryPath = QDir(appDir + "/../lib").absolutePath();
        const QString existingLibraries = env.value("LD_LIBRARY_PATH");
        if (!existingLibraries.isEmpty()) libraryPath += ":" + existingLibraries;
        env.insert("LD_LIBRARY_PATH", libraryPath);
#endif
        args.prepend("-I");
        args.prepend("-B");
        env.remove("PYTHONPATH");
        env.remove("PYTHONHOME");
        env.insert("MAGNITE_PACKAGED", "1");
    }

    m_process->setProcessEnvironment(env);
    m_process->setWorkingDirectory(appDir);
    m_process->start(python, args);

    if (!m_process->waitForStarted(5000) && !packaged) {
#ifdef Q_OS_WIN
        if (python != "python" && python != "py") {
            m_process->start("python", args);
            m_process->waitForStarted(5000);
            if (m_process->state() != QProcess::Running) {
                m_process->start("py", args);
                m_process->waitForStarted(5000);
            }
        } else if (python == "python") {
            // Already tried python? maybe try py as fallback
            if (m_process->state() != QProcess::Running) {
                m_process->start("py", args);
                m_process->waitForStarted(5000);
            }
        }
#else
        if (python != "python3") {
            m_process->start("python3", args);
            m_process->waitForStarted(5000);
        }
#endif
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
        auto *reply = m_nam->get(QNetworkRequest(QUrl(baseUrl() + "/health")));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (m_started) {
                return;
            }
            const auto health = QJsonDocument::fromJson(reply->readAll()).object();
            if (reply->error() == QNetworkReply::NoError
                && health.value("version").toString() == QCoreApplication::applicationVersion()) {
                m_started = true;
                fprintf(stderr, "[magnite] backend health OK (%s)\n",
                        qUtf8Printable(baseUrl()));
                emit started();
            } else if (isRunning()) {
                qWarning() << "Backend health check failed:" << reply->errorString()
                           << "- retrying";
                QTimer::singleShot(500, this, &BackendManager::waitForBackend);
            }
        });
    });
    timer->start(500);
}

void BackendManager::onProcessFinished(int exitCode, QProcess::ExitStatus) {
    m_started = false;
    m_port = 0;
    // Ignore failures caused by our own deliberate termination.
    if (m_stopping)
        return;
    Q_UNUSED(exitCode);
    emit errorOccurred(QProcess::Crashed);
}

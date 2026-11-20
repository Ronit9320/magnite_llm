#include "update_manager.h"
#include "update_config.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStorageInfo>
#include <QTimer>
#include <QVersionNumber>
#include <QtGlobal>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <algorithm>
#ifdef Q_OS_LINUX
#include <gnu/libc-version.h>
#endif

namespace {
QString releaseBase(const QString &repo) { return "https://github.com/" + repo + "/releases/"; }
bool validTag(const QString &tag) {
    static const QRegularExpression pattern("^v[0-9]+\\.[0-9]+(?:\\.[0-9]+)?-preview(?:\\.[0-9]+)?$");
    return pattern.match(tag).hasMatch();
}
QNetworkRequest request(const QUrl &url) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Magnite/" + QCoreApplication::applicationVersion());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    return req;
}
QString manifestName(const QString &platform) { return "update-" + platform + ".json"; }
QString incomingName(const QString &platform) {
    if (platform.startsWith("macos")) return "incoming.zip";
    if (platform.startsWith("windows")) return "incoming-setup.exe";
    return "incoming.AppImage";
}
bool stateIsFailure(const QString &state) {
    return state.startsWith("rolled back") || state.startsWith("recovery required") || state.startsWith("failed");
}
// Copy a bundled installer script out of the client, readable only by this user.
bool stageScript(const QString &resource, const QString &destination) {
    QFile::remove(destination);
    return QFile::copy(resource, destination)
        && QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
} // namespace

QString UpdateManager::currentPlatform() {
#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return "linux-x86_64";
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    return "macos-arm64";
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return "windows-x86_64";
#else
    return {};
#endif
}

QString UpdateManager::packageName(const QString &platform, const QString &version) {
    if (platform == "linux-x86_64") return "Magnite-" + version + "-linux-x86_64.AppImage";
    if (platform == "macos-arm64") return "Magnite-" + version + "-macos-arm64.zip";
    if (platform == "windows-x86_64") return "Magnite-" + version + "-windows-x86_64-setup.exe";
    return {};
}

UpdateManager::UpdateManager(QObject *parent) : QObject(parent) {
    m_status = "Preview channel · " + QCoreApplication::applicationVersion();
    const auto transaction = QSettings().value("updates/lastTransaction").toString();
    if (!transaction.isEmpty()) {
        QFile previousStatus(transaction + "/status");
        if (previousStatus.open(QIODevice::ReadOnly)) {
            const auto previous = QString::fromUtf8(previousStatus.read(1024));
            if (stateIsFailure(previous))
                m_status += "\nLast update: " + previous + ". Details: " + transaction;
        }
    }
}
UpdateManager::~UpdateManager() {
    if (m_reply) { m_reply->disconnect(this); m_reply->abort(); }
}

QString UpdateManager::imagePath() const {
    const QString platform = currentPlatform();
    if (platform == "linux-x86_64") {
        const QFileInfo info(qEnvironmentVariable("APPIMAGE"));
        if (qEnvironmentVariableIsEmpty("APPIMAGE") || info.isSymLink() || !info.isFile()) return {};
        return info.absoluteFilePath();
    }
    if (platform == "macos-arm64") {
        // Packaged layout: <name>.app/Contents/MacOS/Magnite with the runtime in Resources.
        QDir contents(QCoreApplication::applicationDirPath());
        if (!contents.cdUp() || contents.dirName() != "Contents"
            || !QFileInfo::exists(contents.filePath("Resources/runtime/python/bin/python3"))) return {};
        QDir bundle = contents;
        bundle.cdUp();
        const QFileInfo info(bundle.absolutePath());
        // A quarantined app opened in place runs from a read-only App Translocation copy,
        // and an app opened straight from the disk image runs from /Volumes.
        if (!info.fileName().endsWith(".app") || info.isSymLink()
            || info.absoluteFilePath().contains("/AppTranslocation/")
            || info.absoluteFilePath().startsWith("/Volumes/")) return {};
        return info.absoluteFilePath();
    }
    if (platform == "windows-x86_64") {
        // Only an installation made by the per-user installer can be upgraded by it.
        const QDir folder(QCoreApplication::applicationDirPath());
        if (!folder.exists("unins000.exe") || !folder.exists("runtime/python/python.exe")) return {};
        return folder.absolutePath();
    }
    return {};
}

bool UpdateManager::canInstall() const {
    const auto path = imagePath();
    return !path.isEmpty() && QFileInfo(path).isWritable() && QFileInfo(QFileInfo(path).absolutePath()).isWritable();
}

QString UpdateManager::installHint() const {
    const QString platform = currentPlatform();
    if (platform.startsWith("linux")) return " Run a writable Linux AppImage to install updates here.";
    if (platform.startsWith("macos")) {
        const auto app = QCoreApplication::applicationDirPath();
        if (app.contains("/AppTranslocation/") || app.startsWith("/Volumes/"))
            return " Move Magnite Space to your Applications folder, open it from there, then check again.";
        return " This copy cannot replace itself. Move Magnite Space to a folder you can write to, such as Applications.";
    }
    if (platform.startsWith("windows")) return " Install Magnite Space with its Windows installer to receive updates here.";
    return " Automatic installation is not available on this platform.";
}

QString UpdateManager::stagingParent() const {
    // Linux and macOS stage beside the installed copy so the final move is a rename
    // on one filesystem. The Windows installer can run from anywhere.
    if (currentPlatform().startsWith("windows")) return QDir::tempPath();
    return QFileInfo(imagePath()).absolutePath();
}

void UpdateManager::setState(State state, const QString &message) {
    m_state = state; m_status = message; emit changed();
}

bool UpdateManager::verifyManifest(const QByteArray &payload, const QByteArray &signature,
                                  const QByteArray &publicKey, const QString &repository,
                                  QJsonObject *result, QString *error, const QString &platform) {
    auto reject = [error](const QString &reason) { if (error) *error = reason; return false; };
    if (payload.size() > 16384 || signature.size() != 64) return reject("Invalid update metadata size.");
    BIO *bio = BIO_new_mem_buf(publicKey.constData(), static_cast<int>(publicKey.size()));
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr) : nullptr;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool verified = key && EVP_PKEY_base_id(key) == EVP_PKEY_ED25519 && ctx
        && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.constData()), signature.size(),
                            reinterpret_cast<const unsigned char *>(payload.constData()), payload.size()) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); BIO_free(bio);
    if (!verified) return reject("Update signature could not be verified. Nothing was installed.");
    const auto doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) return reject("Invalid update metadata.");
    const auto object = doc.object();
    const auto version = object.value("version").toString();
    const auto size = object.value("size").toDouble();
    const auto sequence = object.value("sequence").toDouble();
    const auto target = platform.split('-');
    const QString package = packageName(platform, version);
    if (target.size() != 2 || package.isEmpty()) return reject("Updates are not published for this platform.");
    const QString expectedUrl = releaseBase(repository) + "download/" + version + "/" + package;
    if (object.value("format").toInt() != 1 || !validTag(version)
        || object.value("channel") != "preview" || object.value("platform") != target[0]
        || object.value("architecture") != target[1] || object.value("variant") != "cpu"
        || object.value("url").toString() != expectedUrl
        || object.value("notes_url").toString() != releaseBase(repository) + "tag/" + version
        || size < 12 || size > 4294967296.0 || size != static_cast<qint64>(size)
        || sequence < 1 || sequence > 2147483647.0 || sequence != static_cast<int>(sequence)
        || !QRegularExpression("^[a-f0-9]{64}$").match(object.value("sha256").toString()).hasMatch()
        || !QRegularExpression("^[a-f0-9]{40}$").match(object.value("commit").toString()).hasMatch())
        return reject("Update metadata is incompatible with this application.");
    if (object.value("data_schema").toInt() != MAGNITE_DATA_SCHEMA)
        return reject("This update needs a data migration. Follow its release instructions.");
    // Each platform states the oldest operating system the package runs on.
    if (target[0] == "linux") {
        const auto minimum = object.value("minimum_glibc").toString();
        if (!QRegularExpression("^[0-9]+\\.[0-9]+$").match(minimum).hasMatch()) return reject("Missing Linux compatibility information.");
#ifdef Q_OS_LINUX
        if (QVersionNumber::fromString(QString::fromLatin1(gnu_get_libc_version())) < QVersionNumber::fromString(minimum))
            return reject("This update requires glibc " + minimum + " or newer.");
#endif
    } else if (target[0] == "macos") {
        const auto minimum = object.value("minimum_macos").toString();
        if (!QRegularExpression("^[0-9]+\\.[0-9]+$").match(minimum).hasMatch()) return reject("Missing macOS compatibility information.");
#ifdef Q_OS_MACOS
        const auto host = QOperatingSystemVersion::current();
        if (QVersionNumber(host.majorVersion(), host.minorVersion()) < QVersionNumber::fromString(minimum))
            return reject("This update requires macOS " + minimum + " or newer.");
#endif
    } else if (target[0] == "windows") {
        const auto minimum = object.value("minimum_windows").toString();
        if (!QRegularExpression("^10\\.0\\.[0-9]+$").match(minimum).hasMatch()) return reject("Missing Windows compatibility information.");
#ifdef Q_OS_WIN
        const auto host = QOperatingSystemVersion::current();
        if (QVersionNumber(host.majorVersion(), host.minorVersion(), host.microVersion()) < QVersionNumber::fromString(minimum))
            return reject("This update requires Windows build " + minimum + " or newer.");
#endif
    }
    if (result) *result = object;
    return true;
}

void UpdateManager::fetchSmall(const QUrl &url, qsizetype limit,
                              std::function<void(QByteArray, QString)> done) {
    auto *reply = m_network.get(request(url));
    m_reply = reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes, limit]() {
        bytes->append(reply->readAll());
        if (bytes->size() > limit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, limit, done]() {
        bytes->append(reply->readAll());
        const bool ok = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && bytes->size() <= limit;
        const QString error = ok ? QString() : "Could not check updates. Check your connection and public release access, then retry.";
        if (m_reply == reply) m_reply = nullptr;
        reply->deleteLater();
        done(ok ? *bytes : QByteArray(), error);
    });
}

void UpdateManager::check() {
    if (m_state == Checking || m_state == Downloading || m_state == Installing || m_state == Ready) return;
    m_cancelled = false; m_manifest = {};
    const QString platform = currentPlatform();
    if (platform.isEmpty()) { setState(Idle, "Updates are not published for this platform."); return; }
    setState(Checking, "Checking preview releases…");
    const QString repo = QStringLiteral(MAGNITE_RELEASE_REPOSITORY);
    fetchSmall(QUrl("https://api.github.com/repos/" + repo + "/releases?per_page=100"), 2 * 1024 * 1024,
        [this, platform](QByteArray data, QString error) {
        if (!error.isEmpty()) { setState(Failed, error); return; }
        const auto doc = QJsonDocument::fromJson(data);
        if (!doc.isArray()) { setState(Failed, "Invalid release response."); return; }
        // Sort canonical preview versions, so an older backport published later
        // cannot hide a newer release. /latest excludes prereleases entirely.
        QList<QJsonObject> releases;
        for (const auto &value : doc.array()) releases.append(value.toObject());
        std::sort(releases.begin(), releases.end(), [](const auto &a, const auto &b) {
            const auto left = a.value("tag_name").toString().mid(1).split("-preview");
            const auto right = b.value("tag_name").toString().mid(1).split("-preview");
            const int compared = QVersionNumber::compare(QVersionNumber::fromString(left.value(0)).normalized(),
                                                        QVersionNumber::fromString(right.value(0)).normalized());
            if (compared) return compared > 0;
            return left.value(1).mid(1).toInt() > right.value(1).mid(1).toInt();
        });
        // The newest preview that publishes this platform's signed metadata.
        for (const auto &release : releases) {
            if (!release.value("draft").toBool() && release.value("prerelease").toBool()
                && validTag(release.value("tag_name").toString())) {
                bool metadata = false, signature = false;
                for (const auto &asset : release.value("assets").toArray()) {
                    const auto name = asset.toObject().value("name").toString();
                    metadata |= name == manifestName(platform);
                    signature |= name == manifestName(platform) + ".sig";
                }
                if (metadata && signature) { fetchManifest(release); return; }
            }
        }
        setState(Idle, "No preview has been published for this platform yet.");
    });
}

void UpdateManager::fetchManifest(const QJsonObject &release) {
    const QString repo = QStringLiteral(MAGNITE_RELEASE_REPOSITORY);
    const QString manifestFile = manifestName(currentPlatform());
    const auto tag = release.value("tag_name").toString();
    const auto base = releaseBase(repo) + "download/" + tag + "/";
    bool manifest = false, signature = false;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        const auto name = asset.value("name").toString();
        if (asset.value("browser_download_url").toString() != base + name) continue;
        manifest |= name == manifestFile;
        signature |= name == manifestFile + ".sig";
    }
    if (!manifest || !signature) { setState(Failed, "The preview release is missing signed update metadata."); return; }
    fetchSmall(QUrl(base + manifestFile), 16384,
        [this, base, repo, tag, manifestFile](QByteArray payload, QString error) {
        if (!error.isEmpty()) { setState(Failed, error); return; }
        fetchSmall(QUrl(base + manifestFile + ".sig"), 64,
            [this, payload, repo, tag](QByteArray signatureBytes, QString signatureError) {
            QJsonObject object;
            if (!signatureError.isEmpty()) { setState(Failed, signatureError); return; }
            QString reason;
            if (!verifyManifest(payload, signatureBytes, QByteArray(magniteUpdatePublicKey), repo, &object, &reason)
                || object.value("version").toString() != tag) {
                setState(Failed, reason.isEmpty() ? "Release and manifest versions differ." : reason); return;
            }
            const int current = qMax(MAGNITE_UPDATE_SEQUENCE, QSettings().value("updates/highestSequence", MAGNITE_UPDATE_SEQUENCE).toInt());
            if (object.value("sequence").toInt() <= current) {
                setState(Idle, "You are up to date · " + QCoreApplication::applicationVersion()); return;
            }
            m_manifest = object;
            setState(Available, availableVersion() + " is available." + (canInstall() ? QString() : installHint()));
        });
    });
}

void UpdateManager::download() {
    if (m_state != Available || !canInstall()) return;
    m_cancelled = false;
    const QString platform = currentPlatform();
    const auto parent = stagingParent();
    const auto size = static_cast<qint64>(m_manifest.value("size").toDouble());
    // macOS also unpacks the archive beside the app; Windows' installer unpacks into the app folder.
    const qint64 needed = platform.startsWith("linux") ? size + 64ll * 1024 * 1024 : 3 * size + 256ll * 1024 * 1024;
    if (QStorageInfo(parent).bytesAvailable() < needed) {
        setState(Available, "Free more disk space before downloading the update."); return;
    }
    if (!platform.startsWith("linux")) {
        // Keep one finished transaction (it holds the previous app on macOS); drop older ones.
        const auto previous = QSettings().value("updates/lastTransaction").toString();
        QFile previousStatus(previous + "/status");
        if (!previous.isEmpty() && QFileInfo(previous).fileName().startsWith(".magnite-update-")
            && previousStatus.open(QIODevice::ReadOnly) && previousStatus.read(1024).startsWith("success")) {
            previousStatus.close();
            QDir(previous).removeRecursively();
            QSettings().remove("updates/lastTransaction");
        }
    }
    m_staging = std::make_unique<QTemporaryDir>(parent + "/.magnite-update-XXXXXX");
    if (!m_staging->isValid()) { setState(Failed, "Cannot create an update directory."); return; }
    m_output = std::make_unique<QSaveFile>(m_staging->filePath(incomingName(platform)));
    if (!m_output->open(QIODevice::WriteOnly)) { m_staging.reset(); setState(Failed, "Cannot save the update."); return; }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_received = 0; m_downloadError.clear();
    setState(Downloading, "Downloading " + availableVersion() + "…");
    auto *reply = m_network.get(request(QUrl(m_manifest.value("url").toString())));
    m_reply = reply;
    auto consume = [this, reply, size]() {
        const auto bytes = reply->readAll();
        m_received += bytes.size();
        if (m_received > size || m_output->write(bytes) != bytes.size()) {
            m_downloadError = "The update was too large or could not be saved.";
            reply->abort(); return;
        }
        m_hash->addData(bytes);
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, size](qint64 received, qint64) {
        m_status = QString("Downloading %1 · %2%").arg(availableVersion()).arg(qMin<qint64>(100, received * 100 / size));
        emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, consume, size, platform]() {
        consume();
        const bool valid = !m_cancelled && m_downloadError.isEmpty() && reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && m_received == size && QString::fromLatin1(m_hash->result().toHex()) == m_manifest.value("sha256").toString();
        if (m_reply == reply) m_reply = nullptr;
        reply->deleteLater();
        if (!valid || !m_output->commit()) {
            m_output.reset(); m_staging.reset();
            setState(m_cancelled ? Available : Failed, m_cancelled ? "Download cancelled. You can retry." : "Update download failed verification or could not be saved. Nothing was installed.");
            return;
        }
        m_output.reset();
        QFile package(m_staging->filePath(incomingName(platform)));
        if (!package.open(QIODevice::ReadOnly)) { m_staging.reset(); setState(Failed, "Cannot read the downloaded update."); return; }
        const auto magic = package.read(12);
        package.close();
        const bool expected = platform.startsWith("macos") ? magic.startsWith(QByteArray("PK\x03\x04", 4))
            : platform.startsWith("windows") ? magic.startsWith("MZ")
            : magic.left(4) == QByteArray("\x7f" "ELF", 4) && magic.mid(8, 3) == QByteArray("AI\x02", 3);
        if (!expected) {
            m_staging.reset(); setState(Failed, "The download is not the expected package type. Nothing was installed."); return;
        }
        setState(Ready, availableVersion() + " is ready. Finish active work, then restart to install.");
    });
}

void UpdateManager::cancel() {
    if (m_state == Downloading && m_reply) { m_cancelled = true; m_reply->abort(); }
}

void UpdateManager::install() {
    if (m_state != Ready || !m_staging || !canInstall()) { emit installFailed("The update is no longer ready to install."); return; }
    QFile::remove(m_staging->filePath("status"));
    QFile::remove(m_staging->filePath("cancel"));
    setState(Installing, "Preparing to restart…");
    if (!currentPlatform().startsWith("macos")) { startInstaller(); return; }
    // Unpack the verified archive next to the installed app before the client exits,
    // so a damaged archive never removes the working copy.
    auto *unpack = new QProcess(this);
    const QString destination = m_staging->filePath("new");
    QDir(destination).removeRecursively();
    unpack->setProgram("/usr/bin/ditto");
    unpack->setArguments({"-x", "-k", m_staging->filePath("incoming.zip"), destination});
    connect(unpack, &QProcess::finished, this, [this, unpack, destination](int code, QProcess::ExitStatus status) {
        unpack->deleteLater();
        const QStringList bundles = QDir(destination).entryList({"*.app"}, QDir::Dirs | QDir::NoDotAndDotDot);
        if (status != QProcess::NormalExit || code != 0 || bundles.size() != 1
            || !QFileInfo(destination + "/" + bundles.first() + "/Contents/MacOS/Magnite").isExecutable()) {
            QDir(destination).removeRecursively();
            setState(Failed, "The update archive could not be unpacked. Nothing was installed.");
            emit installFailed(m_status);
            return;
        }
        QFile::remove(m_staging->filePath("incoming.zip"));
        startInstaller();
    });
    unpack->start();
}

void UpdateManager::startInstaller() {
    const QString platform = currentPlatform();
    const QString pid = QString::number(QCoreApplication::applicationPid());
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    QString helper;
    if (platform.startsWith("linux")) {
        helper = m_staging->filePath("magnite-update-helper");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/magnite-update-helper", helper)
            || !QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
            setState(Failed, "Could not prepare the update installer.");
            emit installFailed(m_status); return;
        }
        const bool extractedRun = env.value("APPDIR").contains("/appimage_extracted_");
        for (const auto &key : env.keys()) {
            if (key.startsWith("QT_") || key.startsWith("QML") || key.startsWith("PYTHON")
                || key == "LD_LIBRARY_PATH" || key == "LD_PRELOAD" || key == "APPIMAGE" || key == "APPDIR" || key == "OWD") env.remove(key);
        }
        env.insert("PATH", "/usr/local/bin:/usr/bin:/bin");
        // The runtime consumes --appimage-extract-and-run before our argument list.
        // Preserve that mode for restart when FUSE is unavailable.
        if (extractedRun) env.insert("APPIMAGE_EXTRACT_AND_RUN", "1");
        process.setProgram(helper);
        process.setArguments({imagePath(), m_staging->path(), pid, availableVersion()});
    } else if (platform.startsWith("macos")) {
        helper = m_staging->filePath("install-update.sh");
        const QString bundle = QDir(m_staging->filePath("new")).entryList({"*.app"}, QDir::Dirs | QDir::NoDotAndDotDot).value(0);
        if (bundle.isEmpty() || !stageScript(":/updates/install-macos.sh", helper)) {
            setState(Failed, "Could not prepare the update installer.");
            emit installFailed(m_status); return;
        }
        for (const auto &key : env.keys())
            if (key.startsWith("QT_") || key.startsWith("PYTHON") || key.startsWith("DYLD_")) env.remove(key);
        env.insert("PATH", "/usr/bin:/bin:/usr/sbin:/sbin");
        process.setProgram("/bin/bash");
        process.setArguments({helper, imagePath(), m_staging->filePath("new/" + bundle), m_staging->path(), pid, availableVersion()});
    } else {
        helper = m_staging->filePath("install-update.ps1");
        if (!stageScript(":/updates/install-windows.ps1", helper)) {
            setState(Failed, "Could not prepare the update installer.");
            emit installFailed(m_status); return;
        }
        for (const auto &key : env.keys())
            if (key.startsWith("QT_") || key.startsWith("PYTHON")) env.remove(key);
        // Use the system PowerShell by absolute path, never one found on PATH.
        const QString root = env.value("SystemRoot", "C:\\Windows");
        process.setProgram(root + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        process.setArguments({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
                              "-File", QDir::toNativeSeparators(helper),
                              "-Installer", QDir::toNativeSeparators(m_staging->filePath("incoming-setup.exe")),
                              "-AppDir", QDir::toNativeSeparators(imagePath()),
                              "-Transaction", QDir::toNativeSeparators(m_staging->path()),
                              "-ParentPid", pid, "-Version", availableVersion()});
    }
    process.setProcessEnvironment(env);
    process.setStandardOutputFile(m_staging->filePath("installer.log"));
    process.setStandardErrorFile(m_staging->filePath("installer.log"), QIODevice::Append);
    if (!process.startDetached()) {
        QFile::remove(helper);
        setState(Failed, "Could not start the update installer.");
        emit installFailed(m_status); return;
    }
    awaitInstallerReady(helper);
}

void UpdateManager::awaitInstallerReady(const QString &helper) {
    // The detached installer must survive the client process (and, on Linux, the mounted image).
    auto *timer = new QTimer(this);
    auto attempts = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this, [this, timer, attempts, helper]() {
        QFile status(m_staging->filePath("status"));
        if (status.open(QIODevice::ReadOnly) && status.readAll() == "ready") {
            timer->stop(); timer->deleteLater();
            m_staging->setAutoRemove(false);
            QSettings().setValue("updates/lastTransaction", m_staging->path());
            emit restartReady(); return;
        }
        if (++*attempts >= 100) {
            timer->stop(); timer->deleteLater(); QFile::remove(helper);
            QFile cancelled(m_staging->filePath("cancel"));
            if (cancelled.open(QIODevice::WriteOnly)) cancelled.write("cancel");
            cancelled.close();
            m_staging.reset();
            setState(Failed, "The installer did not become ready. Check for updates and download again to retry.");
            emit installFailed(m_status);
        }
    });
    timer->start(100);
}

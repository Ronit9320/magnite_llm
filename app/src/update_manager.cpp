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
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStorageInfo>
#include <QTimer>
#include <QVersionNumber>
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
}

UpdateManager::UpdateManager(QObject *parent) : QObject(parent) {
    m_status = "Preview channel · " + QCoreApplication::applicationVersion();
    const auto transaction = QSettings().value("updates/lastTransaction").toString();
    if (!transaction.isEmpty()) {
        QFile previousStatus(transaction + "/status");
        if (previousStatus.open(QIODevice::ReadOnly)) {
            const auto previous = QString::fromUtf8(previousStatus.read(1024));
            if (previous.startsWith("rolled back") || previous.startsWith("recovery required") || previous.startsWith("failed"))
                m_status += "\nLast update: " + previous + ". Details: " + transaction;
        }
    }
}
UpdateManager::~UpdateManager() {
    if (m_reply) { m_reply->disconnect(this); m_reply->abort(); }
}

QString UpdateManager::imagePath() const {
#if defined(Q_OS_LINUX) && defined(__x86_64__)
    const QFileInfo info(qEnvironmentVariable("APPIMAGE"));
    if (qEnvironmentVariableIsEmpty("APPIMAGE") || info.isSymLink() || !info.isFile()) return {};
    return info.absoluteFilePath();
#else
    return {};
#endif
}

bool UpdateManager::canInstall() const {
    const auto path = imagePath();
    return !path.isEmpty() && QFileInfo(path).isWritable() && QFileInfo(QFileInfo(path).absolutePath()).isWritable();
}

void UpdateManager::setState(State state, const QString &message) {
    m_state = state; m_status = message; emit changed();
}

bool UpdateManager::verifyManifest(const QByteArray &payload, const QByteArray &signature,
                                  const QByteArray &publicKey, const QString &repository,
                                  QJsonObject *result, QString *error) {
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
    const QString expectedUrl = releaseBase(repository) + "download/" + version + "/Magnite-" + version + "-linux-x86_64.AppImage";
    if (object.value("format").toInt() != 1 || !validTag(version)
        || object.value("channel") != "preview" || object.value("platform") != "linux"
        || object.value("architecture") != "x86_64" || object.value("variant") != "cpu"
        || object.value("url").toString() != expectedUrl
        || object.value("notes_url").toString() != releaseBase(repository) + "tag/" + version
        || size < 12 || size > 2147483648.0 || size != static_cast<qint64>(size)
        || sequence < 1 || sequence > 2147483647.0 || sequence != static_cast<int>(sequence)
        || !QRegularExpression("^[a-f0-9]{64}$").match(object.value("sha256").toString()).hasMatch()
        || !QRegularExpression("^[a-f0-9]{40}$").match(object.value("commit").toString()).hasMatch())
        return reject("Update metadata is incompatible with this application.");
    if (object.value("data_schema").toInt() != MAGNITE_DATA_SCHEMA)
        return reject("This update needs a data migration. Follow its release instructions.");
    const auto minimum = object.value("minimum_glibc").toString();
    if (!QRegularExpression("^[0-9]+\\.[0-9]+$").match(minimum).hasMatch()) return reject("Missing Linux compatibility information.");
#ifdef Q_OS_LINUX
    if (QVersionNumber::fromString(QString::fromLatin1(gnu_get_libc_version())) < QVersionNumber::fromString(minimum))
        return reject("This update requires glibc " + minimum + " or newer.");
#endif
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
    setState(Checking, "Checking preview releases…");
    const QString repo = QStringLiteral(MAGNITE_RELEASE_REPOSITORY);
    fetchSmall(QUrl("https://api.github.com/repos/" + repo + "/releases?per_page=100"), 2 * 1024 * 1024,
        [this](QByteArray data, QString error) {
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
        for (const auto &release : releases) {
            if (!release.value("draft").toBool() && release.value("prerelease").toBool()
                && validTag(release.value("tag_name").toString())) {
                bool metadata = false, signature = false;
                for (const auto &asset : release.value("assets").toArray()) {
                    const auto name = asset.toObject().value("name").toString();
                    metadata |= name == "update-linux-x86_64.json";
                    signature |= name == "update-linux-x86_64.json.sig";
                }
                if (metadata && signature) { fetchManifest(release); return; }
            }
        }
        setState(Idle, "No published Linux preview is available yet.");
    });
}

void UpdateManager::fetchManifest(const QJsonObject &release) {
    const QString repo = QStringLiteral(MAGNITE_RELEASE_REPOSITORY);
    const auto tag = release.value("tag_name").toString();
    const auto base = releaseBase(repo) + "download/" + tag + "/";
    bool manifest = false, signature = false;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        const auto name = asset.value("name").toString();
        if (asset.value("browser_download_url").toString() != base + name) continue;
        manifest |= name == "update-linux-x86_64.json";
        signature |= name == "update-linux-x86_64.json.sig";
    }
    if (!manifest || !signature) { setState(Failed, "The preview release is missing signed update metadata."); return; }
    fetchSmall(QUrl(base + "update-linux-x86_64.json"), 16384,
        [this, base, repo, tag](QByteArray payload, QString error) {
        if (!error.isEmpty()) { setState(Failed, error); return; }
        fetchSmall(QUrl(base + "update-linux-x86_64.json.sig"), 64,
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
            setState(Available, availableVersion() + " is available." + (canInstall() ? QString() : " Run a writable Linux AppImage to install updates here."));
        });
    });
}

void UpdateManager::download() {
    if (m_state != Available || !canInstall()) return;
    m_cancelled = false;
    const auto parent = QFileInfo(imagePath()).absolutePath();
    const auto size = static_cast<qint64>(m_manifest.value("size").toDouble());
    if (QStorageInfo(parent).bytesAvailable() < size + 64 * 1024 * 1024) {
        setState(Available, "Free more disk space before downloading the update."); return;
    }
    m_staging = std::make_unique<QTemporaryDir>(parent + "/.magnite-update-XXXXXX");
    if (!m_staging->isValid()) { setState(Failed, "Cannot create an update directory beside this AppImage."); return; }
    m_output = std::make_unique<QSaveFile>(m_staging->filePath("incoming.AppImage"));
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, consume, size]() {
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
        QFile image(m_staging->filePath("incoming.AppImage"));
        if (!image.open(QIODevice::ReadOnly)) { m_staging.reset(); setState(Failed, "Cannot read the downloaded update."); return; }
        const auto magic = image.read(12);
        if (magic.left(4) != QByteArray("\x7f" "ELF", 4) || magic.mid(8, 3) != QByteArray("AI\x02", 3)) {
            image.close(); m_staging.reset(); setState(Failed, "The download is not a type 2 AppImage."); return;
        }
        setState(Ready, availableVersion() + " is ready. Finish active work, then restart to install.");
    });
}

void UpdateManager::cancel() {
    if (m_state == Downloading && m_reply) { m_cancelled = true; m_reply->abort(); }
}

void UpdateManager::install() {
    if (m_state != Ready || !m_staging || !canInstall()) { emit installFailed("The update is no longer ready to install."); return; }
    const auto helper = m_staging->filePath("magnite-update-helper");
    QFile::remove(m_staging->filePath("status"));
    QFile::remove(m_staging->filePath("cancel"));
    if (!QFile::copy(QCoreApplication::applicationDirPath() + "/magnite-update-helper", helper)
        || !QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        emit installFailed("Could not prepare the update installer."); return;
    }
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    const bool extractedRun = env.value("APPDIR").contains("/appimage_extracted_");
    for (const auto &key : env.keys()) {
        if (key.startsWith("QT_") || key.startsWith("QML") || key.startsWith("PYTHON")
            || key == "LD_LIBRARY_PATH" || key == "LD_PRELOAD" || key == "APPIMAGE" || key == "APPDIR" || key == "OWD") env.remove(key);
    }
    env.insert("PATH", "/usr/local/bin:/usr/bin:/bin");
    // The runtime consumes --appimage-extract-and-run before our argument list.
    // Preserve that mode for restart when FUSE is unavailable.
    if (extractedRun) env.insert("APPIMAGE_EXTRACT_AND_RUN", "1");
    process.setProcessEnvironment(env);
    process.setProgram(helper);
    process.setArguments({imagePath(), m_staging->path(), QString::number(QCoreApplication::applicationPid()), availableVersion()});
    process.setStandardOutputFile(m_staging->filePath("installer.log"));
    process.setStandardErrorFile(m_staging->filePath("installer.log"), QIODevice::Append);
    if (!process.startDetached()) { QFile::remove(helper); emit installFailed("Could not start the update installer."); return; }
    setState(Installing, "Preparing to restart…");
    // Detached helper must survive the mounted image and the client process.
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
        if (++*attempts >= 50) {
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

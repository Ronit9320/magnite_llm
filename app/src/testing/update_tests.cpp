#include "update_manager.h"
#include "update_config.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&failures](bool ok, const char *message) {
        fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };
    EVP_PKEY_CTX *generator = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY *key = nullptr;
    if (!generator || EVP_PKEY_keygen_init(generator) != 1 || EVP_PKEY_keygen(generator, &key) != 1) return 2;
    EVP_PKEY_CTX_free(generator);
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, key);
    char *data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    const QByteArray publicKey(data, length);
    BIO_free(bio);
    auto sign = [key](const QByteArray &payload) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
        QByteArray signature(64, '\0');
        size_t size = 64;
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(signature.data()), &size,
                       reinterpret_cast<const unsigned char *>(payload.constData()), payload.size());
        EVP_MD_CTX_free(ctx);
        return signature;
    };
    const QString version = "v0.1-preview";
    const QString base = "https://github.com/test/magnite/releases/";
    QJsonObject manifest{{"format", 1}, {"version", version}, {"sequence", 1}, {"data_schema", MAGNITE_DATA_SCHEMA},
        {"channel", "preview"}, {"platform", "linux"}, {"architecture", "x86_64"}, {"variant", "cpu"},
        {"size", 100}, {"sha256", QString(64, 'a')}, {"commit", QString(40, 'b')}, {"minimum_glibc", "2.35"},
        {"url", base + "download/" + version + "/Magnite-" + version + "-linux-x86_64.AppImage"},
        {"notes_url", base + "tag/" + version}};
    auto verifyFor = [&](const QJsonObject &object, const QString &platform) {
        const auto payload = QJsonDocument(object).toJson();
        QString error; QJsonObject result;
        return UpdateManager::verifyManifest(payload, sign(payload), publicKey, "test/magnite", &result, &error, platform);
    };
    auto verify = [&](const QJsonObject &object) { return verifyFor(object, "linux-x86_64"); };
    check(verify(manifest), "valid publisher signature and compatible metadata accepted");
    auto payload = QJsonDocument(manifest).toJson();
    auto signature = sign(payload);
    payload[5] = payload[5] ^ 1;
    QString error;
    check(!UpdateManager::verifyManifest(payload, signature, publicKey, "test/magnite", nullptr, &error, "linux-x86_64"), "tampered manifest rejected");
    check(!UpdateManager::verifyManifest(payload, QByteArray(64, 'x'), publicKey, "test/magnite", nullptr, &error, "linux-x86_64"), "untrusted signature rejected");
    for (const auto &change : QList<QPair<QString, QJsonValue>>{
        {"url", "https://attacker.invalid/update"}, {"data_schema", MAGNITE_DATA_SCHEMA + 1}, {"architecture", "aarch64"},
        {"channel", "stable"}, {"variant", "cuda"}, {"size", -1}, {"size", 1e20},
        {"sequence", 0}, {"sequence", 1.5}, {"version", "../evil"}, {"sha256", "bad"},
#ifdef Q_OS_LINUX
        // A numeric host glibc requirement is evaluated only on Linux.
        {"minimum_glibc", "999.0"},
#endif
        {"minimum_glibc", ""}}) {
        auto invalid = manifest; invalid.insert(change.first, change.second);
        check(!verify(invalid), qPrintable("incompatible signed field rejected: " + change.first));
    }
    check(!UpdateManager::verifyManifest(QByteArray(16385, 'x'), signature, publicKey, "test/magnite", nullptr, &error), "oversized metadata rejected");

    // macOS and Windows publish their own signed metadata for their own packages.
    QJsonObject mac = manifest;
    mac.remove("minimum_glibc");
    mac.insert("platform", "macos"); mac.insert("architecture", "arm64"); mac.insert("minimum_macos", "14.0");
    mac.insert("url", base + "download/" + version + "/" + UpdateManager::packageName("macos-arm64", version));
    QJsonObject windows = manifest;
    windows.remove("minimum_glibc");
    windows.insert("platform", "windows"); windows.insert("minimum_windows", "10.0.17763");
    windows.insert("url", base + "download/" + version + "/" + UpdateManager::packageName("windows-x86_64", version));
    check(verifyFor(mac, "macos-arm64"), "signed macOS metadata accepted for macOS");
    check(verifyFor(windows, "windows-x86_64"), "signed Windows metadata accepted for Windows");
    check(!verifyFor(mac, "linux-x86_64") && !verifyFor(manifest, "macos-arm64") && !verifyFor(windows, "macos-arm64")
              && !verifyFor(manifest, "windows-x86_64"),
          "metadata for another platform is rejected");
    check(!verifyFor(manifest, "") && !verifyFor(manifest, "linux-aarch64"), "unsupported platforms are rejected");
    for (const auto &change : QList<QPair<QString, QJsonValue>>{
        {"minimum_macos", ""}, {"minimum_macos", "fourteen"},
        {"url", base + "download/" + version + "/Magnite-" + version + "-arm64.dmg"},
#ifdef Q_OS_MACOS
        {"minimum_macos", "999.0"},
#endif
        {"architecture", "x86_64"}}) {
        auto invalid = mac; invalid.insert(change.first, change.second);
        check(!verifyFor(invalid, "macos-arm64"), qPrintable("incompatible macOS field rejected: " + change.first));
    }
    for (const auto &change : QList<QPair<QString, QJsonValue>>{
        {"minimum_windows", ""}, {"minimum_windows", "6.1"},
#ifdef Q_OS_WIN
        {"minimum_windows", "10.0.999999"},
#endif
        {"url", base + "download/" + version + "/Magnite-" + version + "-windows-x86_64.zip"}}) {
        auto invalid = windows; invalid.insert(change.first, change.second);
        check(!verifyFor(invalid, "windows-x86_64"), qPrintable("incompatible Windows field rejected: " + change.first));
    }
    check(UpdateManager::packageName("linux-x86_64", version) == "Magnite-" + version + "-linux-x86_64.AppImage",
          "Linux package name is unchanged for existing installations");
    EVP_PKEY_free(key);
    return failures ? 1 : 0;
}

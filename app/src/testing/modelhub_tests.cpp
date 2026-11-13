#include "testing/modelhub_tests.h"
#include "panels/modelhubpanel.h"
#include "backend_client.h"
#include "theme.h"

#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <cstdio>

namespace {
void pump(int milliseconds = 70) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()> &predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) pump(20);
    return predicate();
}

class HubFixture : public QTcpServer {
public:
    QStringList requests;
    QString pullState = "running";
    int detailStatus = 200;
    int telemetryStatus = 200;
    const QString thirdRepo = "test/a-long-model-name-with-extra-architecture-and-instruction-metadata-for-comparison-layout";
    QPointer<QTcpSocket> heldDetail;
    bool holdNextDetail = false;

    HubFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", buffer);
                    if (!buffer.contains("\r\n\r\n") || socket->property("responded").toBool()) return;
                    const auto firstLine = buffer.left(buffer.indexOf("\r\n"));
                    const QUrl url("http://fixture" + QString::fromUtf8(firstLine.split(' ').value(1)));
                    const QString path = url.path();
                    requests << path;
                    socket->setProperty("responded", true);
                    int code = 200;
                    QJsonDocument response;
                    if (path == "/training/telemetry") {
                        code = telemetryStatus;
                        response = QJsonDocument(QJsonObject{{"device", "Test GPU"}, {"kind", "gpu"}, {"accelerator", "CUDA"}, {"total_bytes", 16e9}, {"used_bytes", 2e9}, {"cpu_count", 16}, {"cpu_percent", 24}, {"utilization_percent", 15}});
                    } else if (path == "/models/discover") {
                        QJsonArray results;
                        for (int i = 0; i < 3; ++i) results.append(QJsonObject{
                            {"repo_id", i == 2 ? thirdRepo : QString("test/model-%1").arg(i)}, {"downloads", 100 - i}, {"likes", 10},
                            {"pipeline_tag", i == 2 ? "feature-extraction" : "text-generation"},
                            {"license", i == 1 ? "a-very-long-publisher-license-with-additional-terms" : "apache-2.0"}, {"parameters", 7e9}, {"context_length", 32768},
                            {"formats", QJsonArray{"gguf"}}, {"architecture", "qwen2"}, {"languages", QJsonArray{"en", "fr"}}, {"downloaded", i == 0}});
                        response = QJsonDocument(results);
                    } else if (path == "/models/sizes") {
                        response = QJsonDocument(QJsonObject{{"test/model-0", 4e9}, {"test/model-1", 5e9}, {"test/model-2", QJsonValue()}});
                    } else if (path == "/models/hub-detail") {
                        if (holdNextDetail) {
                            holdNextDetail = false;
                            heldDetail = socket;
                            return;
                        }
                        code = detailStatus;
                        const QString repo = QUrlQuery(url).queryItemValue("repo");
                        response = QJsonDocument(QJsonObject{
                            {"repo_id", repo}, {"pipeline_tag", "text-generation"}, {"architecture", "qwen2"}, {"languages", QJsonArray{"en", "fr"}}, {"downloads", 100}, {"parameters", 7e9}, {"context_length", 32768}, {"license", "apache-2.0"},
                            {"downloaded", repo == "test/model-0"}, {"layers", 32}, {"hidden_size", 4096}, {"attention_heads", 32}, {"kv_heads", 8},
                            {"total_bytes", 5e9}, {"file_count", 1}, {"revision", "fixture123"},
                            {"files", QJsonArray{QJsonObject{{"path", "model.Q4_K_M.gguf"}, {"size_bytes", 5e9}}}},
                            {"detail", "Metadata temporarily unavailable"}});
                    } else if (path == "/models/pull") {
                        code = 202;
                        response = QJsonDocument(QJsonObject{{"id", "hub-job"}});
                    } else if (path.endsWith("/cancel")) {
                        pullState = "cancelled";
                        code = 202;
                        response = QJsonDocument(QJsonObject{});
                    } else if (path == "/models/pull/hub-job") {
                        response = QJsonDocument(QJsonObject{{"status", pullState}, {"progress", 30}, {"bytes_done", 1e9}, {"bytes_total", 5e9}});
                    } else {
                        code = 404;
                        response = QJsonDocument(QJsonObject{});
                    }
                    send(socket, response.toJson(QJsonDocument::Compact), code);
                });
            }
        });
    }

    static void send(QTcpSocket *socket, const QByteArray &body, int code = 200) {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};
}

int runModelHubChecks(const QString &directory) {
    int failures = 0;
    auto check = [&failures](bool value, const char *message) {
        if (!value) { ++failures; fprintf(stderr, "[modelhub] FAIL: %s\n", message); }
    };
    HubFixture fixture;
    if (!fixture.listen(QHostAddress::LocalHost)) { check(false, "fixture listens on loopback"); return failures; }
    BackendClient client;
    client.setBaseUrl(QString("http://127.0.0.1:%1").arg(fixture.serverPort()));
    ModelHubPanel hub;
    hub.resize(1180, 850);
    hub.setBackendClient(&client);
    hub.show();
    auto *filter = hub.findChild<QPushButton *>("hubFiltersToggle");
    auto *options = hub.findChild<QWidget *>("hubFilters");
    auto *local = hub.findChild<QCheckBox *>("hubLocalOnly");
    auto cards = [&hub] {
        int count = 0;
        for (auto *frame : hub.findChildren<QWidget *>("hubModelCard")) if (!frame->isHidden()) ++count;
        return count;
    };
    check(waitFor([&] { return cards() == 3; }), "Hub loads models from the backend");
    check(options->isHidden(), "desktop filters start collapsed");
    filter->click();
    check(!options->isHidden(), "desktop filter button expands options");
    local->setChecked(true);
    check(cards() == 1 && filter->text().contains("1"), "availability filter updates cards and count");
    filter->click();
    check(options->isHidden() && cards() == 1, "collapsing filters preserves selected criteria");
    hub.findChild<QPushButton *>("hubResetFilters")->click();
    check(cards() == 3, "reset restores loaded models");
    auto capture = [&hub, &directory](const QString &name) {
        if (directory.isEmpty()) return;
        pump(100);
        QDir().mkpath(directory);
        hub.grab().save(QDir(directory).filePath(name + (Theme::isDark() ? "-dark" : "-light") + ".png"));
    };
    capture("modelhub-discover");
    check(hub.findChildren<QCheckBox *>("hubCardCompare").isEmpty(), "comparison uses buttons instead of checkboxes");
    for (auto *tag : hub.findChildren<QLabel *>("hubPill")) {
        if (tag->isVisibleTo(&hub)) {
            check(!tag->wordWrap() && tag->height() < 28, "long tags stay compact on one line");
            if (tag->toolTip().size() < 20) check(tag->text() == tag->toolTip(), "short tags remain fully readable");
        }
    }
    filter->click();
    capture("modelhub-filters");
    filter->click();
    check(hub.findChild<QProgressBar *>("hubDeviceUsage")->value() > 0, "device summary includes measured memory usage");
    QTimer::singleShot(80, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        check(dialog && dialog->objectName() == "hubDeviceDialog", "device details opens a native utility window");
        if (!dialog) return;
        const auto popup = dialog->grab().toImage();
        check(popup.pixelColor(0, 0).alpha() == 0 && popup.pixelColor(popup.width()/2, popup.height()/2).alpha() > 0, "popup has transparent rounded corners and an opaque body");
        auto *refresh = dialog->findChild<QPushButton *>("hubRefreshDevice");
        check(waitFor([&] { return refresh->isEnabled(); }), "device readings finish refreshing");
        if (!directory.isEmpty()) dialog->grab().save(QDir(directory).filePath(QString("modelhub-device-") + (Theme::isDark() ? "dark" : "light") + ".png"));
        fixture.telemetryStatus = 503;
        refresh->click();
        check(waitFor([&] { return refresh->isEnabled(); }), "failed device refresh allows retry");
        check(hub.findChild<QProgressBar *>("hubDeviceUsage")->isHidden(), "unavailable measurements do not display a stale device meter");
        fixture.telemetryStatus = 200;
        refresh->click();
        check(waitFor([&] { return refresh->isEnabled() && !hub.findChild<QProgressBar *>("hubDeviceUsage")->isHidden(); }), "device meter recovers after retry");
        dialog->reject();
    });
    hub.findChild<QPushButton *>("hubDeviceDetails")->click();
    for (auto *checkbox : hub.findChildren<QPushButton *>("hubCardCompare")) {
        if (checkbox->isVisibleTo(&hub) && (checkbox->accessibleName() == "Compare test/model-0" || checkbox->accessibleName() == "Compare " + fixture.thirdRepo)) checkbox->click();
    }
    check(hub.findChild<QPushButton *>("hubCompareModels")->isEnabled(), "two selected cards enable comparison");
    capture("modelhub-selected");
    QTimer::singleShot(80, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        check(dialog && dialog->objectName() == "hubCompareDialog", "comparison opens its themed native window");
        if (!dialog) return;
        const auto popup = dialog->grab().toImage();
        check(popup.pixelColor(0, 0).alpha() == 0 && popup.pixelColor(popup.width()/2, popup.height()/2).alpha() > 0, "popup has transparent rounded corners and an opaque body");
        auto *scroll = dialog->findChild<QScrollArea *>("hubComparisonScroll");
        check(scroll && scroll->horizontalScrollBar()->maximum() == 0, "comparison columns fit the dialog width");
        check(dialog->findChildren<QPushButton *>("hubComparisonModelName").size() == 2, "comparison has clickable model headers");
        if (!directory.isEmpty()) dialog->grab().save(QDir(directory).filePath(QString("modelhub-compare-") + (Theme::isDark() ? "dark" : "light") + ".png"));
        dialog->reject();
    });
    hub.findChild<QPushButton *>("hubCompareModels")->click();
    for (auto *checkbox : hub.findChildren<QPushButton *>("hubCardCompare")) {
        if (checkbox->isVisibleTo(&hub) && checkbox->accessibleName() == "Compare test/model-1") checkbox->click();
    }
    QTimer::singleShot(80, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) { check(false, "three-model comparison opens"); return; }
        const auto headers = dialog->findChildren<QPushButton *>("hubComparisonModelName");
        auto *scroll = dialog->findChild<QScrollArea *>("hubComparisonScroll");
        check(headers.size() == 3 && scroll && scroll->horizontalScrollBar()->maximum() == 0, "three comparison columns fit without horizontal scrolling");
        if (!directory.isEmpty()) dialog->grab().save(QDir(directory).filePath(QString("modelhub-compare-three-") + (Theme::isDark() ? "dark" : "light") + ".png"));
        for (auto *header : headers) {
            if (header->property("hubRepo").toString() == "test/model-1") { header->click(); return; }
        }
        check(false, "comparison exposes the selected model link");
        dialog->reject();
    });
    hub.findChild<QPushButton *>("hubCompareModels")->click();
    check(waitFor([&] { return hub.findChild<QPushButton *>("hubBack") != nullptr; }), "comparison model name opens the dedicated page");
    if (auto *back = hub.findChild<QPushButton *>("hubBack")) back->click();
    auto details = [&hub](const QString &repo) {
        for (auto *button : hub.findChildren<QPushButton *>("hubModelName"))
            if (button->property("hubRepo").toString() == repo && !button->isHidden()) { button->click(); return true; }
        return false;
    };
    check(details("test/model-1"), "clicking a model name opens its dedicated page");
    check(waitFor([&] { auto *button = hub.findChild<QPushButton *>("hubDownloadModel"); return button && button->isEnabled(); }), "metadata enables repository download");
    auto *estimate = hub.findChild<QLabel *>("hubMemoryEstimate");
    const auto before = estimate->text();
    hub.findChild<QComboBox *>("hubEstimateContext")->setCurrentIndex(3);
    check(estimate->text() != before, "memory estimate changes with context");
    auto *tabs = hub.findChild<QTabWidget *>("hubDetailTabs");
    check(tabs && tabs->count() == 4, "native detail tabs expose specifications, files and evaluations");
    capture("modelhub-details");
    auto *action = hub.findChild<QPushButton *>("hubDownloadModel");
    check(action && hub.rect().contains(QRect(action->mapTo(&hub, QPoint()), action->size())), "primary model action is above the fold");
    hub.findChild<QPushButton *>("hubClearComparison")->click();
    pump();
    auto *detailCompare = hub.findChild<QPushButton *>("hubDetailCompare");
    check(detailCompare && !detailCompare->isChecked() && detailCompare->text() == "Compare", "clearing comparison synchronizes the model page");
    check(hub.findChild<QComboBox *>("hubEstimateContext")->currentData().toInt() == 16384, "clearing comparison preserves estimate choices");
    tabs = hub.findChild<QTabWidget *>("hubDetailTabs");
    for (int index = 1; index < tabs->count(); ++index) {
        tabs = hub.findChild<QTabWidget *>("hubDetailTabs");
        tabs->setCurrentIndex(index);
        capture(index == 1 ? "modelhub-specifications" : index == 2 ? "modelhub-files" : "modelhub-evaluations");
    }
    tabs->setCurrentIndex(0);
    QTimer::singleShot(80, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        check(dialog && dialog->objectName() == "appMessageDialog", "download confirmation uses the rounded Hub dialog");
        if (!dialog) return;
        if (!directory.isEmpty()) dialog->grab().save(QDir(directory).filePath(QString("modelhub-download-") + (Theme::isDark() ? "dark" : "light") + ".png"));
        for (auto *button : dialog->findChildren<QPushButton *>())
            if (button->text() == "Download repository") button->click();
    });
    hub.findChild<QPushButton *>("hubDownloadModel")->click();
    auto *cancel = hub.findChild<QPushButton *>("hubCancelDownload");
    check(waitFor([&] { return cancel->isVisibleTo(&hub); }), "download has an always reachable cancel action");
    cancel->click();
    check(waitFor([&] { return !cancel->isVisibleTo(&hub); }), "cancel completes through backend polling");
    check(fixture.requests.contains("/models/pull/hub-job/cancel"), "native cancel reaches backend");
    hub.findChild<QPushButton *>("hubBack")->click();
    pump();
    fixture.holdNextDetail = true;
    details("test/model-0");
    check(waitFor([&] { return !fixture.heldDetail.isNull(); }), "metadata reply can remain in flight");
    hub.findChild<QPushButton *>("hubBack")->click();
    pump();
    details(fixture.thirdRepo);
    pump();
    if (fixture.heldDetail) HubFixture::send(fixture.heldDetail, "{\"repo_id\":\"test/model-0\",\"license\":\"STALE REPLY\"}");
    pump();
    bool stale = false;
    for (auto *text : hub.findChildren<QLabel *>()) if (text->isVisibleTo(&hub) && text->text().contains("STALE REPLY")) stale = true;
    check(!stale, "late metadata does not replace a different model");
    hub.resize(850, 680);
    pump();
    check(hub.minimumSizeHint().width() <= 850, "detail layout fits the desktop minimum content width");
    capture("modelhub-details-compact");
    hub.findChild<QPushButton *>("hubBack")->click();
    capture("modelhub-discover-compact");
    filter->click();
    capture("modelhub-filters-compact");
    check(hub.minimumSizeHint().width() <= 850, "expanded filters fit the compact desktop width");
    for (auto *scroll : hub.findChildren<QScrollArea *>("cardScroll")) {
        if (scroll->isVisibleTo(&hub)) check(scroll->horizontalScrollBar()->maximum() == 0, "compact discovery never clips controls horizontally");
    }
    filter->click();
    client.abortAll();
    pump();
    fprintf(stderr, "[modelhub] %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}

#include "testing/uicheck.h"
#include "testing/docs_tests.h"
#include "testing/settings_tests.h"
#include "testing/windowchrome_tests.h"
#include "backend_client.h"
#include "theme.h"
#include "trainingduration.h"
#include "panels/modelspanel.h"
#include "panels/modelcard.h"
#include "panels/trainingpanel.h"
#include "panels/trainingrunpanel.h"
#include "panels/ragpanel.h"
#include "panels/ragdoccard.h"
#include "panels/adapterspanel.h"
#include "panels/adaptercard.h"
#include "panels/chatpanel.h"
#include "panels/chatbubble.h"
#include "panels/chattree.h"
#include "panels/chatstore.h"
#include "panels/greeting.h"
#include "widgets/namedialog.h"
#include "widgets/chathistorylist.h"
#include "widgets/sidebarstate.h"
#include "widgets/sidebar.h"
#include "panels/chathistorycontroller.h"
#include "panels/messagerow.h"
#include "panels/settingspanel.h"
#include "widgets/studiocanvas.h"
#include "widgets/trainingchart.h"
#include <QTableWidget>
#include <QListWidget>
#include "widgets/brandmark.h"
#include "widgets/clickfeedback.h"
#include "widgets/contextmeter.h"
#include "widgets/versionswitcher.h"
#include <QApplication>
#include <QClipboard>
#include <QWheelEvent>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QAccessible>
#include "widgets/toggleswitch.h"
#include "widgets/button.h"
#include "widgets/tabwidget.h"
#include "widgets/combobox.h"
#include "widgets/progressbar.h"
#include "widgets/spinbox.h"
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSettings>
#include <QSpinBox>
#include <QSvgGenerator>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QUrlQuery>
#include <QSet>
#include <cstdio>
#include <cmath>
#include <QDialog>
#include <QStackedWidget>
#include <QTextCursor>
#include <QScreen>
#include <QDateTime>
#include <functional>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void pump(int ms = 80) {
    QEventLoop loop;
    QTimer::singleShot(ms,&loop,&QEventLoop::quit);
    loop.exec();
}
bool waitUntil(const std::function<bool()> &done,int timeoutMs = 4000) {
    for (int waited = 0; !done() && waited < timeoutMs; waited += 20) pump(20);
    return done();
}
void key(QWidget *widget,int code,Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress,code,modifiers), release(QEvent::KeyRelease,code,modifiers);
    QApplication::sendEvent(widget,&press); QApplication::sendEvent(widget,&release);
}
QPushButton *button(QWidget *widget,const QString &text) {
    for (auto *b : widget->findChildren<QPushButton *>()) if (b->text() == text) return b;
    return nullptr;
}
struct Fixture : QTcpServer {
    bool failTraining = false, failQuery = false, holdChat = false;
    // Hub search: hubTotal > 0 serves paged results ("studio/hub-model-NNN");
    // otherwise the original single-result reply is used.
    int hubTotal = 0, searchStatus = 200;
    bool repeatLastOfPrevPage = false, emptySearch = false;
    QSet<QString> downloadedRepos;
    QStringList searchRequests;
    // Download job served at /models/pull/job1. pullMode "fail" is the original
    // behaviour (the start request is rejected); "run" accepts it; "conflict" is a 409.
    QString pullMode = "fail", pullStatus = "running", pullError;
    qint64 pullDone = 0, pullTotal = 0;
    double pullProgress = 0;
    int cancelCalls = 0, cancelStatus = 202, pollCount = 0;
    // Local library: models can be deleted (DELETE /models/<id>); deleteStatus 204 succeeds.
    QStringList removedModels, deleteRequests;
    // Download sizes for search results (/models/sizes): repo -> bytes; absent means "unknown".
    QHash<QString,double> hubSizes;
    QStringList sizesRequests;
    int sizesStatus = 200;
    int deleteStatus = 204;
    QByteArray localModelsJson() const {
        QJsonArray all{
            QJsonObject{{"id","studio/sample-model"},{"name","Sample local model"},{"source","huggingface"},{"ref","studio/sample-model"},{"architecture","llama"},{"size_mb",512},{"license","apache-2.0"}},
            QJsonObject{{"id","studio/long-model-name-for-layout-review-in-a-small-window"},{"name","Layout review"},{"source","local"},{"ref","studio/long-model-name-for-layout-review-in-a-small-window"},{"architecture","gpt2"},{"size_mb",120}}};
        QJsonArray shown;
        for (const auto &m : all) if (!removedModels.contains(m.toObject().value("id").toString())) shown.append(m);
        return QJsonDocument(shown).toJson(QJsonDocument::Compact);
    }
    QByteArray pullJson() const {
        return QJsonDocument(QJsonObject{{"id","job1"},{"ref","studio/hub-model-002"},{"source","huggingface"},
            {"status",pullStatus},{"progress",pullProgress},{"bytes_done",double(pullDone)},{"bytes_total",double(pullTotal)},
            {"message","Downloading from HuggingFace Hub"},{"error",pullError.isEmpty()?QJsonValue():QJsonValue(pullError)}}).toJson(QJsonDocument::Compact);
    }
    // Context usage (/inference/context-usage): the reply echoes the request's max_tokens as the reserve.
    int usageUsed = 1000, usageLimit = 4096, usageStatus = 200;
    bool holdUsageOnce = false;
    QStringList usageRequests;
    QPointer<QTcpSocket> heldUsageSocket;
    QByteArray heldUsageBody;
    void releaseHeldUsage() {
        if (!heldUsageSocket) return;
        heldUsageSocket->write("HTTP/1.1 200 Fixture\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(heldUsageBody.size())+"\r\nConnection: close\r\n\r\n"+heldUsageBody);
        heldUsageSocket->disconnectFromHost();
        heldUsageSocket.clear();
    }
    // Compaction (/inference/compact) and the chat requests the panel makes.
    int compactStatus = 200, compactCovered = 4;
    QString compactMethod = "summary";
    bool holdCompact = false;
    QStringList compactRequests, chatRequests;
    // What /inference/chat streams back; tests change it to tell a fresh reply from an old one.
    // chatEmpty: the reply streams no text (just "done", or nothing at all while held); chatStatus: an HTTP error instead of a stream.
    bool chatEmpty = false;
    int chatStatus = 200;
    QString chatReply = QStringLiteral("A local model can use your documents to give more useful answers.\n\nTry a specific question about your project.");
    QPointer<QTcpSocket> heldCompactSocket;
    // An unrelated request that stays in flight (/held-other), to prove cancelling a compaction leaves it alone.
    bool holdOther = false;
    QPointer<QTcpSocket> heldOtherSocket;
    static void finishHeld(QPointer<QTcpSocket> &socket, const QByteArray &body) {
        if (!socket) return;
        socket->write("HTTP/1.1 200 Fixture\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
        socket->disconnectFromHost();
        socket.clear();
    }
    void releaseHeldOther() { finishHeld(heldOtherSocket, "{}"); }
    void releaseHeldCompact() { finishHeld(heldCompactSocket, R"({"summary":"late summary","covered":4,"method":"summary"})"); }
    QString trainingState = "running";
    int trainingStatusCode = 200;
    bool omitTrainingHistory = false;
    bool serveActiveTraining = false;
    bool designPreview = false;
    bool emptyTrainingJobs = false;
    bool includeSubmittedTraining = false;
    bool deferTrainingCancellation = false;
    int trainingCancels = 0;
    QJsonObject trainingEstimateRequest;
    QJsonObject trainingRequest;
    QJsonObject trainingJob() const {
        if (designPreview) {
            QJsonArray history;
            for (int i=0;i<56;++i) {
                const double t=i/55.0;
                history.append(QJsonObject{{"step",int(10+t*548)},{"loss",0.482+2.318*(std::exp(-t*5)-std::exp(-5))/(1-std::exp(-5))+std::sin(t*38)*0.035*(1-t)},
                    {"learning_rate",0.0002*(t<0.08?t/0.08:0.5*(1+std::cos((t-0.08)/0.92*3.14159265)))},
                    {"grad_norm",0.342+0.65*(std::exp(-t*4)-std::exp(-4))},
                    {"epoch",t*1.86},{"elapsed_seconds",t*835+21}});
                if (i%8==0 || i==55) history.append(QJsonObject{{"step",int(10+t*548)},{"eval_loss",0.536+1.964*(std::exp(-t*4.5)-std::exp(-4.5))/(1-std::exp(-4.5))},{"epoch",t*1.86},{"elapsed_seconds",t*835+21}});
            }
            return QJsonObject{{"id","fixture-job"},{"name","support-assistant-v3"},{"status",trainingState},{"base_model","Qwen2.5-3B"},
                {"dataset_id","fixture-data"},{"config",QJsonObject{{"rank",16},{"alpha",32},{"epochs",3},{"batch_size",4},{"max_length",1024},{"learning_rate",0.0002},{"quantization","4bit"},{"validation_split",0.1}}},
                {"current_step",558},{"total_steps",900},{"progress",62},{"loss",0.482},{"history",history},
                {"created_at","Today, 10:42"},{"started_at",QDateTime::currentDateTimeUtc().addSecs(-856).toString(Qt::ISODate)},
                {"message","Step 558 completed · recorded signals saved locally"}};
        }
        QJsonArray history;
        for (int step : {1, 4, 12, 24, 42}) {
            history.append(QJsonObject{{"step",step},{"loss",2.1-step*0.02},
                {"learning_rate",0.0002*(1.0-step/100.0)}, {"grad_norm",0.9-step*0.01},
                {"epoch",step/40.0},{"elapsed_seconds",step*1.5}});
        }
        history.append(QJsonObject{{"step",42},{"eval_loss",1.36},{"epoch",1.05},{"elapsed_seconds",64.0}});
        return QJsonObject{{"id","fixture-job"},{"name","support-tone-v2"},{"status",trainingState},
            {"base_model","studio/sample-model"},{"dataset_id","fixture-data"},
            {"config",QJsonObject{{"rank",8},{"alpha",16},{"dropout",0.05},{"epochs",3},{"batch_size",1},
                {"learning_rate",0.0002},{"max_length",512},{"quantization","none"},{"validation_split",0.1},{"seed",42}}},
            {"current_step",42},{"total_steps",100},{"progress",42},{"loss",1.28},
            {"history",omitTrainingHistory ? QJsonArray() : history},
            {"message","Training fixture · learning from example conversations"}};
    }
    QPointer<QTcpSocket> chatSocket;
    int starts = 0, clears = 0;
    Fixture() {
        connect(this,&QTcpServer::newConnection,this,[this]() {
            while (auto *socket = nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,bytes]() {
                    bytes->append(socket->readAll());
                    const int headerEnd = bytes->indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("answered").toBool()) return;
                    int length = 0;
                    for (auto line : bytes->left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < headerEnd+4+length) return;
                    socket->setProperty("answered",true);
                    const QString method = QString::fromUtf8(bytes->split(' ').value(0));
                    const QString target = QString::fromUtf8(bytes->split(' ').value(1));
                    const QString path = target.section('?',0,0);
                    QByteArray body = "{}", type = "application/json";
                    int status = 200;
                    if (path == "/models/sizes") {
                        sizesRequests << target;
                        status = sizesStatus;
                        if (sizesStatus == 200) {
                            QJsonObject sizes;
                            for (const auto &repo : QUrlQuery(target.section('?',1)).allQueryItemValues("repo",QUrl::FullyDecoded))
                                sizes.insert(repo, hubSizes.contains(repo) ? QJsonValue(hubSizes.value(repo)) : QJsonValue());
                            body = QJsonDocument(sizes).toJson(QJsonDocument::Compact);
                        } else body = R"({"detail":"fixture sizes failure"})";
                    }
                    else if (method == "DELETE" && path.startsWith("/models/")) {
                        deleteRequests << path;
                        status = deleteStatus;
                        if (deleteStatus == 204) removedModels << path.mid(8);
                        else if (deleteStatus == 409) body = R"({"detail":"Unload this model before deleting it"})";
                        else if (deleteStatus == 404) body = R"({"detail":"Model not found"})";
                    }
                    else if (path == "/models") body = localModelsJson();
                    else if (path == "/models/search") {
                        searchRequests << target;
                        const QUrlQuery query(target.section('?',1));
                        const int offset = query.queryItemValue("offset").toInt(), limit = query.queryItemValue("limit").toInt();
                        if (searchStatus != 200) { status=searchStatus; body=R"({"detail":"Could not reach the Hugging Face Hub: fixture offline"})"; }
                        else if (emptySearch) body = "[]";
                        else if (hubTotal > 0) {
                            QJsonArray page;
                            const int first = repeatLastOfPrevPage && offset > 0 ? offset-1 : offset;
                            for (int i = first; i < qMin(hubTotal, first+limit); ++i) {
                                const QString repo = QString("studio/hub-model-%1").arg(i,3,10,QChar('0'));
                                page.append(QJsonObject{{"repo_id",repo},{"downloads",1000-i},{"likes",i},{"license","mit"},{"downloaded",downloadedRepos.contains(repo)}});
                            }
                            body = QJsonDocument(page).toJson(QJsonDocument::Compact);
                        }
                        else body = R"([{"repo_id":"studio/example-model","downloads":1200,"likes":32,"license":"apache-2.0"}])";
                    }
                    else if (path == "/models/pull") {
                        if (pullMode == "run") { status=202; body=pullJson(); }
                        else if (pullMode == "conflict") { status=409; body=R"({"detail":"Another download is in progress (org/other). Wait for it or cancel it first."})"; }
                        else { status=400; body=R"({"detail":"Fixture download failure"})"; }
                    }
                    else if (path == "/models/pull/job1") { ++pollCount; body=pullJson(); }
                    else if (path == "/models/pull/job1/cancel") { ++cancelCalls; status=cancelStatus; body=cancelStatus==202?pullJson():QByteArray(R"({"detail":"fixture cancel failure"})"); }
                    else if (path == "/datasets") body = designPreview ? R"([{"id":"fixture-data","name":"support-conversations.jsonl","format":"jsonl","rows":2400}])" : R"([{"id":"fixture-data","name":"Example conversations.jsonl","format":"jsonl","rows":4812}])";
                    else if (path == "/training/telemetry") body=R"({"device":"NVIDIA RTX 3060","kind":"VRAM","accelerator":"CUDA","used_bytes":7838315315,"total_bytes":12884901888,"utilization_percent":94,"temperature_c":68,"power_w":142,"detail":"Synthetic UI fixture only; device-wide readings in production."})";
                    else if (path == "/training/estimate") { trainingEstimateRequest=QJsonDocument::fromJson(bytes->mid(headerEnd+4,length)).object(); body=R"({"memory":{"kind":"VRAM","available_bytes":25769803776},"memory_low_bytes":17179869184,"memory_high_bytes":22548578304,"adapter_bytes":167772160,"steps":1800,"duration_low_seconds":7200,"duration_high_seconds":8400,"assumptions":"Fixture advisory budget"})"; if (designPreview) body=R"({"memory":{"kind":"VRAM","available_bytes":12884901888},"memory_low_bytes":7838315315,"memory_high_bytes":7838315315,"steps":900,"assumptions":"Synthetic screenshot fixture only"})"; }
                    else if (path == "/training/jobs") {
                        auto past=trainingJob(); past["id"]="past-job"; past["name"]="support-tone-v1"; past["status"]="done";
                        auto older=past; older["id"]="older-job"; older["name"]="support-tone-baseline";
                        auto config=older["config"].toObject(); config["rank"]=16; older["config"]=config;
                        older["history"]=QJsonArray{QJsonObject{{"step",4},{"loss",2.4},{"learning_rate",0.0001},{"grad_norm",1.1}}};
                        QJsonArray jobs{past,older};
                        if (designPreview) {
                            past["name"]="support-assistant-v2"; past["loss"]=0.612; older["name"]="support-assistant-v1"; older["loss"]=0.784;
                            auto writing=past; writing["id"]="writing-job"; writing["name"]="writing-style-v1"; writing["base_model"]="SmolLM2-1.7B"; writing["loss"]=0.559;
                            jobs=QJsonArray{trainingJob(),past,older,writing};
                        } else if (serveActiveTraining || includeSubmittedTraining) jobs.append(trainingJob());
                        if (emptyTrainingJobs) jobs={};
                        body=QJsonDocument(jobs).toJson();
                    }
                    else if (path == "/training/start") { trainingRequest=QJsonDocument::fromJson(bytes->mid(headerEnd+4,length)).object(); ++starts; if (!failTraining) includeSubmittedTraining=true; status=failTraining?400:202; body=failTraining?R"({"detail":"Fixture training failure"})":R"({"id":"fixture-job"})"; }
                    else if (path.startsWith("/training/cancel/")) { ++trainingCancels; if (!deferTrainingCancellation) trainingState="cancelled"; }
                    else if (path.startsWith("/training/status/")) { status=trainingStatusCode; body=QJsonDocument(trainingJob()).toJson(); }
                    else if (path == "/held-other") {
                        if (holdOther) { heldOtherSocket = socket; return; }
                    }
                    else if (path == "/inference/compact") {
                        const QByteArray requestBody = bytes->mid(headerEnd+4);
                        compactRequests << QString::fromUtf8(requestBody);
                        if (holdCompact) { heldCompactSocket = socket; return; }
                        status = compactStatus;
                        if (compactStatus != 200) body = R"({"detail":"fixture compact failure"})";
                        else {
                            const QString previous = QJsonDocument::fromJson(requestBody).object().value("previous_summary").toString();
                            body = QJsonDocument(QJsonObject{{"summary",compactMethod=="summary"?QString("The user asked about X."):previous},
                                {"covered",compactCovered},{"method",compactMethod}}).toJson(QJsonDocument::Compact);
                        }
                    }
                    else if (path == "/inference/context-usage") {
                        const QByteArray requestBody = bytes->mid(headerEnd+4);
                        usageRequests << QString::fromUtf8(requestBody);
                        status = usageStatus;
                        if (usageStatus != 200) body = R"({"detail":"No inference engine loaded. Load a model first."})";
                        else {
                            const int reserved = QJsonDocument::fromJson(requestBody).object().value("max_tokens").toInt(256);
                            body = QJsonDocument(QJsonObject{{"used",usageUsed},{"reserved",reserved},{"limit",usageLimit},
                                {"fill",double(usageUsed+reserved)/usageLimit},{"exact",true}}).toJson(QJsonDocument::Compact);
                            if (holdUsageOnce) {
                                holdUsageOnce = false; heldUsageSocket = socket; heldUsageBody = body;
                                return;
                            }
                        }
                    }
                    else if (path == "/inference/status" || path == "/inference/load") body=R"({"engine":{"name":"torch","description":"Local · torch"},"loaded_model":"studio/sample-model","active_adapter_id":"fixture-adapter","active_adapter":"Support assistant"})";
                    else if (path == "/inference/chat") {
                        chatRequests << QString::fromUtf8(bytes->mid(headerEnd+4));
                        if (chatStatus != 200) { status=chatStatus; body=R"({"detail":"fixture chat failure"})"; }
                        else {
                            type="text/event-stream";
                            body=chatEmpty?QByteArray():"data: "+QJsonDocument(QJsonObject{{"delta",chatReply}}).toJson(QJsonDocument::Compact)+"\n\n";
                            if (holdChat) {
                                chatSocket=socket;
                                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"+body);
                                return;
                            }
                            body+="data: {\"done\":true}\n\n";
                        }
                    }
                    else if (path == "/system/info") body=R"({"version":"v0.1-preview","system":"Fixture OS","architecture":"test","python":"3.12","engines":{"llama.cpp":"test","vLLM":null,"torch":null,"PEFT":null},"device":{"device":"Fixture CPU","kind":"RAM","total_bytes":17179869184,"accelerator":"SYSTEM"}})";
                    else if (path == "/system/storage") body=R"({"data_directory":"/fixture/magnite","model_cache":"/fixture/model-cache","workspace":{"bytes":820000000,"complete":true},"models":{"bytes":17800000000,"complete":false},"free_bytes":80000000000,"combined_locations":false})";
                    else if (path == "/system/logs") body=R"({"text":"Runtime ready\nNo model loaded","truncated":false})";
                    else if (path == "/adapters") body=R"([{"id":"fixture-adapter","training_job_id":"fixture-job","name":"Support assistant","base_model":"studio/sample-model","rank":8,"method":"lora","size_mb":8.5,"trained_at":"2026-09-30T10:00:00Z"},{"id":"fixture-adapter-2","name":"A long adapter name that should remain readable in a compact window","base_model":"studio/another-model","rank":16,"method":"lora","size_mb":12.3}])";
                    else if (path == "/rag/documents") body=R"([{"id":"doc-1","name":"Project guide.md","path":"/documents/research/project-guide.md","kind":"md","chunks":24},{"id":"doc-2","name":"<b>literal filename</b>.txt","path":"/documents/a-very-long-folder-name/another-long-folder-for-layout-review/meeting-notes.txt","kind":"txt","chunks":12}])";
                    else if (path == "/rag/status") body=R"({"documents":2,"chunks":36,"engine_ready":true,"model":"local embeddings"})";
                    else if (path == "/rag/config") body=R"({"chunk_size":512,"chunk_overlap":64,"top_k":4})";
                    else if (path == "/rag/query") { status=failQuery?500:200; body=failQuery?R"({"detail":"Fixture query failure"})":R"({"results":[{"score":0.92,"document_name":"Project guide.md","index":0,"text":"Magnite keeps the training and inference workflows in your local workspace."}]})"; }
                    else if (path == "/rag/clear") { ++clears; status=204; }
                    else if (path == "/export-ok") { body="test-archive-payload"; type="application/zip"; }
                    else if (path == "/export-fail") { status=500; body="fixture error"; }
                    socket->write("HTTP/1.1 "+QByteArray::number(status)+" Fixture\r\nContent-Type: "+type+"\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};
}

int runUiCheck(const QString &captures) {
    int failures=0;
    auto check = [&failures](bool pass,const QString &message) {
        if (!pass) ++failures;
        fprintf(stderr,"[ui-check] %s %s\n",pass?"ok:":"FAIL:",qUtf8Printable(message));
    };
    check(TrainingTime::duration(0)=="0s" && TrainingTime::duration(59)=="59s" &&
          TrainingTime::duration(60)=="1m 0s" && TrainingTime::duration(3599)=="59m 59s",
          "training durations format seconds and minutes at unit boundaries");
    check(TrainingTime::duration(3600)=="1h 0m" && TrainingTime::duration(8400)=="2h 20m" &&
          TrainingTime::duration(86399)=="23h 59m" && TrainingTime::duration(86400)=="1d 0h" &&
          TrainingTime::duration(90061)=="1d 1h",
          "training durations convert long runs to hours and days");
    check(TrainingTime::duration(-1)=="—" && TrainingTime::duration(qInf())=="—" &&
          TrainingTime::duration(qQNaN())=="—",
          "unavailable training durations do not display invalid values");
    if (qEnvironmentVariableIsSet("MAGNITE_DOCS_DESIGN_CHECK")) return runDocsChecks(captures)?1:0;
    Fixture fixture;
    if (!fixture.listen(QHostAddress::LocalHost,0)) { check(false,"loopback fixture server: "+fixture.errorString()); return 1; }
    BackendClient client;
    client.setBaseUrl(QString("http://127.0.0.1:%1").arg(fixture.serverPort()));
    if (qEnvironmentVariableIsSet("MAGNITE_SETTINGS_DESIGN_CHECK")) return runSettingsChecks(&client,captures)?1:0;
    failures+=runDocsChecks();
    failures+=runWindowChromeChecks();
    failures+=runSettingsChecks(&client,QString());
    if (qEnvironmentVariableIsSet("MAGNITE_TRAINING_DESIGN_CHECK")) {
        fixture.designPreview=true; fixture.serveActiveTraining=true;
        TrainingPanel training; training.resize(1245,1350); training.setBackendClient(&client); training.show(); pump(900);
        training.applySettingsJson(R"json({"baseModel":"Qwen2.5-3B","dataset":"support-conversations.jsonl","rank":16,"batchSize":4,"validationSplit":10,"qlora":true,"quantization":"4-bit (NF4)"})json"); pump(350);
        QDir().mkpath(captures);
        auto *workspace=training.findChild<SegmentedTabBar *>("trainingWorkspaceTabs");
        auto *metrics=training.findChild<SegmentedTabBar *>("trainingMetricTabs");
        auto *scroll=training.findChild<QScrollArea *>("cardScroll");
        auto *index=training.findChild<QListWidget *>("trainingRuns");
        auto *loss=training.findChild<TrainingChart *>("trainingLossChart");
        for (const bool dark : {false,true}) {
            Theme::setDark(dark);
            for (int view=0;view<2;++view) {
                workspace->setCurrentIndex(view); training.resize(1245,view==0?1600:1450); scroll->verticalScrollBar()->setValue(0); pump(700);
                if (view==1) check(index->parentWidget()->width()>=240,"Observatory experiment column has room for names and tags");
                auto *badge=training.findChild<QLabel *>("trainingBadge");
                auto *actions=training.findChild<Button *>("trainingRunActions");
                check(qAbs(badge->mapTo(&training,badge->rect().center()).y()-actions->mapTo(&training,actions->rect().center()).y())<=1,"status and run actions share a vertical center");
                check(actions->labelRect().width()>=actions->fontMetrics().horizontalAdvance(actions->text()),"run action glyph fits without elision");
                const QPointF hoverPoint(loss->width()-26,50);
                QMouseEvent move(QEvent::MouseMove,hoverPoint,loss->mapToGlobal(hoverPoint.toPoint()),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(loss,&move);
                auto *readout=loss->findChild<QLabel *>("trainingChartHover");
                check(readout->isVisible() && readout->text().contains("Train") && readout->text().contains("Val.") && readout->text().contains("0.482"),"chart hover immediately displays measured train and validation values");
                check(loss->rect().contains(readout->geometry()),"chart hover stays inside chart at its right edge");
                check(loss->toolTip().isEmpty(),"chart does not use a native tooltip");
                check(loss->grab().save(captures+QString("/hover-%1-%2.png").arg(view).arg(dark?"dark":"light")),"capture instant chart readout");
                QEvent leave(QEvent::Leave); QApplication::sendEvent(loss,&leave);
                check(!readout->isVisible(),"chart hover clears when pointer leaves");
                const auto name=QString("%1-%2.png").arg(view==0?"studio":"observatory",dark?"dark":"light");
                check(training.grab().save(captures+"/"+name),"capture "+name);
                check(scroll->horizontalScrollBar()->maximum()==0,"reference workspace has no horizontal overflow");
            }
        }
        auto *logToggle=training.findChild<QPushButton *>("trainingLogToggle"); logToggle->click(); pump(100);
        auto *log=training.findChild<QPlainTextEdit *>("trainingEventLog");
        auto *logSearch=training.findChild<QLineEdit *>("trainingLogSearch");
        check(log->isVisible() && log->toPlainText().contains("[INFO]") && log->toPlainText().contains("Loss"),"event log includes severity and recorded measurements");
        check(!log->verticalScrollBar()->property("scrollActive").toBool(),"inner scrollbar is quiet before a scroll gesture");
        auto *innerBar=log->verticalScrollBar();
        QMouseEvent barHover(QEvent::MouseMove,QPointF(2,20),innerBar->mapToGlobal(QPoint(2,20)),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(innerBar,&barHover);
        check(!innerBar->property("scrollActive").toBool(),"hovering an idle inner track does not reveal its handle");
        const auto originalLog=log->toPlainText();
        QMetaObject::invokeMethod(&training,"onPollStatus"); pump(150);
        check(log->toPlainText()==originalLog,"unchanged polling does not duplicate events");
        logSearch->setText("validation");
        check(!log->toPlainText().isEmpty() && log->toPlainText().split('\n').size()<originalLog.split('\n').size(),"log filter narrows recorded events");
        training.findChild<QPushButton *>("trainingLogCopy")->click();
        check(QApplication::clipboard()->text()==log->toPlainText(),"copy uses visible filtered events");
        logSearch->clear();
        log->verticalScrollBar()->setValue(0);
        auto *follow=training.findChild<QPushButton *>("trainingLogFollow");
        check(!follow->isChecked(),"reading older events pauses following"); follow->click();
        check(log->verticalScrollBar()->value()==log->verticalScrollBar()->maximum(),"follow latest returns to newest event");
        workspace->setCurrentIndex(0); training.resize(829,628); pump(500);
        scroll->ensureWidgetVisible(log); pump(100);
        log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
        const int outerBefore=scroll->verticalScrollBar()->value();
        QWheelEvent wheel(QPointF(20,20),log->viewport()->mapToGlobal(QPoint(20,20)),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(log->viewport(),&wheel);
        check(scroll->verticalScrollBar()->value()==outerBefore,"wheel at nested log boundary leaves the page still");
        check(log->verticalScrollBar()->property("scrollActive").toBool(),"inner scrollbar appears during scrolling");
        check(scroll->verticalScrollBar()->width()==3,"page scrollbar uses the finer 3px track");
        auto *pageBar=scroll->verticalScrollBar();
        check(training.width()-pageBar->mapTo(&training,pageBar->rect().topRight()).x()<=8,"page scrollbar sits at the outer edge");
        check(scroll->widget()->layout()->contentsMargins().right()>=24,"content is separated from the page scrollbar");
        check(training.findChild<QWidget *>("trainingLogCard")->grab().save(captures+"/event-log-compact.png"),"capture readable event log and thin scrollbars");
        pump(1000);
        check(!log->verticalScrollBar()->property("scrollActive").toBool(),"inner scrollbar disappears after scrolling stops");
        check(training.findChild<QWidget *>("trainingLogCard")->grab().save(captures+"/event-log-idle.png"),"capture event log with hidden scrollbar");
        log->verticalScrollBar()->setValue(0); const int beforeTop=scroll->verticalScrollBar()->value();
        QWheelEvent wheelUp(QPointF(20,20),log->viewport()->mapToGlobal(QPoint(20,20)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(log->viewport(),&wheelUp);
        check(scroll->verticalScrollBar()->value()==beforeTop,"wheel at the top of an inner box stays contained");
        logToggle->click();
        check(!training.findChild<QPushButton *>("trainingArchiveToggle")->isVisible() && !training.findChild<QPushButton *>("trainingCompareAction")->isVisible(),"Studio has no compare actions");
        workspace->setCurrentIndex(1); pump(100);
        auto *compareAction=training.findChild<QPushButton *>("trainingCompareAction");
        check(compareAction->isVisible(),"Compare run lives in Observatory"); compareAction->click(); pump(150);
        check(training.findChild<QComboBox *>("compareRunA")->isVisible(),"Observatory opens the comparison controls");
        workspace->setCurrentIndex(0); pump(100);
        check(!training.findChild<QComboBox *>("compareRunA")->isVisible() && !training.findChild<QPushButton *>("trainingArchiveToggle")->isChecked(),"returning to Studio closes the comparison");
        QElapsedTimer themeTime; themeTime.start();
        for (const bool dark : {false,true,false,true}) Theme::setDark(dark);
        fprintf(stderr,"[ui-check] four theme switches: %lld ms\n",themeTime.elapsed()); pump(100);
        check(Theme::isDark() && training.updatesEnabled() && log->updatesEnabled(),"rapid appearance switches settle with updates enabled");
        check(scroll->horizontalScrollBar()->maximum()==0,"compact Studio fits");
        workspace->setCurrentIndex(1); pump(500); check(scroll->horizontalScrollBar()->maximum()==0,"compact Observatory fits");
        workspace->setCurrentIndex(0); metrics->setCurrentIndex(3); pump(650);
        check(training.findChild<QStackedWidget *>("trainingChartStack")->currentIndex()==3,"throughput tab selects measured step rates");
        metrics->setCurrentIndex(0);
        auto *toggle=training.findChild<QCheckBox *>("trainingSmoothing"); toggle->setChecked(false);
        check(loss->sampleCount()>50,"smoothing preserves raw sample count"); toggle->setChecked(true);
        QListWidgetItem *past=nullptr;
        for(int i=0;i<index->count();++i) if(index->item(i)->data(Qt::UserRole)=="past-job") past=index->item(i);
        QMetaObject::invokeMethod(index,"itemClicked",Q_ARG(QListWidgetItem *,past)); pump(150);
        check(training.findChild<QLabel *>("trainingBadge")->text().contains("Completed"),"selecting history displays that run's status");
        QMetaObject::invokeMethod(&training,"onPollStatus"); pump(150);
        check(training.findChild<QLabel *>("trainingBadge")->text().contains("Completed"),"live polling does not replace a selected historical run");
        const auto savedRecipe=training.settingsJson();
        QCheckBox *rawToggle=nullptr;
        for (auto *box : training.findChildren<QCheckBox *>()) if (box->text()=="Visualisation") rawToggle=box;
        training.findChild<QPushButton *>("trainingFollowActive")->click(); pump(100);
        rawToggle->setChecked(false); pump(100);
        auto *raw=training.findChild<QPlainTextEdit *>("trainingRawData"); auto cursor=raw->textCursor();
        cursor.setPosition(10); cursor.setPosition(30,QTextCursor::KeepAnchor); raw->setTextCursor(cursor); raw->verticalScrollBar()->setValue(30);
        const auto selection=raw->textCursor().selectedText(); const int rawScroll=raw->verticalScrollBar()->value();
        pump(1100); QMetaObject::invokeMethod(&training,"onPollStatus"); pump(100);
        check(raw->textCursor().selectedText()==selection && raw->verticalScrollBar()->value()==rawScroll,"raw job updates preserve text selection and scroll position");
        rawToggle->setChecked(true);
        for (int i=0;i<index->count();++i) if (index->item(i)->data(Qt::UserRole)=="past-job") {
            QMetaObject::invokeMethod(index,"itemClicked",Q_ARG(QListWidgetItem *,index->item(i))); break;
        }
        auto *next=training.findChild<QPushButton *>("trainingReuseRun"); next->click(); pump(450);
        auto *configure=training.findChild<QPushButton *>("trainingConfigureRun");
        auto *dialog=training.findChild<QDialog *>("trainingRunDialog");
        check(dialog->isVisible() && dialog->findChildren<QComboBox *>().size()==5,"new-run dialog keeps all native dropdowns, including advanced precision and modules");
        check(training.settingsJson()==savedRecipe,"duplicating a run leaves the saved next recipe unchanged");
        auto *dialogScroll=dialog->findChild<QScrollArea *>("trainingDialogScroll");
        auto *advanced=dialog->findChild<QPushButton *>("trainingDraftAdvanced"); advanced->setChecked(true); pump(100);
        check(dialogScroll->horizontalScrollBar()->maximum()==0,"advanced dialog fields fit without horizontal scrolling");
        check(dialog->height()<=dialog->screen()->availableGeometry().height() && dialog->width()<=dialog->screen()->availableGeometry().width(),"run editor fits the available screen, including high DPI");
        check(dialog->findChild<QPushButton *>("trainingDraftSave")->isVisible(),"save action stays visible when advanced content scrolls");
        advanced->setChecked(false); Theme::setDark(false); pump(350);
        check(dialog->grab().save(captures+"/new-run-light.png"),"capture polished new run dialog");
        auto *draftRank=dialog->findChild<QSpinBox *>("trainingDraft_rank"); draftRank->setValue(32);
        auto *precision=dialog->findChild<QComboBox *>("trainingDraft_quantization"); precision->setCurrentIndex(2); pump(450);
        check(fixture.trainingEstimateRequest["rank"].toInt()==32 && fixture.trainingEstimateRequest["quantization"]=="8bit", "dialog estimates follow the current draft, including precision");
        check(training.settingsJson()==savedRecipe,"live draft estimates do not mutate the saved recipe");
        auto *dataset=dialog->findChild<QComboBox *>("trainingDraft_dataset"); dataset->setCurrentText("missing-dataset");
        dialog->findChild<QPushButton *>("trainingDraftSave")->click(); pump(100);
        auto *dialogStatus=dialog->findChild<QLabel *>("trainingDialogStatus");
        check(dialog->isVisible() && dialogStatus->property("state")=="error" && dataset->property("invalid").toBool(),"invalid dataset is explained and highlighted in the modal");
        const auto validationMessage=dialogStatus->text(); QMetaObject::invokeMethod(&training,"onPollStatus"); pump(100);
        check(dialogStatus->text()==validationMessage,"live job polling does not replace modal validation feedback");
        dialog->reject(); pump(100); check(training.settingsJson()==savedRecipe,"cancelled modal draft leaves the next recipe unchanged");
        configure->click(); pump(350);
        dataset=dialog->findChild<QComboBox *>("trainingDraft_dataset"); dataset->setCurrentText("support-conversations.jsonl");
        draftRank=dialog->findChild<QSpinBox *>("trainingDraft_rank"); draftRank->setValue(32);
        dialog->findChild<QComboBox *>("trainingDraft_quantization")->setCurrentIndex(2);
        dialog->findChild<QPushButton *>("trainingDraftSave")->click(); pump(100);
        const auto committed=QJsonDocument::fromJson(training.settingsJson().toUtf8()).object();
        check(committed["rank"].toInt()==32 && committed["quantization"]=="8-bit (bitsandbytes)","saving the modal commits its explicit advanced precision");
        auto *indexSearch=training.findChild<QLineEdit *>("trainingIndexSearch"); indexSearch->setText("no-such-run"); pump(80);
        check(!index->isVisible() && !training.findChild<QTableWidget *>("trainingExperimentTable")->isVisible(),"no-match search replaces empty rows with an explanation");
        indexSearch->clear(); workspace->setCurrentIndex(1); pump(80);
        index->setFocus(); index->setCurrentRow(0); key(index,Qt::Key_Down); pump(100);
        check(training.findChild<QLabel *>("trainingRunTitle")->text()=="support-assistant-v2","arrow keys select and display experiments");
        check(training.findChild<QPushButton *>("trainingFollowActive")->isVisible(),"historical selection offers a direct return to the active run");
        training.findChild<QPushButton *>("trainingFollowActive")->click(); pump(100);
        fixture.deferTrainingCancellation=true; QMetaObject::invokeMethod(&training,"onStop"); pump(100);
        QMetaObject::invokeMethod(&training,"onPollStatus"); pump(100); QMetaObject::invokeMethod(&training,"onStop"); pump(80);
        check(fixture.trainingCancels==1 && !training.findChild<QPushButton *>("trainingStopRun")->isEnabled(),"pending cancellation stays disabled across status polls and duplicate stops");
        fixture.trainingState="cancelled"; QMetaObject::invokeMethod(&training,"onPollStatus"); pump(100);
        check(training.findChild<QLabel *>("trainingBadge")->text()=="Stopped","terminal cancellation replaces the stopping state");
        fixture.serveActiveTraining=false; workspace->setCurrentIndex(0);
        configure->click(); pump(350); fixture.failTraining=true;
        for (auto *button : dialog->findChildren<QPushButton *>()) if (button->property("dialogStart").toBool()) button->click();
        pump(150);
        check(dialog->isVisible() && dialog->findChild<QLabel *>("trainingDialogStatus")->text().contains("Could not start"),"failed submission keeps the dialog open with a recoverable error");
        check(training.findChild<QLabel *>("trainingRunTitle")->text()=="support-assistant-v3","failed submission restores the previous recorded experiment");
        check(dialog->grab().save(captures+"/new-run-error.png"),"capture submission error"); dialog->reject();
        auto *feedback=training.findChild<QFrame *>("trainingFeedback");
        check(feedback->isVisible() && feedback->mapTo(&training,QPoint()).y()<training.findChild<QWidget *>("trainingStudio")->mapTo(&training,QPoint()).y(),"submission feedback remains visible above the monitor");
        training.findChild<QPushButton *>("trainingDismissStatus")->click();
        check(!feedback->isVisible(),"notifications can be dismissed without changing run state");
        fixture.emptyTrainingJobs=true; training.refreshOptions(); pump(450);
        check(training.findChild<QLabel *>("trainingRunTitle")->text()=="Your next experiment" && loss->sampleCount()==0,"an empty archive clears stale run details");
        check(training.grab().save(captures+"/empty-studio.png"),"capture empty Studio");
        auto *recipe=training.findChild<QWidget *>("trainingRecipe"); auto *studio=training.findChild<QWidget *>("trainingStudio");
        check(studio->mapTo(&training,QPoint()).y()<recipe->mapTo(&training,QPoint()).y(),"monitor remains before next recipe");
        fprintf(stderr,"[ui-check] %s (%d failures)\n",failures?"FAIL":"PASS",failures); return failures?1:0;
    }
    {
        ToggleSwitch toggle("Enable feature"); toggle.resize(toggle.sizeHint()); toggle.show(); toggle.activateWindow(); pump();
        QSettings().setValue("appearance/studio3ReducedMotion",false);
        toggle.setFocus(Qt::TabFocusReason); key(&toggle,Qt::Key_Space); pump(35);
        check(toggle.isChecked() && toggle.isAnimating(),"switch: Space changes state and animates the thumb");
        auto *accessible=QAccessible::queryAccessibleInterface(&toggle);
        check(accessible && accessible->state().checkable && accessible->state().checked && accessible->text(QAccessible::Name)=="Enable feature","switch: screen readers retain label and checked state");
        const auto moving=toggle.grab().toImage(); pump(220);
        check(!toggle.isAnimating() && moving!=toggle.grab().toImage(),"switch: animation changes frames and settles");
        toggle.setEnabled(false); key(&toggle,Qt::Key_Space); toggle.click();
        check(toggle.isChecked(),"switch: disabled controls cannot be toggled");
        toggle.setEnabled(true); QSettings().setValue("appearance/studio3ReducedMotion",true);
        key(&toggle,Qt::Key_Space);
        check(!toggle.isChecked() && !toggle.isAnimating(),"switch: reduced motion changes state immediately");
        const QPointF labelPoint(toggle.width()-5,toggle.height()/2.0);
        QMouseEvent down(QEvent::MouseButtonPress,labelPoint,toggle.mapToGlobal(labelPoint.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease,labelPoint,toggle.mapToGlobal(labelPoint.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&toggle,&down); QApplication::sendEvent(&toggle,&up);
        check(toggle.isChecked(),"switch: clicking the label activates the control");
        toggle.setLayoutDirection(Qt::RightToLeft);
        check(toggle.minimumSizeHint().height()>=32 && toggle.minimumSizeHint().width()>=42,"switch: target size accommodates the track and text in RTL");
        QSettings().setValue("appearance/studio3ReducedMotion",false); toggle.hide();
    }
    ModelsPanel models; TrainingPanel training; RagPanel knowledge;
    AdaptersPanel adapters; ChatPanel chat; SettingsPanel settings;
    models.setBackendClient(&client); training.setBackendClient(&client);
    knowledge.setBackendClient(&client); adapters.setBackendClient(&client); chat.setBackendClient(&client);
    pump(200);
    {
        auto telemetry=[](const char *json) { return QJsonDocument::fromJson(json).object(); };
        training.applyTelemetry(telemetry(R"({"accelerator":"CUDA","device":"GPU","utilization_percent":94,"cpu_percent":10,"used_bytes":6442450944,"total_bytes":12884901888})"));
        check(training.computeText("utilizationTitle")=="GPU utilization" && training.computeText("utilization")=="94%" && training.litUtilizationSegments()==30,"compute card prefers the GPU reading when the device reports one");
        training.applyTelemetry(telemetry(R"({"accelerator":"SYSTEM","device":"Local system","cpu_percent":40,"used_bytes":6000000000,"total_bytes":8000000000})"));
        check(training.computeText("utilizationTitle")=="CPU utilization" && training.computeText("utilization")=="40%" && training.litUtilizationSegments()==13,"compute card shows system CPU when there is no GPU reading");
        check(training.computeText("memory")=="5.6 GB / 7.5 GB","compute card shows measured system memory");
        training.applyTelemetry(telemetry(R"({"accelerator":"SYSTEM","cpu_percent":1.2})"));
        check(training.litUtilizationSegments()==1,"any measured utilization above zero lights at least one segment");
        training.applyTelemetry(telemetry(R"({"accelerator":"SYSTEM","cpu_percent":0})"));
        check(training.computeText("utilization")=="0%" && training.litUtilizationSegments()==0,"zero utilization lights nothing but still reads 0%");
        training.applyTelemetry(telemetry("{}"));
        check(training.computeText("utilizationTitle")=="GPU utilization" && training.computeText("utilization")=="—" && training.litUtilizationSegments()==0,"compute card never invents a utilization value");
    }
    const QList<QPair<QString,QWidget *>> panels={{"library",&models},{"training",&training},{"knowledge",&knowledge},{"adapters",&adapters},{"chat",&chat},{"settings",&settings}};
    bool allSwitches=true;
    for (const auto &entry : panels)
        for (auto *checkbox : entry.second->findChildren<QCheckBox *>()) allSwitches &= qobject_cast<ToggleSwitch *>(checkbox)!=nullptr;
    check(allSwitches,"every checkbox setting uses the shared switch presentation");
    if (!captures.isEmpty()) QDir().mkpath(captures);
    auto capture = [&](QWidget *widget,const QString &name) {
        if (!captures.isEmpty()) check(widget->grab().save(captures+"/"+name+".png"),"capture "+name);
    };
    {
        QWidget components; components.setObjectName("componentPreview");
        auto *layout=new QVBoxLayout(&components); layout->setContentsMargins(20,20,20,20); layout->setSpacing(14);
        auto *title=new QLabel("Magnite · shared components",&components); title->setObjectName("panelTitle"); layout->addWidget(title);
        auto *description=new QLabel("Short transitions, quiet surfaces and clear keyboard focus.",&components); description->setObjectName("cardMeta"); layout->addWidget(description);
        auto *actions=new QHBoxLayout();
        auto *primary=new Button("Primary action",&components); primary->setObjectName("primaryBtn");
        auto *secondary=new Button("Secondary",&components);
        auto *danger=new Button("Remove",&components); danger->setObjectName("destructiveBtn");
        auto *disabled=new Button("Unavailable",&components); disabled->setEnabled(false);
        for (auto *action : {primary,secondary,danger,disabled}) actions->addWidget(action);
        layout->addLayout(actions);
        auto *fields=new QHBoxLayout();
        auto *input=new QLineEdit(&components); input->setPlaceholderText("Search models…"); input->setAccessibleName("Search models");
        auto *select=new ComboBox(&components); select->addItems({"Balanced","Fast","Quality"});
        auto *number=new SpinBox(&components); number->setRange(1,128); number->setValue(16);
        fields->addWidget(input,2); fields->addWidget(select,1); fields->addWidget(number,1); layout->addLayout(fields);
        auto *tabs=new TabWidget(&components);
        tabs->addTab(new QLabel("Overview of the selected run."),"Overview");
        tabs->addTab(new QLabel("Measured training and validation metrics."),"Metrics");
        tabs->addTab(new QLabel("Settings are saved using the existing controls."),"Settings");
        layout->addWidget(tabs);
        auto *switcher=new ToggleSwitch("Live visualisation",&components); switcher->setChecked(true); layout->addWidget(switcher);
        auto *progress=new ProgressBar(&components); progress->setTextVisible(false); progress->setFixedHeight(8); progress->setValue(64); layout->addWidget(progress);
        components.resize(680,340); components.show(); components.activateWindow(); pump(60);
        QSettings().setValue("appearance/studio3ReducedMotion",false);
        int activations=0; QObject::connect(primary,&QPushButton::clicked,[&] { ++activations; });
        primary->setFocus(Qt::TabFocusReason); key(primary,Qt::Key_Space);
        check(activations==1,"button: keyboard activation retains native click behavior");
        primary->setEnabled(false); primary->click(); check(activations==1,"button: disabled actions cannot activate"); primary->setEnabled(true);
        QEnterEvent enter(QPointF(4,4),QPointF(4,4),primary->mapToGlobal(QPoint(4,4))); QApplication::sendEvent(primary,&enter); pump(30);
        const auto early=primary->grab().toImage(); const bool animating=primary->isAnimating(); pump(200);
        check(animating && !primary->isAnimating() && early!=primary->grab().toImage(),"button: hover transitions change frames and settle");
        tabs->setCurrentIndex(1); pump(30);
        auto *bar=qobject_cast<SegmentedTabBar *>(tabs->tabBar());
        const auto tabEarly=bar->grab().toImage(); const bool tabMoving=bar->isAnimating(); pump(220);
        check(tabs->currentIndex()==1 && tabMoving && !bar->isAnimating() && tabEarly!=bar->grab().toImage(),"tabs: selected pill slides while the native page changes immediately");
        tabs->setCurrentIndex(0); tabs->setTabEnabled(1,false); bar->setFocus(); key(bar,Qt::Key_Right); pump(220);
        check(tabs->currentIndex()==2,"tabs: keyboard navigation skips disabled pages"); tabs->setTabEnabled(1,true);
        progress->setValue(85); pump(30);
        check(progress->value()==85 && progress->isAnimating(),"progress: presentation animates while the real value stays current");
        progress->setValue(0); check(progress->value()==0 && !progress->isAnimating(),"progress: a new operation resets immediately");
        QSettings().setValue("appearance/studio3ReducedMotion",true);
        QEvent leave(QEvent::Leave); QApplication::sendEvent(primary,&leave); progress->setValue(64); tabs->setCurrentIndex(0);
        check(!primary->isAnimating() && !progress->isAnimating() && !bar->isAnimating(),"components: reduced motion settles transitions immediately");
        const auto filledProgress=progress->grab().toImage(); progress->reset();
        check(progress->value()<progress->minimum() && progress->grab().toImage()!=filledProgress,"progress: native reset clears the rendered fill");
        progress->setValue(64);
        input->setFocus(); pump(40);
        QKeyEvent typing(QEvent::KeyPress,Qt::Key_A,Qt::NoModifier,"a"); QApplication::sendEvent(input,&typing);
        auto *halo=input->findChild<QWidget *>("componentFocusHalo");
        check(!halo && input->text()=="a","input: typing works without a highlighting focus border"); input->clear();
        {
        // Compare with an empty render: all changed pixels must stay inside
        // the button's padded content area, even for a constrained long label.
        QSettings().setValue("appearance/studio3ReducedMotion",true);
        Button narrow("A long action label that must stay inside its control");
        narrow.setFixedSize(150,36); narrow.show(); pump(30);
        const auto label=narrow.text();
        const auto content=narrow.labelRect();
        const auto withLabel=narrow.grab().toImage(); narrow.setText(QString());
        const auto empty=narrow.grab().toImage();
        bool contained=true,hasInk=false;
        const qreal scale=withLabel.devicePixelRatio();
        for (int y=0;y<withLabel.height();++y) for (int x=0;x<withLabel.width();++x)
            if (withLabel.pixel(x,y)!=empty.pixel(x,y)) {
                hasInk=true; contained &= content.contains(QPoint(int(x/scale),int(y/scale)));
            }
        check(hasInk && contained && content.left()>=6 && content.right()<narrow.width()-5,"button: long text is contained within its padded content box");
        narrow.setText(label); narrow.grab();
        auto *accessible=QAccessible::queryAccessibleInterface(&narrow);
        check(accessible && accessible->text(QAccessible::Name)==label,"button: shortened visual text retains the complete accessible label");
        narrow.hide();
        Button multiline("A descriptive action\nthat wraps on a narrow card"); multiline.setObjectName("promptSuggestion");
        check(multiline.heightForWidth(160)>multiline.heightForWidth(400),"button: multiline actions request enough height when constrained");
        TabWidget tabs; tabs.addTab(new QWidget(),"Runtime"); tabs.addTab(new QWidget(),"Storage & privacy"); tabs.addTab(new QWidget(),"Appearance");
        tabs.resize(520,120); tabs.show(); tabs.setCurrentIndex(2); tabs.clearFocus(); pump(40);
        auto *bar=tabs.tabBar(); const auto image=bar->grab().toImage();
        QRegion allowed; for (int index=0;index<bar->count();++index) allowed+=bar->tabRect(index).adjusted(10,2,-10,-2);
        bool tabInkContained=true;
        const qreal tabScale=image.devicePixelRatio();
        for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
            const auto pixel=image.pixelColor(x,y);
            if (pixel==QColor(Theme::textPrimary()) || pixel==QColor(Theme::textSecondary()))
                tabInkContained &= allowed.contains(QPoint(int(x/tabScale),int(y/tabScale)));
        }
        check(tabInkContained,"tabs: label ink stays inside each padded tab");
        capture(&tabs,"text-bounds-tabs"); tabs.hide();
        QSettings().setValue("appearance/studio3ReducedMotion",false);
    }
    for (bool dark : {true,false}) {
            Theme::setDark(dark); pump(60); capture(&components,dark?"components-dark":"components-light");
        }
        Theme::setDark(true);
        QMenu menu; menu.addAction("Open run"); auto *unavailable=menu.addAction("Unavailable"); unavailable->setEnabled(false); menu.addSeparator(); auto *rename=menu.addAction("Rename");
        menu.popup(components.mapToGlobal(QPoint(240,80))); pump(50); key(&menu,Qt::Key_Down); key(&menu,Qt::Key_Down);
        check(menu.activeAction()==rename,"menu: keyboard navigation skips disabled actions and separators");
        capture(&menu,"component-menu"); key(&menu,Qt::Key_Escape); check(!menu.isVisible(),"menu: Escape dismisses the menu");
        QSettings().setValue("appearance/studio3ReducedMotion",false); components.hide();
    }
    for (bool dark : {true,false}) {
        Theme::setDark(dark);
        for (int width : {848,1104}) {
            for (const auto &[name,panel] : panels) {
                panel->resize(width,628); panel->show(); pump();
                auto *scroll = panel->findChild<QScrollArea *>("cardScroll");
                check(panel->width() <= width && panel->height() <= 628 && (!scroll || scroll->horizontalScrollBar()->maximum()==0),
                      QString("%1 fits %2px in %3 mode").arg(name).arg(width).arg(dark?"dark":"light"));
                capture(panel,QString("%1-%2-%3").arg(dark?"dark":"light").arg(width).arg(name));
                if (name == "settings") {
                    for (int category = 1; category < 8; ++category) {
                        settings.selectCategory(category); pump();
                        capture(panel, QString("%1-%2-settings-category-%3").arg(dark?"dark":"light").arg(width).arg(category));
                    }
                    settings.selectCategory(3); pump();
                }
                if (width == 848 && (name == "settings" || name == "training")) {
                    if (name=="training") {
                        training.findChild<QPushButton *>("trainingSettingsToggle")->setChecked(true); pump();
                    }
                    auto *combo = panel->findChild<QComboBox *>();
                    if(name=="settings")for(auto *candidate:settings.findChildren<QComboBox *>())if(candidate->accessibleName()=="Default inference engine")combo=candidate;
                    const int previous = combo->currentIndex();
                    combo->setFocus();
                    key(combo, Qt::Key_Down, Qt::AltModifier);
                    pump();
                    check(combo->view()->isVisible(), name + " dropdown opens with keyboard");
                    check(combo->view()->sizeHintForRow(0) >= 32,
                          name + " dropdown has readable row spacing");
                    capture(combo->view()->window(), QString("%1-%2-dropdown")
                            .arg(dark?"dark":"light").arg(name));
                    key(combo->view(), Qt::Key_Escape);
                    check(!combo->view()->isVisible() && combo->currentIndex() == previous,
                          name + " dropdown Escape preserves selection");
                    if (combo->count() > 1) {
                        combo->setCurrentIndex(0);
                        combo->showPopup(); pump();
                        key(combo->view(), Qt::Key_Down);
                        key(combo->view(), Qt::Key_Return);
                        check(combo->currentIndex() == 1 && !combo->view()->isVisible(),
                              name + " dropdown supports keyboard selection");
                        combo->setCurrentIndex(previous);
                    }
                }
                panel->hide();
            }
        }
    }
    Theme::setDark(true);
    models.show(); pump();
    auto cards=models.findChildren<ModelCard *>();
    check(cards.size()==2,"populated model cards");
    if (!cards.isEmpty()) { cards.first()->setFocus(); key(cards.first(),Qt::Key_Space); check(cards.first()->isCardSelected(),"keyboard model selection"); }
    for (auto *edit:models.findChildren<QLineEdit *>())
        if (edit->accessibleName()=="Search models") edit->setText("example");
    QMetaObject::invokeMethod(&models,"onSearch"); pump();
    cards=models.findChildren<ModelCard *>();
    check(cards.size()==1,"search replaces model cards");
    if (!cards.isEmpty()) {
        key(cards.first(),Qt::Key_Space);
        QMetaObject::invokeMethod(&models,"onDownload"); pump();
        check(button(&models,"Download")->isEnabled(),"failed download restores retry action");
    }
    // ---- Hub search paging and "already downloaded" markers ----------------
    {
        auto loadMore = [&models]() -> QPushButton * {
            for (auto *b : models.findChildren<QPushButton *>()) if (b->accessibleName()=="Load more results") return b;
            return nullptr;
        };
        auto setQuery = [&models](const QString &text) {
            for (auto *edit : models.findChildren<QLineEdit *>())
                if (edit->accessibleName()=="Search models") edit->setText(text);
        };
        auto hubCards = [&models]() { return models.findChildren<ModelCard *>(); };
        auto cardFor = [&hubCards](const QString &repo) -> ModelCard * {
            for (auto *c : hubCards()) if (c->id()==repo) return c;
            return nullptr;
        };
        auto hasLabel = [](ModelCard *card,const QString &text) {
            if (!card) return false;
            for (auto *l : card->findChildren<QLabel *>()) if (l->text()==text) return true;
            return false;
        };
        auto actionText = [](ModelCard *card) {
            for (auto *b : card->findChildren<QPushButton *>()) if (b->objectName()=="primaryBtn") return b->text();
            return QString();
        };
        auto lastParam = [&fixture](const QString &name) {
            return QUrlQuery(fixture.searchRequests.last().section('?',1)).queryItemValue(name);
        };
        auto statusText = [&models]() { return models.findChild<QLabel *>("statusLabel")->text(); };
        auto searchDone = [&models]() { for (auto *b : models.findChildren<QPushButton *>()) if (b->text()=="Search") return b->isEnabled(); return false; };

        fixture.hubTotal = 120;
        fixture.downloadedRepos = {"studio/hub-model-001","studio/hub-model-060"};
        fixture.searchRequests.clear();
        setQuery("hub"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        check(hubCards().size()==50,"first search page shows 50 results, not 25");
        check(lastParam("limit")=="50" && lastParam("offset")=="0" && lastParam("q")=="hub","first page requests limit=50 offset=0");
        check(loadMore() && loadMore()->isVisible() && loadMore()->isEnabled(),"full page offers Load more results");
        check(statusText().contains("50") && statusText().contains("1 already downloaded"),"status reports result and downloaded counts");

        { const QSize original=models.size(); models.resize(original.width(),1300); pump(120);
          capture(&models,"models-hub-results"); models.resize(original); pump(); }
        ModelCard *done = cardFor("studio/hub-model-001"), *fresh = cardFor("studio/hub-model-002");
        check(done && done->isDownloaded() && hasLabel(done,"✓ Downloaded") && actionText(done)=="Load model","downloaded result is marked and offers Load model");
        check(fresh && !fresh->isDownloaded() && hasLabel(fresh,"Available on the Hub") && actionText(fresh)=="Download","new result still offers Download");
        check(done && done->localId()=="huggingface:studio--hub-model-001","downloaded result maps to its library id");

        if (done && fresh) {
            key(done,Qt::Key_Space);
            check(!button(&models,"Download")->isEnabled(),"toolbar Download is disabled for a downloaded model");
            key(fresh,Qt::Key_Space);
            check(button(&models,"Download")->isEnabled(),"toolbar Download is enabled for a model not yet downloaded");
            QString loaded; auto conn=QObject::connect(&models,&ModelsPanel::loadRequested,[&loaded](const QString &id){ loaded=id; });
            QMetaObject::invokeMethod(&models,"onDownload"); pump(); // run a real download attempt (fixture rejects it)
            QPushButton *act=nullptr; for (auto *b : done->findChildren<QPushButton *>()) if (b->objectName()=="primaryBtn") act=b;
            if (act) act->click();
            pump();
            check(loaded=="huggingface:studio--hub-model-001","Load model on a downloaded result loads the library model");
            QObject::disconnect(conn);
            const int before=fixture.searchRequests.size();
            check(before>=1,"fixture saw the search request");
        }

        key(cardFor("studio/hub-model-000"),Qt::Key_Space);
        loadMore()->click(); check(loadMore() && !loadMore()->isEnabled() && loadMore()->text()=="Loading…","Load more is disabled and shows progress while a page is loading");
        waitUntil([&]() { return hubCards().size()==100; });
        check(hubCards().size()==100,"Load more appends the next 50 results");
        check(lastParam("offset")=="50","second page requests offset=50");
        check(cardFor("studio/hub-model-000") && cardFor("studio/hub-model-000")->isCardSelected(),"Load more keeps the current selection");
        ModelCard *later = cardFor("studio/hub-model-060");
        check(later && later->isDownloaded() && hasLabel(later,"✓ Downloaded"),"downloaded marker also applies on later pages");
        check(loadMore() && loadMore()->isVisible() && loadMore()->isEnabled(),"Load more is usable again after a page arrives");

        loadMore()->click(); waitUntil([&]() { return hubCards().size()==120; });
        QSet<QString> unique; for (auto *c : hubCards()) unique.insert(c->id());
        check(hubCards().size()==120 && unique.size()==120,"all 120 results are shown exactly once");
        check(!loadMore() || !loadMore()->isVisible(),"short final page hides Load more");

        // A failed page must keep what is on screen and allow a retry.
        setQuery("hub"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        check(hubCards().size()==50 && lastParam("offset")=="0","a new search starts again from the first page");
        fixture.searchStatus=502; loadMore()->click(); waitUntil([&]() { return loadMore()->isEnabled(); });
        check(hubCards().size()==50,"failed page keeps the results already shown");
        check(statusText().contains("Hugging Face Hub") && models.findChild<QLabel *>("statusLabel")->property("state")=="error","failed page explains why");
        check(loadMore()->isEnabled() && loadMore()->isVisible(),"failed page can be retried");
        fixture.searchStatus=200; loadMore()->click(); waitUntil([&]() { return hubCards().size()==100; });
        check(hubCards().size()==100,"retry after failure loads the page");

        // The Hub can reorder between pages: never show the same model twice,
        // and still advance by what the server returned.
        fixture.repeatLastOfPrevPage=true; fixture.hubTotal=400;
        setQuery("hub"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        loadMore()->click(); waitUntil([&]() { return hubCards().size()>=99; });
        unique.clear(); for (auto *c : hubCards()) unique.insert(c->id());
        check(hubCards().size()==99 && unique.size()==99,"overlapping pages do not duplicate cards");
        loadMore()->click(); waitUntil([&]() { return fixture.searchRequests.size()>0 && lastParam("offset")=="100"; });
        check(lastParam("offset")=="100","paging advances by the number the server returned");
        fixture.repeatLastOfPrevPage=false;

        // Paging stops at the backend's offset ceiling instead of erroring.
        fixture.hubTotal=5000; fixture.searchRequests.clear();
        setQuery("hub"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        for (int i=0;i<60 && loadMore() && loadMore()->isVisible();++i) { const int n=hubCards().size(); loadMore()->click(); waitUntil([&]() { return hubCards().size()>n; }); }
        int maxOffset=0; for (const auto &r : fixture.searchRequests) maxOffset=qMax(maxOffset,QUrlQuery(r.section('?',1)).queryItemValue("offset").toInt());
        check(maxOffset<=1000,"offset never exceeds the backend limit of 1000");
        check(!loadMore() || !loadMore()->isVisible(),"Load more disappears at the offset ceiling");

        // Empty and local-view states.
        fixture.emptySearch=true; setQuery("nothing"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        check(hubCards().isEmpty() && (!loadMore() || !loadMore()->isVisible()),"empty search shows no cards and no Load more");
        fixture.emptySearch=false; fixture.hubTotal=120; setQuery("hub"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        check(loadMore() && loadMore()->isVisible(),"Load more is back after a fresh search");
        for (auto *combo : models.findChildren<QComboBox *>()) combo->setCurrentIndex(0);
        pump(200);
        check(!loadMore() || !loadMore()->isVisible(),"switching to Local Models hides Load more");
        fixture.hubTotal=0; fixture.downloadedRepos.clear();
    }
    // ---- Download sizes in search results ------------------------------------
    {
        auto setQuery = [&models](const QString &text) {
            for (auto *edit : models.findChildren<QLineEdit *>())
                if (edit->accessibleName()=="Search models") edit->setText(text);
        };
        auto searchDone = [&models]() { for (auto *b : models.findChildren<QPushButton *>()) if (b->text()=="Search") return b->isEnabled(); return false; };
        auto loadMore = [&models]() -> QPushButton * {
            for (auto *b : models.findChildren<QPushButton *>()) if (b->accessibleName()=="Load more results") return b;
            return nullptr;
        };
        auto stats = [&models](const QString &repo) {
            for (auto *c : models.findChildren<ModelCard *>())
                if (c->id()==repo)
                    for (auto *l : c->findChildren<QLabel *>()) if (l->text().contains("downloads")) return l->text();
            return QString();
        };
        auto cardCount = [&models]() { return models.findChildren<ModelCard *>().size(); };
        QLocale::setDefault(QLocale(QLocale::English,QLocale::UnitedStates));

        // The card on its own: nothing until known, then next to downloads and likes.
        {
            const QJsonObject hit{{"repo_id","a/b"},{"downloads",12},{"likes",3},{"license","mit"}};
            ModelCard card(hit,true);
            auto text=[&card]() { for (auto *l : card.findChildren<QLabel *>()) if (l->text().contains("downloads")) return l->text(); return QString(); };
            check(text()=="12 downloads · 3 likes","a result shows no size until it is known");
            card.setSize(1800000000);
            check(text()=="12 downloads · 3 likes · 1.8 GB","size appears next to downloads and likes");
            card.setSize(412000000);
            check(text()=="12 downloads · 3 likes · 412.0 MB","a later size replaces the earlier one");
            card.setSize(0);
            check(text()=="12 downloads · 3 likes · size unknown","a size the Hub could not give reads size unknown");
            ModelCard local(QJsonObject{{"id","studio/x"},{"source","local"},{"size_mb",5}},false);
            local.setSize(123456789);
            bool leaked=false; for (auto *l : local.findChildren<QLabel *>()) if (l->text().contains("123")) leaked=true;
            check(!leaked,"local cards ignore Hub sizes");
        }

        fixture.hubTotal=120; fixture.downloadedRepos={"studio/hub-model-001"}; fixture.sizesRequests.clear(); fixture.sizesStatus=200;
        fixture.hubSizes={{"studio/hub-model-000",412000000.0},{"studio/hub-model-001",1800000000.0},{"studio/hub-model-003",5000.0}};
        setQuery("size"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        waitUntil([&]() { return stats("studio/hub-model-000").contains("412.0 MB"); });
        check(fixture.sizesRequests.size()==1 && fixture.sizesRequests.first().count("repo=")==50,"one request asks for the sizes of all 50 results");
        check(stats("studio/hub-model-000")=="1000 downloads · 0 likes · 412.0 MB","results show their download size");
        check(stats("studio/hub-model-001").endsWith("· 1.8 GB"),"already-downloaded results show a size too");
        check(stats("studio/hub-model-002").endsWith("· size unknown"),"a result the Hub gave no size for says so");
        check(stats("studio/hub-model-003").endsWith("· 5.0 kB"),"small sizes use the right unit");
        {   // On a card this wide the whole line fits; it must not wrap at a narrow default width.
            bool oneLine=false;
            for (auto *c : models.findChildren<ModelCard *>()) if (c->id()=="studio/hub-model-000")
                for (auto *l : c->findChildren<QLabel *>()) if (l->text().contains("downloads"))
                    oneLine = c->width() > 400 && l->height() < l->fontMetrics().lineSpacing()*3/2;
            check(oneLine,"downloads, likes and size share one line when there is room");
        }
        { const QSize original=models.size(); models.resize(original.width(),900); pump(120);
          capture(&models,"models-hub-sizes"); models.resize(original); pump(); }

        // Load more asks only about the new page.
        loadMore()->click(); waitUntil([&]() { return cardCount()==100; }); waitUntil([&]() { return fixture.sizesRequests.size()==2; });
        const QString second=fixture.sizesRequests.value(1);
        check(second.count("repo=")==50 && !second.contains("hub-model-000") && second.contains("hub-model-050"),"Load more asks for the new results' sizes only");
        waitUntil([&]() { return stats("studio/hub-model-050").contains("size unknown"); });
        check(stats("studio/hub-model-050").endsWith("· size unknown") && stats("studio/hub-model-000").contains("412.0 MB"),"sizes on earlier pages are kept");

        // A failing size lookup never disturbs the results or the status line.
        fixture.sizesStatus=500; fixture.sizesRequests.clear();
        setQuery("size"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        waitUntil([&]() { return fixture.sizesRequests.size()==1; }); pump(150);
        check(cardCount()==50 && stats("studio/hub-model-000")=="1000 downloads · 0 likes","a failed size lookup leaves results without sizes");
        check(models.findChild<QLabel *>("statusLabel")->property("state").toString()!="error","a failed size lookup is not reported as an error");

        // Browsing the local library asks the Hub for nothing.
        fixture.sizesStatus=200; fixture.sizesRequests.clear();
        for (auto *combo : models.findChildren<QComboBox *>()) combo->setCurrentIndex(0);
        pump(300);
        check(fixture.sizesRequests.isEmpty(),"the local library makes no size requests");
        fixture.hubTotal=0; fixture.downloadedRepos.clear(); fixture.hubSizes.clear();
    }
    // ---- Download progress, size and cancel --------------------------------
    {
        auto setQuery = [&models](const QString &text) {
            for (auto *edit : models.findChildren<QLineEdit *>())
                if (edit->accessibleName()=="Search models") edit->setText(text);
        };
        auto searchDone = [&models]() { for (auto *b : models.findChildren<QPushButton *>()) if (b->text()=="Search") return b->isEnabled(); return false; };
        auto cardFor = [&models](const QString &repo) -> ModelCard * {
            for (auto *c : models.findChildren<ModelCard *>()) if (c->id()==repo) return c;
            return nullptr;
        };
        auto toolbarCancel = [&models]() { return models.findChild<QPushButton *>("cancelDownload"); };
        auto cardCancel = [&cardFor](const QString &repo) -> QPushButton * {
            if (auto *card = cardFor(repo))
                for (auto *b : card->findChildren<QPushButton *>()) if (b->accessibleName().startsWith("Cancel download ")) return b;
            return nullptr;
        };
        auto progressText = [&cardFor](const QString &repo) {
            if (auto *card = cardFor(repo))
                for (auto *l : card->findChildren<QLabel *>()) if (l->objectName()=="monoLabel") return l->text();
            return QString();
        };
        auto progressShown = [&cardFor](const QString &repo) {
            auto *card = cardFor(repo);
            auto *bar = card ? card->findChild<QProgressBar *>() : nullptr;
            return bar && bar->isVisible();
        };
        auto statusText = [&models]() { return models.findChild<QLabel *>("statusLabel")->text(); };
        auto statusState = [&models]() { return models.findChild<QLabel *>("statusLabel")->property("state").toString(); };
        auto actionEnabled = [&cardFor](const QString &repo) {
            if (auto *card = cardFor(repo))
                for (auto *b : card->findChildren<QPushButton *>()) if (b->objectName()=="primaryBtn") return b->isEnabled();
            return false;
        };
        const QString repo = "studio/hub-model-002";
        auto startDownload = [&]() {
            fixture.pullMode="run"; fixture.pullStatus="running";
            if (!cardFor(repo)) { fixture.hubTotal=60; setQuery("dl"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone); }
            if (auto *card = cardFor(repo)) key(card,Qt::Key_Space);
            QMetaObject::invokeMethod(&models,"onDownload");
        };
        auto finish = [&](const QString &state) { fixture.pullStatus=state; QMetaObject::invokeMethod(&models,"onPullPoll"); pump(120); };

        // formatBytes: SI units like the Hub, with the locale pinned for a stable check.
        const QLocale previousLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::English,QLocale::UnitedStates));
        check(ModelCard::formatBytes(0)=="0 bytes" && ModelCard::formatBytes(999)=="999 bytes" &&
              ModelCard::formatBytes(1500)=="1.5 kB" && ModelCard::formatBytes(412000000)=="412.0 MB" &&
              ModelCard::formatBytes(1800000000LL)=="1.8 GB" && ModelCard::formatBytes(-5)=="0 bytes","byte sizes are formatted in SI units");

        fixture.hubTotal=60; fixture.downloadedRepos.clear(); fixture.pullDone=412000000; fixture.pullTotal=1800000000; fixture.pullProgress=22.9;
        setQuery("dl"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        check(toolbarCancel() && toolbarCancel()->isHidden() && !cardFor(repo)->findChild<QProgressBar *>()->isVisible(),"no Cancel or progress before a download starts");

        // 1. Running download: bytes, percent, status, Cancel available, nothing else startable.
        startDownload();
        waitUntil([&]() { return progressText(repo).contains("412.0 MB"); });
        check(progressShown(repo) && progressText(repo)=="412.0 MB / 1.8 GB · 22%","card shows downloaded size, total size and percent");
        check(statusText().contains("412.0 MB of 1.8 GB") && statusText().contains("22%"),"status line shows downloaded and total size");
        check(toolbarCancel()->isVisible() && toolbarCancel()->isEnabled(),"Cancel download is offered while downloading");
        check(cardCancel(repo) && cardCancel(repo)->isVisible() && cardCancel(repo)->isEnabled(),"the downloading card has its own Cancel button");
        check(!button(&models,"Download")->isEnabled(),"Download is disabled while a download runs");
        check(!actionEnabled(repo),"the downloading card cannot start the same download again");
        { const QSize original=models.size(); models.resize(original.width(),1300); pump(120);
          capture(&models,"models-downloading"); models.resize(original); pump(); }

        // 2. Cancel: sent once, controls lock, polling continues until the server confirms.
        toolbarCancel()->click(); waitUntil([&]() { return fixture.cancelCalls==1; });
        check(fixture.cancelCalls==1,"Cancel download asks the backend to cancel");
        check(!toolbarCancel()->isEnabled() && cardCancel(repo)->text()=="Cancelling…" && !cardCancel(repo)->isEnabled(),"controls show Cancelling… and cannot be pressed twice");
        check(statusText().contains("Cancelling"),"status explains the download is being cancelled");
        QMetaObject::invokeMethod(&models,"onCancelDownload"); pump(120);
        check(fixture.cancelCalls==1,"a second Cancel is ignored while one is in flight");
        finish("cancelled");
        check(!progressShown(repo) && toolbarCancel()->isHidden(),"cancelled download clears progress and Cancel");
        check(button(&models,"Download")->isEnabled() && actionEnabled(repo),"Download is available again after a cancel, on the toolbar and the card");
        check(statusText().contains("cancelled") && statusState()!="error","status says the download was cancelled");
        check(!models.findChildren<ModelCard *>().isEmpty() && cardCancel(repo) && cardCancel(repo)->isEnabled() && cardCancel(repo)->text()=="Cancel","results stay on screen and the card's Cancel button is reset");
        { const int polls=fixture.pollCount; pump(1000); check(fixture.pollCount==polls,"polling stops once the download is cancelled"); }

        // 3. The card's own Cancel button does the same.
        startDownload(); waitUntil([&]() { return progressText(repo).contains("412.0 MB"); });
        cardCancel(repo)->click(); waitUntil([&]() { return fixture.cancelCalls==2; });
        check(fixture.cancelCalls==2,"card Cancel asks the backend to cancel");
        finish("cancelled");
        check(!progressShown(repo) && toolbarCancel()->isHidden(),"card Cancel ends the download cleanly");

        // 4. Cancel pressed before the start request has answered is not lost.
        startDownload();
        QMetaObject::invokeMethod(&models,"onCancelDownload");
        waitUntil([&]() { return fixture.cancelCalls==3; });
        check(fixture.cancelCalls==3,"Cancel during start-up is delivered once the job exists");
        finish("cancelled");
        check(toolbarCancel()->isHidden() && button(&models,"Download")->isEnabled(),"start-up cancel leaves the panel ready");

        // 5. A cancel the server rejects leaves the download running and Cancel usable.
        fixture.cancelStatus=500; startDownload(); waitUntil([&]() { return progressText(repo).contains("412.0 MB"); });
        toolbarCancel()->click(); waitUntil([&]() { return fixture.cancelCalls==4; }); pump(150);
        check(statusState()=="error" && statusText().contains("Could not cancel"),"failed cancel is reported");
        check(toolbarCancel()->isEnabled() && cardCancel(repo)->text()=="Cancel","failed cancel can be retried");
        check(progressShown(repo),"failed cancel does not hide a download that is still running");
        fixture.cancelStatus=202; toolbarCancel()->click(); waitUntil([&]() { return fixture.cancelCalls==5; });
        finish("cancelled");
        check(toolbarCancel()->isHidden(),"retried cancel succeeds");

        // 6. Unknown total: percent only.
        fixture.pullDone=0; fixture.pullTotal=0; fixture.pullProgress=35;
        startDownload(); waitUntil([&]() { return progressText(repo)=="35%"; });
        check(progressText(repo)=="35%" && !statusText().contains(" of "),"unknown size shows percent only");

        // 7. Leaving the results mid-download keeps Cancel reachable.
        for (auto *combo : models.findChildren<QComboBox *>()) combo->setCurrentIndex(0);
        pump(200);
        check(toolbarCancel()->isVisible() && toolbarCancel()->isEnabled(),"Cancel stays available after switching to Local Models");
        { bool anyDeleteEnabled=false; for (auto *c : models.findChildren<ModelCard *>()) for (auto *b : c->findChildren<QPushButton *>()) if (b->accessibleName().startsWith("Delete model ") && b->isEnabled()) anyDeleteEnabled=true;
          check(!anyDeleteEnabled,"deleting is disabled on every card while a download runs"); }
        toolbarCancel()->click(); waitUntil([&]() { return fixture.cancelCalls==6; });
        finish("cancelled");
        check(toolbarCancel()->isHidden(),"cancel from the Local Models view finishes cleanly");

        // 8. Failure and conflict are explained and recoverable.
        fixture.pullDone=412000000; fixture.pullTotal=1800000000; fixture.pullProgress=22.9; fixture.pullError="Disk full";
        startDownload(); waitUntil([&]() { return progressText(repo).contains("412.0 MB"); });
        finish("error");
        check(statusState()=="error" && statusText().contains("Disk full") && toolbarCancel()->isHidden() && !progressShown(repo),"failed download is explained and controls recover");
        fixture.pullError.clear(); fixture.pullMode="conflict"; fixture.pullStatus="running";
        if (auto *card = cardFor(repo)) key(card,Qt::Key_Space);
        QMetaObject::invokeMethod(&models,"onDownload"); pump(250);
        check(statusState()=="error" && statusText().contains("Another download is in progress"),"a refused start shows the server's reason");
        check(toolbarCancel()->isHidden() && button(&models,"Download")->isEnabled(),"refused start leaves the panel ready");

        // 9. Completion still returns to the library.
        for (auto *combo : models.findChildren<QComboBox *>()) combo->setCurrentIndex(1);
        fixture.pullMode="run"; setQuery("dl"); QMetaObject::invokeMethod(&models,"onSearch"); waitUntil(searchDone);
        startDownload(); waitUntil([&]() { return progressShown(repo); });
        finish("done");
        check(toolbarCancel()->isHidden() && models.findChild<QComboBox *>()->currentIndex()==0,"completed download shows the local library and hides Cancel");
        QLocale::setDefault(previousLocale);
        fixture.hubTotal=0; fixture.pullMode="fail";
    }
    // ---- Deleting models ---------------------------------------------------
    {
        auto toolbarDelete = [&models]() { return button(&models,"Delete"); };
        auto cardFor = [&models](const QString &id) -> ModelCard * {
            for (auto *c : models.findChildren<ModelCard *>()) if (c->id()==id) return c;
            return nullptr;
        };
        auto cardDelete = [&cardFor](const QString &id) -> QPushButton * {
            if (auto *card = cardFor(id))
                for (auto *b : card->findChildren<QPushButton *>()) if (b->accessibleName().startsWith("Delete model ")) return b;
            return nullptr;
        };
        auto statusText = [&models]() { return models.findChild<QLabel *>("statusLabel")->text(); };
        auto statusState = [&models]() { return models.findChild<QLabel *>("statusLabel")->property("state").toString(); };
        auto cardCount = [&models]() { return models.findChildren<ModelCard *>().size(); };
        // Answer the confirmation dialog when it appears, recording what it said.
        auto armDialog = [](bool accept,QString *text) {
            auto *timer = new QTimer; timer->setInterval(20);
            QObject::connect(timer,&QTimer::timeout,[=]() {
                for (auto *w : QApplication::topLevelWidgets())
                    if (auto *d = qobject_cast<QMessageBox *>(w); d && d->isVisible()) {
                        if (text) *text = d->text() + "\n" + d->informativeText();
                        if (accept) { for (auto *b : d->buttons()) if (b->text()=="Delete") { b->click(); break; } }
                        else d->reject();
                        timer->stop(); timer->deleteLater(); return;
                    }
            });
            timer->start();
        };
        int changed=0; auto conn=QObject::connect(&models,&ModelsPanel::modelsChanged,[&changed]() { ++changed; });
        const QString sample="studio/sample-model", layout="studio/long-model-name-for-layout-review-in-a-small-window";

        fixture.removedModels.clear(); fixture.deleteRequests.clear(); fixture.deleteStatus=204;
        for (auto *combo : models.findChildren<QComboBox *>()) combo->setCurrentIndex(0);
        waitUntil([&]() { return cardCount()==2; });
        check(cardCount()==2 && cardDelete(sample) && cardDelete(layout) && cardDelete(sample)->isEnabled(),"every local model card offers Delete");
        { const QSize original=models.size(); models.resize(original.width(),900); pump(120);
          capture(&models,"models-local-delete"); models.resize(original); pump(); }
        check(!toolbarDelete()->isEnabled(),"toolbar Delete needs a selection");

        // Declining changes nothing, and the dialog explains the consequences.
        key(cardFor(sample),Qt::Key_Space);
        QString text; armDialog(false,&text);
        toolbarDelete()->click(); waitUntil([&]() { return !text.isEmpty(); }); pump(100);
        check(text.contains(sample) && text.contains("512 MB"),"confirmation names the model and the space it frees");
        check(text.contains("Hugging Face cache") && text.contains("other tools"),"confirmation warns the Hugging Face cache is shared");
        check(text.contains("1 adapter"),"confirmation warns that adapters depend on the model");
        check(fixture.deleteRequests.isEmpty() && cardCount()==2,"declining the confirmation deletes nothing");

        // A model nothing depends on gets a plain confirmation.
        QString plain; armDialog(false,&plain);
        cardDelete(layout)->click(); waitUntil([&]() { return !plain.isEmpty(); }); pump(100);
        check(plain.contains(layout) && !plain.contains("Hugging Face cache") && !plain.contains("adapter"),"confirmation only mentions what applies");
        check(fixture.deleteRequests.isEmpty(),"declining from a card deletes nothing either");

        // A refusal from the backend is explained and leaves everything in place.
        fixture.deleteStatus=409; QString t409; armDialog(true,&t409);
        cardDelete(sample)->click(); waitUntil([&]() { return fixture.deleteRequests.size()==1; }); pump(150);
        check(statusState()=="error" && statusText().contains("Unload this model before deleting it"),"a refused delete shows the server's reason");
        check(cardCount()==2 && cardDelete(sample)->isEnabled() && toolbarDelete()->isEnabled(),"a refused delete leaves the library and buttons intact");

        // Success removes it, reports what was freed, and tells other panels.
        fixture.deleteStatus=204; changed=0; QString ok; armDialog(true,&ok);
        cardDelete(sample)->click(); waitUntil([&]() { return fixture.deleteRequests.size()==2; });
        waitUntil([&]() { return cardCount()==1; });
        check(fixture.deleteRequests.last()=="/models/"+sample,"Delete asks the backend to delete that model");
        check(cardCount()==1 && !cardFor(sample) && cardFor(layout),"the deleted model leaves the library");
        check(statusText().contains("Deleted") && statusText().contains("512 MB"),"status reports what was deleted and freed");
        check(changed>0,"other panels are told the model list changed");
        check(!toolbarDelete()->isEnabled(),"nothing stays selected after a delete");

        // Already gone: refresh instead of failing.
        fixture.deleteStatus=404; QString gone; armDialog(true,&gone);
        cardDelete(layout)->click(); waitUntil([&]() { return fixture.deleteRequests.size()==3; }); pump(150);
        check(statusText().contains("already") && statusState()!="error","a model that was already removed is not an error");

        QObject::disconnect(conn);
        fixture.removedModels.clear(); fixture.deleteStatus=204;
    }
    knowledge.show(); pump();
    auto docs=knowledge.findChildren<RagDocCard *>();
    check(docs.size()==2,"populated document cards");
    if (docs.size()==2) {
        key(docs[0],Qt::Key_Space); key(docs[1],Qt::Key_Space,Qt::ControlModifier);
        check(docs[0]->isCardSelected() && docs[1]->isCardSelected(),"keyboard document multi-selection");
        key(docs[1],Qt::Key_Space,Qt::ControlModifier);
        check(!docs[1]->isCardSelected(),"keyboard document deselection");
        bool literal=false; for (auto *label:docs[1]->findChildren<QLabel *>()) if(label->text().startsWith("<b>")) literal=label->textFormat()==Qt::PlainText;
        check(literal,"filenames display literally");
    }
    QTimer::singleShot(0,[](){ for(auto *widget:QApplication::topLevelWidgets()) if(auto *dialog=qobject_cast<QMessageBox *>(widget)) dialog->reject(); });
    QMetaObject::invokeMethod(&knowledge,"onClearAll"); pump();
    check(fixture.clears==0,"Cancel preserves Knowledge contents");
    auto *query=knowledge.findChild<QLineEdit *>("documentQuery"); query->setText("What is Magnite?");
    QMetaObject::invokeMethod(&knowledge,"onTestQuery"); pump();
    check(knowledge.findChild<QLabel *>("statusLabel")->text().startsWith("Retrieved"),"document query succeeds");
    fixture.failQuery=true; QMetaObject::invokeMethod(&knowledge,"onTestQuery"); pump();
    check(button(&knowledge,"Search")->isEnabled() && knowledge.findChild<QLabel *>("statusLabel")->property("state")=="error","document query failure recovers controls");
    knowledge.hide(); models.hide();

    int completed=0;
    QObject::connect(&training,&TrainingPanel::trainingCompleted,[&completed](){ ++completed; });
    training.show(); pump();
    auto *start=button(&training,"Start Training");
    auto *lossChart=training.findChild<TrainingChart *>("trainingLossChart");
    auto *runs=training.findChild<QListWidget *>("trainingRuns");
    auto *visualise=training.findChildren<QCheckBox *>().last();
    for (auto *box : training.findChildren<QCheckBox *>()) if (box->text()=="Visualisation") visualise=box;
    check(visualise->isChecked(),"training defaults to visualisation");
    runs->setCurrentRow(0); pump();
    check(lossChart->sampleCount()==6,"Studio restores the most recent saved experiment when idle");
    auto *settingsToggle=training.findChild<QPushButton *>("trainingSettingsToggle");
    auto *archiveToggle=training.findChild<QPushButton *>("trainingArchiveToggle");
    check(settingsToggle && archiveToggle,"training exposes recipe settings and experiment archive");
    auto *workspace=training.findChild<SegmentedTabBar *>("trainingWorkspaceTabs");
    auto *metricTabs=training.findChild<SegmentedTabBar *>("trainingMetricTabs");
    check(workspace && workspace->currentIndex()==0 && workspace->tabText(0)=="Studio",
          "training opens in Studio with Observatory second");
    auto *studio=training.findChild<QWidget *>("trainingStudio");
    auto *recipe=training.findChild<QWidget *>("trainingRecipe");
    check(studio->mapTo(&training,QPoint()).y()<recipe->mapTo(&training,QPoint()).y(),
          "live training studio comes before the next run recipe");
    training.resize(1104,1520);
    settingsToggle->setChecked(true); pump(250);
    capture(&training,"training-iteration2-setup");
    Theme::setDark(false); pump(); capture(&training,"training-iteration2-setup-light");
    Theme::setDark(true);
    settingsToggle->setChecked(false); workspace->setCurrentIndex(1); archiveToggle->setChecked(true); pump(250);
    check(training.findChild<QTableWidget *>("trainingExperimentTable")->isVisible() && lossChart->isVisible(),"experiment table stays alongside the live canvas");
    auto *compareA=training.findChild<QComboBox *>("compareRunA");
    auto *compareB=training.findChild<QComboBox *>("compareRunB");
    compareA->setCurrentIndex(1); compareB->setCurrentIndex(2); pump(650);
    auto *comparisonLoss=training.findChild<TrainingChart *>("comparisonLossChart");
    check(comparisonLoss->sampleCount()==7,"comparison overlays both measured histories without interpolation");
    auto *comparisonTable=training.findChild<QTableWidget *>("trainingComparison");
    check(comparisonTable->item(8,1)->text()=="8" && comparisonTable->item(8,2)->text()=="16",
          "comparison shows each run's saved rank rather than current setup");
    check(comparisonTable->item(6,2)->text()=="—","comparison leaves missing validation unmeasured");
    auto *method=training.findChild<QComboBox *>("trainingMethod");
    check(!(method->model()->flags(method->model()->index(2,0)) & Qt::ItemIsEnabled),"full fine-tuning is unavailable in the method chooser");
    if (auto *scroll=training.findChild<QScrollArea *>("cardScroll"))
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    capture(&training,"training-iteration2-compare");
    Theme::setDark(false); pump(); capture(&training,"training-iteration2-compare-light");
    Theme::setDark(true);
    compareB->setCurrentIndex(1); pump();
    check(comparisonLoss->sampleCount()==0,"comparison requires distinct runs");
    check(lossChart->sampleCount()==6,"invalid comparison removes the stale baseline from the primary chart");
    compareB->setCurrentIndex(2);
    training.refreshOptions(); pump();
    check(compareB->currentData().toString()=="older-job","comparison selection survives history refresh by identity");
    archiveToggle->setChecked(false);
    const auto preset=QJsonDocument::fromJson(training.settingsJson().toUtf8()).object();
    auto changed=preset; changed["dropout"]=0.12; changed["validationSplit"]=10;
    training.applySettingsJson(QString::fromUtf8(QJsonDocument(changed).toJson()));
    QMetaObject::invokeMethod(&training,"onStart"); QMetaObject::invokeMethod(&training,"onStart"); pump();
    check(fixture.starts==1 && !start->isEnabled(),"training ignores duplicate starts");
    check(runs->isEnabled(),"past run review remains available during active training");
    check(method->isEnabled() && settingsToggle->isEnabled(),"next recipe remains editable while the active run keeps its saved settings");
    check(lossChart->sampleCount()==6 && lossChart->accessibleDescription().contains("measured samples"),"live loss chart uses raw history including validation");
    check(training.findChild<QLabel *>("trainingConfiguration")->text().contains("LoRA rank: 8"),
          "run context uses the saved backend configuration");
    metricTabs->setCurrentIndex(1); pump(220);
    check(!lossChart->isVisible(),"metric tabs select a single primary chart");
    metricTabs->setCurrentIndex(0); workspace->setCurrentIndex(1); pump(650);
    check(training.findChild<QWidget *>("trainingObservatorySignals")->isVisible() && runs->isVisible(),
          "Observatory exposes secondary signals and experiment history");
    capture(&training,"training-observatory-native");
    Theme::setDark(false); pump(); capture(&training,"training-observatory-native-light"); Theme::setDark(true);
    workspace->setCurrentIndex(0); pump();
    check(!training.findChild<QWidget *>("trainingObservatorySignals")->isVisible(),
          "Studio restores the focused primary signal view");
    check(fixture.trainingRequest["dropout"].toDouble()==0.12 && fixture.trainingRequest["validation_split"].toDouble()==0.1,"training sends configured dropout and validation fraction");
    QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    check(lossChart->sampleCount()==6,"repeated status polls do not duplicate chart samples");
    pump(600);
    capture(&training,"training-running");
    visualise->setChecked(false); pump();
    check(training.findChild<QPlainTextEdit *>("trainingRawData")->isVisible() && !lossChart->isVisible(),"raw data toggle replaces visualisation");
    check(QJsonDocument::fromJson(training.findChild<QPlainTextEdit *>("trainingRawData")->toPlainText().toUtf8()).object()["history"].toArray().size()==6,"raw view contains actual run history");
    visualise->setChecked(true); pump(600);
    training.resize(1104,1240); pump();
    capture(&training,"training-running-full");
    Theme::setDark(false); pump(); capture(&training,"training-running-full-light");
    Theme::setDark(true); training.resize(1104,628); pump();
    check(training.findChild<QWidget *>("trainingCharts")->isVisible(),"Studio keeps the selected chart visible in a compact window");
    fixture.trainingStatusCode=503;
    QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    check(!start->isEnabled() && lossChart->sampleCount()==6,"status outage keeps run tracked and preserves measurements");
    fixture.trainingStatusCode=200;
    fixture.omitTrainingHistory=true;
    QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    check(lossChart->sampleCount()==0,"absent telemetry stays empty without fabricated samples");
    fixture.omitTrainingHistory=false;
    QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    QMetaObject::invokeMethod(&training,"onStop"); pump(); QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    check(start->isEnabled() && completed==0 && training.findChild<QLabel *>("metricValue")->text()=="Training stopped","cancelled training resets controls without reporting completion");
    auto *reuse=training.findChild<QPushButton *>("trainingReuseRun");
    check(reuse->isEnabled() && lossChart->sampleCount()==6,"stopped run retains measurements and can seed another recipe");
    reuse->click(); pump();
    auto *reuseDialog=training.findChild<QDialog *>("trainingRunDialog");
    check(reuseDialog->findChild<QSpinBox *>("trainingDraft_rank")->value()==8 &&
          reuseDialog->findChild<QLineEdit *>("trainingDraft_adapterName")->text()=="support-tone-v2-next",
          "use as next run creates a draft of the saved settings with a new name");
    reuseDialog->reject();
    check(training.findChild<QLabel *>("trainingConfiguration")->text().contains("LoRA rank: 8"),
          "next recipe editing leaves the previous run context intact");
    auto *runSearch=training.findChild<QLineEdit *>("trainingRunSearch");
    runSearch->setText("no-such-experiment"); pump();
    bool allHidden=true; for(int i=0;i<runs->count();++i) allHidden &= runs->item(i)->isHidden();
    check(allHidden,"experiment search filters the local run list"); runSearch->clear();
    fixture.failTraining=true; QMetaObject::invokeMethod(&training,"onStart"); pump();
    check(start->isEnabled() && training.findChild<QLabel *>("statusLabel")->property("state")=="error","training submission failure is recoverable");
    fixture.failTraining=false; fixture.trainingState="done"; QMetaObject::invokeMethod(&training,"onStart"); pump();
    check(start->isEnabled() && completed==1,"training completion resets controls");
    fixture.trainingState="running"; fixture.serveActiveTraining=true;
    training.refreshOptions(); pump();
    check(!start->isEnabled() && lossChart->sampleCount()==6,"training page reconnects to an existing active backend job");
    fixture.serveActiveTraining=false;
    QMetaObject::invokeMethod(&training,"onStop"); pump();
    QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    check(start->isEnabled() && completed==1,"reattached run supports cancellation");
    settingsToggle->setChecked(true);
    training.resize(848,628); pump();
    bool trainingFits=true; for(auto *scroll:training.findChildren<QScrollArea *>()) trainingFits &= scroll->horizontalScrollBar()->maximum()==0;
    if (!trainingFits) for(auto *scroll:training.findChildren<QScrollArea *>()) {
        if (!scroll->horizontalScrollBar()->maximum()) continue;
        fprintf(stderr,"[ui-check] overflow: viewport=%d widget=%d minimum=%d hint=%d\n",scroll->viewport()->width(),scroll->widget()->width(),scroll->widget()->minimumSizeHint().width(),scroll->widget()->sizeHint().width());
        for(auto *child:scroll->widget()->findChildren<QWidget *>())
            if(child->minimumSizeHint().width()>scroll->viewport()->width()-30)
                fprintf(stderr,"[ui-check] wide child: %s %s minimum=%d hint=%d\n",child->metaObject()->className(),qUtf8Printable(child->objectName()),child->minimumSizeHint().width(),child->sizeHint().width());
    }
    check(trainingFits,"expanded training settings fit without horizontal scrolling");
    capture(&training,"training-expanded-compact"); training.hide();
    TrainingRunPanel runReview; runReview.setBackendClient(&client);
    check(runReview.findChild<ToggleSwitch *>()!=nullptr,"run review uses the shared switch presentation");
    QString requestedRun;
    QObject::connect(&training,&TrainingPanel::runDetailsRequested,[&](const QString &id) { requestedRun=id; });
    QMetaObject::invokeMethod(runs,"itemActivated",Q_ARG(QListWidgetItem *,runs->item(0)));
    check(requestedRun=="past-job","past run activation opens run review by job identity");
    QObject::connect(&adapters,&AdaptersPanel::runDetailsRequested,[&](const QString &id) { requestedRun=id; });
    if (auto *viewRun=button(&adapters,"View run data")) viewRun->click();
    check(requestedRun=="fixture-job","adapter action opens its linked training run");
    fixture.trainingState="done";
    runReview.resize(1104,1000); runReview.show(); runReview.openRun("fixture-job"); pump(650);
    check(runReview.findChild<QPlainTextEdit *>("runRawData")->toPlainText().contains("support-tone-v2"),"run review loads the requested run and configuration");
    capture(&runReview,"training-run-review");
    for (auto *box : runReview.findChildren<QCheckBox *>()) if (box->text()=="Visualisation") box->setChecked(false);
    check(runReview.findChild<QPlainTextEdit *>("runRawData")->isVisible(),"run review supports raw data mode");
    runReview.hide();
    TrainingChart animated("loss"); animated.resize(420,180); animated.show();
    QSettings().setValue("appearance/studio3ReducedMotion",false);
    QJsonArray animationSamples{QJsonObject{{"step",1},{"loss",2.0}},QJsonObject{{"step",10},{"loss",1.0}}};
    animated.setHistory(animationSamples,10); pump(60);
    check(animated.isAnimating(),"new measurements animate into the chart");
    const auto early=animated.grab().toImage(); pump(650);
    check(!animated.isAnimating() && early!=animated.grab().toImage(),"chart animation changes frames and settles");
    animated.setHistory(animationSamples,10);
    check(!animated.isAnimating(),"unchanged polls do not replay animation");
    QSettings().setValue("appearance/studio3ReducedMotion",true);
    animationSamples.append(QJsonObject{{"step",11},{"loss",0.9}}); animated.setHistory(animationSamples,12);
    check(!animated.isAnimating(),"reduced motion displays measurements immediately");
    // Simulate a long run after the backend has trimmed its early history.
    QJsonArray retained;
    for (int step=6900; step<=7900; step+=10)
        retained.append(QJsonObject{{"step",step},{"loss",1.2 + (step%30)*0.01},
                                   {"eval_loss",1.1}});
    animated.setHistory(retained,20000);
    const QPointF middle(230,85);
    QMouseEvent retainedHover(QEvent::MouseMove,middle,middle,Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&animated,&retainedHover);
    auto *retainedReadout=animated.findChild<QLabel *>("trainingChartHover");
    const int hoveredStep=retainedReadout->text().section(' ',1,1).toInt();
    check(hoveredStep>7200 && hoveredStep<7600,"long-run hover maps the middle of the plot to the retained step window");
    capture(&animated,"training-retained-history");
    const auto retainedImage=animated.grab().toImage().scaled(animated.size());
    bool curveAtLeft=false;
    const QColor series(Theme::accentPrimary());
    for (int x=80; x<140; ++x) for (int y=36; y<145; ++y) {
        const QColor pixel=retainedImage.pixelColor(x,y);
        if (qAbs(pixel.red()-series.red())<12 && qAbs(pixel.green()-series.green())<12 && qAbs(pixel.blue()-series.blue())<12)
            curveAtLeft=true;
    }
    check(curveAtLeft,"retained long-run curve fills the left side of the plot");
    animated.findChild<Button *>("trainingChartExpand")->click(); pump();
    auto *expandedDialog=animated.findChild<QDialog *>("trainingChartExpanded");
    auto *expandedChart=expandedDialog?expandedDialog->findChild<TrainingChart *>():nullptr;
    check(expandedDialog && expandedDialog->isVisible() && !expandedDialog->isFullScreen() &&
          expandedChart && expandedChart->width()>animated.width() && animated.width()==420,
          "expand opens only the graph in a larger window");
    if (expandedChart) {
        capture(expandedDialog,"training-expanded-graph");
        retained.append(QJsonObject{{"step",7910},{"loss",0.8}});
        animated.setHistory(retained,20000);
        check(expandedChart->sampleCount()==animated.sampleCount(),"expanded graph follows live history updates");
        expandedDialog->resize(1000,600); pump();
        check(expandedChart->width()>900,"expanded graph follows window resizing");
        expandedDialog->close(); pump();
        animated.findChild<Button *>("trainingChartExpand")->click(); pump();
        check(animated.findChild<QDialog *>("trainingChartExpanded")->isVisible(),"expanded graph can be closed and reopened");
        animated.findChild<QDialog *>("trainingChartExpanded")->close(); pump();
    }
    QSettings().setValue("appearance/studio3ReducedMotion",false); animated.hide();

    chat.show(); pump();
    auto *input=chat.findChild<QPlainTextEdit *>("chatInput"); auto *send=chat.findChild<QPushButton *>("primaryBtn");
    check(!send->isEnabled(),"empty chat cannot be sent");
    input->setPlainText("How can I use my documents?"); send->click(); pump();
    check(chat.findChild<QWidget *>("chatActivity")->isHidden() && chat.findChildren<ChatBubble *>().size()==2,"chat success ends animation and displays response");
    capture(&chat,"chat-conversation");
    fixture.holdChat=true; input->setPlainText("Continue"); send->click(); pump();
    check(!chat.findChild<QWidget *>("chatActivity")->isHidden() && !button(&chat,"Load model")->isEnabled(),"streaming prevents overlapping model loads");
    QMetaObject::invokeMethod(&chat,"onClearChat"); pump();
    check(chat.findChild<QWidget *>("chatActivity")->isHidden() && chat.findChild<QLabel *>("emptyConversation")->isVisible(),"new chat stops a live stream and restores welcome");
    QWidget host; host.resize(350,700);
    ChatBubble token(ChatBubble::Assistant,QString(600,'x'),&host); host.show(); pump();
    check(token.width()<=350 && token.height()>150,"long unbroken responses wrap without clipping height");
    host.hide(); chat.hide();

    {
        // Earlier streaming/clear-chat callbacks must not enter the debounce
        // request counts. Give these checks their own panel and HTTP fixture.
        Fixture fixture;
        if (!fixture.listen(QHostAddress::LocalHost,0)) {
            check(false,"context usage fixture server: "+fixture.errorString());
            return 1;
        }
        BackendClient client;
        client.setBaseUrl(QString("http://127.0.0.1:%1").arg(fixture.serverPort()));
        ChatPanel chat;
        chat.resize(1104,628);
        chat.setBackendClient(&client);
        QSettings().remove("chat/meterStyle"); QSettings().remove("chat/autoCompactPercent");
        QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection);
        chat.reloadContextSettings(); chat.show(); pump();
        auto *ring=chat.findChild<QWidget *>("contextRing"); auto *bar=chat.findChild<QWidget *>("contextBar"); auto *banner=chat.findChild<QWidget *>("contextBanner");
        auto *input=chat.findChild<QPlainTextEdit *>("chatInput"); auto *statusLine=chat.findChild<QLabel *>("statusLabel");
        auto *tokensSpin=chat.findChild<QSpinBox *>(); auto *sendButton=button(&chat,"Send  ↑");
        check(ring && bar && banner && input && statusLine && tokensSpin && sendButton,"chat panel hosts the context meter pieces");
        if (ring && bar && banner && input && statusLine && tokensSpin && sendButton) {
            auto lastUsage=[&fixture]() { return QJsonDocument::fromJson(fixture.usageRequests.value(fixture.usageRequests.size()-1).toUtf8()).object(); };
            auto requests=[&fixture]() { return int(fixture.usageRequests.size()); };
            auto status=[statusLine]() { return statusLine->text(); };
            fixture.usageUsed=1000; fixture.usageLimit=4096; fixture.usageStatus=200;
            int before=requests();
            chat.refreshEngineStatus();
            check(waitUntil([&]{ return requests()>before && ring->isVisible(); }),"meter appears once a model is known to be loaded");
            check(ring->toolTip().contains("1,000 of 4,096") && ring->toolTip().contains("256 reserved") && !bar->isVisible() && !banner->isVisible(),"default Ring shows the numbers on hover; bar and banner stay hidden");
            check(lastUsage().value("max_tokens").toInt()==256,"usage request carries the Response tokens reserve");

            fixture.usageUsed=3000; chat.refreshContextUsage();
            check(waitUntil([&]{ return status().contains("getting full"); }) && statusLine->property("state")=="warn","70-90% shows the warning in the status line");
            fixture.usageUsed=3900; chat.refreshContextUsage();
            check(waitUntil([&]{ return status().contains("almost full"); }),"above 90% the warning says it is almost full");

            fixture.usageUsed=1000; before=requests();
            input->setPlainText("a"); input->setPlainText("ab"); input->setPlainText("abc"); pump(100);
            check(requests()==before,"typing waits for a pause before asking for usage");
            check(waitUntil([&]{ return requests()==before+1; }),"typing asks for usage once after the pause");
            pump(700);
            check(requests()==before+1,"quick typing sends exactly one usage request");
            const QJsonArray typed=lastUsage().value("messages").toArray();
            check(!typed.isEmpty() && typed.last().toObject().value("content").toString()=="abc" && typed.last().toObject().value("role").toString()=="user","the typed draft is counted as a pending user message");
            input->clear();

            fixture.usageUsed=1000; fixture.holdUsageOnce=true; before=requests();
            chat.refreshContextUsage(); waitUntil([&]{ return requests()==before+1; });
            fixture.usageUsed=2000; chat.refreshContextUsage();
            check(waitUntil([&]{ return ring->toolTip().contains("2,000 of"); }),"the newer usage reply is shown");
            fixture.releaseHeldUsage(); pump(300);
            check(ring->toolTip().contains("2,000 of"),"a late, older usage reply is ignored");

            fixture.usageStatus=409; chat.refreshContextUsage();
            check(waitUntil([&]{ return !ring->isVisible(); }),"a backend that reports no model hides the meter");
            input->setPlainText("still sendable"); pump(50);
            check(sendButton->isEnabled() && statusLine->property("state")!="error","no usage never blocks sending or reports an error");
            input->clear(); fixture.usageStatus=200;

            QSettings().setValue("chat/meterStyle","bar"); chat.reloadContextSettings(); fixture.usageUsed=1000;
            check(waitUntil([&]{ return bar->isVisible() && !ring->isVisible(); }),"switching the setting to Bar swaps the meter without restarting");
            QSettings().setValue("chat/meterStyle","off"); chat.reloadContextSettings(); pump(100);
            check(!bar->isVisible() && !ring->isVisible() && !banner->isVisible(),"Off hides every meter piece");
            QSettings().setValue("chat/meterStyle","ringBanner"); chat.reloadContextSettings(); fixture.usageUsed=3900; chat.refreshContextUsage();
            check(waitUntil([&]{ return banner->isVisible(); }),"Ring + banner shows the banner when nearly full");
            pump(100); check(!status().contains("almost full"),"with the banner showing, the status line does not repeat the warning");
            QSettings().remove("chat/meterStyle"); chat.reloadContextSettings(); fixture.usageUsed=1000; chat.refreshContextUsage();
            check(waitUntil([&]{ return ring->isVisible() && !banner->isVisible(); }),"removing the setting returns to Ring");

            fixture.usageLimit=2048; tokensSpin->setValue(8192);
            check(waitUntil([&]{ return status().contains("Reduce Response tokens"); }),"Response tokens larger than the window asks to reduce them");
            fixture.usageLimit=4096; tokensSpin->setValue(256); pump(700);

            before=requests();
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection);
            check(waitUntil([&]{ return requests()>before; }) && lastUsage().value("messages").toArray().isEmpty(),"clearing the chat measures an empty conversation");
            if (!captures.isEmpty()) {
                const bool wasDark=Theme::isDark();
                chat.resize(1104,628);
                struct Shot { const char *style; int used; const char *name; };
                for (bool dark : {true,false}) {
                    Theme::setDark(dark);
                    for (const Shot &shot : {Shot{"ring",1000,"ring-ok"},Shot{"ring",3000,"ring-warn"},Shot{"ringBanner",3900,"banner-critical"},Shot{"bar",3000,"bar-warn"}}) {
                        QSettings().setValue("chat/meterStyle",shot.style); chat.reloadContextSettings();
                        fixture.usageUsed=shot.used; const int n=requests(); chat.refreshContextUsage();
                        waitUntil([&]{ return requests()>n; }); pump(150);
                        capture(&chat,QString("chat-meter-%1-%2").arg(shot.name,dark?"dark":"light"));
                    }
                }
                Theme::setDark(wasDark);
                QSettings().remove("chat/meterStyle"); chat.reloadContextSettings();
            }
        }
        chat.hide();
    }
    {
        QSettings().remove("chat/meterStyle"); QSettings().remove("chat/autoCompactPercent");
        chat.reloadContextSettings();
        QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection);
        chat.show(); pump();
        auto *input=chat.findChild<QPlainTextEdit *>("chatInput"); auto *statusLine=chat.findChild<QLabel *>("statusLabel");
        auto *sendButton=button(&chat,"Send  ↑"); auto *stopButton=button(&chat,"Stop"); auto *ring=chat.findChild<QWidget *>("contextRing");
        auto *tokensSpin=chat.findChild<QSpinBox *>();
        check(input && statusLine && sendButton && stopButton && ring && tokensSpin,"compaction checks can find the chat controls");
        if (input && statusLine && sendButton && stopButton && ring && tokensSpin) {
            fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); }
            fixture.usageStatus=200; fixture.usageUsed=1000; fixture.usageLimit=4096; fixture.compactStatus=200;
            fixture.compactMethod="summary"; fixture.compactCovered=4; fixture.holdCompact=false;
            fixture.compactRequests.clear(); fixture.chatRequests.clear();
            auto sendAndWait=[&](const QString &text) {
                const int n=fixture.chatRequests.size();
                input->setPlainText(text); pump(20); sendButton->click();
                const bool sent=waitUntil([&]{ return fixture.chatRequests.size()>n && sendButton->text()=="Send  ↑"; });
                pump(150); return sent;
            };
            auto json=[](const QStringList &list) { return QJsonDocument::fromJson(list.value(list.size()-1).toUtf8()).object(); };
            auto dividers=[&chat]() { return chat.findChildren<QLabel *>("contextDivider"); };
            auto status=[statusLine]() { return statusLine->text(); };

            bool seeded=true; for (int i=0;i<4;++i) seeded&=sendAndWait(QString("turn %1").arg(i));
            check(seeded && fixture.chatRequests.size()==4,"four ordinary turns are sent");
            check(fixture.compactRequests.isEmpty() && dividers().isEmpty(),"below the threshold nothing is compacted");

            fixture.usageUsed=3600; // (3600+256)/4096 = 94%, above the 85% default
            check(sendAndWait("turn 4"),"a send at the threshold still goes through");
            check(fixture.compactRequests.size()==1,"a send at or above the threshold compacts exactly once");
            {
                const QJsonObject request=json(fixture.compactRequests); const QJsonArray posted=request.value("messages").toArray();
                check(request.value("keep_recent").toInt()==4 && qAbs(request.value("target_fill").toDouble()-0.85)<1e-9 && request.value("previous_summary").toString().isEmpty() && request.value("max_tokens").toInt()==256,
                      "compact request carries keep_recent 4, the 85% target, no earlier summary and the reserve");
                check(posted.size()==9 && posted.last().toObject().value("content").toString()=="turn 4","the new message is part of what is compacted, as the last message");
                const QJsonArray sent=json(fixture.chatRequests).value("messages").toArray();
                check(sent.size()==6 && sent.at(0).toObject().value("role").toString()=="system" &&
                      sent.at(0).toObject().value("content").toString()=="Summary of the earlier conversation: The user asked about X." &&
                      sent.at(1).toObject().value("content").toString()=="turn 2" && sent.last().toObject().value("content").toString()=="turn 4",
                      "the model receives the summary plus only the messages after the compacted ones");
            }
            check(dividers().size()==1 && dividers().first()->text().contains("summarized"),"a divider says the earlier messages were summarized");
            check(chat.findChildren<ChatBubble *>().size()==10,"every earlier message stays on screen");

            fixture.compactCovered=3;
            check(sendAndWait("turn 5"),"a second compaction goes through");
            {
                const QJsonObject request=json(fixture.compactRequests); const QJsonArray posted=request.value("messages").toArray();
                check(fixture.compactRequests.size()==2 && request.value("previous_summary").toString()=="The user asked about X." && posted.size()==7 && posted.at(0).toObject().value("content").toString()=="turn 2",
                      "a repeat compaction posts only the uncompacted tail and the earlier summary");
                check(json(fixture.chatRequests).value("messages").toArray().size()==5,"covered messages accumulate across compactions");
            }

            fixture.compactMethod="dropped"; fixture.compactCovered=2;
            check(sendAndWait("turn 6"),"a fallback that drops old messages still sends");
            check(dividers().size()==3 && dividers().last()->text().contains("dropped"),"the divider says messages were dropped when the fallback ran");
            {
                const QJsonArray sent=json(fixture.chatRequests).value("messages").toArray();
                check(sent.size()==5 && sent.at(0).toObject().value("content").toString()=="Summary of the earlier conversation: The user asked about X.","dropping keeps the earlier summary and advances the covered count");
            }

            fixture.compactMethod="summary"; fixture.compactCovered=0;
            const int dividersBefore=dividers().size();
            check(sendAndWait("turn 7") && dividers().size()==dividersBefore,"a compaction that covers nothing adds no divider");

            fixture.compactCovered=4; fixture.compactStatus=500;
            check(sendAndWait("turn 8") && dividers().size()==dividersBefore+1 && dividers().last()->text().contains("Could not compact"),"a failed compaction says so in the transcript and sends anyway");
            check(json(fixture.chatRequests).value("messages").toArray().at(0).toObject().value("role").toString()=="system","after a failed compaction the earlier summary is still sent");
            fixture.compactStatus=200;

            const int compactsBeforeUnknown=fixture.compactRequests.size();
            fixture.usageStatus=409;
            check(sendAndWait("turn 9") && fixture.compactRequests.size()==compactsBeforeUnknown,"when usage is unknown the message is sent without compacting");
            check(json(fixture.chatRequests).value("messages").toArray().at(0).toObject().value("role").toString()=="system","the existing summary survives a model/usage hiccup");
            fixture.usageStatus=200;

            QSettings().setValue("chat/autoCompactPercent",0); chat.reloadContextSettings(); fixture.usageUsed=3900;
            const int compactsBeforeOff=fixture.compactRequests.size();
            check(sendAndWait("turn 10") && fixture.compactRequests.size()==compactsBeforeOff,"auto-compact Off never compacts, even when nearly full");
            QSettings().setValue("chat/autoCompactPercent",90); chat.reloadContextSettings(); fixture.usageUsed=3600; // (3600+256)/4096 = 94% still >= 90
            const int compactsAt90=fixture.compactRequests.size();
            check(sendAndWait("turn 11") && fixture.compactRequests.size()==compactsAt90+1 && qAbs(json(fixture.compactRequests).value("target_fill").toDouble()-0.90)<1e-9,"the chosen percentage is used as the target");
            QSettings().remove("chat/autoCompactPercent"); chat.reloadContextSettings();

            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            check(dividers().isEmpty(),"clearing the chat removes the dividers");
            check(sendAndWait("fresh start"),"a new chat sends");
            {
                const QJsonArray sent=json(fixture.chatRequests).value("messages").toArray();
                check(sent.size()==1 && sent.at(0).toObject().value("role").toString()=="user","clearing the chat also forgets the summary");
            }

            fixture.usageUsed=3900;
            const int compactsShort=fixture.compactRequests.size();
            check(sendAndWait("short one") && fixture.compactRequests.size()==compactsShort,"a short history is never compacted, even when nearly full");

            bool grown=true; fixture.usageUsed=1000; for (int i=0;i<3;++i) grown&=sendAndWait(QString("more %1").arg(i));
            fixture.usageLimit=2048; tokensSpin->setValue(8192); pump(700); fixture.usageUsed=3900;
            const int compactsReserve=fixture.compactRequests.size();
            check(grown && sendAndWait("reserve too big") && fixture.compactRequests.size()==compactsReserve,"Response tokens at or above the window never start a compaction");
            fixture.usageLimit=4096; tokensSpin->setValue(256); pump(700);

            fixture.usageUsed=3600; fixture.holdCompact=true;
            const int chatsBeforeStop=fixture.chatRequests.size(), compactsBeforeStop=fixture.compactRequests.size();
            input->setPlainText("stop me"); pump(20); sendButton->click();
            check(waitUntil([&]{ return fixture.compactRequests.size()==compactsBeforeStop+1 && fixture.heldCompactSocket; }) && status().contains("Compacting") && stopButton->isEnabled() && !sendButton->isEnabled(),
                  "while compacting, status says so, Stop is available and Send is not");
            stopButton->click(); pump(300);
            check(input->toPlainText()=="stop me" && sendButton->isEnabled() && status().contains("Compaction stopped") && fixture.chatRequests.size()==chatsBeforeStop,
                  "Stop during compaction keeps the typed text, re-enables Send and sends nothing");
            fixture.holdCompact=false; fixture.heldCompactSocket.clear();
            check(sendAndWait("stop me") && fixture.compactRequests.size()==compactsBeforeStop+2 && json(fixture.chatRequests).value("messages").toArray().last().toObject().value("content").toString()=="stop me",
                  "after stopping a compaction the same message can be sent");

            QSettings().setValue("chat/meterStyle","off"); chat.reloadContextSettings(); fixture.usageUsed=3600; fixture.holdCompact=true;
            const int compactsOff=fixture.compactRequests.size();
            input->setPlainText("quiet compaction"); pump(20); sendButton->click();
            check(waitUntil([&]{ return fixture.compactRequests.size()==compactsOff+1; }) && status().contains("Compacting") && !ring->isVisible(),
                  "with the meter Off, compaction still says Compacting… and shows no meter");
            stopButton->click(); pump(300); fixture.holdCompact=false; fixture.heldCompactSocket.clear();
            QSettings().remove("chat/meterStyle"); chat.reloadContextSettings();

            // Cancelling a compaction aborts only that request.
            fixture.usageUsed=3600; fixture.holdCompact=true; fixture.holdOther=true; fixture.heldCompactSocket.clear();
            int otherStatus=-1; client.get("/held-other",[&otherStatus](int code,const QJsonDocument &){ otherStatus=code; });
            const int compactsNarrow=fixture.compactRequests.size();
            input->setPlainText("cancel narrowly"); pump(20); sendButton->click();
            check(waitUntil([&]{ return fixture.heldOtherSocket && fixture.compactRequests.size()==compactsNarrow+1 && fixture.heldCompactSocket; }),"a compaction and an unrelated request are in flight together");
            stopButton->click(); pump(300);
            check(otherStatus==-1,"stopping a compaction leaves other in-flight requests alone");
            fixture.releaseHeldOther(); pump(200);
            check(otherStatus==200,"the unrelated request still completes normally");
            fixture.holdOther=false; fixture.holdCompact=false; input->clear();

            // Starting a new chat while compacting cancels the compaction; the late reply changes nothing.
            fixture.holdCompact=true; fixture.heldCompactSocket.clear(); fixture.usageUsed=3600;
            const int chatsBeforeClear=fixture.chatRequests.size(), compactsBeforeClear=fixture.compactRequests.size();
            input->setPlainText("clear while compacting"); pump(20); sendButton->click();
            check(waitUntil([&]{ return fixture.compactRequests.size()==compactsBeforeClear+1 && fixture.heldCompactSocket; }),"a compaction is in flight before the chat is cleared");
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            fixture.releaseHeldCompact(); pump(300);
            check(fixture.chatRequests.size()==chatsBeforeClear && dividers().isEmpty() && sendButton->isEnabled(),"clearing the chat during compaction cancels it and a late reply changes nothing");
            fixture.holdCompact=false; input->clear();

            // Transcript rows: the screen is a drawing of the conversation tree's active path.
            fixture.usageUsed=1000; fixture.compactStatus=200; fixture.compactMethod="summary"; fixture.compactCovered=4;
            fixture.chatRequests.clear(); fixture.compactRequests.clear();
            auto rows=[&chat]() { return chat.findChildren<MessageRow *>(); };
            check(sendAndWait("row one") && sendAndWait("row two"),"two turns are sent for the row checks");
            check(rows().size()==4 && rows().at(0)->bubble()->text()=="row one" && rows().at(2)->bubble()->text()=="row two" &&
                  rows().at(1)->bubble()->text().startsWith("A local model") && rows().at(0)->bubble()->role()==ChatBubble::User && rows().at(1)->bubble()->role()==ChatBubble::Assistant,
                  "messages render as rows, one per path message, bubble text matches");
            {
                QSet<int> ids; bool all=true; for (auto *row : rows()) { all&=row->node()!=0; ids.insert(row->node()); }
                check(all && ids.size()==4,"every row knows its own conversation node, replies included once committed");
            }
            fixture.usageUsed=3600;
            check(sendAndWait("row three") && fixture.compactRequests.size()==1 && dividers().size()==1,"a compaction draws its divider");
            const QString dividerText=dividers().isEmpty()?QString():dividers().first()->text();
            check(rows().size()==6 && chat.findChildren<ChatBubble *>().size()==6,"six rows and six bubbles after three turns");
            QMetaObject::invokeMethod(&chat,"redrawForTest",Qt::DirectConnection); pump(100);
            check(dividers().size()==1 && dividers().first()->text()==dividerText && dividerText.contains("summarized"),"a divider is shown again when the transcript is redrawn");
            check(rows().size()==6 && chat.findChildren<ChatBubble *>().size()==6 && rows().at(4)->bubble()->text()=="row three","redrawing leaves no stale rows or bubbles");
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            check(rows().isEmpty() && dividers().isEmpty(),"clearing the chat leaves no rows");

            // Editing a message you already sent.
            fixture.usageUsed=1000; fixture.compactStatus=200; fixture.compactMethod="summary"; fixture.compactCovered=4; fixture.holdCompact=false;
            fixture.chatReply="reply A"; fixture.chatRequests.clear(); fixture.compactRequests.clear(); fixture.usageRequests.clear();
            auto pencilOf=[](MessageRow *row) { return row->findChild<QPushButton *>("editButton"); };
            auto boxOf=[](MessageRow *row) { return row->findChild<QPlainTextEdit *>("messageEditBox"); };
            auto sendKey=[](QWidget *target,int key,Qt::KeyboardModifiers modifiers=Qt::NoModifier) { QKeyEvent press(QEvent::KeyPress,key,modifiers); QApplication::sendEvent(target,&press); };
            auto openEditor=[&](int index) { const auto all=rows(); if (index>=all.size() || !pencilOf(all.at(index))) return false; pencilOf(all.at(index))->click(); pump(40); return all.at(index)->isEditing(); };
            auto saveEdit=[&](int index,const QString &text) { auto *row=rows().at(index); boxOf(row)->setPlainText(text); row->findChild<QPushButton *>("editSave")->click(); };
            auto waitDone=[&](int requestsBefore) { return waitUntil([&]{ return fixture.chatRequests.size()>requestsBefore && sendButton->text()=="Send  ↑"; }) && (pump(150),true); };
            auto messagesOf=[](const QString &body) { return QJsonDocument::fromJson(body.toUtf8()).object().value("messages").toArray(); };
            auto mentions=[](const QJsonArray &messages,const QString &text) { for (const auto &m : messages) if (m.toObject().value("content").toString().contains(text)) return true; return false; };

            check(sendAndWait("turn one") && sendAndWait("turn two") && sendAndWait("turn three") && rows().size()==6,"three turns are on screen before editing");
            {
                int pencils=0, onReplies=0;
                for (auto *row : rows()) if (pencilOf(row)) { ++pencils; if (row->bubble()->role()==ChatBubble::Assistant) ++onReplies; }
                check(pencils==3 && onReplies==0,"every message you sent has an edit pencil, replies have none");
            }
            {
                auto *row=rows().at(2); auto *pencil=pencilOf(row);
                QEnterEvent enter(QPointF(5,5),QPointF(5,5),QPointF(5,5)); QApplication::sendEvent(row,&enter); pump(20);
                const bool shown=pencil->isVisibleTo(row);
                QEvent leave(QEvent::Leave); QApplication::sendEvent(row,&leave); pump(20);
                check(shown && !pencil->isVisibleTo(row),"hovering a row shows its pencil, leaving hides it");
            }
            check(openEditor(2) && boxOf(rows().at(2)) && boxOf(rows().at(2))->toPlainText()=="turn two" && !rows().at(2)->bubble()->isVisibleTo(rows().at(2)),"clicking the pencil opens the editor pre-filled and hides the bubble");
            pump(80); capture(&chat,"chat-editing");
            check(openEditor(0) && !rows().at(2)->isEditing() && rows().at(0)->isEditing(),"opening a second editor closes the first");
            sendKey(boxOf(rows().at(0)),Qt::Key_Escape); pump(40);
            check(!rows().at(0)->isEditing() && rows().at(0)->bubble()->isVisibleTo(rows().at(0)) && rows().size()==6 && fixture.chatRequests.size()==3,"Esc cancels and changes nothing");
            {
                openEditor(2); saveEdit(2,"   \n "); pump(200);
                const bool blankCancelled=!rows().at(2)->isEditing() && fixture.chatRequests.size()==3;
                openEditor(2); rows().at(2)->findChild<QPushButton *>("editSave")->click(); pump(200);
                check(blankCancelled && !rows().at(2)->isEditing() && fixture.chatRequests.size()==3 && rows().size()==6,"empty or unchanged text counts as cancel");
            }
            input->setPlainText("my draft"); pump(20);
            check(openEditor(2) && !sendButton->isEnabled(),"while an editor is open the composer's Send is off, so a typed edit cannot be lost");
            fixture.chatReply="reply to edit";
            const int beforeEdit=fixture.chatRequests.size();
            saveEdit(2,"  turn two edited\nsecond line é ✓  ");
            check(waitDone(beforeEdit),"saving an edit sends it");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==3 && sent.at(0).toObject().value("content").toString()=="turn one" && sent.at(1).toObject().value("role").toString()=="assistant" &&
                      sent.at(2).toObject().value("role").toString()=="user" && sent.at(2).toObject().value("content").toString()==QString("turn two edited\nsecond line é ✓"),
                      "an edit sends the messages before it plus the edited text, with only outer whitespace trimmed");
            }
            check(rows().size()==4 && rows().at(2)->bubble()->text()==QString("turn two edited\nsecond line é ✓") && rows().at(3)->bubble()->text()=="reply to edit","the screen shows the edited message and a fresh reply");
            {
                bool gone=true; for (auto *row : rows()) gone&=!row->bubble()->text().contains("turn three");
                check(gone,"the old continuation is gone from the screen");
            }
            check(input->toPlainText()=="my draft" && sendButton->isEnabled(),"the draft in the main composer is untouched by editing");
            input->clear();
            const int beforeFirst=fixture.chatRequests.size();
            check(openEditor(0),"the first message can be edited too");
            saveEdit(0,"turn one edited");
            check(waitDone(beforeFirst),"editing the first message sends it");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==1 && sent.at(0).toObject().value("content").toString()=="turn one edited" && rows().size()==2 && rows().at(0)->bubble()->text()=="turn one edited",
                      "editing the first message branches from the start: just it and its reply remain");
            }

            // A long chat, edited at the compaction threshold.
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            fixture.usageUsed=1000; fixture.chatReply="reply A";
            bool grownLong=true; for (int i=1;i<=5;++i) grownLong&=sendAndWait(QString("long %1").arg(i));
            check(grownLong && rows().size()==10,"a five-turn chat is built for the threshold checks");
            pump(700); fixture.usageUsed=3600; fixture.usageRequests.clear(); fixture.compactRequests.clear();
            const int beforeLong=fixture.chatRequests.size();
            check(openEditor(6),"turn four can be edited");
            saveEdit(6,"long 4 edited");
            check(waitDone(beforeLong) && fixture.compactRequests.size()==1,"an edit at the threshold compacts first");
            {
                QJsonArray measured; for (const QString &body : fixture.usageRequests) if (measured.isEmpty() && mentions(messagesOf(body),"long 4 edited")) measured=messagesOf(body);   // the first one is the measurement made when the edit was saved
                check(measured.size()==7 && !mentions(measured,"long 5") && measured.last().toObject().value("content").toString()=="long 4 edited","the usage request for an edit excludes the old continuation");
                const QJsonObject compact=json(fixture.compactRequests); const QJsonArray posted=compact.value("messages").toArray();
                check(posted.size()==7 && !mentions(posted,"long 5") && posted.last().toObject().value("content").toString()=="long 4 edited" && compact.value("previous_summary").toString().isEmpty(),
                      "compaction for an edit uses only the messages before the edit point");
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==4 && sent.at(0).toObject().value("role").toString()=="system" && !mentions(sent,"long 5") && sent.last().toObject().value("content").toString()=="long 4 edited","the model gets the summary, the messages kept before the edit and the edited text");
            }
            check(dividers().size()==1 && rows().size()==8,"the compaction divider and the shortened conversation are on screen");

            // Stop during a compaction started by an edit.
            fixture.holdCompact=true; fixture.heldCompactSocket.clear(); fixture.usageUsed=3600;
            const int chatsBeforeEditStop=fixture.chatRequests.size(), compactsBeforeEditStop=fixture.compactRequests.size(), rowsBeforeEditStop=rows().size();
            openEditor(6); saveEdit(6,"another try");
            check(waitUntil([&]{ return fixture.compactRequests.size()==compactsBeforeEditStop+1 && fixture.heldCompactSocket; }) && status().contains("Compacting"),"saving an edit at the threshold starts a compaction");
            stopButton->click(); pump(300);
            check(rows().size()==rowsBeforeEditStop && rows().at(6)->isEditing() && boxOf(rows().at(6))->toPlainText()=="another try" && rows().at(6)->bubble()->text()=="long 4 edited" && fixture.chatRequests.size()==chatsBeforeEditStop,
                  "Stop during that compaction reopens the editor with the typed text and leaves the conversation unchanged");
            // Clearing the chat while an edit is being compacted.
            fixture.heldCompactSocket.clear();
            rows().at(6)->findChild<QPushButton *>("editSave")->click();
            check(waitUntil([&]{ return fixture.compactRequests.size()==compactsBeforeEditStop+2 && fixture.heldCompactSocket; }),"the edit is compacting again");
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            fixture.releaseHeldCompact(); pump(300);
            check(fixture.chatRequests.size()==chatsBeforeEditStop && rows().isEmpty() && dividers().isEmpty() && sendButton->isEnabled()==false,"clearing the chat while an edit is being compacted cancels it and a late reply changes nothing");
            fixture.holdCompact=false; input->clear();

            // Backend not ready.
            fixture.usageUsed=1000;
            check(sendAndWait("ready check") && rows().size()==2,"a chat exists for the offline check");
            openEditor(0); client.setBaseUrl(QString());
            saveEdit(0,"edited offline"); pump(100);
            check(rows().at(0)->isEditing() && boxOf(rows().at(0))->toPlainText()=="edited offline" && status().contains("Backend not connected") && rows().at(0)->bubble()->text()=="ready check",
                  "with the backend not ready the editor stays open with its text");
            client.setBaseUrl(QString("http://127.0.0.1:%1").arg(fixture.serverPort()));
            sendKey(boxOf(rows().at(0)),Qt::Key_Escape); pump(40);

            // Pencils follow the streaming state.
            fixture.holdChat=true; input->setPlainText("stream me"); pump(20); sendButton->click();
            check(waitUntil([&]{ return fixture.chatSocket && sendButton->text()=="Responding…"; }),"a held reply is streaming");
            {
                bool off=true; for (auto *row : rows()) if (auto *p=pencilOf(row)) off&=!p->isEnabled();
                check(off,"pencils are disabled while a reply is streaming");
            }
            fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); }
            waitUntil([&]{ return sendButton->text()=="Send  ↑"; }); pump(200);
            {
                bool on=true; for (auto *row : rows()) if (auto *p=pencilOf(row)) on&=p->isEnabled();
                check(on,"pencils are enabled again once the reply ends");
            }

            // Clearing the chat while an editor is open.
            openEditor(0);
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            input->setPlainText("after clear"); pump(20);
            check(rows().isEmpty() && sendButton->isEnabled(),"clearing the chat while an editor is open leaves no rows and Send works");
            input->clear();

            // Switching between versions.
            fixture.usageUsed=1000; fixture.chatReply="orig reply"; fixture.usageRequests.clear(); fixture.chatRequests.clear();
            auto versionOf=[](MessageRow *row) { auto *sw=row->findChild<VersionSwitcher *>("versionSwitcher"); auto *l=sw?sw->findChild<QLabel *>("versionLabel"):nullptr; return (!sw || sw->isHidden() || !l)?QString():l->text(); };
            auto arrow=[](MessageRow *row,const char *name) { return row->findChild<QPushButton *>(name); };
            auto texts=[&]() { QStringList out; for (auto *row : rows()) out << row->bubble()->text(); return out; };
            check(sendAndWait("s1") && sendAndWait("s2") && sendAndWait("s3") && rows().size()==6,"a three-turn chat exists for the version checks");
            bool noneYet=true; for (auto *row : rows()) noneYet&=versionOf(row).isEmpty();
            check(noneYet,"a message with one version shows no switcher");
            input->setPlainText("draft stays"); pump(20);
            fixture.chatReply="edit reply";
            const int beforeV=fixture.chatRequests.size();
            input->clear(); openEditor(2); saveEdit(2,"s2 edited"); input->setPlainText("draft stays"); // the composer's Send is off while editing, so the draft is typed after saving
            check(waitDone(beforeV) && rows().size()==4,"editing turn two leaves its new version on screen");
            check(versionOf(rows().at(2))=="2 / 2" && versionOf(rows().at(0)).isEmpty() && versionOf(rows().at(1)).isEmpty() && versionOf(rows().at(3)).isEmpty(),"an edited message shows 2 / 2 below its bubble, nothing else shows a switcher");
            fixture.usageRequests.clear();
            arrow(rows().at(2),"versionPrev")->click(); pump(200);
            check(texts()==QStringList({"s1","orig reply","s2","orig reply","s3","orig reply"}) && versionOf(rows().at(2))=="1 / 2","previous shows the original message and its whole conversation");
            check(waitUntil([&]{ return !fixture.usageRequests.isEmpty(); }) && messagesOf(fixture.usageRequests.last()).size()==7 && messagesOf(fixture.usageRequests.last()).last().toObject().value("content").toString()=="draft stays",
                  "after a switch the usage request matches the conversation on screen (plus the draft)");
            arrow(rows().at(2),"versionNext")->click(); pump(200);
            check(texts()==QStringList({"s1","orig reply","s2 edited","edit reply"}) && versionOf(rows().at(2))=="2 / 2","next returns to the edit and its continuation");
            { QEnterEvent hover(QPointF(5,5),QPointF(5,5),QPointF(5,5)); QApplication::sendEvent(rows().at(2),&hover); pump(60); capture(&chat,"chat-versions"); QEvent unhover(QEvent::Leave); QApplication::sendEvent(rows().at(2),&unhover); }
            check(input->toPlainText()=="draft stays","switching leaves the draft in the main composer alone");
            input->clear();
            const int beforeCont=fixture.chatRequests.size();
            fixture.chatReply="edit branch reply";
            check(sendAndWait("s4"),"a message is sent on the edited version");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==5 && sent.at(2).toObject().value("content").toString()=="s2 edited" && !mentions(sent,"s3") && sent.last().toObject().value("content").toString()=="s4" && fixture.chatRequests.size()==beforeCont+1,
                      "the next send continues from the version on screen");
            }
            arrow(rows().at(2),"versionPrev")->click(); pump(200);
            fixture.chatReply="orig branch reply";
            check(sendAndWait("s5"),"a message is sent on the original version");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==7 && sent.at(2).toObject().value("content").toString()=="s2" && mentions(sent,"s3") && !mentions(sent,"s4") && !mentions(sent,"s2 edited"),"after switching back, the next send continues the original conversation");
            }
            fixture.chatReply="third reply";
            openEditor(2); saveEdit(2,"s2 again");
            check(waitDone(fixture.chatRequests.size()-1) && versionOf(rows().at(2))=="3 / 3" && rows().size()==4,"a second edit of the same message shows 3 / 3");
            check(!arrow(rows().at(2),"versionNext")->isEnabled() && arrow(rows().at(2),"versionPrev")->isEnabled(),"next is off at the last version");
            arrow(rows().at(2),"versionPrev")->click(); pump(200);
            check(versionOf(rows().at(2))=="2 / 3" && texts()==QStringList({"s1","orig reply","s2 edited","edit reply","s4","edit branch reply"}),"stepping back goes through the middle version with its own continuation");
            arrow(rows().at(2),"versionPrev")->click(); pump(200);
            check(versionOf(rows().at(2))=="1 / 3" && !arrow(rows().at(2),"versionPrev")->isEnabled() && rows().size()==8,"previous is off at the first version and the original conversation is whole");

            // Arrows follow what the panel is doing.
            {
                auto switchers=[&chat]() { QList<VersionSwitcher *> shown; for (auto *sw : chat.findChildren<VersionSwitcher *>()) if (!sw->isHidden()) shown << sw; return shown; };
                auto allOff=[&]() { auto all=switchers(); if (all.isEmpty()) return false; for (auto *sw : all) if (sw->findChild<QPushButton *>("versionPrev")->isEnabled() || sw->findChild<QPushButton *>("versionNext")->isEnabled()) return false; return true; };
                fixture.holdChat=true; input->setPlainText("hold me"); pump(20); sendButton->click();
                check(waitUntil([&]{ return fixture.chatSocket && sendButton->text()=="Responding…"; }) && allOff(),"arrows are disabled while a reply is streaming");
                fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); }
                waitUntil([&]{ return sendButton->text()=="Send  ↑"; }); pump(200);
                check(!allOff() && !switchers().isEmpty(),"arrows work again once the reply ends");

                fixture.usageUsed=3600; fixture.holdCompact=true; fixture.heldCompactSocket.clear();
                const int compactsV=fixture.compactRequests.size();
                input->setPlainText("compact me"); pump(20); sendButton->click();
                check(waitUntil([&]{ return fixture.compactRequests.size()==compactsV+1 && fixture.heldCompactSocket; }) && allOff(),"arrows are disabled while a send is being prepared");
                stopButton->click(); pump(300); fixture.holdCompact=false; fixture.heldCompactSocket.clear(); fixture.usageUsed=1000; input->clear();
                check(!allOff(),"arrows work again after the compaction is stopped");

                check(openEditor(0) && allOff(),"arrows are disabled while an editor is open, so the edit cannot be lost");
                sendKey(boxOf(rows().at(0)),Qt::Key_Escape); pump(40);
                check(!allOff(),"arrows work again once the editor closes");
            }
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            check(chat.findChildren<VersionSwitcher *>().isEmpty(),"clearing the chat removes the switchers");

            // A version switch brings back that version's compaction.
            fixture.usageUsed=1000; fixture.compactStatus=200; fixture.compactMethod="summary"; fixture.compactCovered=4; fixture.chatReply="c reply";
            bool grownC=true; for (int i=1;i<=5;++i) grownC&=sendAndWait(QString("c%1").arg(i));
            fixture.usageUsed=3600;
            check(grownC && sendAndWait("c6") && dividers().size()==1 && rows().size()==12,"a six-turn chat is compacted once");
            fixture.usageUsed=1000;
            const int beforeC=fixture.chatRequests.size();
            openEditor(2); saveEdit(2,"c2 edited");
            check(waitDone(beforeC) && rows().size()==4 && dividers().isEmpty(),"editing before the compaction leaves the divider behind");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==3 && sent.at(0).toObject().value("role").toString()=="user" && sent.last().toObject().value("content").toString()=="c2 edited","the edited version is sent without the other version's summary");
            }
            arrow(rows().at(2),"versionPrev")->click(); pump(200);
            check(rows().size()==12 && dividers().size()==1 && dividers().first()->text().contains("summarized"),"switching back shows the compaction divider again");
            check(sendAndWait("c7"),"a message is sent after switching back");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.at(0).toObject().value("role").toString()=="system" && sent.at(0).toObject().value("content").toString()=="Summary of the earlier conversation: The user asked about X." && sent.last().toObject().value("content").toString()=="c7",
                      "switching back restores that version's summary for the next send");
            }
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            input->clear();

            // Retrying the latest reply.
            fixture.usageUsed=1000; fixture.chatEmpty=false; fixture.chatStatus=200; fixture.usageStatus=200; fixture.holdChat=false; fixture.holdCompact=false;
            fixture.chatRequests.clear(); fixture.compactRequests.clear(); fixture.usageRequests.clear();
            auto retryOf=[](MessageRow *row) { return row->findChild<QPushButton *>("retryButton"); };
            auto clickRetry=[&](MessageRow *row) { if (auto *b=retryOf(row)) b->click(); };
            auto retryOn=[&]() { QList<MessageRow *> on; for (auto *row : rows()) if (auto *b=retryOf(row)) if (b->isEnabled()) on << row; return on; };
            fixture.chatReply="old reply";
            check(sendAndWait("t1") && sendAndWait("t2") && rows().size()==4,"a two-turn chat exists for the retry checks");
            check(retryOn().size()==1 && retryOn().first()==rows().last() && !retryOf(rows().at(0)),"only the latest reply has a retry button");
            {
                auto *row=rows().last(); auto *b=retryOf(row);
                QEnterEvent enter(QPointF(5,5),QPointF(5,5),QPointF(5,5)); QApplication::sendEvent(row,&enter); pump(20);
                const bool shown=b && b->isVisibleTo(row);
                QEvent leave(QEvent::Leave); QApplication::sendEvent(row,&leave); pump(20);
                check(shown && b && !b->isVisibleTo(row),"hovering the latest reply shows its retry button, leaving hides it");
            }
            input->setPlainText("retry draft"); pump(20);
            fixture.chatReply="new reply"; const int beforeRetry=fixture.chatRequests.size();
            clickRetry(rows().last());
            check(waitDone(beforeRetry),"retry sends a request");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==3 && sent.at(0).toObject().value("content").toString()=="t1" && sent.at(1).toObject().value("role").toString()=="assistant" && sent.at(2).toObject().value("role").toString()=="user" && sent.at(2).toObject().value("content").toString()=="t2",
                      "retry sends the conversation up to the user message, with no extra user message");
            }
            check(rows().size()==4 && rows().last()->bubble()->text()=="new reply" && versionOf(rows().last())=="2 / 2" && versionOf(rows().at(2)).isEmpty(),"retry shows the new reply as version 2 / 2 and the message above keeps one version");
            check(input->toPlainText()=="retry draft","retry leaves the draft in the main composer alone");
            input->clear();
            arrow(rows().last(),"versionPrev")->click(); pump(200);
            check(rows().last()->bubble()->text()=="old reply" && versionOf(rows().last())=="1 / 2","previous returns to the old reply");
            arrow(rows().last(),"versionNext")->click(); pump(200);
            check(rows().last()->bubble()->text()=="new reply" && retryOn().size()==1,"next returns to the new reply, which can be retried again");

            // While a retry streams.
            fixture.chatReply="streaming retry"; fixture.holdChat=true; fixture.chatSocket.clear();
            clickRetry(rows().last());
            check(waitUntil([&]{ return fixture.chatSocket && rows().size()==4 && rows().last()->bubble()->text().contains("streaming retry"); }),"the retry streams into the place of the old reply");
            check(!texts().contains("new reply") && retryOn().isEmpty(),"the old reply is not shown while the retry streams, and retry is off");
            pump(500); fixture.usageRequests.clear();   // from here on every usage request is made while the retry streams
            chat.refreshContextUsage();
            check(waitUntil([&]{ return !fixture.usageRequests.isEmpty(); }) && messagesOf(fixture.usageRequests.last()).size()==3,"the usage request during a retry excludes the old reply");
            { bool off=true; for (auto *sw : chat.findChildren<VersionSwitcher *>()) if (!sw->isHidden()) off&=!sw->findChild<QPushButton *>("versionNext")->isEnabled() && !sw->findChild<QPushButton *>("versionPrev")->isEnabled(); check(off,"version arrows are off while a retry streams"); }
            fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->write("data: {\"done\":true}\n\n"); fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); }
            waitUntil([&]{ return sendButton->text()=="Send  ↑"; }); pump(200);
            check(rows().size()==4 && versionOf(rows().last())=="3 / 3" && rows().last()->bubble()->text().contains("streaming retry"),"the finished retry is the third version of the reply");

            // A retry that gives nothing back keeps the previous reply.
            fixture.chatEmpty=true;
            clickRetry(rows().last());
            waitUntil([&]{ return sendButton->text()=="Send  ↑" && fixture.chatRequests.size()>beforeRetry+1 && status().contains("Retry failed"); }); pump(150);
            check(rows().size()==4 && versionOf(rows().last())=="3 / 3" && rows().last()->bubble()->text().contains("streaming retry") && status().contains("Retry failed"),"a retry that returns no text keeps the previous reply and its count");
            fixture.chatEmpty=false;
            fixture.chatStatus=500; fixture.usageStatus=409; const int errorsBefore=fixture.chatRequests.size();
            clickRetry(rows().last());
            waitUntil([&]{ return fixture.chatRequests.size()>errorsBefore && sendButton->text()=="Send  ↑"; }); pump(150);
            check(rows().size()==4 && versionOf(rows().last())=="3 / 3" && rows().last()->bubble()->text().contains("streaming retry"),"a failed retry request (no model loaded) keeps the previous reply");
            fixture.chatStatus=200; fixture.usageStatus=200;

            // Stop before any text, then after some text.
            fixture.chatEmpty=true; fixture.holdChat=true; fixture.chatSocket.clear();
            clickRetry(rows().last());
            check(waitUntil([&]{ return fixture.chatSocket && sendButton->text()=="Responding…"; }),"a silent retry is held open");
            stopButton->click(); pump(300);
            check(rows().size()==4 && versionOf(rows().last())=="3 / 3" && rows().last()->bubble()->text().contains("streaming retry") && retryOn().size()==1,"Stop before any text keeps the previous reply");
            fixture.chatSocket.clear(); fixture.chatEmpty=false; fixture.chatReply="partial retry";
            clickRetry(rows().last());
            check(waitUntil([&]{ return fixture.chatSocket && rows().last()->bubble()->text().contains("partial retry"); }),"a retry streams some text");
            stopButton->click(); pump(300);
            fixture.holdChat=false; fixture.chatSocket.clear();
            check(rows().size()==4 && versionOf(rows().last())=="4 / 4" && rows().last()->bubble()->text().contains("partial retry"),"Stop after some text keeps the partial reply as a new version");

            // Retry is not offered without a backend.
            {
                const int requestsBefore=fixture.chatRequests.size();
                client.setBaseUrl(QString());
                clickRetry(rows().last()); pump(100);
                check(status().contains("Backend not connected") && fixture.chatRequests.size()==requestsBefore && rows().size()==4 && versionOf(rows().last())=="4 / 4","retry with the backend not connected changes nothing");
                client.setBaseUrl(QString("http://127.0.0.1:%1").arg(fixture.serverPort()));
            }

            // Clearing the chat while a retry streams.
            fixture.chatReply="held again"; fixture.holdChat=true; fixture.chatSocket.clear();
            clickRetry(rows().last());
            check(waitUntil([&]{ return fixture.chatSocket && sendButton->text()=="Responding…"; }),"a retry is streaming before the chat is cleared");
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(150);
            fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); } pump(150);
            input->setPlainText("after retry clear"); pump(20);
            check(rows().isEmpty() && dividers().isEmpty() && sendButton->isEnabled(),"clearing the chat while a retry streams leaves nothing behind");
            input->clear();

            // Retry at the compaction threshold.
            fixture.chatReply="turn reply"; fixture.usageUsed=1000; fixture.compactStatus=200; fixture.compactMethod="summary"; fixture.compactCovered=4;
            bool grownR=true; for (int i=1;i<=4;++i) grownR&=sendAndWait(QString("r%1").arg(i));
            fixture.chatReply="last old"; grownR&=sendAndWait("r5");
            pump(700); fixture.compactRequests.clear(); fixture.usageRequests.clear();
            check(grownR && rows().size()==10,"a five-turn chat exists for the retry-at-threshold check");
            fixture.usageUsed=3600; fixture.chatReply="retry result";
            const int beforeRC=fixture.chatRequests.size();
            clickRetry(rows().last());
            check(waitDone(beforeRC) && fixture.compactRequests.size()==1,"retry at the threshold compacts first");
            {
                const QJsonObject compact=json(fixture.compactRequests); const QJsonArray posted=compact.value("messages").toArray();
                check(posted.size()==9 && posted.last().toObject().value("content").toString()=="r5" && !mentions(posted,"last old"),"compaction for a retry posts only the messages up to the user message");
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==6 && sent.at(0).toObject().value("role").toString()=="system" && sent.last().toObject().value("content").toString()=="r5" && !mentions(sent,"last old"),"the retry request carries the summary and the kept messages");
            }
            check(dividers().size()==1 && rows().size()==10 && versionOf(rows().last())=="2 / 2","the compaction divider is shown and the reply is a new version");
            fixture.usageUsed=1000; fixture.chatReply="follow up reply";
            check(sendAndWait("r6"),"a message is sent after the retry");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.at(0).toObject().value("role").toString()=="system" && mentions(sent,"retry result") && !mentions(sent,"last old") && sent.last().toObject().value("content").toString()=="r6",
                      "the next send uses the new reply and the summary made during the retry");
            }
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            input->clear();

            // Review fix 1: a retry that fails after compacting must leave the conversation exactly as it was,
            // and a successful one must keep its summary with its own reply.
            fixture.chatEmpty=false; fixture.chatStatus=200; fixture.usageStatus=200; fixture.holdChat=false; fixture.holdCompact=false;
            fixture.chatReply="turn reply"; fixture.usageUsed=1000; fixture.compactStatus=200; fixture.compactMethod="summary"; fixture.compactCovered=4;
            bool grownF=true; for (int i=1;i<=5;++i) grownF&=sendAndWait(QString("f%1").arg(i));
            pump(700); fixture.compactRequests.clear();
            check(grownF && rows().size()==10,"a five-turn chat exists for the failed-retry checks");
            fixture.usageUsed=3600; fixture.chatEmpty=true;
            const int beforeFailedRetry=fixture.chatRequests.size();
            clickRetry(rows().last());
            waitUntil([&]{ return fixture.compactRequests.size()==1 && fixture.chatRequests.size()>beforeFailedRetry && sendButton->text()=="Send  ↑"; }); pump(250);
            check(fixture.compactRequests.size()==1 && rows().size()==10 && dividers().isEmpty() && versionOf(rows().last()).isEmpty(),"a retry that compacts and then returns nothing leaves the conversation as it was");
            fixture.chatEmpty=false; fixture.usageUsed=1000; fixture.chatReply="after failed retry";
            check(sendAndWait("f6"),"a message is sent after the failed retry");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==11 && sent.at(0).toObject().value("role").toString()=="user" && !mentions(sent,"Summary of the earlier"),"a failed retry leaves no summary behind for the next send");
            }
            fixture.usageUsed=3600; fixture.chatReply="retry won"; fixture.compactRequests.clear();
            const int beforeGoodRetry=fixture.chatRequests.size();
            clickRetry(rows().last());
            check(waitDone(beforeGoodRetry) && fixture.compactRequests.size()==1 && versionOf(rows().last())=="2 / 2" && dividers().size()==1,"a retry that compacts and succeeds is a new version with its divider");
            arrow(rows().last(),"versionPrev")->click(); pump(200);
            check(rows().last()->bubble()->text()=="after failed retry" && dividers().isEmpty(),"the old reply comes back without the new reply's divider");
            fixture.usageUsed=1000; fixture.chatReply="on the old reply";
            check(sendAndWait("f7"),"a message is sent on the old reply");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.at(0).toObject().value("role").toString()=="user" && !mentions(sent,"Summary of the earlier") && mentions(sent,"after failed retry"),"the old reply is not sent with the summary made for the other reply");
            }
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            input->clear();

            // Review fix 2: a message that got no reply can be sent again from its editor, without a second copy.
            fixture.chatStatus=500; fixture.usageStatus=409; fixture.usageUsed=1000;
            const int beforeNoReply=fixture.chatRequests.size();
            input->setPlainText("no reply yet"); pump(20); sendButton->click();
            waitUntil([&]{ return fixture.chatRequests.size()>beforeNoReply && sendButton->text()=="Send  ↑"; }); pump(250);
            fixture.chatStatus=200; fixture.usageStatus=200; fixture.chatReply="finally";
            check(openEditor(0),"the unanswered message can be opened for editing");
            rows().at(0)->findChild<QPushButton *>("editSave")->click();
            check(waitDone(beforeNoReply+1),"saving it unchanged sends it again");
            {
                const QJsonArray sent=messagesOf(fixture.chatRequests.last());
                check(sent.size()==1 && sent.at(0).toObject().value("content").toString()=="no reply yet","it is sent once, not as a second copy of the message");
            }
            check(rows().size()==2 && rows().at(1)->bubble()->text()=="finally" && versionOf(rows().at(0)).isEmpty(),"the reply arrives under the same message and it gets no new version");
            openEditor(0); rows().at(0)->findChild<QPushButton *>("editSave")->click(); pump(200);
            check(!rows().at(0)->isEditing() && fixture.chatRequests.size()==beforeNoReply+2,"once it has a reply, saving it unchanged is a cancel again");
            QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(100);
            input->clear();

            // Chat history: the panel saves every change and can open, rename and delete saved chats.
            {
                QTemporaryDir historyDir; const QString chatsPath=historyDir.path()+"/chats"; ChatStore history(chatsPath);
                chat.setChatStore(&history);
                fixture.chatReply="reply A"; fixture.usageUsed=1000; fixture.usageLimit=4096; fixture.compactStatus=200; fixture.compactMethod="summary"; fixture.compactCovered=4;
                fixture.holdChat=false; fixture.holdCompact=false;
                QStringList announced; const auto announcedLink=QObject::connect(&chat,&ChatPanel::currentChatChanged,[&announced](const QString &id) { announced << id; });
                int busyFlips=0; const auto busyLink=QObject::connect(&chat,&ChatPanel::busyChanged,[&busyFlips](bool) { ++busyFlips; });
                auto chatFiles=[&]() { return QDir(chatsPath).entryList({"*.json"},QDir::Files); };
                auto savedTexts=[&](const QString &id) { QStringList out; if (const auto t=history.load(id)) for (const auto &m : t->messages()) out << m.value("content").toString(); return out; };
                auto releaseChat=[&]() { fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); } pump(100); };
                auto usageMessages=[&]() {
                    const int n=fixture.usageRequests.size(); chat.refreshContextUsage(); waitUntil([&]{ return fixture.usageRequests.size()>n; });
                    return QJsonDocument::fromJson(fixture.usageRequests.last().toUtf8()).object().value("messages").toArray();
                };
                auto newChat=[&]() { QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(60); };
                auto versions=[&]() { QStringList out; for (auto *row : rows()) out << versionOf(row); return out; };
                auto dividerTexts=[&]() { QStringList out; for (auto *label : dividers()) out << label->text(); return out; };

                newChat();
                check(chat.currentChatId().isEmpty() && chatFiles().isEmpty() && history.list().isEmpty() && button(&chat,"+  New chat")==nullptr,"history: a blank chat writes nothing, and the toolbar has no New chat button any more");

                // The first message creates the file at once; a half-streamed reply is not saved.
                fixture.holdChat=true; input->setPlainText("history one"); pump(20); sendButton->click();
                check(waitUntil([&]{ return fixture.chatSocket && sendButton->text()=="Responding…"; }),"history: the first reply is streaming");
                const QString id1=chat.currentChatId();
                check(!id1.isEmpty() && chatFiles()==QStringList({id1+".json"}) && savedTexts(id1)==QStringList({"history one"}) && announced.size()==2 && announced.last()==id1,"history: the first message creates the chat file at once, ending at the user's message");
                pump(400);
                check(savedTexts(id1)==QStringList({"history one"}) && history.list().at(0).title=="history one","history: a reply still streaming is not saved");
                stopButton->click(); pump(400);
                check(savedTexts(id1)==QStringList({"history one","reply A"}),"history: Stop keeps the partial reply and saves it");
                releaseChat();

                // Every kind of change is saved.
                // Stream completion and the debounced disk save are separate events.
                check(sendAndWait("history two") && waitUntil([&] {
                    return savedTexts(id1)==QStringList({"history one","reply A","history two","reply A"});
                }),"history: a finished reply is saved");
                fixture.chatReply="reply B"; int before=fixture.chatRequests.size();
                check(openEditor(2),"history: the second message can be edited"); saveEdit(2,"history two edited"); check(waitDone(before),"history: the edit is sent"); pump(300);
                {
                    const auto t=history.load(id1);
                    check(t && savedTexts(id1)==QStringList({"history one","reply A","history two edited","reply B"}) && t->versionCount(t->activePath().at(2))==2,"history: an edit is saved with both versions");
                }
                fixture.chatReply="reply C"; before=fixture.chatRequests.size();
                clickRetry(rows().last()); check(waitDone(before),"history: the reply is retried"); pump(300);
                {
                    const auto t=history.load(id1);
                    check(t && savedTexts(id1).last()=="reply C" && t->versionCount(t->activePath().last())==2,"history: a retry is saved with both versions");
                }
                arrow(rows().last(),"versionPrev")->click(); pump(400);
                check(savedTexts(id1).last()=="reply B","history: switching to an older version is saved");
                arrow(rows().last(),"versionNext")->click(); pump(400);
                check(savedTexts(id1).last()=="reply C","history: switching back is saved");
                fixture.usageUsed=3600; fixture.chatReply="reply D";
                check(sendAndWait("history three") && dividers().size()==1,"history: a compaction happens in the chat to be saved");
                fixture.usageUsed=1000; pump(300);
                {
                    const auto t=history.load(id1);
                    check(t && t->compaction().covered>0 && !t->compaction().summary.isEmpty(),"history: the compaction snapshot is saved");
                    int noteCount=0; if (t) for (auto id : t->activePath()) noteCount+=t->notes(id).size();
                    check(noteCount==1,"history: the compaction divider note is saved");
                }

                // Reopening restores the chat exactly.
                const QStringList beforeTexts=texts(), beforeVersions=versions(), beforeDividers=dividerTexts();
                const QJsonArray beforeRequest=usageMessages();
                newChat();
                check(rows().isEmpty() && chat.currentChatId().isEmpty() && history.list().size()==1 && announced.last().isEmpty(),"history: starting a new chat keeps the old one saved");
                input->setPlainText("my draft"); pump(20);
                check(chat.openChat(id1) && chat.currentChatId()==id1 && announced.last()==id1,"history: a saved chat can be opened");
                check(texts()==beforeTexts && versions()==beforeVersions && dividerTexts()==beforeDividers && input->toPlainText()=="my draft","history: the reopened chat shows the same messages, versions and dividers, and the draft is untouched");
                input->clear(); pump(20);
                check(usageMessages()==beforeRequest && beforeRequest.size()>0 && beforeRequest.at(0).toObject().value("role").toString()=="system","history: the reopened chat would send exactly what it sent before, summary included");
                arrow(rows().at(2),"versionPrev")->click(); pump(300);
                check(texts().at(2)=="history two","history: older versions are still reachable after reopening");
                arrow(rows().at(2),"versionNext")->click(); pump(300);
                check(chat.openChat(id1),"history: opening the chat that is already open is fine");

                // A second chat, then the busy guards.
                newChat(); fixture.chatReply="reply A";
                check(sendAndWait("second chat") && history.list().size()==2,"history: a second chat is saved next to the first");
                const QString id2=chat.currentChatId();
                check(!id2.isEmpty() && id2!=id1 && history.list().at(0).id==id2,"history: the newest chat is listed first");
                fixture.holdChat=true; input->setPlainText("busy one"); pump(20); sendButton->click();
                check(waitUntil([&]{ return fixture.chatSocket && sendButton->text()=="Responding…"; }) && chat.isBusy(),"history: a reply is streaming and the chat is busy");
                const QStringList rowsBusy=texts();
                check(!chat.openChat(id1) && chat.currentChatId()==id2 && texts()==rowsBusy,"history: a chat cannot be opened while a reply streams");
                check(!chat.deleteChat(id2) && history.list().size()==2 && !chat.renameChat(id2,"nope"),"history: the busy chat can neither be deleted nor renamed");
                auto titleOf=[&](const QString &id) { for (const auto &row : history.list()) if (row.id==id) return row.title; return QString("<missing>"); };
                check(chat.renameChat(id1,"First chat") && titleOf(id1)=="First chat","history: another, idle chat can be renamed meanwhile");
                stopButton->click(); pump(300); releaseChat();
                check(!chat.isBusy() && busyFlips>=2,"history: busy turns on and off");
                check(openEditor(0) && !chat.openChat(id1) && chat.isBusy(),"history: a chat cannot be opened while a message editor is open");
                sendKey(boxOf(rows().at(0)),Qt::Key_Escape); pump(60);
                check(!rows().at(0)->isEditing() && !chat.isBusy(),"history: closing the editor ends the busy state");
                QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(60);
                check(chat.openChat(id1),"history: the first chat is opened again");
                check(sendAndWait("pad 1") && sendAndWait("pad 2") && sendAndWait("pad 3"),"history: the chat is long enough to be compacted");
                fixture.holdCompact=true; fixture.heldCompactSocket.clear(); fixture.usageUsed=3600;
                input->setPlainText("prepare me"); pump(20); sendButton->click();
                check(waitUntil([&]{ return fixture.heldCompactSocket; }) && chat.isBusy() && !chat.openChat(id2) && chat.currentChatId()==id1,"history: a chat cannot be opened while a send is being prepared");
                newChat(); fixture.releaseHeldCompact(); pump(200); fixture.holdCompact=false; fixture.usageUsed=1000; input->clear();

                // Rename and delete.
                check(chat.openChat(id1) && chat.renameChat(id1,"  Renamed chat ") && titleOf(id1)=="Renamed chat" && !chat.renameChat(id1,"   ") && !chat.renameChat("not-an-id","x"),"history: a chat can be renamed; an empty title or unknown id cannot");
                fixture.chatReply="reply E";
                check(sendAndWait("history four"),"history: the renamed chat continues");
                pump(300);
                bool renamedKept=false; for (const auto &row : history.list()) if (row.id==id1) renamedKept=row.title=="Renamed chat" && row.customTitle;
                check(renamedKept,"history: a renamed chat keeps its name as it grows");
                arrow(rows().last(),"versionPrev")->click();   // a change that is still waiting to be saved ...
                check(chat.deleteChat(id1) && !history.load(id1) && chat.currentChatId().isEmpty() && rows().isEmpty() && announced.last().isEmpty(),"history: deleting the open chat leaves a blank chat");
                pump(500);
                check(!chatFiles().contains(id1+".json") && history.list().size()==1 && history.list().at(0).id==id2,"history: a deleted chat is not brought back by a late save");
                check(sendAndWait("after delete") && chat.currentChatId()!=id1 && !chat.currentChatId().isEmpty() && history.list().size()==2,"history: the next message starts a new chat, not the deleted one");
                const QString id3=chat.currentChatId();
                check(chat.deleteChat(id2) && chat.currentChatId()==id3 && rows().size()==2 && history.list().size()==1,"history: deleting another chat leaves the open one alone");
                check(!chat.deleteChat(id2) && !chat.deleteChat("../x"),"history: deleting an unknown chat does nothing");

                // Bad files and failures.
                newChat(); input->setPlainText("keep this draft"); pump(20);
                { QFile f(chatsPath+"/"+id3+".json"); check(f.open(QIODevice::WriteOnly|QIODevice::Truncate),"open corrupt-chat fixture"); f.write("junk"); }
                const QStringList rowsBefore=texts();
                check(!chat.openChat(id3) && chat.currentChatId().isEmpty() && texts()==rowsBefore && status().contains("could not be opened") && !chat.openChat("0b6c3a52-9d0e-4f1c-8a77-0123456789ab"),"history: an unreadable chat does not open and nothing changes");
                check(input->toPlainText()=="keep this draft","history: the draft survives a failed open"); input->clear();
                history.remove(id3);
                ChatTree victimTree; victimTree.addChild(victimTree.addChild(ChatTree::kRoot,"user","victim chat"),"assistant","ok");
                const QString victim=history.create(); history.save(victim,victimTree);
                // A directory's read-only flag does not prevent writes on Windows.
                // Preserve the files and put a regular file in the directory's place.
                const QString savedChatsPath=chatsPath+".saved";
                check(QDir().rename(chatsPath,savedChatsPath),"preserve chats before blocking storage");
                QFile storageBlocker(chatsPath);
                check(storageBlocker.open(QIODevice::WriteOnly),"block the chats directory with a file");
                storageBlocker.close();
                check(sendAndWait("cannot be saved") && rows().size()==2 && !history.lastError().isEmpty(),"history: a chat that cannot be saved still works");
                const QString id4=chat.currentChatId();
                check(storageBlocker.remove() && QDir().rename(savedChatsPath,chatsPath),"restore chats after the failed save");
                {   // a failure to rename or delete is reported, not silent
                    const QString victimPath=chatsPath+"/"+victim+".json";
                    check(QFile::rename(victimPath,victimPath+".saved") && QDir().mkdir(victimPath),"block a chat file with a directory");
                    const bool renamedOk=chat.renameChat(victim,"cannot rename"); const QString renameStatus=status();
                    const bool deletedOk=chat.deleteChat(victim); const QString deleteStatus=status();
                    check(!renamedOk && renameStatus.contains("Could not rename") && statusLine->property("state")=="warn","history: a rename that fails says so");
                    check(!deletedOk && deleteStatus.contains("Could not delete") && statusLine->property("state")=="warn" && history.list().size()>=1,"history: a delete that fails says so and keeps the chat");
                    check(QDir().rmdir(victimPath) && QFile::rename(victimPath+".saved",victimPath),"restore the blocked chat file");
                }
                chat.flushSave(); pump(100);
                check(savedTexts(id4).size()==2,"history: a save that failed is tried again when the chat is flushed (switching chats or closing)");
                check(sendAndWait("now it can"),"history: the next message after the folder is fixed is sent"); pump(300);
                check(chat.currentChatId()==id4 && savedTexts(id4).size()==4,"history: the chat is saved in full once the folder is writable again");

                // The meter's prompt starts a new chat and keeps the old one.
                if (auto *meter=chat.findChild<ContextMeter *>()) {
                    emit meter->newChatRequested(); pump(60);
                    check(rows().isEmpty() && chat.currentChatId().isEmpty() && !savedTexts(id4).isEmpty(),"history: the meter's new-chat prompt starts a new chat and keeps the old one");
                }

                // Without a store nothing is written and nothing breaks.
                chat.setChatStore(nullptr); newChat();
                const int filesBefore=chatFiles().size();
                check(sendAndWait("no store") && chat.currentChatId().isEmpty() && rows().size()==2,"history: without a store a chat still works");
                fixture.chatReply="reply F"; before=fixture.chatRequests.size();
                clickRetry(rows().last()); check(waitDone(before) && rows().last()->bubble()->text()=="reply F","history: retry works without a store");
                check(openEditor(0),"history: editing works without a store"); sendKey(boxOf(rows().at(0)),Qt::Key_Escape); pump(60);
                check(chatFiles().size()==filesBefore && !chat.openChat(id4) && !chat.renameChat(id4,"x") && !chat.deleteChat(id4),"history: without a store nothing is written and nothing can be opened, renamed or deleted");
                newChat();
                QObject::disconnect(announcedLink); QObject::disconnect(busyLink);
            }

            // The greeting on every blank chat.
            {
                QSettings().remove("profile/name"); Greeting::setUserName("Archit");
                int hour=9; chat.setHourProviderForTest([&hour]() { return hour; });
                auto greeting=[&chat]() { auto *label=chat.findChild<QLabel *>("emptyConversation"); return label?label->text():QString(); };
                auto blank=[&]() { QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection); pump(40); };
                blank();
                check(Greeting::variants(9,"Archit").contains(greeting()) && !greeting().contains("Your model. Your way.") && chat.findChild<QLabel *>("emptyConversation")->isVisible(),"greeting: a blank chat greets by name for the time of day");
                bool repeated=false; QString previous=greeting();
                for (int i=0;i<12;++i) { blank(); repeated|=greeting()==previous; previous=greeting(); }
                check(!repeated,"greeting: a new blank chat never repeats the previous wording");
                blank(); const QString fixedText=greeting();
                QMetaObject::invokeMethod(&chat,"redrawForTest",Qt::DirectConnection); chat.resize(900,640); Theme::setDark(!Theme::isDark()); pump(60); Theme::setDark(!Theme::isDark()); pump(60);
                check(greeting()==fixedText,"greeting: the wording stays put while the blank chat is on screen (redraw, resize, theme change)");
                hour=23; blank();
                check(Greeting::variants(23,"Archit").contains(greeting()),"greeting: a late-night chat says so");
                hour=14; fixture.chatReply="hello there";
                check(sendAndWait("greeting test") && !chat.findChild<QLabel *>("emptyConversation")->isVisible(),"greeting: a chat with messages shows no greeting");
                blank();
                check(chat.findChild<QLabel *>("emptyConversation")->isVisible() && Greeting::variants(14,"Archit").contains(greeting()),"greeting: going back to a blank chat greets again");
                // Changing the name in Settings re-greets the blank chat.
                SettingsPanel nameSettings;
                auto *nameEdit=nameSettings.findChild<QLineEdit *>("profileName");
                check(nameEdit && nameEdit->text()=="Archit" && nameEdit->accessibleName()=="Your name","greeting: Settings shows the stored name in a 'Your name' box");
                if (nameEdit) {
                    const auto link=QObject::connect(&nameSettings,&SettingsPanel::settingsChanged,&chat,&ChatPanel::reloadContextSettings);
                    nameEdit->setText("  Ada  "); emit nameEdit->editingFinished(); pump(40);
                    check(Greeting::userName()=="Ada" && Greeting::variants(14,"Ada").contains(greeting()),"greeting: renaming yourself in Settings changes the greeting at once");
                    nameEdit->setText(""); emit nameEdit->editingFinished(); pump(40);
                    check(Greeting::userName().isEmpty() && Greeting::variants(14,"").contains(greeting()),"greeting: clearing the name drops it from the greeting");
                    nameEdit->setText(QString(60,'n')); emit nameEdit->editingFinished(); pump(40);
                    check(Greeting::userName().size()==40,"greeting: a very long name is capped at 40 characters");
                    QObject::disconnect(link);
                }
                // A name saved after the panel was built (the first-launch question) shows up in Settings.
                {
                    QSettings().remove("profile/name"); SettingsPanel late; Greeting::setUserName("Zed");
                    auto *edit=late.findChild<QLineEdit *>("profileName");
                    late.show(); pump(40);
                    check(edit && edit->text()=="Zed","settings: the name box shows a name saved after the panel was built");
                    if (edit) { edit->setFocus(); emit edit->editingFinished(); pump(20); check(Greeting::userName()=="Zed","settings: pressing Return in the name box keeps the name"); }
                    late.hide(); Greeting::setUserName("Archit");
                }
                // Opening a saved chat does not use up a greeting: New chat afterwards never repeats the one last shown.
                {
                    QTemporaryDir greetDir; ChatStore greetStore(greetDir.path()+"/chats"); chat.setChatStore(&greetStore);
                    blank(); fixture.chatReply="hi";
                    check(sendAndWait("greet me"),"greeting: a chat to reopen is sent");
                    const QString greetId=chat.currentChatId();
                    bool repeated2=false;
                    for (int i=0;i<30;++i) { blank(); const QString shown=greeting(); chat.openChat(greetId); blank(); repeated2|=greeting()==shown; }
                    check(!repeated2,"greeting: opening a saved chat between two blank chats does not make the greeting repeat");
                    chat.setChatStore(nullptr); blank();
                }
                chat.setHourProviderForTest({}); QSettings().remove("profile/name");
                blank();
            }
            input->clear();
        }
        QMetaObject::invokeMethod(&chat,"onClearChat",Qt::DirectConnection);
        chat.hide();
    }
    settings.show(); pump();
    bool hosted=false; for(auto *edit:settings.findChildren<QLineEdit *>()) hosted|=edit->echoMode()==QLineEdit::Password;
    QComboBox *engineCombo=nullptr; for(auto *combo:settings.findChildren<QComboBox *>()) if(combo->accessibleName()=="Default inference engine") engineCombo=combo;
    check(!hosted && engineCombo && engineCombo->count()==3,"Settings exposes only supported local engines");
    auto *motion=settings.findChild<QCheckBox *>("reducedMotionToggle"); motion->setChecked(true);
    check(QSettings().value("appearance/studio3ReducedMotion").toBool(),"reduced motion saves immediately");
    settings.selectCategory(1); pump(); capture(&settings,"settings-appearance"); settings.hide();
    LogoAnimation logo;
    check(logo.frameCount()==540 && logo.framesPerSecond()==30,"restored logo has 540 frames at 30 fps");
    auto renderLogo = [&logo](qint64 ms, int size = 420) {
        QImage image(size,size,QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        logo.paint(painter,QRectF(0,0,size,size),ms);
        return image;
    };
    const auto firstLogoFrame = renderLogo(0);
    check(firstLogoFrame != renderLogo(1000),"restored logo changes frames");
    check(firstLogoFrame == renderLogo(18000),"restored logo loops after 18 seconds");
    bool allFrames = true;
    for (int i=0;i<540;++i) {
        QImage image(32,32,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter painter(&image);
        allFrames = logo.paint(painter,QRectF(0,0,32,32),(i*1000+29)/30) && allFrames;
    }
    check(allFrames,"all restored vector frames decode");
    check(firstLogoFrame.pixelColor(210,210).alpha()>0 && firstLogoFrame.pixelColor(0,0).alpha()==0,
          "logo background is transparent");
    check(renderLogo(0,840).pixelColor(420,420).alpha()>0,"restored logo renders at high DPI");
    if (!captures.isEmpty()) renderLogo(0,840).save(captures+"/restored-logo.png");
    StudioCanvas wheel; wheel.setProperty("brandMark",true); wheel.setProperty("activityIndicator",true);
    wheel.resize(210,210); wheel.show(); wheel.activateWindow(); pump();
    for(auto activity:{StudioCanvas::Training,StudioCanvas::Chat}) {
        wheel.setActivity(activity); wheel.setMotionEnabled(true);
        auto image=wheel.grab().toImage(); pump(180); check(image!=wheel.grab().toImage(),"activity wheel animates");
        wheel.setMotionEnabled(false); image=wheel.grab().toImage(); pump(180);
        check(image==wheel.grab().toImage(),"reduced motion wheel stays still");
    }
    wheel.setActivity(StudioCanvas::Idle); wheel.setMotionEnabled(true);
    auto image=wheel.grab().toImage(); pump(180); check(image==wheel.grab().toImage(),"idle training wheel stays still");
    capture(&wheel,"konark-wheel"); wheel.hide();
    if(!captures.isEmpty()) {
        QSvgGenerator svg; svg.setFileName(captures+"/konark-wheel.svg"); svg.setSize(QSize(216,216)); svg.setViewBox(QRect(0,0,216,216));
        svg.setTitle("Magnite · Konark-inspired wheel"); QPainter painter(&svg);
        BrandMark::paint(painter,QRectF(0,0,216,216),QColor(Theme::accentPrimary()));
    }
    {
        using Ids=QList<ChatTree::NodeId>;
        ChatTree tree;
        check(tree.isEmpty() && tree.activePath().isEmpty() && tree.messages().isEmpty() && tree.lastOnPath()==ChatTree::kRoot,"chat tree: an empty tree has an empty path and no messages");
        const auto q1=tree.addChild(ChatTree::kRoot,"user","q1"); const auto a1=tree.addChild(q1,"assistant","a1");
        const auto q2=tree.addChild(a1,"user","q2"); const auto a2=tree.addChild(q2,"assistant","a2");
        {
            const auto msgs=tree.messages();
            check(!tree.isEmpty() && tree.activePath()==Ids({q1,a1,q2,a2}) && msgs.size()==4 && msgs.at(0).value("role").toString()=="user" && msgs.at(0).value("content").toString()=="q1" &&
                  msgs.at(3).value("role").toString()=="assistant" && msgs.at(3).value("content").toString()=="a2" && tree.lastOnPath()==a2 && tree.pathIndexOf(q2)==2 &&
                  tree.parentOf(a1)==q1 && tree.parentOf(q1)==ChatTree::kRoot && tree.role(a1)=="assistant" && tree.content(q2)=="q2",
                  "chat tree: messages added in turn form the path in order");
            check(tree.messages(2).size()==2 && tree.messages(2).at(1).value("content").toString()=="a1" && tree.messages(0).isEmpty() && tree.messages(99).size()==4,
                  "chat tree: messages(pathCount) returns only the leading messages");
        }
        const auto q2b=tree.addChild(a1,"user","q2 edited");
        check(tree.activePath()==Ids({q1,a1,q2b}) && tree.versionCount(q2b)==2 && tree.versionIndex(q2b)==1 && tree.versionIndex(q2)==0 &&
              tree.versionsOf(q2)==Ids({q2,q2b}) && tree.pathIndexOf(q2)==-1 && tree.messages().size()==3 && tree.versionCount(q1)==1 && tree.versionIndex(q1)==0,
              "chat tree: a sibling becomes the newest, active version and counts n of n");
        const auto q2c=tree.addChild(a1,"user","q2 again");
        check(tree.versionCount(q2c)==3 && tree.versionIndex(q2c)==2 && tree.versionIndex(q2)==0,"chat tree: a second edit of the same message counts 3 of 3");
        tree.activate(q2);
        check(tree.activePath()==Ids({q1,a1,q2,a2}) && tree.versionIndex(q2)==0,"chat tree: activating an older version restores its whole continuation");
        const auto q1b=tree.addChild(ChatTree::kRoot,"user","q1 edited");
        check(tree.activePath()==Ids({q1b}) && tree.parentOf(q1b)==ChatTree::kRoot && tree.versionCount(q1)==2 && tree.versionIndex(q1b)==1,"chat tree: editing the first message branches from the root");
        tree.activate(q1);
        check(tree.activePath()==Ids({q1,a1,q2,a2}),"chat tree: switching the first message back restores the whole earlier conversation");
        const auto a2c=tree.addChild(q2,"assistant","a2 retried");
        const auto a2b=tree.addChild(q2b,"assistant","a2 for edit");
        tree.activate(q2b);
        check(tree.activePath()==Ids({q1,a1,q2b,a2b}) && tree.versionCount(a2c)==2 && tree.versionsOf(a2b)==Ids({a2b}) && tree.versionCount(a2b)==1,
              "chat tree: an edit inside an older version branches again");
        tree.activate(q2);
        check(tree.activePath()==Ids({q1,a1,q2,a2c}) && tree.versionsOf(a2c)==Ids({a2,a2c}),"chat tree: each version remembers its own latest reply");

        ChatTree t2;
        const auto u1=t2.addChild(ChatTree::kRoot,"user","u1"); const auto r1=t2.addChild(u1,"assistant","r1");
        const auto u2=t2.addChild(r1,"user","u2"); const auto r2=t2.addChild(u2,"assistant","r2"); const auto u3=t2.addChild(r2,"user","u3");
        check(t2.compaction().covered==0 && t2.compaction().summary.isEmpty(),"chat tree: no snapshot means no compaction");
        t2.setCompaction(u2,{"S0",1}); t2.setCompaction(u3,{"S",2});
        check(t2.compaction().summary=="S" && t2.compaction().covered==2,"chat tree: compaction is the last snapshot on the path");
        check(t2.compaction(4).summary=="S0" && t2.compaction(4).covered==1 && t2.compaction(3).summary=="S0" && t2.compaction(2).covered==0,
              "chat tree: compaction(pathCount) ignores nodes at or after pathCount");
        t2.addNote(u3,"Context compacted");
        check(t2.notes(u3)==QStringList({"Context compacted"}) && t2.notes(u2).isEmpty(),"chat tree: notes stay with their node");
        const auto u3b=t2.addChild(r2,"user","u3 edited");
        check(t2.compaction().summary=="S0" && t2.compaction().covered==1 && t2.notes(u3b).isEmpty(),"chat tree: a snapshot on a node that left the path no longer applies");
        t2.activate(u3);
        check(t2.compaction().summary=="S" && t2.compaction().covered==2 && t2.notes(u3)==QStringList({"Context compacted"}),"chat tree: switching back restores the snapshot and the notes");

        t2.clear();
        check(t2.isEmpty() && t2.activePath().isEmpty() && t2.compaction().covered==0 && t2.addChild(ChatTree::kRoot,"user","fresh")==1,"chat tree: clear resets everything");

        // JSON round-trip: the whole tree, including versions that are not on screen, comes back unchanged.
        {
            ChatTree c; // compacted conversation with an edited first message, an edited reply branch and a retried reply
            const auto c1=c.addChild(ChatTree::kRoot,"user","c1 é世 \"quoted\"\nline2"); const auto d1=c.addChild(c1,"assistant","d1");
            const auto c2=c.addChild(d1,"user","c2"); const auto d2=c.addChild(c2,"assistant","d2"); const auto c3=c.addChild(d2,"user","c3");
            c.setCompaction(c3,{"summary of c1..d1",2}); c.addNote(c3,"Context compacted"); c.addNote(c3,"second note");
            c.addChild(c2,"assistant","d2 retried"); c.addChild(d1,"user","c2 edited"); c.addChild(ChatTree::kRoot,"user","c1 edited");
            c.activate(c1); c.activate(c2); // a non-trivial active choice: older versions active, newer ones parked
            const QJsonObject json=c.toJson();
            const auto back=ChatTree::fromJson(json);
            check(back.has_value() && back->toJson()==json,"chat tree json: a reloaded tree writes exactly the same json");
            if (back) {
                bool same=back->activePath()==c.activePath() && back->lastOnPath()==c.lastOnPath() && back->messages()==c.messages() &&
                          back->compaction().summary==c.compaction().summary && back->compaction().covered==c.compaction().covered;
                for (ChatTree::NodeId id=1; id<=12; ++id)
                    same &= back->role(id)==c.role(id) && back->content(id)==c.content(id) && back->parentOf(id)==c.parentOf(id) &&
                            back->versionsOf(id)==c.versionsOf(id) && back->versionIndex(id)==c.versionIndex(id) && back->notes(id)==c.notes(id);
                check(same,"chat tree json: path, messages, versions, notes and compaction match after a reload");
                ChatTree reloaded=*back;
                c.activate(d2); reloaded.activate(d2);
                check(reloaded.activePath()==c.activePath() && reloaded.messages()==c.messages() && reloaded.compaction().covered==c.compaction().covered && reloaded.compaction().covered==2,
                      "chat tree json: the request a reloaded conversation would send equals the original's, after switching versions too");
                reloaded.activate(ChatTree::NodeId(8)); c.activate(ChatTree::NodeId(8)); // the edited first message: another branch
                check(reloaded.messages()==c.messages() && reloaded.compaction().covered==0,"chat tree json: a parked first-message version comes back with its own continuation");
                check(reloaded.addChild(reloaded.lastOnPath(),"assistant","after reload")==9,"chat tree json: new messages continue the ids after a reload");
            }
            ChatTree empty;
            const auto emptyBack=ChatTree::fromJson(empty.toJson());
            check(emptyBack.has_value() && emptyBack->isEmpty() && emptyBack->activePath().isEmpty(),"chat tree json: an empty tree round-trips");

            ChatTree g; const auto gq=g.addChild(ChatTree::kRoot,"user","hi"); const auto gr=g.addChild(gq,"assistant","yo");
            g.addNote(gq,"n"); g.setCompaction(gr,{"S",1});
            const QJsonObject good=g.toJson();
            check(ChatTree::fromJson(good).has_value(),"chat tree json: the small good tree is accepted");
            auto withNode=[&good](int index,const QString &name,const QJsonValue &value) {
                QJsonObject o=good; QJsonArray nodes=o.value("nodes").toArray(); QJsonObject n=nodes.at(index).toObject();
                n.insert(name,value); nodes.replace(index,n); o.insert("nodes",nodes); return o;
            };
            auto withTop=[&good](const QString &name,const QJsonValue &value) { QJsonObject o=good; o.insert(name,value); return o; };
            auto rejects=[&check](const QJsonObject &o,const QString &why) { check(!ChatTree::fromJson(o).has_value(),"chat tree json: rejects "+why); };
            rejects(withNode(1,"parent",99),"a parent id out of range");
            rejects(withNode(1,"parent",0),"a child whose parent field disagrees with its parent's child list");
            rejects(withNode(0,"children",QJsonArray{2,2}),"a child listed twice");
            rejects(withNode(0,"children",QJsonArray{1}),"a node that is its own child");
            rejects(withNode(0,"activeChild",5),"an active child that is not one of the children");
            rejects(withNode(0,"activeChild",0),"a node with children but no active child");
            rejects(withTop("rootActive",7),"a root active child that is not a root child");
            rejects(withTop("rootChildren",QJsonArray()),"nodes that cannot be reached from the root");
            rejects(withTop("rootChildren",QJsonArray{1,2}),"a root child that is not parented by the root");
            rejects(withNode(0,"role","robot"),"an unknown role");
            rejects(withNode(0,"content",5),"a content that is not text");
            rejects(withNode(0,"notes",QJsonArray{1}),"a note that is not text");
            rejects(withNode(1,"compaction",QJsonObject{{"summary","x"},{"covered",-1}}),"a negative covered count");
            rejects(withNode(1,"compaction",QJsonObject{{"summary","x"},{"covered",5}}),"a covered count larger than the conversation up to that message");
            rejects(withNode(0,"id",9),"a node whose id is not its position");
            rejects(withTop("nodes","nope"),"nodes of the wrong type");
            rejects(QJsonObject(),"an object with nothing in it");
            QJsonObject missing=good; missing.remove("nodes"); rejects(missing,"a missing nodes list");
            QJsonObject missingRoot=good; missingRoot.remove("rootChildren"); rejects(missingRoot,"missing rootChildren");
        }
    }
    {
        // ChatStore at scale: how long does launching with a big history take? (report #13, measured not assumed)
        QTemporaryDir tmp; const QString dir=tmp.path()+"/chats";
        {
            ChatStore seed(dir); const int chats=2000;
            for(int i=0;i<chats;++i) {
                ChatTree t; ChatTree::NodeId at=ChatTree::kRoot;
                for(int m=0;m<3;++m) { at=t.addChild(at,"user",QString("question %1 of chat %2 — a few more words so the file has a realistic size").arg(m).arg(i)); at=t.addChild(at,"assistant",QString("answer %1 ").arg(m).repeated(40)); }
                seed.save(seed.create(),t);
            }
        }
        QElapsedTimer timer; timer.start();
        ChatStore cold(dir);   // first launch with this many chats: every file is parsed and the index is written
        const qint64 coldMs=timer.elapsed();
        timer.restart();
        ChatStore warm(dir);   // the next launch: titles and dates come from the index
        const qint64 warmMs=timer.elapsed();
        fprintf(stderr,"[ui-check] chat store: 2000 chats listed in %lld ms on the first launch and %lld ms on the next\n",static_cast<long long>(coldMs),static_cast<long long>(warmMs));
        check(cold.list().size()==2000 && warm.list().size()==2000 && warm.list().first().id==cold.list().first().id,"chat store: 2000 saved chats are all listed, in the same order, after a relaunch");
        // Keep timings diagnostic: scheduling and filesystem caches can change the ratio on CI.
    }
    {
        // ChatStore: damaged chats are reported and never touched; a sibling index keeps launch cheap and heals itself.
        QTemporaryDir tmp; const QString dir=tmp.path()+"/chats"; QDir().mkpath(dir);
        auto raw=[&dir](const QString &id) { QFile f(dir+"/"+id+".json"); f.open(QIODevice::ReadOnly); return f.readAll(); };
        auto writeRaw=[&dir](const QString &id,const QByteArray &bytes) { QFile f(dir+"/"+id+".json"); f.open(QIODevice::WriteOnly|QIODevice::Truncate); f.write(bytes); };
        QStringList good;
        {
            ChatStore seed(dir);
            for(int i=0;i<5;++i) { const QString id=seed.create(); ChatTree t; t.addChild(ChatTree::kRoot,"user",QString("chat %1").arg(i)); seed.save(id,t); good<<id; }
        }
        const QString truncated=QUuid::createUuid().toString(QUuid::WithoutBraces), garbage=QUuid::createUuid().toString(QUuid::WithoutBraces), newer=QUuid::createUuid().toString(QUuid::WithoutBraces);
        writeRaw(truncated,"{\"version\":1,\"id\":"); writeRaw(garbage,"this is not json"); writeRaw(newer,"{\"version\":999}");
        { QFile notes(dir+"/notes.json"); notes.open(QIODevice::WriteOnly); notes.write("not a chat"); }
        const QByteArray before1=raw(truncated), before2=raw(garbage), before3=raw(newer);
        QStringList damaged={truncated,garbage,newer}; damaged.sort();
        {
            ChatStore store(dir);
            check(store.list().size()==5 && store.unreadableIds()==damaged,"chat store: damaged chats are counted, healthy ones listed, and a non-chat json file is ignored");
            check(raw(truncated)==before1 && raw(garbage)==before2 && raw(newer)==before3 && QFile::exists(dir+"/notes.json"),"chat store: damaged files are left exactly as they were");
        }
        check(QFile::exists(dir+".index") && !QFile::exists(dir+"/index.json"),"chat store: the launch index lives beside the chats folder, not inside it");
        {
            // Same size and modification time: the index is trusted, the file is not parsed again.
            const QString victim=good.first(); const QByteArray original=raw(victim);
            const QFileInfo info(dir+"/"+victim+".json"); const QDateTime stampBefore=info.lastModified();
            writeRaw(victim,QByteArray(original.size(),'x'));
            { QFile f(dir+"/"+victim+".json"); f.open(QIODevice::ReadWrite); f.setFileTime(stampBefore,QFileDevice::FileModificationTime); }
            ChatStore cached(dir);
            check(cached.list().size()==5 && cached.unreadableIds()==damaged,"chat store: an unchanged file (same size and time) is taken from the index, not re-read");
            // A changed file is re-read: the damage shows up.
            writeRaw(victim,QByteArray("changed on disk and damaged"));
            ChatStore healed(dir);
            check(healed.list().size()==4 && healed.unreadableIds().contains(victim),"chat store: a file that changed since the index was written is read again");
            writeRaw(victim,original);
            ChatStore repaired(dir);
            check(repaired.list().size()==5 && repaired.unreadableIds()==damaged,"chat store: repairing the file brings the chat back");
        }
        {
            // A read that fails only for a moment (a sync client or antivirus holding the file) is not damage and must not be remembered as damage.
            QString held; { ChatStore s(dir); held=s.create(); ChatTree t; t.addChild(ChatTree::kRoot,"user","a chat on a synced folder"); s.save(held,t); }
            const QString path=dir+"/"+held+".json";
#ifdef Q_OS_WIN
            // QFile permissions cannot deny read access on Windows; hold the file without sharing instead.
            const HANDLE heldFile=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,0,nullptr,
                                               OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            check(heldFile!=INVALID_HANDLE_VALUE,"chat store: the fixture exclusively locks its chat file");
#else
            const auto original=QFile::permissions(path);
            check(QFile::setPermissions(path,QFileDevice::Permissions()),"chat store: the fixture removes read permissions");
#endif
            ChatStore during(dir);
            check(during.unreadableIds().contains(held) && during.list().size()==5,"chat store: a file that cannot be opened is reported while it is locked");
#ifdef Q_OS_WIN
            if (heldFile!=INVALID_HANDLE_VALUE) CloseHandle(heldFile);
#else
            check(QFile::setPermissions(path,original),"chat store: the fixture restores read permissions");
#endif
            ChatStore after(dir);
            check(after.list().size()==6 && after.unreadableIds()==damaged,"chat store: once the file can be opened again the chat is back (a failed read is not cached as damage)");
        }
        {
            { QFile f(dir+".index"); f.open(QIODevice::WriteOnly|QIODevice::Truncate); f.write("garbage, not json"); }
            ChatStore rebuilt(dir);
            check(rebuilt.list().size()==6 && rebuilt.unreadableIds()==damaged,"chat store: a damaged index is ignored and rebuilt");
            QFile::remove(dir+"/"+good.at(1)+".json");
            ChatStore afterDelete(dir);
            check(afterDelete.list().size()==5 && !afterDelete.unreadableIds().contains(good.at(1)),"chat store: a chat deleted outside the app drops out of the list");
        }
        {
            ChatHistoryList history; ChatStore store(dir);
            history.setUnreadableCount(store.unreadableIds().size());
            auto *notice=history.findChild<QLabel *>("historyNotice");
            check(notice && !notice->isHidden() && notice->text()=="3 chats could not be read","sidebar: a visible notice counts the chats that could not be read");
            history.setUnreadableCount(1); check(notice && notice->text()=="1 chat could not be read","sidebar: the notice is singular for one chat");
            history.setUnreadableCount(0); check(notice && notice->isHidden(),"sidebar: no notice when every chat is readable");
        }
    }
    {
        // ChatStore: one json file per chat, atomic writes, bad files skipped and never touched.
        const bool hadHome=qEnvironmentVariableIsSet("MAGNITE_HOME");
        const QByteArray savedHome=qgetenv("MAGNITE_HOME");
        QSettings storageSettings;
        const QVariant savedDir=storageSettings.value("storage/dataDir");
        qunsetenv("MAGNITE_HOME"); storageSettings.remove("storage/dataDir");
#ifdef Q_OS_WIN
        QString expectedHome=qEnvironmentVariable("APPDATA");
        if (expectedHome.isEmpty()) expectedHome=QDir::homePath()+"/.magnite";
        expectedHome+="/Magnite";
#else
        const QString expectedHome=QDir::homePath()+"/.magnite";
#endif
        check(ChatStore::defaultDir()==QFileInfo(expectedHome).absoluteFilePath()+"/chats","saved chats use the platform backend data home");
        storageSettings.setValue("storage/dataDir","settings-chats-home");
        check(ChatStore::defaultDir()==QFileInfo("settings-chats-home").absoluteFilePath()+"/chats","saved chats respect the configured data directory");
        qputenv("MAGNITE_HOME","env-chats-home");
        check(ChatStore::defaultDir()==QFileInfo("env-chats-home").absoluteFilePath()+"/chats","saved chats prefer MAGNITE_HOME over saved settings");
        if (hadHome) qputenv("MAGNITE_HOME",savedHome); else qunsetenv("MAGNITE_HOME");
        if (savedDir.isValid()) storageSettings.setValue("storage/dataDir",savedDir); else storageSettings.remove("storage/dataDir");
        QTemporaryDir tmp; const QString dir=tmp.path()+"/chats";
        auto makeTree=[](const QString &question,const QString &answer) { ChatTree t; const auto q=t.addChild(ChatTree::kRoot,"user",question); if(!answer.isEmpty()) t.addChild(q,"assistant",answer); return t; };
        auto files=[&dir]() { return QDir(dir).entryList(QDir::Files|QDir::NoDotAndDotDot,QDir::Name); };
        auto ids=[](const QList<ChatSummary> &rows) { QStringList out; for(const auto &r:rows) out<<r.id; return out; };
        auto loneSurrogate=[](const QString &text) {
            for(int i=0;i<text.size();++i) { if(text.at(i).isHighSurrogate()) { if(i+1>=text.size()||!text.at(i+1).isLowSurrogate()) return true; ++i; } else if(text.at(i).isLowSurrogate()) return true; }
            return false;
        };
        int changes=0;
        ChatStore store(dir);
        QObject::connect(&store,&ChatStore::changed,[&changes]() { ++changes; });
        check(store.list().isEmpty() && !QDir(dir).exists(),"chat store: a new store on a fresh folder is empty and creates nothing yet");
        check(ChatStore::validId("0b6c3a52-9d0e-4f1c-8a77-0123456789ab") && !ChatStore::validId("") && !ChatStore::validId("../x") && !ChatStore::validId("a/b") &&
              !ChatStore::validId("0B6C3A52-9D0E-4F1C-8A77-0123456789AB") && !ChatStore::validId("0b6c3a52-9d0e-4f1c-8a77-0123456789a") && !ChatStore::validId("0b6c3a52-9d0e-4f1c-8a77-0123456789ab.json"),
              "chat store: only real lowercase uuids are valid ids");
        const QString idA=store.create();
        check(ChatStore::validId(idA) && idA!=store.create() && store.list().isEmpty() && !QDir(dir).exists() && changes==0,"chat store: create gives a fresh valid id and writes nothing");
        const ChatTree treeA=makeTree("How do I join two tables?","Use JOIN.");
        check(store.save(idA,treeA) && files()==QStringList({idA+".json"}) && changes==1,"chat store: saving writes exactly one file and announces the change");
        check(store.list().size()==1 && store.list().at(0).id==idA && store.list().at(0).title=="How do I join two tables?" && !store.list().at(0).customTitle && store.list().at(0).created.isValid() && store.list().at(0).updated.isValid(),
              "chat store: the saved chat is listed with a title from its first message");
        const auto loadedA=store.load(idA);
        check(loadedA.has_value() && loadedA->toJson()==treeA.toJson(),"chat store: a saved chat loads back identical");
        const QString idB=store.create(); store.save(idB,makeTree("Second chat",""));
        check(ids(store.list())==QStringList({idB,idA}),"chat store: the chat saved most recently is listed first");
        ChatTree longer=treeA; longer.addChild(longer.lastOnPath(),"user","and left joins?");
        const QDateTime createdA=store.list().at(1).created;
        store.save(idA,longer);
        check(ids(store.list())==QStringList({idA,idB}) && store.list().at(0).created==createdA && store.list().at(0).updated>store.list().at(1).updated,"chat store: saving again moves the chat to the top and keeps its creation time");
        changes=0;
        check(store.rename(idB,"  My renamed chat  ") && changes==1 && ids(store.list())==QStringList({idA,idB}),"chat store: renaming changes the title but not the order");
        {
            const auto row=store.list().at(1);
            check(row.title=="My renamed chat" && row.customTitle,"chat store: a renamed chat is trimmed and marked custom");
        }
        ChatTree moreB=makeTree("Second chat","reply"); store.save(idB,moreB);
        check(store.list().at(0).id==idB && store.list().at(0).title=="My renamed chat","chat store: a renamed chat keeps its name when saved again");
        check(!store.rename(idB,"   ") && !store.rename(idB,"") && !store.rename("not-an-id","x") && !store.rename("0b6c3a52-9d0e-4f1c-8a77-0123456789ab","x") && store.list().at(0).title=="My renamed chat",
              "chat store: an empty title, a bad id or an unknown chat cannot rename");
        check(!store.remove("0b6c3a52-9d0e-4f1c-8a77-0123456789ab") && !store.remove("../x"),"chat store: removing an unknown or invalid id does nothing");
        {
            ChatStore again(dir);
            check(ids(again.list())==ids(store.list()) && again.list().at(0).title==store.list().at(0).title && again.list().at(0).updated==store.list().at(0).updated && again.list().at(1).created==store.list().at(1).created,
                  "chat store: a new store on the same folder lists the same chats in the same order");
        }
        check(files().size()==2 && files().filter(QRegularExpression("\\.json$")).size()==2,"chat store: no temporary files are left behind");

        // titles
        check(ChatStore::titleFor(ChatTree())=="New chat" && ChatStore::titleFor(makeTree("   \n\t ",""))=="New chat","chat store: an empty chat or a blank first message is titled New chat");
        check(ChatStore::titleFor(makeTree("a\n\nb\t\tc   d",""))=="a b c d","chat store: line breaks and runs of spaces collapse in a title");
        check(ChatStore::titleFor(makeTree(QString(40,'x'),""))==QString(40,'x') && ChatStore::titleFor(makeTree(QString(100,'y'),""))==QString(40,'y')+QString::fromUtf8("\u2026"),"chat store: a title is cut to 40 characters plus an ellipsis only when longer");
        {
            QString emoji; for(int i=0;i<100;++i) emoji+=QString::fromUtf8("\U0001F600");
            const QString cut=ChatStore::titleFor(makeTree(emoji,""));
            check(!loneSurrogate(cut) && cut.endsWith(QString::fromUtf8("\u2026")) && cut.size()<=41,"chat store: a long title of non-ASCII characters is never cut inside a character");
            check(ChatStore::titleFor(makeTree("Caf\u00e9 \u4e16\u754c \"quoted\"",""))=="Caf\u00e9 \u4e16\u754c \"quoted\"","chat store: accents, CJK and quotes survive in a title");
        }
        {
            ChatTree edited=makeTree("original first message","r"); edited.addChild(ChatTree::kRoot,"user","edited first message");
            check(ChatStore::titleFor(edited)=="original first message","chat store: the title comes from the first version of the first message");
        }

        // removal
        check(store.remove(idA) && !files().contains(idA+".json") && ids(store.list())==QStringList({idB}) && !store.load(idA).has_value(),"chat store: removing a chat deletes its file and its row");

        // bad files
        {
            const QString good=store.create(); store.save(good,makeTree("good chat","ok"));
            auto write=[&dir,&check](const QString &name,const QByteArray &bytes) { QFile f(dir+"/"+name); check(f.open(QIODevice::WriteOnly),"open chat-store fixture"); f.write(bytes); };
            const QString idJunk="11111111-1111-4111-8111-111111111111", idEmpty="22222222-2222-4222-8222-222222222222", idNewer="33333333-3333-4333-8333-333333333333",
                          idMismatch="44444444-4444-4444-8444-444444444444", idBroken="55555555-5555-4555-8555-555555555555";
            write("garbage.json","not json at all"); write(idJunk+".json","{ truncated"); write(idEmpty+".json","");
            QJsonObject newer; { QFile f(dir+"/"+good+".json"); check(f.open(QIODevice::ReadOnly),"read chat-store fixture"); newer=QJsonDocument::fromJson(f.readAll()).object(); }
            QJsonObject n2=newer; n2["version"]=99; n2["id"]=idNewer; write(idNewer+".json",QJsonDocument(n2).toJson());
            QJsonObject n3=newer; n3["id"]="66666666-6666-4666-8666-666666666666"; write(idMismatch+".json",QJsonDocument(n3).toJson());
            QJsonObject n4=newer; n4["id"]=idBroken; QJsonObject tree=n4["tree"].toObject(); tree["rootActive"]=42; n4["tree"]=tree; write(idBroken+".json",QJsonDocument(n4).toJson());
            const int before=files().size();
            ChatStore withBad(dir);
            const QStringList listed=ids(withBad.list());
            check(listed.contains(good) && listed.contains(idB) && listed.size()==2,"chat store: unreadable, empty, newer-version, mismatched and broken files are left out of the list");
            check(!withBad.load(idJunk) && !withBad.load(idEmpty) && !withBad.load(idNewer) && !withBad.load(idMismatch) && !withBad.load(idBroken) && withBad.load(good).has_value(),"chat store: loading a bad file gives nothing, loading a good one still works");
            check(files().size()==before && files().contains("garbage.json") && files().contains(idJunk+".json") && files().contains(idEmpty+".json") && files().contains(idNewer+".json"),"chat store: bad files stay on disk untouched");
            check(withBad.save(good,makeTree("good chat","ok again")) && withBad.rename(good,"still works"),"chat store: saving and renaming still work next to bad files");
            for(const auto &name:{"garbage.json"}) QFile::remove(dir+"/"+name);
            for(const auto &id:{idJunk,idEmpty,idNewer,idMismatch,idBroken}) QFile::remove(dir+"/"+id+".json");
        }

        // failures
        {
            QFile blocker(tmp.path()+"/blocker"); check(blocker.open(QIODevice::WriteOnly),"open blocked-store fixture"); blocker.write("x"); blocker.close();
            ChatStore blocked(tmp.path()+"/blocker/chats");
            check(!blocked.save(blocked.create(),makeTree("x","y")) && !blocked.lastError().isEmpty() && blocked.list().isEmpty(),"chat store: a folder that cannot be created fails the save with a message");
            const QString keep=ids(store.list()).first();
            const auto before=store.load(keep);
            check(QDir().rename(dir,dir+".saved"),"preserve the store before blocking writes");
            QFile storageBlocker(dir);
            check(storageBlocker.open(QIODevice::WriteOnly),"block the store directory with a file");
            storageBlocker.close();
            const bool saved=store.save(keep,makeTree("changed","changed"));
            const QString error=store.lastError();
            check(storageBlocker.remove() && QDir().rename(dir+".saved",dir),"restore the store after the failed save");
            const auto after=store.load(keep);
            check(!saved && !error.isEmpty() && before.has_value() && after.has_value() && before->toJson()==after->toJson(),"chat store: a failed save reports an error and leaves the previous file intact");
        }
    }
    {
        // Greeting: time-of-day wording and the stored name.
        QSettings().remove("profile/name"); QSettings().remove("profile/nameAsked");
        const QStringList morning=Greeting::variants(5,"Archit");
        check(morning.size()==3 && morning.at(0)=="Good morning, Archit" && morning.at(1)=="Morning, Archit" && morning.at(2)=="Rise and shine, Archit","greeting: the morning wordings use the name");
        check(Greeting::variants(14,"Archit")==QStringList({"Good afternoon, Archit","Afternoon, Archit","Hope your day is going well, Archit"}),"greeting: the afternoon wordings");
        check(Greeting::variants(19,"Archit")==QStringList({"Good evening, Archit","Evening, Archit","Winding down, Archit?"}),"greeting: the evening wordings");
        check(Greeting::variants(23,"Archit")==QStringList({"Up late, Archit?","Burning the midnight oil, Archit?","Still up, Archit?"}),"greeting: the late-night wordings, ending in a question");
        const QMap<int,QString> bands={{0,"Up late"},{3,"Up late"},{4,"Up late"},{5,"Good morning"},{11,"Good morning"},{12,"Good afternoon"},{16,"Good afternoon"},{17,"Good evening"},{21,"Good evening"},{22,"Up late"},{23,"Up late"}};
        bool edges=true; for(auto it=bands.begin();it!=bands.end();++it) edges&=Greeting::variants(it.key(),"A").at(0).startsWith(it.value());
        check(edges,"greeting: every hour lands in the right band, including 4/5, 11/12, 16/17, 21/22 and midnight");
        check(Greeting::variants(-3,"A").at(0).startsWith("Up late") && Greeting::variants(27,"A").at(0).startsWith("Up late"),"greeting: an hour outside 0-23 does not crash");
        check(Greeting::variants(5,"").at(0)=="Good morning" && Greeting::variants(5,"   ").at(1)=="Morning" && Greeting::variants(5,"").at(2)=="Rise and shine","greeting: without a name the wording drops it");
        check(Greeting::variants(23,"").at(0)=="Up late?" && Greeting::variants(23,"").at(1)=="Burning the midnight oil?" && Greeting::variants(19,"").at(2)=="Winding down?","greeting: without a name the question marks stay");
        {
            const QStringList options=Greeting::variants(5,"A");
            bool never=true; for(int r=0;r<=20;++r) for(const QString &previous:options) never&=Greeting::pick(options,previous,r)!=previous;
            check(never,"greeting: pick never repeats the previous wording");
            check(Greeting::pick({"only"},"only",7)=="only" && Greeting::pick(options,QString(),1)==options.at(1) && Greeting::pick(options,QString(),4)==options.at(1),"greeting: pick uses the random number, and a single option is returned even if repeated");
            check(!Greeting::pick(options,"x",-5).isEmpty() && !Greeting::pick(options,"x",2000000000).isEmpty() && Greeting::pick({}, "x", 3).isEmpty(),"greeting: pick copes with negative, huge and empty input");
        }
        check(Greeting::cleanName("  Ada  ")=="Ada" && Greeting::cleanName("   ").isEmpty() && Greeting::cleanName(QString(60,'n')).size()==40 && Greeting::cleanName("Zo\u00eb \u4e16\u754c")=="Zo\u00eb \u4e16\u754c","greeting: names are trimmed, capped at 40 characters and keep non-ASCII");
        {
            QString emoji; for(int i=0;i<30;++i) emoji+=QString::fromUtf8("\U0001F600");
            const QString cut=Greeting::cleanName(emoji);
            bool lone=false; for(int i=0;i<cut.size();++i) { if(cut.at(i).isHighSurrogate()) { if(i+1>=cut.size()||!cut.at(i+1).isLowSurrogate()) lone=true; ++i; } else if(cut.at(i).isLowSurrogate()) lone=true; }
            check(!lone && cut.size()<=40,"greeting: a long name is never cut inside a character");
        }
        check(Greeting::userName().isEmpty() && !Greeting::nameAsked(),"greeting: no name and not asked before anything is stored");
        Greeting::setUserName("  Archit "); Greeting::setNameAsked(true);
        check(Greeting::userName()=="Archit" && Greeting::nameAsked() && QSettings().value("profile/name").toString()=="Archit","greeting: the name and the asked flag are stored in Settings");
        Greeting::setUserName("   ");
        check(Greeting::userName().isEmpty(),"greeting: a blank name clears it");
        QSettings().remove("profile/name"); QSettings().remove("profile/nameAsked");
        const QString suggestion=Greeting::accountNameSuggestion();
        check(suggestion.isEmpty() || suggestion.at(0).isUpper() || !suggestion.at(0).isLetter(),"greeting: the account name suggestion starts with a capital letter");
    }
    {
        // Sidebar state: the decisions behind the collapsible left menu.
        const QString key=SidebarState::kSettingsKey;
        QSettings().remove(key);
        SidebarState s;
        check(!s.collapsed() && s.width()==195 && s.showLabels() && s.showHistory(true) && !s.showHistory(false) && !s.showNewChatButton(true),"sidebar state: expanded by default");
        s.setCollapsed(true);
        check(s.width()==52 && !s.showLabels() && !s.showHistory(true) && s.showNewChatButton(true) && !s.showNewChatButton(false),"sidebar state: collapsed hides labels and list, offers + only on Playground");
        check(s.toggleTooltip()=="Expand sidebar \u00b7 Ctrl+B","sidebar state: the collapsed toggle offers to expand");
        check(s.toggle()==false && !s.collapsed() && s.toggleTooltip()=="Collapse sidebar \u00b7 Ctrl+B" && s.toggle()==true && s.collapsed(),"sidebar state: toggle returns the new state and flips back and forth");
        check(!SidebarState::load().collapsed(),"sidebar state: nothing stored means expanded");
        SidebarState(true).save();
        check(SidebarState::load().collapsed() && QSettings().value(key).toBool(),"sidebar state: a collapsed choice is stored and loaded");
        SidebarState(false).save();
        check(!SidebarState::load().collapsed(),"sidebar state: an expanded choice is stored and loaded");
        QSettings().setValue(key,"true");
        check(SidebarState::load().collapsed(),"sidebar state: a stored text \"true\" counts as collapsed");
        QSettings().setValue(key,"banana");
        check(!SidebarState::load().collapsed(),"sidebar state: a garbage value counts as expanded");
        QSettings().remove(key);
    }
    {
        // The sidebar widget, expanded: the left menu as the main window used to build it inline.
        const bool wasDark=Theme::isDark();
        QSettings().remove(SidebarState::kSettingsKey);
        const QList<Sidebar::NavItem> items={{"Library","box",0},{"Training","graduation-cap",1},{"Knowledge","database",2},{"Adapters","layers",3},{"Playground","message-circle",4}};
        Sidebar sidebar(items,{"Settings","settings",5},4);
        QList<int> requested; QObject::connect(&sidebar,&Sidebar::panelRequested,[&](int panel){ requested<<panel; });
        QList<ChatSummary> saved; for (int i=0;i<7;++i) saved.append({QString("00000000-0000-4000-8000-00000000000%1").arg(i),QString("Chat %1").arg(i),false,QDateTime::currentDateTime(),QDateTime::currentDateTime()});
        sidebar.history()->setChats(saved,QString());
        sidebar.resize(195,680); sidebar.show(); pump();
        auto navButtons=[&]{ QMap<int,QPushButton *> out; for (auto *b : sidebar.findChildren<QPushButton *>("navBtn")) out.insert(b->property("panel").toInt(),b); return out; };
        auto nav=navButtons();
        QStringList tips;
        const QStringList names={"Library","Training","Knowledge","Adapters","Playground","Settings"};
        for(int i=0;i<names.size();++i)tips<<names[i]+" · "+QKeySequence(Qt::CTRL | (Qt::Key_1+i)).toString(QKeySequence::NativeText);
        bool tipsOk=nav.size()==6; for (int i=0;i<6&&tipsOk;++i) tipsOk=nav.contains(i)&&nav[i]->toolTip()==tips[i]&&nav[i]->isCheckable();
        check(tipsOk&&sidebar.width()==195,"sidebar: six nav buttons with today's tooltips in a 195px column");
        nav[1]->click();
        check(requested==QList<int>({1}),"sidebar: clicking a button asks for its panel");
        requested.clear(); bool shortcutsOk=true;
        for (int n=1;n<=6;++n) {
            QShortcut *found=nullptr; for (auto *sc : sidebar.findChildren<QShortcut *>()) if (sc->key().toString()==QString("Ctrl+%1").arg(n)) found=sc;
            shortcutsOk&=found!=nullptr; if (found) emit found->activated();
        }
        check(shortcutsOk&&requested==QList<int>({0,1,2,3,4,5}),"sidebar: Ctrl+1..6 ask for their panels");
        sidebar.setCurrentPanel(4);
        bool onlyChat=true; for (auto it=nav.begin();it!=nav.end();++it) onlyChat&=it.value()->isChecked()==(it.key()==4);
        check(onlyChat&&sidebar.history()->isVisibleTo(&sidebar),"sidebar: the open workspace is checked and Playground shows the chat list");
        sidebar.setCurrentPanel(1);
        bool onlyTraining=true; for (auto it=nav.begin();it!=nav.end();++it) onlyTraining&=it.value()->isChecked()==(it.key()==1);
        check(onlyTraining&&!sidebar.history()->isVisibleTo(&sidebar),"sidebar: other workspaces hide the chat list");
        bool iconsOk=true;
        for (bool dark : {true,false}) {
            Theme::setDark(dark); sidebar.refreshIcons();
            iconsOk&=nav[1]->icon().pixmap(18,18).toImage()!=nav[0]->icon().pixmap(18,18).toImage();
        }
        check(iconsOk,"sidebar: the open workspace's icon is tinted differently in both themes");
        sidebar.setCurrentPanel(4); pump();
        bool fits=true; QList<QWidget *> column; for (int i=0;i<5;++i) column<<nav[i]; column<<sidebar.history();
        for (int i=0;i<column.size();++i) {
            fits&=sidebar.rect().contains(column[i]->geometry());
            if (i+1<column.size()) fits&=column[i]->geometry().bottom()<column[i+1]->geometry().top();
        }
        fits&=sidebar.rect().contains(nav[5]->geometry())&&sidebar.history()->geometry().bottom()<nav[5]->geometry().top();
        check(fits,"sidebar: everything fits 195x680 with a populated chat list, nothing overlapping");
        check(sidebar.brandSeal()&&sidebar.brandSeal()->property("brandMark").toBool(),"sidebar: the brand seal is the animated brand mark");
        Theme::setDark(wasDark);
    }
    {
        // The sidebar widget, collapsed into a slim icon strip.
        const bool wasDark=Theme::isDark();
        const QString key=SidebarState::kSettingsKey;
        QSettings().remove(key);
        const QList<Sidebar::NavItem> items={{"Library","box",0},{"Training","graduation-cap",1},{"Knowledge","database",2},{"Adapters","layers",3},{"Playground","message-circle",4}};
        const QStringList labels={"Library","Training","Knowledge","Adapters","Playground","Settings"};
        Sidebar strip(items,{"Settings","settings",5},4);
        QList<ChatSummary> saved; for (int i=0;i<3;++i) saved.append({QString("00000000-0000-4000-8000-00000000000%1").arg(i),QString("Chat %1").arg(i),false,QDateTime::currentDateTime(),QDateTime::currentDateTime()});
        strip.history()->setChats(saved,QString());
        strip.show(); strip.setCurrentPanel(4); pump();
        int collapsedSignals=0; bool lastCollapsed=false;
        QObject::connect(&strip,&Sidebar::collapsedChanged,[&](bool c){ ++collapsedSignals; lastCollapsed=c; });
        auto nav=[&]{ QMap<int,QPushButton *> out; for (auto *b : strip.findChildren<QPushButton *>("navBtn")) out.insert(b->property("panel").toInt(),b); return out; };
        auto *toggle=strip.findChild<QPushButton *>("sidebarToggle");
        check(toggle&&!strip.isCollapsed()&&strip.width()==195&&toggle->toolTip()=="Collapse sidebar \u00b7 Ctrl+B"&&toggle->accessibleName()=="Collapse sidebar","sidebar: the expanded toggle offers to collapse");
        strip.setCollapsed(true); pump();
        auto buttons=nav();
        bool strippedOk=strip.width()==52&&strip.minimumWidth()==52&&strip.maximumWidth()==52&&buttons.size()==6;
        for (int i=0;i<6&&strippedOk;++i) {
            const QString shortcut=QKeySequence(Qt::CTRL | (Qt::Key_1+i)).toString(QKeySequence::NativeText);
            strippedOk=buttons[i]->text().isEmpty()&&buttons[i]->accessibleName()==labels[i]&&
                       buttons[i]->toolTip()==labels[i]+" \u00b7 "+shortcut;
        }
        check(strippedOk,"sidebar: collapsing makes a 52px icon strip; the labels stay as tooltips and accessible names");
        bool hidden=!strip.findChild<QWidget *>("logoLabel")->isVisibleTo(&strip);
        for (auto *tag : strip.findChildren<QLabel *>("studioTag")) hidden&=!tag->isVisibleTo(&strip);
        check(hidden&&strip.brandSeal()->width()<=32&&strip.brandSeal()->isVisibleTo(&strip),"sidebar: the strip hides the wordmark and tags and shrinks the logo to a small mark");
        strip.setCurrentPanel(4);
        check(!strip.history()->isVisibleTo(&strip),"sidebar: the chat list stays hidden while collapsed even on Playground");
        check(toggle->toolTip()=="Expand sidebar \u00b7 Ctrl+B"&&toggle->accessibleName()=="Expand sidebar"&&collapsedSignals==1&&lastCollapsed,"sidebar: the collapsed toggle offers to expand and the change is announced once");
        strip.setCollapsed(false); pump();
        buttons=nav(); bool backOk=strip.width()==195&&strip.history()->isVisibleTo(&strip)&&strip.findChild<QWidget *>("logoLabel")->isVisibleTo(&strip)&&strip.brandSeal()->width()==35;
        for (int i=0;i<6&&backOk;++i) backOk=buttons[i]->text()==labels[i];
        check(backOk,"sidebar: expanding brings back the text, the logo, the wordmark and the chat list");
        strip.setCollapsed(true); pump();
        for (bool dark : {true,false}) {
            Theme::setDark(dark); strip.refreshIcons(); pump();
            buttons=nav(); strip.setCurrentPanel(4);
            strip.resize(52,680); pump();
            QList<QWidget *> column={strip.brandSeal(),toggle}; for (int i=0;i<5;++i) column<<buttons[i];
            bool fits=true; for (int i=0;i<column.size();++i) {
                fits&=strip.rect().contains(column[i]->geometry());
                if (i+1<column.size()) fits&=column[i]->geometry().bottom()<column[i+1]->geometry().top();
            }
            fits&=strip.rect().contains(buttons[5]->geometry())&&buttons[4]->geometry().bottom()<buttons[5]->geometry().top();
            check(fits,QString("sidebar: the strip fits 52x680 in %1 mode").arg(dark?"dark":"light"));
            const QImage shot=strip.grab().toImage();
            auto hasAccent=[&](QPushButton *b) {
                const QColor accent(Theme::accentPrimary()); const qreal dpr=shot.devicePixelRatio(); const QRect r=b->geometry();
                for (int x=qRound(r.left()*dpr);x<qRound((r.left()+5)*dpr);++x) for (int y=qRound(r.top()*dpr);y<=qRound(r.bottom()*dpr);++y) {
                    const QColor c=shot.pixelColor(x,y);
                    if (qAbs(c.red()-accent.red())<=12&&qAbs(c.green()-accent.green())<=12&&qAbs(c.blue()-accent.blue())<=12) return true;
                }
                return false;
            };
            check(!hasAccent(buttons[4])&&!hasAccent(buttons[0]),QString("sidebar: active workspace uses its icon without a highlighting border (%1)").arg(dark?"dark":"light"));
            {
                // Icon centring: the ink of an idle button's icon sits in the middle of the button.
                const qreal dpr=shot.devicePixelRatio(); const QRect r=buttons[0]->geometry();
                const QColor back=shot.pixelColor(qRound((r.left()+2)*dpr),qRound((r.top()-4)*dpr));
                int minX=100000,maxX=-1;
                for (int x=qRound((r.left()+4)*dpr);x<qRound((r.right()-2)*dpr);++x) for (int y=qRound((r.top()+4)*dpr);y<qRound((r.bottom()-4)*dpr);++y) {
                    const QColor c=shot.pixelColor(x,y);
                    if (qAbs(c.red()-back.red())+qAbs(c.green()-back.green())+qAbs(c.blue()-back.blue())>90) { minX=qMin(minX,x); maxX=qMax(maxX,x); }
                }
                const qreal inkCentre=(minX+maxX)/2.0/dpr, buttonCentre=r.left()+r.width()/2.0;
                check(maxX>minX&&qAbs(inkCentre-buttonCentre)<=2.5,QString("sidebar: the strip's icons sit in the middle of their buttons (%1)").arg(dark?"dark":"light"));
            }
            check(buttons[4]->icon().pixmap(18,18).toImage()!=buttons[0]->icon().pixmap(18,18).toImage(),QString("sidebar: the strip's icons are re-tinted after a theme switch (%1)").arg(dark?"dark":"light"));
        }
        Theme::setDark(wasDark);
        strip.setCollapsed(false); collapsedSignals=0; QSettings().remove(key);
        const QImage openChevron=toggle->icon().pixmap(14,14).toImage();
        toggle->click(); pump();
        check(strip.isCollapsed()&&collapsedSignals==1&&QSettings().value(key).toBool()&&strip.width()==52,"sidebar: clicking the toggle collapses, announces it and stores the choice");
        check(toggle->icon().pixmap(14,14).toImage()!=openChevron,"sidebar: the toggle's arrow flips with the state");
        toggle->click(); pump();
        check(!strip.isCollapsed()&&!QSettings().value(key).toBool()&&strip.width()==195,"sidebar: clicking again expands and stores the choice");
        QShortcut *ctrlB=nullptr; for (auto *sc : strip.findChildren<QShortcut *>()) if (sc->key().toString()=="Ctrl+B") ctrlB=sc;
        bool ctrlBOk=ctrlB&&ctrlB->context()==Qt::WindowShortcut;
        if (ctrlB) { emit ctrlB->activated(); pump(); ctrlBOk&=strip.isCollapsed(); emit ctrlB->activated(); pump(); ctrlBOk&=!strip.isCollapsed(); }
        check(ctrlBOk,"sidebar: Ctrl+B toggles the sidebar from anywhere in the window");
        QPlainTextEdit box; QKeyEvent override(QEvent::ShortcutOverride,Qt::Key_B,Qt::ControlModifier); override.ignore();
        QApplication::sendEvent(&box,&override);
        check(!override.isAccepted(),"sidebar: the chat box does not swallow Ctrl+B");
        QSettings().setValue(key,true);
        Sidebar saved1(items,{"Settings","settings",5},4); saved1.show();
        bool startOk=saved1.width()==52&&saved1.isCollapsed();
        for (auto *b : saved1.findChildren<QPushButton *>("navBtn")) startOk&=b->text().isEmpty();
        check(startOk,"sidebar: a stored collapsed choice shows at once, before anything is processed");
        QSettings().setValue(key,"banana");
        Sidebar garbage(items,{"Settings","settings",5},4); garbage.show();
        check(!garbage.isCollapsed()&&garbage.width()==195,"sidebar: a garbage stored value starts expanded");
        QSettings().remove(key);
    }
    {
        // The + under Playground, the busy state, focus and the slide.
        const QString key=SidebarState::kSettingsKey, reducedKey="appearance/studio3ReducedMotion";
        QSettings().remove(key); QSettings().remove(reducedKey);
        const QList<Sidebar::NavItem> items={{"Library","box",0},{"Training","graduation-cap",1},{"Knowledge","database",2},{"Adapters","layers",3},{"Playground","message-circle",4}};
        Sidebar bar(items,{"Settings","settings",5},4);
        bar.setAnimationsEnabled(false);
        QList<ChatSummary> saved; for (int i=0;i<3;++i) saved.append({QString("00000000-0000-4000-8000-00000000000%1").arg(i),QString("Chat %1").arg(i),false,QDateTime::currentDateTime(),QDateTime::currentDateTime()});
        bar.history()->setChats(saved,QString());
        bar.show(); pump();
        auto nav=[&]{ QMap<int,QPushButton *> out; for (auto *b : bar.findChildren<QPushButton *>("navBtn")) out.insert(b->property("panel").toInt(),b); return out; };
        auto *plus=bar.findChild<QPushButton *>("sidebarNewChat");
        bool matrix=plus!=nullptr;
        for (bool collapsed : {false,true}) for (int panel : {1,4}) {
            bar.setCollapsed(collapsed); bar.setCurrentPanel(panel);
            matrix&=plus&&plus->isVisibleTo(&bar)==(collapsed&&panel==4);
        }
        check(matrix,"sidebar: the + shows only while collapsed and Playground is open");
        bar.setCollapsed(true); bar.setCurrentPanel(4); pump();
        auto buttons=nav();
        const QRect pr=plus->geometry(), play=buttons[4]->geometry(), settings=buttons[5]->geometry();
        check(plus->toolTip()=="New chat"&&plus->accessibleName()=="New chat"&&pr.top()>play.bottom()&&pr.bottom()<settings.top()&&qAbs(pr.center().x()-bar.width()/2)<=2,"sidebar: the + sits just under Playground, centred, named New chat");
        int asked=0; QObject::connect(&bar,&Sidebar::newChatRequested,[&]{ ++asked; });
        plus->click();
        check(asked==1,"sidebar: the + asks for a new chat");
        auto *listNew=bar.history()->findChild<QPushButton *>("historyNew");
        bar.setBusy(true);
        check(listNew&&!plus->isEnabled()&&!listNew->isEnabled(),"sidebar: busy turns off the + and the list's New chat row together");
        bar.setBusy(false);
        check(plus->isEnabled()&&listNew->isEnabled(),"sidebar: not busy turns both back on");
        bar.setCollapsed(false); bar.setCurrentPanel(4); pump();
        bar.history()->findChild<QListWidget *>("historyList")->setFocus(); pump();
        check(bar.focusWidget()==bar.history()->findChild<QListWidget *>("historyList"),"sidebar: the chat list can hold focus (setup for the next check)");
        bar.setCollapsed(true); pump();
        check(bar.focusWidget()==nav()[4],"sidebar: collapsing moves focus off the hidden chat list to Playground");
        bar.setCollapsed(false); bar.setCurrentPanel(4); nav()[0]->setFocus(); bar.setCollapsed(true); pump();
        check(bar.focusWidget()==nav()[0],"sidebar: collapsing leaves focus alone when it was not in the chat list");
        {
            // Focus that moved out of the sidebar after the chat list had it must stay where it is (e.g. the chat box).
            QWidget host; auto *row=new QHBoxLayout(&host); row->setContentsMargins(0,0,0,0);
            Sidebar inner(items,{"Settings","settings",5},4); inner.setAnimationsEnabled(false); inner.history()->setChats(saved,QString());
            auto *box=new QLineEdit; row->addWidget(&inner); row->addWidget(box);
            host.resize(600,680); host.show(); inner.setCurrentPanel(4); pump();
            inner.history()->findChild<QListWidget *>("historyList")->setFocus(); box->setFocus(); pump();
            inner.setCollapsed(true); pump();
            check(host.focusWidget()==box,"sidebar: collapsing does not steal focus that already moved to the chat box");
        }
        // The slide.
        bar.setCollapsed(false); pump();
        bar.setAnimationsEnabled(true);
        bar.toggle();
        check(bar.isCollapsed()&&bar.width()==195,"sidebar: a slide starts from the current width (state flips at once)");
        bool inRange=true; for (int i=0;i<40&&bar.width()!=52;++i) { pump(10); inRange&=bar.width()>=52&&bar.width()<=195; }
        check(waitUntil([&]{ return bar.width()==52; },1000)&&inRange&&bar.minimumWidth()==52&&bar.maximumWidth()==52,"sidebar: collapsing slides down to exactly 52px");
        bar.toggle();
        check(!bar.isCollapsed()&&bar.width()==52,"sidebar: expanding starts from the strip width");
        check(waitUntil([&]{ return bar.width()==195&&bar.maximumWidth()==195; },1000),"sidebar: expanding slides up to exactly 195px");
        bool textBack=true; for (auto *b : bar.findChildren<QPushButton *>("navBtn")) textBack&=!b->text().isEmpty();
        check(textBack&&bar.findChild<QWidget *>("logoLabel")->isVisibleTo(&bar),"sidebar: the text and wordmark are back once the slide ends");
        bar.toggle(); bar.toggle(); pump(300);
        bool looksExpanded=bar.width()==195&&!bar.isCollapsed()&&!QSettings().value(key).toBool()&&bar.property("collapsed")==false&&bar.brandSeal()->width()==35&&bar.findChild<QWidget *>("logoLabel")->isVisibleTo(&bar);
        for (auto *b : bar.findChildren<QPushButton *>("navBtn")) looksExpanded&=!b->text().isEmpty();
        check(looksExpanded,"sidebar: two quick toggles end expanded, 195px, drawn expanded (text, wordmark, full logo), stored as expanded");
        bar.toggle(); bar.toggle(); bar.toggle();
        check(waitUntil([&]{ return bar.width()==52; },1000)&&bar.isCollapsed()&&QSettings().value(key).toBool(),"sidebar: three quick toggles end collapsed, 52px, stored as collapsed");
        QSettings().setValue(reducedKey,true);
        bar.toggle();
        check(!bar.isCollapsed()&&bar.width()==195,"sidebar: with reduced motion the change is instant");
        QSettings().remove(reducedKey); QSettings().remove(key);
    }
    {
        // The first-launch question.
        NameDialog asked("Archit");
        auto *edit=asked.findChild<QLineEdit *>("nameInput"); auto *save=button(&asked,"Save"); auto *skip=button(&asked,"Skip");
        bool heading=false; for (auto *label : asked.findChildren<QLabel *>()) heading|=label->text()=="What should I call you?";
        check(edit && save && skip && heading && edit->text()=="Archit" && edit->accessibleName()=="Your name","name dialog: asks 'What should I call you?' with the suggestion filled in, plus Save and Skip");
        asked.show(); pump(40);
        if (edit && save && skip) {
            edit->setText("  Ada  "); save->click(); pump(20);
            check(asked.result()==QDialog::Accepted && asked.name()=="Ada","name dialog: Save accepts the trimmed name");
            NameDialog skipped("Archit"); skipped.show(); button(&skipped,"Skip")->click(); pump(20);
            check(skipped.result()==QDialog::Rejected,"name dialog: Skip rejects");
            NameDialog escaped("Archit"); escaped.show(); key(&escaped,Qt::Key_Escape); pump(20);
            check(escaped.result()==QDialog::Rejected,"name dialog: Esc counts as Skip");
            NameDialog longName(QString(60,'z')); check(longName.name().size()==40 && NameDialog("").name().isEmpty(),"name dialog: a long suggestion is capped and an empty one stays empty");
            NameDialog blank(""); blank.show(); button(&blank,"Save")->click(); pump(20);
            check(blank.name().isEmpty(),"name dialog: saving an empty name gives no name");
        }
    }
    {
        // The saved-chats list under Playground.
        ChatHistoryList list; list.resize(220,420);
        auto *newButton=list.findChild<QPushButton *>("historyNew"); auto *view=list.findChild<QListWidget *>("historyList");
        check(newButton && view && view->accessibleName()=="Saved chats" && newButton->accessibleName()=="New chat" && !newButton->icon().isNull(),"history list: has a New chat button and a Saved chats list");
        if (newButton && view) {
            auto makeChats=[](int n) { QList<ChatSummary> out; for (int i=0;i<n;++i) { ChatSummary c; c.id=QString("id%1").arg(i); c.title=QString("Chat number %1").arg(i); out << c; } return out; };
            QStringList opened, renamed, deleted; int newRequests=0;
            QObject::connect(&list,&ChatHistoryList::openRequested,[&opened](const QString &id) { opened << id; });
            QObject::connect(&list,&ChatHistoryList::renameRequested,[&renamed](const QString &id) { renamed << id; });
            QObject::connect(&list,&ChatHistoryList::deleteRequested,[&deleted](const QString &id) { deleted << id; });
            QObject::connect(&list,&ChatHistoryList::newChatRequested,[&newRequests]() { ++newRequests; });
            auto clickRow=[&](int row) {
                const QPoint pos=view->visualItemRect(view->item(row)).center(); const QPoint global=view->viewport()->mapToGlobal(pos);
                QMouseEvent press(QEvent::MouseButtonPress,QPointF(pos),QPointF(global),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease,QPointF(pos),QPointF(global),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(view->viewport(),&press); QApplication::sendEvent(view->viewport(),&release); pump(20);
            };
            auto currentId=[&]() { return view->currentItem()?view->currentItem()->data(Qt::UserRole).toString():QString(); };
            list.setChats({},QString()); list.show(); pump(40);
            check(view->count()==0 && !view->isVisibleTo(&list) && newButton->isVisibleTo(&list),"history list: with no chats only the New chat button shows");
            list.setChats(makeChats(12),"id2"); pump(60);
            check(view->count()==12 && view->item(0)->text()=="Chat number 0" && view->item(11)->data(Qt::UserRole).toString()=="id11" && currentId()=="id2","history list: rows appear in the order given and the open chat is the current one");
            check(view->height()==ChatHistoryList::kVisibleRows*ChatHistoryList::kRowHeight && view->verticalScrollBar()->maximum()>0,"history list: with many chats it shows five rows and scrolls");
            {
                int fullyVisible=0; for (int i=0;i<view->count();++i) if (view->viewport()->rect().contains(view->visualItemRect(view->item(i)))) ++fullyVisible;
                check(fullyVisible==5,"history list: exactly five rows are visible at once");
            }
            view->verticalScrollBar()->setValue(60); list.setChats(makeChats(12),"id2"); pump(30);
            check(view->verticalScrollBar()->value()==60,"history list: refreshing the same chats keeps the scroll position");
            list.setChats(makeChats(3),"id1"); pump(60);
            check(view->height()==3*ChatHistoryList::kRowHeight && view->verticalScrollBar()->maximum()==0 && currentId()=="id1","history list: with three chats the list is three rows tall and does not scroll");
            list.setChats(makeChats(12),"id2"); pump(60);

            clickRow(3);
            check(opened==QStringList({"id3"}),"history list: clicking a row asks to open that chat, once");
            newButton->click(); check(newRequests==1,"history list: New chat asks for a new chat");
            view->setCurrentRow(5); key(view,Qt::Key_Return); key(view,Qt::Key_Delete);
            check(opened==QStringList({"id3","id5"}) && deleted==QStringList({"id5"}),"history list: Enter opens the current row and Delete asks to delete it");
            {
                QMenu *menu=list.contextMenuFor("id4"); QStringList names; for (auto *a : menu->actions()) names << a->text();
                check(names==QStringList({"Rename","Delete"}),"history list: the row menu offers Rename and Delete");
                menu->actions().at(0)->trigger(); menu->actions().at(1)->trigger();
                check(renamed==QStringList({"id4"}) && deleted==QStringList({"id5","id4"}),"history list: the menu actions ask to rename and delete that chat");
                delete menu;
            }

            opened.clear(); deleted.clear(); renamed.clear(); list.setBusy(true);
            clickRow(3); view->setCurrentRow(5); key(view,Qt::Key_Return); view->setCurrentRow(2); key(view,Qt::Key_Delete);
            check(!newButton->isEnabled() && opened.isEmpty() && deleted.isEmpty(),"history list: while busy nothing can be opened or deleted from the keyboard, and New chat is off");
            list.setChats(makeChats(12),"id2"); clickRow(4);
            check(opened.isEmpty() && currentId()=="id2","history list: a refused click does not move the highlight off the open chat");
            {
                QMenu *openMenu=list.contextMenuFor("id2"); QMenu *otherMenu=list.contextMenuFor("id4");
                check(!openMenu->actions().at(0)->isEnabled() && !openMenu->actions().at(1)->isEnabled() && otherMenu->actions().at(0)->isEnabled() && otherMenu->actions().at(1)->isEnabled(),"history list: while busy the open chat's menu is off, other chats' menus still work");
                delete openMenu; delete otherMenu;
            }
            view->setCurrentRow(7); key(view,Qt::Key_Delete);
            check(deleted==QStringList({"id7"}),"history list: another chat can still be deleted while busy");
            list.setBusy(false); clickRow(6);
            check(newButton->isEnabled() && opened==QStringList({"id6"}),"history list: busy off restores opening and New chat");

            {
                QList<ChatSummary> long1=makeChats(2); long1[0].title=QString(200,'w'); list.setChats(long1,"id1"); pump(40);
                check(view->item(0)->toolTip()==QString(200,'w') && view->textElideMode()==Qt::ElideRight && view->horizontalScrollBarPolicy()==Qt::ScrollBarAlwaysOff,"history list: a very long title is elided on the row and complete in its tooltip");
            }
            for (bool dark : {true,false}) { Theme::setDark(dark); list.setChats(makeChats(9),"id3"); list.refreshIcons(); pump(80); capture(&list,dark?"history-list-dark":"history-list-light"); }
            Theme::setDark(true); list.hide();
        }
    }
    {
        // The controller joins store, panel and list; dialogs are replaceable so no window pops up.
        QTemporaryDir dir; ChatStore store(dir.path()+"/chats");
        auto sample=[](const QString &question) { ChatTree t; const auto q=t.addChild(ChatTree::kRoot,"user",question); t.addChild(q,"assistant","answer to "+question); return t; };
        QStringList savedIds; for (const QString question : {"alpha chat","beta chat","gamma chat"}) { const QString id=store.create(); store.save(id,sample(question)); savedIds << id; }
        ChatPanel hostedChat; hostedChat.setBackendClient(&client); hostedChat.setChatStore(&store);
        ChatHistoryList historyList; QWidget dialogParent;
        ChatHistoryController controller(&store,&hostedChat,&historyList,&dialogParent);
        auto *view=historyList.findChild<QListWidget *>("historyList"); auto *newButton=historyList.findChild<QPushButton *>("historyNew");
        historyList.resize(220,400); historyList.show(); pump(40);
        auto titles=[&]() { QStringList out; for (int i=0;i<view->count();++i) out << view->item(i)->text(); return out; };
        auto titleOf=[&](const QString &id) { for (const auto &row : store.list()) if (row.id==id) return row.title; return QString("<missing>"); };
        check(view && newButton && view->count()==3 && titles()==QStringList({"gamma chat","beta chat","alpha chat"}) && view->selectedItems().isEmpty(),"history controller: the list shows exactly what the store holds, newest first, with nothing open");
        const QString idNew=store.create(); store.save(idNew,sample("delta chat")); pump(20);
        check(view->count()==4 && view->item(0)->text()=="delta chat","history controller: a chat saved later appears in the list");
        emit historyList.openRequested(savedIds.at(0)); pump(40);
        check(hostedChat.currentChatId()==savedIds.at(0) && view->currentItem() && view->currentItem()->data(Qt::UserRole).toString()==savedIds.at(0) && hostedChat.findChildren<MessageRow *>().size()==2,"history controller: opening from the list opens the chat and highlights its row");
        emit historyList.newChatRequested(); pump(40);
        check(hostedChat.currentChatId().isEmpty() && view->selectedItems().isEmpty() && view->count()==4 && hostedChat.findChildren<MessageRow *>().isEmpty(),"history controller: New chat shows a blank chat and keeps every saved chat listed");
        QString askedWith; QString answer="Renamed by check";
        controller.askTitle=[&](const QString &current) { askedWith=current; return answer; };
        emit historyList.renameRequested(savedIds.at(1)); pump(40);
        check(askedWith=="beta chat" && titleOf(savedIds.at(1))=="Renamed by check" && titles().contains("Renamed by check"),"history controller: rename asks with the current title and applies the answer");
        answer="  "; emit historyList.renameRequested(savedIds.at(1)); pump(40);
        check(titleOf(savedIds.at(1))=="Renamed by check","history controller: an empty answer changes nothing");
        QString confirmedFor; bool agree=false;
        controller.confirmDelete=[&](const QString &title) { confirmedFor=title; return agree; };
        emit historyList.deleteRequested(savedIds.at(2)); pump(40);
        check(confirmedFor=="gamma chat" && titleOf(savedIds.at(2))=="gamma chat" && view->count()==4,"history controller: a refused delete changes nothing");
        agree=true; emit historyList.deleteRequested(savedIds.at(2)); pump(40);
        check(titleOf(savedIds.at(2))=="<missing>" && view->count()==3,"history controller: a confirmed delete removes the chat and its row");
        emit historyList.openRequested(savedIds.at(0)); pump(40); emit historyList.deleteRequested(savedIds.at(0)); pump(40);
        check(hostedChat.currentChatId().isEmpty() && titleOf(savedIds.at(0))=="<missing>" && view->count()==2 && hostedChat.findChildren<MessageRow *>().isEmpty(),"history controller: deleting the open chat leaves a blank chat");
        // Busy state reaches the list.
        {
            fixture.holdChat=true; fixture.chatReply="held";
            auto *typed=hostedChat.findChild<QPlainTextEdit *>("chatInput"); typed->setPlainText("a held question"); pump(20);
            QMetaObject::invokeMethod(&hostedChat,"onSend",Qt::DirectConnection);
            check(waitUntil([&]{ return fixture.chatSocket && !newButton->isEnabled(); }),"history controller: while a reply streams the list's New chat is off");
            const QString streaming=hostedChat.currentChatId(); emit historyList.openRequested(savedIds.at(1)); pump(40);
            check(hostedChat.currentChatId()==streaming,"history controller: opening another chat while a reply streams is refused");
            QMetaObject::invokeMethod(&hostedChat,"onClearChat",Qt::DirectConnection); pump(60);
            fixture.holdChat=false; if (fixture.chatSocket) { fixture.chatSocket->disconnectFromHost(); fixture.chatSocket.clear(); } pump(100);
            check(newButton->isEnabled() && hostedChat.currentChatId().isEmpty(),"history controller: when the reply ends the list is usable again");
        }
        QMetaObject::invokeMethod(&hostedChat,"onClearChat",Qt::DirectConnection);
        historyList.hide();
        hostedChat.setBackendClient(nullptr); pump(500); client.abortAll(); pump(100);   // no request may outlive the panel it reports to
    }
    {
        QWidget host; auto *hostLayout=new QVBoxLayout(&host);
        auto *switcher=new VersionSwitcher(&host); hostLayout->addWidget(switcher);
        auto *messageRow=new MessageRow(ChatBubble::User,5,"hello",&host); hostLayout->addWidget(messageRow);
        host.resize(400,200); host.show(); pump();
        auto *prev=switcher->findChild<QPushButton *>("versionPrev"); auto *next=switcher->findChild<QPushButton *>("versionNext"); auto *label=switcher->findChild<QLabel *>("versionLabel");
        check(prev && next && label && switcher->objectName()=="versionSwitcher","switcher: has its previous, next and label parts");
        if (prev && next && label) {
            switcher->setVersion(0,1); pump();
            check(switcher->isHidden(),"switcher: hidden for one version");
            switcher->setVersion(1,3); pump();
            check(!switcher->isHidden() && label->text()=="2 / 3" && prev->isEnabled() && next->isEnabled(),"switcher: shows '2 / 3' for index 1 of 3 with both arrows on");
            switcher->setVersion(0,3); check(!prev->isEnabled() && next->isEnabled() && label->text()=="1 / 3","switcher: previous is disabled at the first version");
            switcher->setVersion(2,3); check(prev->isEnabled() && !next->isEnabled() && label->text()=="3 / 3","switcher: next is disabled at the last version");
            QList<int> steps; QObject::connect(switcher,&VersionSwitcher::step,[&steps](int delta) { steps << delta; });
            switcher->setVersion(1,3); prev->click(); next->click();
            check(steps==QList<int>({-1,1}),"switcher: step emits -1 and +1");
            switcher->setEnabledForUse(false);
            check(!prev->isEnabled() && !next->isEnabled(),"switcher: disabled for use disables both arrows");
            switcher->setEnabledForUse(true); check(prev->isEnabled() && next->isEnabled(),"switcher: enabling for use restores the arrows by position");
            check(prev->accessibleName()=="Previous version" && next->accessibleName()=="Next version","switcher: arrows have accessible names");
        }
        auto *replyRow=new MessageRow(ChatBubble::Assistant,6,"hi there",&host); hostLayout->addWidget(replyRow); host.resize(400,300); pump(40);
        {
            auto *pencil=messageRow->findChild<QPushButton *>("editButton"); auto *retry=replyRow->findChild<QPushButton *>("retryButton"); auto *box=messageRow->findChild<QPlainTextEdit *>("messageEditBox");
            check(pencil && retry && box && pencil->accessibleName()=="Edit message" && retry->accessibleName()=="Retry reply" && box->accessibleName()=="Edit your message","row: pencil, retry and editor have accessible names");
            check(pencil && retry && pencil->focusPolicy()!=Qt::NoFocus && retry->focusPolicy()!=Qt::NoFocus,"row: pencil and retry accept keyboard focus");
            check(messageRow->focusPolicy()==Qt::TabFocus && replyRow->focusPolicy()==Qt::NoFocus,"row: a row with an action is a tab stop, one without is not");
            replyRow->setRetryAvailable(true);
            check(replyRow->focusPolicy()==Qt::TabFocus,"row: a reply that can be retried becomes a tab stop");
            auto *outside=new QPushButton("Outside message rows",&host); hostLayout->addWidget(outside);
            host.activateWindow(); pump(40);
            // Move Qt's real focus, rather than sending synthetic focus events
            // that leave QApplication::focusWidget() pointing at the old row.
            QEvent leave(QEvent::Leave); QApplication::sendEvent(messageRow,&leave); QApplication::sendEvent(replyRow,&leave);
            messageRow->setFocus(Qt::TabFocusReason); pump(20);
            const bool editRevealed=messageRow->hasFocus() && pencil->isVisibleTo(messageRow);
            pencil->setFocus(Qt::TabFocusReason); pump(20);
            check(pencil->hasFocus() && pencil->isVisibleTo(messageRow),"row: moving focus to its action keeps the pencil visible");
            replyRow->setFocus(Qt::TabFocusReason); pump(20);
            check(editRevealed && replyRow->hasFocus() && retry->isVisibleTo(replyRow),"row: tabbing to a row reveals its pencil or retry button");
            outside->setFocus(Qt::TabFocusReason); pump(40);
            check(outside->hasFocus() && !pencil->isVisibleTo(messageRow) && !retry->isVisibleTo(replyRow),"row: leaving a row with the keyboard hides them again");
            replyRow->setRetryAvailable(false);
        }
        auto *rowSwitcher=messageRow->findChild<VersionSwitcher *>("versionSwitcher");
        check(rowSwitcher && rowSwitcher->isHidden(),"row: no switcher is shown while a message has one version");
        if (rowSwitcher) {
            messageRow->setVersion(1,2); pump();
            auto *rowLabel=rowSwitcher->findChild<QLabel *>("versionLabel");
            check(!rowSwitcher->isHidden() && rowLabel && rowLabel->text()=="2 / 2" && rowSwitcher->y()>=messageRow->bubble()->geometry().bottom(),"row: shows '2 / 2' below its bubble");
            QList<QPair<int,int>> stepped; QObject::connect(messageRow,&MessageRow::versionStep,[&stepped](ChatTree::NodeId node,int delta) { stepped << qMakePair(int(node),delta); });
            rowSwitcher->findChild<QPushButton *>("versionPrev")->click();
            check(stepped.size()==1 && stepped.first().first==5 && stepped.first().second==-1,"row: a version step reports the row's node and direction");
            messageRow->setActionsEnabled(false);
            check(!rowSwitcher->findChild<QPushButton *>("versionPrev")->isEnabled() && !rowSwitcher->findChild<QPushButton *>("versionNext")->isEnabled(),"row: disabling actions disables the arrows");
        }
    }
    {
        using namespace ContextMeterLogic;
        check(levelFor(0.69)==Level::Ok && levelFor(0.70)==Level::Warn && levelFor(0.90)==Level::Warn &&
              levelFor(0.91)==Level::Critical && levelFor(1.5)==Level::Critical,"meter levels switch at 70% and above 90%");
        check(!shouldCompact(0.84,85) && shouldCompact(0.85,85) && !shouldCompact(0.99,0) &&
              !shouldCompact(0.99,86) && shouldCompact(0.70,70),"auto-compact fires at or above the chosen percentage only");
        check(parseAutoCompact(QVariant())==85 && parseAutoCompact(QVariant("abc"))==85 && parseAutoCompact(60)==85 &&
              parseAutoCompact(95)==85 && parseAutoCompact(0)==0 && parseAutoCompact(75)==75 && parseAutoCompact(QVariant("90"))==90,
              "stored auto-compact values outside Off/70-90 step 5 fall back to 85");
        check(parseStyle("bar")==Style::Bar && parseStyle("")==Style::Ring && parseStyle("garbage")==Style::Ring &&
              parseStyle(styleKey(Style::RingBanner))==Style::RingBanner && parseStyle("off")==Style::Off,"meter style keys round-trip; unknown keys mean Ring");
        check(usageText(1390,256,4096,true)=="1,390 / 4,096 tokens" && usageText(1390,256,4096,false)=="~1,390 / 4,096 tokens","usage text groups digits and marks estimates with ~");

        ContextMeter meter; QWidget host; auto *hostLayout=new QVBoxLayout(&host);
        hostLayout->addWidget(meter.ring()); hostLayout->addWidget(meter.bar()); hostLayout->addWidget(meter.banner());
        host.show(); pump();
        check(meter.ring()->objectName()=="contextRing" && meter.bar()->objectName()=="contextBar" && meter.banner()->objectName()=="contextBanner","meter widgets have stable object names");
        check(!meter.ring()->isVisible() && !meter.bar()->isVisible() && !meter.banner()->isVisible(),"meter shows nothing before the first usage");
        meter.setUsage(1400,256,4096,true);
        check(meter.level()==Level::Ok && meter.warningText().isEmpty(),"under 70% is quiet");
        meter.setUsage(3000,256,4096,true);
        check(meter.level()==Level::Warn && !meter.warningText().isEmpty(),"70-90% warns");
        meter.setUsage(3900,256,4096,true);
        check(meter.level()==Level::Critical,"over 90% is critical");
        meter.setAutoCompact(85); check(meter.warningText().contains("compact",Qt::CaseInsensitive),"critical warning mentions auto-compact when on");
        meter.setAutoCompact(0); check(!meter.warningText().contains("will compact",Qt::CaseInsensitive) && meter.warningText().contains("fail",Qt::CaseInsensitive),"critical warning says the next message may fail when auto-compact is off");
        meter.setUsage(0,5000,2048,true);
        check(meter.warningText()=="Reduce Response tokens","reserve larger than the window asks to reduce Response tokens");
        meter.setStyle(Style::Ring); meter.setUsage(1400,256,4096,true); pump();
        check(meter.ring()->isVisible() && !meter.bar()->isVisible() && !meter.banner()->isVisible(),"Ring style shows only the ring");
        check(meter.ring()->toolTip().contains("1,400 of 4,096") && meter.ring()->toolTip().contains("256 reserved"),"ring tooltip gives the exact numbers");
        meter.setStyle(Style::Bar); pump();
        check(!meter.ring()->isVisible() && meter.bar()->isVisible(),"Bar style shows only the bar");
        auto *barLabel=meter.bar()->findChild<QLabel *>("contextBarText");
        check(barLabel && barLabel->text().contains("1,400 / 4,096 tokens") && barLabel->text().contains("256 reserved"),"bar shows the numbers without hovering");
        meter.setStyle(Style::RingBanner); meter.setUsage(3900,256,4096,true); pump();
        check(meter.ring()->isVisible() && meter.banner()->isVisible(),"Ring + banner shows the banner when critical");
        meter.setUsage(1400,256,4096,true); pump();
        check(meter.ring()->isVisible() && !meter.banner()->isVisible(),"Ring + banner hides the banner below critical");
        meter.setStyle(Style::Ring); meter.setUsage(3900,256,4096,true); pump();
        check(!meter.banner()->isVisible(),"Ring style never shows the banner");
        meter.setStyle(Style::Off); pump();
        check(!meter.ring()->isVisible() && !meter.bar()->isVisible() && !meter.banner()->isVisible(),"Off hides every meter widget");
        meter.setStyle(Style::Ring); meter.clearUsage(); pump();
        check(!meter.ring()->isVisible() && meter.warningText().isEmpty() && !meter.hasUsage(),"clearing usage hides the meter and its warning");
        bool asked=false; QObject::connect(&meter,&ContextMeter::newChatRequested,[&asked](){ asked=true; });
        meter.setStyle(Style::RingBanner); meter.setUsage(3900,256,4096,true); pump();
        if (auto *button=meter.banner()->findChild<QPushButton *>("contextBannerNewChat")) button->click();
        check(asked,"banner offers Start new chat");
        host.hide();
    }
    {
        QSettings().remove("chat/meterStyle"); QSettings().remove("chat/autoCompactPercent");
        SettingsPanel chatSettings;
        auto *styleCombo=chatSettings.findChild<QComboBox *>("meterStyleCombo");
        auto *compactCombo=chatSettings.findChild<QComboBox *>("autoCompactCombo");
        check(styleCombo && compactCombo,"Settings has a Chat card with meter style and auto-compact");
        if (styleCombo && compactCombo) {
            check(styleCombo->currentData().toString()=="ring" && compactCombo->currentText()=="85%","chat settings default to Ring and 85%");
            check(styleCombo->count()==4 && compactCombo->count()==6 && compactCombo->itemText(0)=="Off","meter styles and auto-compact choices are offered");
            int changes=0; QObject::connect(&chatSettings,&SettingsPanel::settingsChanged,[&changes](){ ++changes; });
            styleCombo->setCurrentIndex(styleCombo->findData("bar"));
            check(QSettings().value("chat/meterStyle").toString()=="bar" && changes==1,"changing the meter style saves immediately and notifies once");
            compactCombo->setCurrentIndex(compactCombo->findData(0));
            check(QSettings().value("chat/autoCompactPercent").toInt()==0 && changes==2,"turning auto-compact Off saves 0 and notifies once");
            compactCombo->setCurrentIndex(compactCombo->findData(70));
            check(QSettings().value("chat/autoCompactPercent").toInt()==70,"auto-compact accepts 70%");
            QSettings().setValue("chat/autoCompactPercent",61); QSettings().setValue("chat/meterStyle","nonsense");
            SettingsPanel reopened;
            auto *reopenedStyle=reopened.findChild<QComboBox *>("meterStyleCombo"); auto *reopenedCompact=reopened.findChild<QComboBox *>("autoCompactCombo");
            check(reopenedStyle && reopenedCompact && reopenedStyle->currentData().toString()=="ring" && reopenedCompact->currentText()=="85%","invalid stored chat settings load as the defaults");
        }
        QSettings().remove("chat/meterStyle"); QSettings().remove("chat/autoCompactPercent");
    }
    ClickFeedback feedback(qApp);
    QPushButton click("Run"); click.resize(120,44); click.show(); click.activateWindow(); pump();
    int clicks=0; QObject::connect(&click,&QPushButton::clicked,[&clicks](){ ++clicks; });
    QSettings().setValue("appearance/studio3ReducedMotion",false); key(&click,Qt::Key_Space);
    check(clicks==1 && !click.findChildren<QWidget *>().isEmpty(),"keyboard click preserves activation and adds feedback");
    pump(400); check(click.findChildren<QWidget *>().isEmpty(),"click feedback releases its overlay");
    QSettings().setValue("appearance/studio3ReducedMotion",true); key(&click,Qt::Key_Space);
    check(clicks==2 && click.findChildren<QWidget *>().isEmpty(),"reduced motion preserves activation without ripple");
    click.hide();

    QTemporaryDir temporary;
    const QString path=temporary.filePath("export.zip");
    QFile file(path); check(file.open(QIODevice::WriteOnly),"prepare export fixture"); file.write("original"); file.close();
    bool done=false; QString error;
    client.download("/export-fail",path,[&](const QString &e){error=e;done=true;});
    for(int i=0;i<40 && !done;++i) pump(25);
    check(file.open(QIODevice::ReadOnly),"read export destination"); const auto original=file.readAll(); file.close();
    check(done && !error.isEmpty() && original=="original","failed export preserves destination file");
    done=false; client.download("/export-ok",path,[&](const QString &e){error=e;done=true;});
    for(int i=0;i<40 && !done;++i) pump(25);
    check(file.open(QIODevice::ReadOnly),"read export destination"); const auto archive=file.readAll(); file.close();
    check(done && error.isEmpty() && archive=="test-archive-payload","successful export writes complete archive");
    // Deliver pending callbacks while their panels still exist.
    client.abortAll(); pump();
    fprintf(stderr,"[ui-check] %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}

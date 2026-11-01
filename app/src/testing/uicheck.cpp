#include "testing/uicheck.h"
#include "backend_client.h"
#include "theme.h"
#include "panels/modelspanel.h"
#include "panels/modelcard.h"
#include "panels/trainingpanel.h"
#include "panels/ragpanel.h"
#include "panels/ragdoccard.h"
#include "panels/adapterspanel.h"
#include "panels/adaptercard.h"
#include "panels/chatpanel.h"
#include "panels/chatbubble.h"
#include "panels/settingspanel.h"
#include "widgets/studiocanvas.h"
#include "widgets/brandmark.h"
#include "widgets/clickfeedback.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSvgGenerator>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QSet>
#include <cstdio>
#include <functional>
#include <memory>

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
    QString trainingState = "running";
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
                    if (method == "DELETE" && path.startsWith("/models/")) {
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
                    else if (path == "/datasets") body = R"([{"id":"fixture-data","name":"Example conversations.jsonl"}])";
                    else if (path == "/training/jobs") body = "[]";
                    else if (path == "/training/start") { ++starts; status=failTraining?400:202; body=failTraining?R"({"detail":"Fixture training failure"})":R"({"id":"fixture-job"})"; }
                    else if (path.startsWith("/training/cancel/")) { trainingState="cancelled"; }
                    else if (path.startsWith("/training/status/")) body=QJsonDocument(QJsonObject{{"status",trainingState},{"current_step",42},{"total_steps",100},{"progress",42},{"loss",1.28},{"message","Training fixture · learning from example conversations"}}).toJson();
                    else if (path == "/inference/status" || path == "/inference/load") body=R"({"engine":{"name":"torch","description":"Local · torch"},"loaded_model":"studio/sample-model","active_adapter_id":"fixture-adapter","active_adapter":"Support assistant"})";
                    else if (path == "/inference/chat") {
                        type="text/event-stream";
                        body="data: {\"delta\":\"A local model can use your documents to give more useful answers.\\n\\nTry a specific question about your project.\"}\n\n";
                        if (holdChat) {
                            chatSocket=socket;
                            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"+body);
                            return;
                        }
                        body+="data: {\"done\":true}\n\n";
                    }
                    else if (path == "/adapters") body=R"([{"id":"fixture-adapter","name":"Support assistant","base_model":"studio/sample-model","rank":8,"method":"lora","size_mb":8.5,"trained_at":"2026-09-30T10:00:00Z"},{"id":"fixture-adapter-2","name":"A long adapter name that should remain readable in a compact window","base_model":"studio/another-model","rank":16,"method":"lora","size_mb":12.3}])";
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
    Fixture fixture;
    if (!fixture.listen(QHostAddress::LocalHost,0)) { check(false,"loopback fixture server: "+fixture.errorString()); return 1; }
    BackendClient client;
    client.setBaseUrl(QString("http://127.0.0.1:%1").arg(fixture.serverPort()));
    ModelsPanel models; TrainingPanel training; RagPanel knowledge;
    AdaptersPanel adapters; ChatPanel chat; SettingsPanel settings;
    models.setBackendClient(&client); training.setBackendClient(&client);
    knowledge.setBackendClient(&client); adapters.setBackendClient(&client); chat.setBackendClient(&client);
    pump(200);
    const QList<QPair<QString,QWidget *>> panels={{"library",&models},{"training",&training},{"knowledge",&knowledge},{"adapters",&adapters},{"chat",&chat},{"settings",&settings}};
    if (!captures.isEmpty()) QDir().mkpath(captures);
    auto capture = [&](QWidget *widget,const QString &name) {
        if (!captures.isEmpty()) check(widget->grab().save(captures+"/"+name+".png"),"capture "+name);
    };
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
                    auto *tabs = settings.findChild<QTabWidget *>();
                    for (int tab = 1; tab < tabs->count(); ++tab) {
                        tabs->setCurrentIndex(tab); pump();
                        capture(panel, QString("%1-%2-settings-tab-%3")
                                .arg(dark?"dark":"light").arg(width).arg(tab));
                    }
                    tabs->setCurrentIndex(0); pump();
                }
                if (width == 848 && (name == "settings" || name == "training")) {
                    auto *combo = panel->findChild<QComboBox *>();
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
            if (act) act->click(); pump();
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
    QMetaObject::invokeMethod(&training,"onStart"); QMetaObject::invokeMethod(&training,"onStart"); pump();
    check(fixture.starts==1 && !start->isEnabled(),"training ignores duplicate starts");
    capture(&training,"training-running");
    QMetaObject::invokeMethod(&training,"onStop"); pump(); QMetaObject::invokeMethod(&training,"onPollStatus"); pump();
    check(start->isEnabled() && completed==0 && training.findChild<QLabel *>("metricValue")->text()=="Training stopped","cancelled training resets controls without reporting completion");
    fixture.failTraining=true; QMetaObject::invokeMethod(&training,"onStart"); pump();
    check(start->isEnabled() && training.findChild<QLabel *>("statusLabel")->property("state")=="error","training submission failure is recoverable");
    fixture.failTraining=false; fixture.trainingState="done"; QMetaObject::invokeMethod(&training,"onStart"); pump();
    check(start->isEnabled() && completed==1,"training completion resets controls");
    if (auto *advanced=button(&training,"Hyperparameters ▾")) advanced->click();
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

    settings.show(); pump();
    bool hosted=false; for(auto *edit:settings.findChildren<QLineEdit *>()) hosted|=edit->echoMode()==QLineEdit::Password;
    check(!hosted && settings.findChild<QComboBox *>()->count()==3,"Settings exposes only supported local engines");
    auto *motion=settings.findChild<QCheckBox *>("reducedMotionToggle"); motion->setChecked(true);
    check(QSettings().value("appearance/studio3ReducedMotion").toBool(),"reduced motion saves immediately");
    settings.findChild<QTabWidget *>()->setCurrentIndex(2); pump(); capture(&settings,"settings-appearance"); settings.hide();
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

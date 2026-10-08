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
#include "widgets/starfield.h"
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
#include <cstdio>
#include <memory>

namespace {
void pump(int ms = 80) {
    QEventLoop loop;
    QTimer::singleShot(ms,&loop,&QEventLoop::quit);
    loop.exec();
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
                    const QString path = QString::fromUtf8(bytes->split(' ').value(1)).section('?',0,0);
                    QByteArray body = "{}", type = "application/json";
                    int status = 200;
                    if (path == "/models") body = R"([{"id":"studio/sample-model","name":"Sample local model","source":"huggingface","architecture":"llama","size_mb":512,"license":"apache-2.0"},{"id":"studio/long-model-name-for-layout-review-in-a-small-window","name":"Layout review","source":"local","architecture":"gpt2","size_mb":120}])";
                    else if (path == "/models/search") body = R"([{"repo_id":"studio/example-model","downloads":1200,"likes":32,"license":"apache-2.0"}])";
                    else if (path == "/models/pull") { status=400; body=R"({"detail":"Fixture download failure"})"; }
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
    QMovie logo(":/brand/animated-symbol.gif");
    check(logo.isValid() && logo.jumpToFrame(0),"bundled logo animation decodes");
    const auto firstLogoFrame = logo.currentImage();
    bool advanced = true;
    for (int i=0;i<25;++i) advanced = logo.jumpToNextFrame() && advanced;
    check(advanced && firstLogoFrame != logo.currentImage(),"supplied logo changes frames");
    check(!firstLogoFrame.isNull() && firstLogoFrame.pixelColor(0,0).alpha()==0,
          "logo background is transparent");
    Starfield wheel; wheel.setProperty("kineticGlyph",true); wheel.setProperty("activityIndicator",true);
    wheel.resize(210,210); wheel.show(); wheel.activateWindow(); pump();
    for(auto activity:{Starfield::Training,Starfield::Chat}) {
        wheel.setActivity(activity); wheel.setMotionEnabled(true);
        auto image=wheel.grab().toImage(); pump(180); check(image!=wheel.grab().toImage(),"activity wheel animates");
        wheel.setMotionEnabled(false); image=wheel.grab().toImage(); pump(180);
        check(image==wheel.grab().toImage(),"reduced motion wheel stays still");
    }
    wheel.setActivity(Starfield::Idle); wheel.setMotionEnabled(true);
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

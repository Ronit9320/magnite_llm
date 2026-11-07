#include "trainingrunpanel.h"
#include "backend_client.h"
#include "widgets/surface.h"
#include "widgets/trainingchart.h"
#include "widgets/toggleswitch.h"
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

TrainingRunPanel::TrainingRunPanel(QWidget *parent) : BasePanel("Training run",parent) {
    setHeaderSubtitle("Review measurements, configuration and the outcome of this run.");
    auto *toolbar=new QHBoxLayout();
    auto *back=new QPushButton("‹ Back",this); back->setObjectName("secondaryBtn");
    connect(back,&QPushButton::clicked,this,&TrainingRunPanel::backRequested);
    toolbar->addWidget(back); toolbar->addStretch();
    m_visualise=new ToggleSwitch("Visualisation",this); m_visualise->setChecked(true);
    toolbar->addWidget(m_visualise); contentLayout()->addLayout(toolbar);
    auto *overview=Surface::card(this,"Run overview"); contentLayout()->addWidget(overview->parentWidget());
    m_summary=new QLabel(this); m_summary->setObjectName("cardTitle"); m_summary->setWordWrap(true);
    m_result=new QLabel(this); m_result->setWordWrap(true); m_result->setObjectName("statusLabel");
    for (auto *label : {m_summary,m_result}) { label->setTextFormat(Qt::PlainText); overview->addWidget(label); }
    m_visuals=new QWidget(this);
    auto *visuals=new QVBoxLayout(m_visuals); visuals->setContentsMargins(0,0,0,0); visuals->setSpacing(12);
    auto *loss=Surface::card(this,"Loss · training / validation");
    m_loss=new TrainingChart("loss",this); m_loss->setMinimumHeight(230); loss->addWidget(m_loss);
    auto *legend=new QLabel("Solid orange: training · Dashed green: held-out validation",this);
    legend->setObjectName("cardMeta"); legend->setWordWrap(true); loss->addWidget(legend);
    visuals->addWidget(loss->parentWidget());
    auto *rate=Surface::card(this,"Learning rate"); m_rate=new TrainingChart("learning_rate",this); rate->addWidget(m_rate);
    auto *grad=Surface::card(this,"Gradient norm"); m_gradient=new TrainingChart("grad_norm",this); grad->addWidget(m_gradient);
    auto *row=new QHBoxLayout(); row->addWidget(rate->parentWidget()); row->addWidget(grad->parentWidget()); visuals->addLayout(row);
    auto *configuration=Surface::card(this,"Configuration & timing");
    m_configuration=new QLabel(this); m_configuration->setObjectName("cardMeta");
    m_configuration->setTextFormat(Qt::PlainText); m_configuration->setWordWrap(true);
    configuration->addWidget(m_configuration); visuals->addWidget(configuration->parentWidget());
    contentLayout()->addWidget(m_visuals);
    m_raw=new QPlainTextEdit(this); m_raw->setObjectName("runRawData"); m_raw->setReadOnly(true);
    m_raw->setMinimumHeight(380); contentLayout()->addWidget(m_raw); m_raw->hide();
    connect(m_visualise,&QCheckBox::toggled,this,[this](bool enabled) { m_visuals->setVisible(enabled); m_raw->setVisible(!enabled); });
    m_timer=new QTimer(this); m_timer->setInterval(1000);
    connect(m_timer,&QTimer::timeout,this,&TrainingRunPanel::refresh);
}

void TrainingRunPanel::setBackendClient(BackendClient *client) { m_client=client; if (isVisible()) refresh(); }

void TrainingRunPanel::openRun(const QString &id) {
    ++m_generation; m_pending=false; m_timer->stop(); m_id=id;
    m_summary->setText("Loading run…"); m_result->clear(); m_configuration->clear(); m_raw->clear();
    m_loss->setHistory({}); m_rate->setHistory({}); m_gradient->setHistory({});
    refresh();
}
void TrainingRunPanel::showEvent(QShowEvent *event) { BasePanel::showEvent(event); refresh(); }
void TrainingRunPanel::hideEvent(QHideEvent *event) { m_timer->stop(); BasePanel::hideEvent(event); }
void TrainingRunPanel::refresh() {
    if (m_pending || m_id.isEmpty() || !m_client || !m_client->isReady()) return;
    const int generation=m_generation; m_pending=true;
    m_client->get("/training/status/"+m_id,[this,generation](int status,const QJsonDocument &doc) {
        if (generation!=m_generation) return;
        m_pending=false;
        if (status!=200 || !doc.isObject()) {
            m_result->setText(status==404 ? "This run is no longer available in the local training history." : "Could not load run data. Retrying while this page is open.");
            applyStatusTone(m_result,"error");
            if (isVisible() && status!=404) m_timer->start();
            return;
        }
        display(doc.object());
        const auto state=doc.object()["status"].toString();
        if (isVisible() && (state=="running" || state=="queued")) m_timer->start(); else m_timer->stop();
    });
}
void TrainingRunPanel::display(const QJsonObject &job) {
    const auto state=job["status"].toString();
    m_summary->setText(job["name"].toString("Training run")+" · "+state);
    const auto history=job["history"].toArray();
    const int total=job["total_steps"].toInt();
    m_loss->setHistory(history,total); m_rate->setHistory(history,total); m_gradient->setHistory(history,total);
    QString loss=job["loss"].isDouble() ? QString::number(job["loss"].toDouble(),'f',4) : "—";
    QString validation="—";
    for (const auto &sample : history) if (sample.toObject()["eval_loss"].isDouble())
        validation=QString::number(sample.toObject()["eval_loss"].toDouble(),'f',4);
    m_result->setText(QString("%1\nStep %2 / %3 · Training loss %4 · Validation loss %5")
        .arg(job["message"].toString()).arg(job["current_step"].toInt()).arg(total).arg(loss,validation));
    applyStatusTone(m_result,state=="done" ? "ok" : state=="error" ? "error" : "");
    const auto c=job["config"].toObject();
    QStringList facts{"Base model: "+job["base_model"].toString(),"Dataset: "+job["dataset_id"].toString()};
    const QList<QPair<QString,QString>> fields{{"rank","Rank"},{"alpha","Alpha"},{"dropout","Dropout"},{"epochs","Epochs"},
        {"learning_rate","Learning rate"},{"batch_size","Batch size"},{"max_length","Maximum length"},
        {"target_modules","Target modules"},{"quantization","Quantization"},{"validation_split","Validation fraction"},{"seed","Seed"}};
    for (const auto &field : fields) if (c.contains(field.first))
        facts << field.second+": "+c[field.first].toVariant().toString();
    facts << "Started: "+job["started_at"].toString("—") << "Finished: "+job["finished_at"].toString("—");
    facts << QString("%1 retained metric entries · latest 1,000 available").arg(history.size());
    m_configuration->setText(facts.join('\n'));
    m_raw->setPlainText(QString::fromUtf8(QJsonDocument(job).toJson(QJsonDocument::Indented)));
}

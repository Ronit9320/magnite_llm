#pragma once
#include "panels/basepanel.h"
#include <QJsonObject>
class BackendClient;
class TrainingChart;
class QLabel;
class QPlainTextEdit;
class QTimer;
class QCheckBox;

// Contextual run review, deliberately absent from sidebar navigation.
class TrainingRunPanel : public BasePanel {
    Q_OBJECT
public:
    explicit TrainingRunPanel(QWidget *parent=nullptr);
    void setBackendClient(BackendClient *client);
    void openRun(const QString &id);
signals:
    void backRequested();
protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    void refresh();
    void display(const QJsonObject &job);
    BackendClient *m_client=nullptr;
    QString m_id;
    QTimer *m_timer;
    QLabel *m_summary;
    QLabel *m_result;
    QLabel *m_configuration;
    QPlainTextEdit *m_raw;
    QWidget *m_visuals;
    QCheckBox *m_visualise;
    TrainingChart *m_loss;
    TrainingChart *m_rate;
    TrainingChart *m_gradient;
    bool m_pending=false;
    int m_generation=0;
};

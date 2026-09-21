#pragma once

#include <QFrame>

#include <QJsonObject>
#include <QString>

class QLabel;
class QProgressBar;
class QWidget;

// A clickable glass card for a model entry (design doc §8). Renders either a
// local model (name, source, architecture soft-pill, size, license) or a Hub
// search hit (name, source, downloads, likes, license). A thin gradient
// progress bar + mono % appear while the model is being pulled.
class ModelCard : public QFrame {
    Q_OBJECT

public:
    // isHub selects which fields are rendered from `data`.
    ModelCard(const QJsonObject &data, bool isHub, QWidget *parent = nullptr);

    QString id() const { return m_id; }
    bool isCardSelected() const { return m_selected; }

    void setSelected(bool selected);
    // 0..100 updates the bar + % text; any other value hides the bar.
    void setProgress(double percent);

signals:
    void cardClicked(ModelCard *card);
    void actionRequested(ModelCard *card);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void appendProgressRow();

    QString m_id;
    bool m_selected = false;
    QWidget *m_progressRow = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
};
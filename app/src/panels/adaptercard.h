#pragma once

#include <QFrame>

#include <QJsonObject>
#include <QString>

class QPaintEvent;
class QPushButton;
class QLabel;

// A glass card for a trained LoRA/QLoRA adapter (design doc §8). Shows the
// adapter name (Space Grotesk), a base-model badge, rank, method pill
// (LoRA/QLoRA), size and trained date. The card owns its action row: Activate
// (primary; re-click on the active card clears the adapter — a toggle), plus
// secondary Export / Import / Delete buttons. The adapter currently serving
// Chat gets the accent border + glow via setActive().
class AdapterCard : public QFrame {
    Q_OBJECT

public:
    explicit AdapterCard(const QJsonObject &data, QWidget *parent = nullptr);

    QString id() const { return m_id; }
    bool isActive() const { return m_active; }

    void setActive(bool active);

signals:
    void activateRequested(AdapterCard *card);
    void exportRequested(AdapterCard *card);
    void deleteRequested(AdapterCard *card);
    void importRequested();

private:
    QPushButton *m_activateButton;
    QLabel *m_activeLabel = nullptr;
    QString m_id;
    bool m_active = false;
};
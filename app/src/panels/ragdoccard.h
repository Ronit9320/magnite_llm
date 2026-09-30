#pragma once

#include <QFrame>

#include <QJsonObject>
#include <QString>

// A full-width glass card for one ingested RAG document (design doc §8).
// Shows a type icon (file-text tinted by kind: pdf/txt/md/html), the document
// name, its source path and chunk count. Clicking selects the card so the
// panel's "Remove Selected" can delete it.
class RagDocCard : public QFrame {
    Q_OBJECT

public:
    explicit RagDocCard(const QJsonObject &data, QWidget *parent = nullptr);

    QString id() const { return m_id; }
    bool isCardSelected() const { return m_selected; }

    void setSelected(bool selected);

signals:
    void cardClicked(RagDocCard *card, bool ctrl);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QString m_id;
    QString m_path;
    bool m_selected = false;
};
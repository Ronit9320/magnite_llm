#pragma once

#include <QFrame>

#include <QString>

// A full-width glass card for one ingested document (design doc §8). Shows a
// type icon (file-text tinted by extension: pdf/txt/md/html), the file name
// and its path. Clicking selects the card so the panel's "Remove Selected"
// can delete it; cards are cosmetic until the Phase 6 store lands.
class RagDocCard : public QFrame {
    Q_OBJECT

public:
    explicit RagDocCard(const QString &filePath, QWidget *parent = nullptr);

    QString filePath() const { return m_path; }
    bool isCardSelected() const { return m_selected; }

    void setSelected(bool selected);

signals:
    void cardClicked(RagDocCard *card, bool ctrl);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    QString m_path;
    bool m_selected = false;
};
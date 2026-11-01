#pragma once

#include <QFrame>

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

// A clickable surface card for a model entry (design doc §8). Renders either a
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
    // Hub hits only: the model is already in the local library.
    bool isDownloaded() const { return m_downloaded; }
    // The local-library id for this card (the Hub repo id maps to
    // "huggingface:org--name"; local cards already carry their own id).
    QString localId() const;

    void setSelected(bool selected);
    // 0..100 updates the bar + text; any other value hides the progress row.
    // With a known total the text reads "412.0 MB / 1.8 GB · 22%", otherwise "22%".
    void setProgress(double percent, qint64 bytesDone = -1, qint64 bytesTotal = 0);
    // Disables the Cancel button and shows "Cancelling…" until the download ends.
    void setCancelling(bool cancelling);
    // "412.0 MB", "1.8 GB" (SI units, matching the Hugging Face site).
    static QString formatBytes(qint64 bytes);
    // Library sizes arrive in MB: "512 MB", "1.9 GB"; "-" when unknown.
    static QString formatSizeMb(double megabytes);
    // The JSON this card was built from (id, ref, source, size_mb ...).
    const QJsonObject &data() const { return m_data; }
    // Hub results only: the download size once known, shown next to downloads and likes.
    // bytes <= 0 means the Hub could not say ("size unknown").
    void setSize(qint64 bytes);
    // Local cards only: lets the panel lock Delete during a download or a delete.
    void setDeleteEnabled(bool enabled);

signals:
    void cardClicked(ModelCard *card);
    void actionRequested(ModelCard *card);
    void cancelRequested(ModelCard *card);
    void deleteRequested(ModelCard *card);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void appendProgressRow();

    QString m_id;
    QJsonObject m_data;
    bool m_selected = false;
    bool m_downloaded = false;
    bool m_isHub = false;
    QWidget *m_progressRow = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_actionButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QLabel *m_statsLabel = nullptr;   // Hub results: "N downloads · M likes[ · size]"
    QString m_statsBase;
};
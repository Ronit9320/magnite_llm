#pragma once

#include <QJsonObject>
#include <QWidget>

class BlueprintPreview;
class ComboBox;
class QLabel;
class QPlainTextEdit;
class SpinBox;

// The checkpoint and adapter a model node runs with. In live mode it also holds the output
// budget, response format and system prompt, and checks the chosen checkpoint with the backend.
class BlueprintBindingFields : public QWidget {
    Q_OBJECT
public:
    BlueprintBindingFields(BlueprintPreview &preview, const QJsonObject &graph, const QJsonObject &node,
                           QWidget *parent = nullptr);

    QJsonObject node() const;
    // Why the binding cannot be saved yet, or empty.
    QString error() const;
    // A live checkpoint has been checked and the binding can be saved.
    bool ready() const { return m_ready; }

signals:
    void edited();
    void readyChanged(bool ready);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshFixture();
    void refreshLive();
    void loadLiveModels();
    void setReady(bool ready);

    BlueprintPreview &m_preview;
    const QJsonObject m_node;
    const bool m_reasoning;
    const bool m_linked;
    ComboBox *m_models;
    ComboBox *m_adapters;
    QLabel *m_note;
    SpinBox *m_budget = nullptr;
    ComboBox *m_response = nullptr;
    QPlainTextEdit *m_prompt = nullptr;
    bool m_ready = true;
};

// Where a dataset node's rows come from: a saved local dataset or inline JSON rows.
class BlueprintDatasetFields : public QWidget {
    Q_OBJECT
public:
    BlueprintDatasetFields(BlueprintPreview &preview, const QJsonObject &node, QWidget *parent = nullptr);

    QJsonObject node() const;
    QString error() const;

signals:
    void edited();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void populate();

    BlueprintPreview &m_preview;
    const QJsonObject m_node;
    ComboBox *m_datasets;
    QPlainTextEdit *m_rows;
};

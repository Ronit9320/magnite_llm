#pragma once

#include <QJsonObject>
#include <QList>
#include <QWidget>

class BlueprintPreview;
class ComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;

// The fields that configure one Blueprint node: kind, operation, name, the operation's
// setting, a live checkpoint or adapter binding, and the port editors. The Configure node
// dialog and the in-place inspector both host it.
class BlueprintNodeFields : public QWidget {
    Q_OBJECT
public:
    BlueprintNodeFields(BlueprintPreview &preview, const QString &kind, const QJsonObject &node,
                        QWidget *parent = nullptr);

    // The node as edited, or an empty object while error() is not empty.
    QJsonObject node() const;
    // Why the current fields cannot be saved, or empty.
    QString error() const;
    // Shows error() below the fields and focuses the field it is about. Returns false when valid.
    bool showError();
    // Port ids or types differ from the node the fields were built for.
    bool portsChanged() const;
    QJsonObject original() const { return m_original; }
    QWidget *firstField() const;
    // Leaves the live checkpoint or adapter picker out (a host that shows its own binding).
    void setBindingShown(bool shown);
    // Leaves out the descriptions and port notes (a host whose info panel explains the node).
    void setHelpShown(bool shown);

signals:
    void edited();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct PortEditor {
        QJsonObject port;
        QString direction;
        QLineEdit *name;
        ComboBox *type;
        QLineEdit *key;
        QWidget *row;
    };

    void loadOperations(const QString &preferred = {});
    void loadOptions();
    QString defaultName() const;
    static bool editableKeys(const QString &operation, const QString &side);
    void refreshBindings(const QString &selection);
    void appendPort(QVBoxLayout *rows, const QString &side, const QJsonObject &port, bool chooseType, bool editable);
    QJsonObject build(QString *error, QWidget **focus) const;
    void fail(const QString &message);
    void watchLine(QLineEdit *edit);

    BlueprintPreview &m_preview;
    const QJsonObject m_original;
    ComboBox *m_kind;
    ComboBox *m_choice = nullptr;
    ComboBox *m_operation;
    QLineEdit *m_name;
    QLabel *m_detail;
    bool m_helpShown = true;
    QLabel *m_error;
    QVBoxLayout *m_options;
    QWidget *m_fields = nullptr;
    QLineEdit *m_line = nullptr;
    ComboBox *m_binding = nullptr;
    QJsonObject m_resourcePort;
    QString m_resourceSide;
    QPlainTextEdit *m_text = nullptr;
    QList<PortEditor> m_ports;
    bool m_bindingShown = true;
};

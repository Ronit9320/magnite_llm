#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

class BlueprintBindingFields;
class BlueprintDatasetFields;
class BlueprintNodeFields;
class BlueprintPreview;
class QLabel;
class QVBoxLayout;

// The floating node inspector. Its fields edit the selected node in place: each committed
// change is emitted as nodeChanged for the node the fields were built for, never for a
// node selected later.
class BlueprintInspector : public QWidget {
    Q_OBJECT
public:
    explicit BlueprintInspector(BlueprintPreview &preview, QWidget *parent = nullptr);
    // Width of the floating panel body that holds the inspector.
    static constexpr int panelWidth = 360;

    // Shows nodeId from graph; an empty id clears. A pending text edit for the previous
    // node is committed (for that node) first.
    void setNode(const QJsonObject &graph, const QString &nodeId);
    QString nodeId() const { return m_nodeId; }
    // Rebuilds from the last graph, e.g. after the host declined a change.
    void reload();

signals:
    void nodeChanged(const QJsonObject &node, bool portsChanged);
    void disconnectRequested(const QJsonObject &edge);
    void schemaRequested(const QString &side, const QString &portId);
    void trainRequested(const QString &id);
    void testRequested(const QString &id);
    void saveToLibraryRequested(const QString &id);
    void deleteRequested(const QString &id);
    void detailsRequested(const QString &id);
    void closeRequested();

private:
    void rebuild();
    void flush();
    void commitNodeFields();
    void commitBinding();
    void commitDataset();
    void addPorts(QVBoxLayout *body);

    BlueprintPreview &m_preview;
    QVBoxLayout *m_root;
    QPointer<QWidget> m_content;
    QJsonObject m_graph;
    QJsonObject m_node;
    QString m_nodeId;
    quint64 m_generation = 0;
    QPointer<BlueprintNodeFields> m_fields;
    QPointer<BlueprintBindingFields> m_binding;
    QPointer<BlueprintDatasetFields> m_dataset;
    QPointer<QLabel> m_bindingError;
    // A binding edit made while the live checkpoint check runs; saved once the check finishes.
    bool m_bindingPending = false;
};

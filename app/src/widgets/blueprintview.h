#pragma once

#include "widgets/blueprintscene.h"

#include <QGraphicsView>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <functional>

class BlueprintMinimap;
class BlueprintPreview;
class Button;
class FloatingPanel;
class QGraphicsPathItem;
class QLabel;
class QTimer;

// Where an overlay widget sits on the view.
enum class OverlayEdge { TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight };

// The Blueprints canvas: camera (pan, zoom, fit), selection, node moving and the outward signals.
class BlueprintView : public QGraphicsView {
    Q_OBJECT
public:
    explicit BlueprintView(const BlueprintPreview &preview, QWidget *parent = nullptr);
    ~BlueprintView() override;

    BlueprintScene *graphScene() const { return m_scene; }
    void setGraph(const QJsonObject &graph);
    void setSelection(const QString &nodeId);
    QString selection() const;
    void fit();
    void zoomBy(qreal factor);
    int zoomPercent() const;
    void revealNode(const QString &id);
    QPointF centerInGraph() const;
    // Moves the camera to a graph point; like panning, it ends automatic fitting.
    void centerAt(const QPointF &graphPoint);
    // Pins a widget over the canvas, margin pixels from its edge; panels are inset by their shadow. A Top
    // overlay with an expanding width spans the view, Left and Right overlays fill the height below it, and
    // Top and Bottom overlays centre between the visible side overlays. Overlays follow resizes and their
    // own show, hide and layout.
    void addOverlay(QWidget *widget, OverlayEdge edge, int margin = 12);
    // Takes a widget out of the overlay layout; it stays parented to the view until reparented.
    void removeOverlay(QWidget *widget);
    // The viewport minus the visible Left and Right overlays.
    QRect freeArea() const;
    // Checks or unchecks the dock's "Show node library" toggle (libraryToggled reports the change).
    void setLibraryShown(bool shown);
    bool libraryShown() const;
    void setRunStates(const QHash<QString, QString> &statusByNode, const QString &activeNode);
    void clearRunStates();
    // Returns why an edge cannot be dropped, or an empty string when it can.
    void setDropValidator(std::function<QString(const QJsonObject &edge)> validator) { m_validator = std::move(validator); }
    // The edge of the wire being moved off its input, or empty. Validators judge drops against the graph without it.
    QJsonObject movingEdge() const;

signals:
    void zoomChanged(int percent);
    void libraryToggled(bool shown);
    void addNodeClicked();
    void nodeSelected(const QString &id);
    void nodeMenuRequested(const QString &id, const QPoint &screenPosition, const QPointF &graphPosition);
    void canvasMenuRequested(const QPoint &screenPosition, const QPointF &graphPosition);
    void quickAddRequested(const QPoint &screenPosition, const QPointF &graphPosition);
    void detailsRequested(const QString &id);
    void deleteRequested(const QString &id);
    void nodeMoved(const QString &id, const QPoint &position);
    void connectRequested(const QJsonObject &edge);
    void disconnectRequested(const QJsonObject &edge);
    // A picked-up wire was dropped on another input: one edit replacing oldEdge with newEdge.
    void wireMoved(const QJsonObject &oldEdge, const QJsonObject &newEdge);
    void wireDropped(const QString &fromNode, const QString &outPort, const QPoint &screen, const QPointF &graph);
    // A node library entry (BlueprintLibraryDock::Entry JSON) was dropped at a graph position.
    void entryDropped(const QByteArray &entry, const QPointF &graph);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    NodeItem *nodeAt(const QPoint &viewPosition) const;
    void zoomAt(qreal factor, const QPointF &viewPosition);
    void panBy(const QPointF &viewDelta);
    void applyRunStates();
    void onSelectionChanged();
    void requestQuickAdd();
    bool beginConnect(PinItem *pin);
    void updateConnect(const QPointF &viewPosition);
    void finishConnect(const QPoint &viewPosition);
    void cancelConnect();
    void showDropMessage(const QString &text, const QPointF &scenePosition);
    void selectWire(WireItem *wire);
    struct Overlay {
        QWidget *widget;
        OverlayEdge edge;
        int margin;
    };
    QHash<QWidget *, QRect> overlayLayout() const;
    void placeOverlays();
    void buildDock();
    qreal zoom() const { return transform().m11(); }

    BlueprintScene *m_scene;
    QList<Overlay> m_overlays;
    bool m_placing = false;
    QLabel *m_zoomLabel = nullptr;
    Button *m_libraryToggle = nullptr;
    QString m_selected;
    QHash<QString, QString> m_status;
    QString m_activeNode;
    bool m_autoFit = true;
    bool m_quiet = false;
    bool m_panning = false;
    bool m_panMoved = false;
    QPointF m_pressPos;
    QPointF m_panStartCenter;
    NodeItem *m_dragged = nullptr;
    // Dragging a node near the edge keeps scrolling the canvas.
    QTimer *m_edgePan = nullptr;
    QPointF m_edgePointer;
    QPointF edgeVelocity(const QPointF &viewPosition) const;
    void edgePanStep();
    QPointF m_dragStart;
    WireItem *m_wire = nullptr;  // the selected wire, if any
    std::function<QString(const QJsonObject &)> m_validator;
    PinItem *m_source = nullptr;             // output pin a wire is being dragged from
    WireItem *m_pickedUp = nullptr;          // existing wire detached while it is dragged
    QGraphicsPathItem *m_temporary = nullptr;
    QLabel *m_dropMessage = nullptr;
    QTimer *m_dropTimer = nullptr;
};

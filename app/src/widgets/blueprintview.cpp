#include "widgets/blueprintview.h"

#include "theme.h"
#include "widgets/blueprintminimap.h"
#include "widgets/button.h"
#include "widgets/floatingpanel.h"
#include "widgets/blueprintstyle.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGraphicsPathItem>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>

namespace {
constexpr qreal gridStep = 24;
constexpr qreal dotsMinZoom = 0.35;
constexpr qreal minZoom = 0.25;
constexpr qreal maxZoom = 2.5;
constexpr qreal fitFloor = 0.05;
constexpr qreal keyPan = 45;
constexpr qreal sceneExtent = 100000;
}  // namespace

BlueprintView::BlueprintView(const BlueprintPreview &preview, QWidget *parent)
    : QGraphicsView(parent), m_scene(new BlueprintScene(preview, this)) {
    setObjectName("blueprintView");
    setProperty("transientScroll", true);  // the wheel zooms here; SmoothWheel must not scroll it
    setAccessibleName("Workflow graph");
    setAccessibleDescription("Drag the background to pan and drag a node to move it. Scroll to zoom. "
                             "Use the arrow keys to pan, plus or minus to zoom, and F or Home to fit. "
                             "Press Delete to remove the selected node, Space or Tab to add a node, and I for its details. "
                             "Press Control+Tab to move focus forward into the bottom dock and on out of the graph, and Shift+Tab to move it back.");
    setScene(m_scene);
    m_scene->setSceneRect(-sceneExtent, -sceneExtent, 2 * sceneExtent, 2 * sceneExtent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(240, 275);
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::NoDrag);
    setRubberBandSelectionMode(Qt::IntersectsItemShape);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setCacheMode(QGraphicsView::CacheBackground);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &BlueprintView::onSelectionChanged);
    buildDock();
    addOverlay(new BlueprintMinimap(this), OverlayEdge::BottomRight);
}

void BlueprintView::buildDock() {
    auto *dock = new FloatingPanel;
    dock->setObjectName("blueprintDock");
    auto *row = new QHBoxLayout(dock);
    row->setContentsMargins(0, 0, 0, 0);  // the panel's own contentsMargins leave room for the shadow
    row->setSpacing(2);
    auto iconButton = [dock, row](const QString &icon, const QString &name) {
        auto *button = new Button(dock);
        button->setFlat(true);
        button->setObjectName("blueprintIconButton");
        button->setAccessibleName(name);
        button->setToolTip(name);
        button->setFixedSize(32, 32);
        button->setProperty("themeIcon", icon);
        button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 16));
        row->addWidget(button);
        return button;
    };
    auto *library = iconButton("layers", "Show node library");
    library->setCheckable(true);
    m_libraryToggle = library;
    auto *add = new Button("Add node", dock);
    add->setFlat(true);
    add->setAccessibleName("Add node");
    add->setIcon(Theme::coloredIcon("plus", QColor(Theme::textSecondary()), 16));
    add->setProperty("themeIcon", "plus");
    row->addWidget(add);
    row->addSpacing(6);
    auto *out = iconButton("minus", "Zoom out");
    m_zoomLabel = new QLabel("100%", dock);
    m_zoomLabel->setObjectName("blueprintCode");
    m_zoomLabel->setAccessibleName("Zoom level");
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setMinimumWidth(44);
    row->addWidget(m_zoomLabel);
    auto *in = iconButton("plus", "Zoom in");
    auto *fitButton = iconButton("expand", "Fit graph");
    connect(library, &QAbstractButton::toggled, this, &BlueprintView::libraryToggled);
    connect(add, &QAbstractButton::clicked, this, &BlueprintView::addNodeClicked);
    connect(out, &QAbstractButton::clicked, this, [this] { zoomBy(1 / 1.2); });
    connect(in, &QAbstractButton::clicked, this, [this] { zoomBy(1.2); });
    connect(fitButton, &QAbstractButton::clicked, this, &BlueprintView::fit);
    connect(this, &BlueprintView::zoomChanged, m_zoomLabel, [this](int percent) { m_zoomLabel->setText(QString("%1%").arg(percent)); });
    addOverlay(dock, OverlayEdge::Bottom);
}

void BlueprintView::addOverlay(QWidget *widget, OverlayEdge edge, int margin) {
    widget->setParent(this);
    widget->installEventFilter(this);
    m_overlays.append({widget, edge, margin});
    widget->show();  // overlays start visible; hide() one to take it out of the layout
    placeOverlays();
}

void BlueprintView::removeOverlay(QWidget *widget) {
    for (int i = 0; i < m_overlays.size(); ++i)
        if (m_overlays.at(i).widget == widget) {
            widget->removeEventFilter(this);
            m_overlays.removeAt(i);
            break;
        }
}

void BlueprintView::setLibraryShown(bool shown) {
    if (m_libraryToggle) m_libraryToggle->setChecked(shown);
}

bool BlueprintView::libraryShown() const { return m_libraryToggle && m_libraryToggle->isChecked(); }

namespace {
bool shownOverlay(const QWidget *w) { return !(w->isHidden() && w->testAttribute(Qt::WA_WState_ExplicitShowHide)); }
}  // namespace

// Where every overlay goes. Top overlays that expand horizontally span the view; Left and Right overlays
// fill the height below the top row; Top and Bottom overlays centre in the room between the side overlays.
QHash<QWidget *, QRect> BlueprintView::overlayLayout() const {
    const QRect area = viewport()->geometry();
    QHash<QWidget *, QRect> boxes;
    const auto pad = [](const QWidget *w) { return qobject_cast<const FloatingPanel *>(w) ? FloatingPanel::shadowMargin() : 0; };
    const auto sizeOf = [](const QWidget *w) {
        QSize size = w->sizeHint().expandedTo(w->minimumSizeHint()).expandedTo(w->minimumSize());
        if (w->minimumSize() == w->maximumSize()) size = w->minimumSize();
        return size;
    };
    // Pass 1: the top row, which side overlays start below.
    int sideTop = area.top();
    for (const auto &overlay : m_overlays) {
        if (overlay.edge != OverlayEdge::Top || !shownOverlay(overlay.widget)) continue;
        if (!(overlay.widget->sizePolicy().horizontalPolicy() & QSizePolicy::ExpandFlag)) continue;
        const int m = overlay.margin - pad(overlay.widget);
        const QRect box(area.left() + m, area.top() + m, area.width() - 2 * m, sizeOf(overlay.widget).height());
        boxes.insert(overlay.widget, box);
        sideTop = qMax(sideTop, box.bottom() - pad(overlay.widget) + 1);
    }
    // Pass 2: the side overlays, and the room they leave.
    int left = area.left(), right = area.right();
    for (const auto &overlay : m_overlays) {
        if (overlay.edge != OverlayEdge::Left && overlay.edge != OverlayEdge::Right) continue;
        const int m = overlay.margin - pad(overlay.widget);
        const int width = sizeOf(overlay.widget).width();
        const int top = sideTop + m;
        const int x = overlay.edge == OverlayEdge::Left ? area.left() + m : area.right() + 1 - m - width;
        const QRect box(x, top, width, area.bottom() + 1 - m - top);
        boxes.insert(overlay.widget, box);
        if (!overlay.widget->isVisibleTo(this)) continue;
        if (overlay.edge == OverlayEdge::Left) left = qMax(left, box.right() - pad(overlay.widget) + overlay.margin + 1);
        else right = qMin(right, box.left() + pad(overlay.widget) - overlay.margin - 1);
    }
    // Pass 3: everything else.
    for (const auto &overlay : m_overlays) {
        if (boxes.contains(overlay.widget)) continue;
        const int m = overlay.margin - pad(overlay.widget);
        const QSize size = sizeOf(overlay.widget);
        QPoint at;
        switch (overlay.edge) {
        case OverlayEdge::TopLeft: case OverlayEdge::BottomLeft: at.setX(area.left() + m); break;
        case OverlayEdge::TopRight: case OverlayEdge::BottomRight: at.setX(area.right() + 1 - m - size.width()); break;
        default: at.setX(left + (right + 1 - left - size.width()) / 2); break;
        }
        switch (overlay.edge) {
        case OverlayEdge::BottomLeft: case OverlayEdge::Bottom: case OverlayEdge::BottomRight: at.setY(area.bottom() + 1 - m - size.height()); break;
        default: at.setY(area.top() + m); break;
        }
        boxes.insert(overlay.widget, QRect(at, size));
    }
    return boxes;
}

QRect BlueprintView::freeArea() const {
    // Computed from the layout rather than the widgets' geometry, so it is right before a queued placement runs.
    QRect area = viewport()->geometry();
    const auto boxes = overlayLayout();
    for (const auto &overlay : m_overlays) {
        if (!overlay.widget->isVisibleTo(this)) continue;
        // The panel body, not its shadow, is what covers the canvas.
        const int pad = qobject_cast<const FloatingPanel *>(overlay.widget) ? FloatingPanel::shadowMargin() : 0;
        const QRect box = boxes.value(overlay.widget).adjusted(pad, pad, -pad, -pad);
        if (overlay.edge == OverlayEdge::Left) area.setLeft(qMax(area.left(), box.right() + overlay.margin + 1));
        if (overlay.edge == OverlayEdge::Right) area.setRight(qMin(area.right(), box.left() - overlay.margin - 1));
    }
    return area;
}

void BlueprintView::placeOverlays() {
    if (m_placing) return;
    m_placing = true;
    const auto boxes = overlayLayout();
    for (const auto &overlay : m_overlays) {
        if (!shownOverlay(overlay.widget)) continue;  // hidden on purpose
        overlay.widget->setGeometry(boxes.value(overlay.widget));
        overlay.widget->raise();
    }
    m_placing = false;
}

bool BlueprintView::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::LayoutRequest)
        for (const auto &overlay : m_overlays)
            if (overlay.widget == watched) {
                QMetaObject::invokeMethod(this, [this] { placeOverlays(); }, Qt::QueuedConnection);
                break;
            }
    return QGraphicsView::eventFilter(watched, event);
}

int BlueprintView::zoomPercent() const { return qRound(zoom() * 100); }

QPointF BlueprintView::centerInGraph() const {
    return mapToScene(QPoint(viewport()->width() / 2, viewport()->height() / 2));
}

BlueprintView::~BlueprintView() {
    // The scene is a child, so it is cleared after this object's members are gone; its last
    // selectionChanged must not reach the half-destroyed view.
    m_scene->disconnect(this);
}

void BlueprintView::setGraph(const QJsonObject &graph) {
    const QString previous = m_selected;
    cancelConnect();
    m_quiet = true;
    m_dragged = nullptr;
    m_wire = nullptr;
    m_scene->setGraph(graph);
    m_quiet = false;
    if (auto *item = m_scene->node(previous)) {
        m_quiet = true;
        item->setSelected(true);
        m_quiet = false;
    } else if (!previous.isEmpty()) {
        m_selected.clear();
        emit nodeSelected({});
    }
    applyRunStates();
    if (m_autoFit) fit();
}

QString BlueprintView::selection() const { return m_selected; }

QJsonObject BlueprintView::movingEdge() const { return m_pickedUp ? m_pickedUp->edge() : QJsonObject(); }

void BlueprintView::setSelection(const QString &nodeId) {
    // Callers echo nodeSelected back; an unchanged node selection keeps a selected wire.
    if (nodeId == m_selected) return;
    m_quiet = true;
    m_scene->clearSelection();
    m_selected.clear();
    m_wire = nullptr;
    if (auto *item = m_scene->node(nodeId)) {
        item->setSelected(true);
        m_selected = nodeId;
    }
    m_quiet = false;
}

void BlueprintView::onSelectionChanged() {
    if (m_quiet) return;
    // Selection is single-valued: keep the node being pressed (or the first), drop the rest.
    const auto selected = m_scene->selectedItems();
    if (selected.size() > 1) {
        QGraphicsItem *keep = selected.first();
        if (m_dragged && m_dragged->isSelected()) keep = m_dragged;
        m_quiet = true;
        for (auto *item : selected)
            if (item != keep) item->setSelected(false);
        m_quiet = false;
    }
    QString id;
    m_wire = nullptr;
    for (auto *node : m_scene->nodes())
        if (node->isSelected()) {
            id = node->id();
            break;
        }
    for (auto *wire : m_scene->wires())
        if (wire->isSelected()) {
            m_wire = wire;
            break;
        }
    if (id == m_selected) return;
    m_selected = id;
    emit nodeSelected(id);
}

void BlueprintView::selectWire(WireItem *wire) {
    m_quiet = true;
    m_scene->clearSelection();
    wire->setSelected(true);
    m_quiet = false;
    onSelectionChanged();
}

void BlueprintView::fit() {
    QRectF extent;
    for (auto *node : m_scene->nodes())
        extent = extent.united(QRectF(node->pos(), NodeItem::sizeFor(node->node())).adjusted(-24, -34, 24, 28));
    if (extent.isEmpty()) extent = QRectF(0, 0, 680, 438);
    m_autoFit = true;
    const QSize size = viewport()->size();
    const qreal value = qBound(fitFloor, qMin((size.width() - 26) / extent.width(), (size.height() - 26) / extent.height()), qreal(1));
    setTransform(QTransform::fromScale(value, value));
    centerOn(extent.center());
    emit zoomChanged(zoomPercent());
    viewport()->update();
}

void BlueprintView::zoomAt(qreal factor, const QPointF &viewPosition) {
    const qreal current = zoom();
    const qreal next = qBound(qMin(minZoom, current), current * factor, maxZoom);
    m_autoFit = false;
    if (qFuzzyCompare(next, current)) return;
    const QPointF anchored = mapToScene(viewPosition.toPoint());
    setTransform(QTransform::fromScale(next, next));
    centerOn(centerInGraph() + anchored - mapToScene(viewPosition.toPoint()));
    emit zoomChanged(zoomPercent());
}

void BlueprintView::zoomBy(qreal factor) {
    zoomAt(factor, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

void BlueprintView::centerAt(const QPointF &graphPoint) {
    m_autoFit = false;
    centerOn(graphPoint);
}

void BlueprintView::panBy(const QPointF &viewDelta) {
    m_autoFit = false;
    centerOn(centerInGraph() - viewDelta / zoom());
}

void BlueprintView::revealNode(const QString &id) {
    auto *item = m_scene->node(id);
    if (!item) return;
    const QRectF box(item->pos(), NodeItem::sizeFor(item->node()));
    // Only move the camera when the node is not already fully in view.
    if (mapToScene(viewport()->rect().adjusted(5, 5, -5, -5)).boundingRect().contains(box)) return;
    m_autoFit = false;
    centerOn(box.center());
}

void BlueprintView::setRunStates(const QHash<QString, QString> &statusByNode, const QString &activeNode) {
    m_status = statusByNode;
    m_activeNode = activeNode;
    applyRunStates();
}

void BlueprintView::clearRunStates() { setRunStates({}, {}); }

void BlueprintView::applyRunStates() {
    for (auto *node : m_scene->nodes()) node->setRunStatus(m_status.value(node->id()));
    for (auto *wire : m_scene->wires())
        wire->setActive(!m_activeNode.isEmpty() && wire->edge()["to"].toString() == m_activeNode);
}

void BlueprintView::drawBackground(QPainter *painter, const QRectF &rect) {
    painter->fillRect(rect, QColor(Theme::canvasBackground()));
    if (zoom() < dotsMinZoom) return;
    QPen pen{QColor(Theme::blueprintCanvasDot())};
    pen.setWidthF(2.5);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    QVector<QPointF> dots;
    const qreal left = std::floor(rect.left() / gridStep) * gridStep;
    const qreal top = std::floor(rect.top() / gridStep) * gridStep;
    for (qreal y = top; y <= rect.bottom(); y += gridStep)
        for (qreal x = left; x <= rect.right(); x += gridStep) dots.append({x, y});
    painter->drawPoints(dots.constData(), dots.size());
}

void BlueprintView::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    placeOverlays();
    if (m_autoFit) fit();
}

void BlueprintView::changeEvent(QEvent *event) {
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        resetCachedContent();
        viewport()->update();
    }
}

void BlueprintView::wheelEvent(QWheelEvent *event) {
    const qreal delta = event->pixelDelta().isNull() ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta != 0) {
        zoomAt(std::exp(delta * .0016), event->position());
        event->accept();
    } else event->ignore();
}

NodeItem *BlueprintView::nodeAt(const QPoint &viewPosition) const {
    for (auto *item : items(viewPosition))
        for (auto *node : m_scene->nodes())
            if (node == item || node->isAncestorOf(item)) return node;
    return nullptr;
}

void BlueprintView::mousePressEvent(QMouseEvent *event) {
    const bool left = event->button() == Qt::LeftButton, middle = event->button() == Qt::MiddleButton;
    if (!left && !middle) {
        event->accept();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    if (m_source) {  // a wire drag is in progress; ignore other buttons
        event->accept();
        return;
    }
    if (left) {
        // The top-most pin or wire under the cursor wins over panning; nodes keep their own handling.
        for (auto *item : items(event->position().toPoint())) {
            if (item == m_temporary) continue;
            if (auto *pin = dynamic_cast<PinItem *>(item)) {
                if (beginConnect(pin)) {
                    updateConnect(event->position());
                    event->accept();
                    return;
                }
                break;
            }
            if (auto *wire = dynamic_cast<WireItem *>(item)) {
                selectWire(wire);
                event->accept();
                return;
            }
            break;
        }
    }
    auto *node = left ? nodeAt(event->position().toPoint()) : nullptr;
    if (node) {
        m_dragged = node;
        m_dragStart = node->pos();
        // Ctrl/Shift must not extend the selection: it is always one node.
        QMouseEvent plain(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                          event->button(), event->buttons(), Qt::NoModifier);
        m_quiet = true;  // Qt clears then selects; report the result once
        QGraphicsView::mousePressEvent(&plain);
        m_quiet = false;
        onSelectionChanged();
        event->setAccepted(plain.isAccepted());
        return;
    }
    m_panning = true;
    m_panMoved = false;
    m_pressPos = event->position();
    m_panStartCenter = centerInGraph();
    event->accept();
}

void BlueprintView::mouseMoveEvent(QMouseEvent *event) {
    if (m_source) {
        updateConnect(event->position());
        event->accept();
        return;
    }
    if (m_panning) {
        const QPointF delta = event->position() - m_pressPos;
        if (!m_panMoved && delta.manhattanLength() >= QApplication::startDragDistance()) {
            m_panMoved = true;
            m_autoFit = false;
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
        if (m_panMoved) centerOn(m_panStartCenter - delta / zoom());
        event->accept();
        return;
    }
    if (m_dragged && event->buttons().testFlag(Qt::LeftButton)) {
        m_autoFit = false;
        viewport()->setCursor(Qt::ClosedHandCursor);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void BlueprintView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_source) {
        if (event->button() == Qt::LeftButton) finishConnect(event->position().toPoint());
        event->accept();
        return;
    }
    if (m_panning) {
        if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
            m_panning = false;
            viewport()->unsetCursor();
            if (!m_panMoved && event->button() == Qt::LeftButton) m_scene->clearSelection();
            event->accept();
        }
        return;
    }
    QMouseEvent plain(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                      event->button(), event->buttons(), Qt::NoModifier);  // Ctrl+release would toggle the selection
    QGraphicsView::mouseReleaseEvent(&plain);
    event->setAccepted(plain.isAccepted());
    if (m_dragged && event->button() == Qt::LeftButton) {
        NodeItem *node = std::exchange(m_dragged, nullptr);
        viewport()->unsetCursor();
        if (node->pos().toPoint() != m_dragStart.toPoint()) emit nodeMoved(node->id(), node->pos().toPoint());
    }
}

void BlueprintView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && !nodeAt(event->position().toPoint())) {
        emit quickAddRequested(event->globalPosition().toPoint(), mapToScene(event->position().toPoint()));
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

bool BlueprintView::beginConnect(PinItem *pin) {
    WireItem *picked = nullptr;
    PinItem *source = pin;
    if (!pin->isOutput()) {
        for (auto *wire : m_scene->wires())
            if (wire->to() == pin) picked = wire;
        if (!picked) return false;
        source = picked->from();
    }
    if (m_dropMessage) m_dropMessage->hide();
    m_source = source;
    m_pickedUp = picked;
    if (picked) picked->hide();
    m_temporary = new QGraphicsPathItem;
    m_temporary->setZValue(2);
    m_temporary->setAcceptedMouseButtons(Qt::NoButton);
    QPen pen(BlueprintStyle::typeColor(source->portType()), 2);
    pen.setCapStyle(Qt::RoundCap);
    m_temporary->setPen(pen);
    m_scene->addItem(m_temporary);
    for (auto *node : m_scene->nodes())
        for (auto *input : node->pins()) {
            if (input->isOutput()) continue;
            const QJsonObject candidate{{"from", source->nodeId()}, {"out", source->portId()},
                                        {"to", input->nodeId()}, {"in", input->portId()}};
            const bool allowed = !m_validator || m_validator(candidate).isEmpty();
            input->setHighlight(allowed ? PinItem::Highlight::Compatible : PinItem::Highlight::Dimmed);
        }
    viewport()->setCursor(Qt::CrossCursor);
    return true;
}

void BlueprintView::updateConnect(const QPointF &viewPosition) {
    if (m_temporary) m_temporary->setPath(WireItem::pathBetween(m_source->anchor(), mapToScene(viewPosition.toPoint())));
}

void BlueprintView::cancelConnect() {
    if (!m_source) return;
    for (auto *node : m_scene->nodes())
        for (auto *pin : node->pins()) pin->setHighlight(PinItem::Highlight::None);
    if (m_pickedUp) m_pickedUp->show();
    delete m_temporary;
    m_temporary = nullptr;
    m_source = nullptr;
    m_pickedUp = nullptr;
    viewport()->unsetCursor();
}

void BlueprintView::finishConnect(const QPoint &viewPosition) {
    PinItem *target = nullptr;
    bool overNode = false;
    for (auto *item : items(viewPosition)) {
        if (item == m_temporary) continue;
        if (auto *pin = dynamic_cast<PinItem *>(item)) {
            if (!pin->isOutput()) target = pin;
            overNode = true;
            break;
        }
        if (dynamic_cast<WireItem *>(item)) continue;
        overNode = true;
        break;
    }
    const QString fromNode = m_source->nodeId(), outPort = m_source->portId();
    const QJsonObject old = m_pickedUp ? m_pickedUp->edge() : QJsonObject();
    const bool picked = m_pickedUp != nullptr;
    const bool samePin = picked && target && m_pickedUp->to() == target;
    QJsonObject next;
    QString error;
    if (target && !samePin) {
        next = QJsonObject{{"from", fromNode}, {"out", outPort}, {"to", target->nodeId()}, {"in", target->portId()}};
        error = m_validator ? m_validator(next) : QString();
    }
    const QPointF messageAt = target ? target->anchor() : QPointF();
    cancelConnect();  // restores any picked-up wire and clears highlights before signals can rebuild the scene
    if (target && !samePin) {
        if (!error.isEmpty()) {
            showDropMessage(error, messageAt);
            return;
        }
        if (picked) emit wireMoved(old, next);
        else emit connectRequested(next);
    } else if (!overNode) {
        if (picked) emit disconnectRequested(old);
        else emit wireDropped(fromNode, outPort, viewport()->mapToGlobal(viewPosition), mapToScene(viewPosition));
    }
}

void BlueprintView::showDropMessage(const QString &text, const QPointF &scenePosition) {
    if (!m_dropMessage) {
        m_dropMessage = new QLabel(viewport());
        m_dropMessage->setObjectName("blueprintDropMessage");
        m_dropMessage->setWordWrap(true);
        m_dropMessage->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dropTimer = new QTimer(this);
        m_dropTimer->setSingleShot(true);
        m_dropTimer->setInterval(2500);
        connect(m_dropTimer, &QTimer::timeout, m_dropMessage, &QLabel::hide);
    }
    m_dropMessage->setText(text);
    m_dropMessage->setFixedWidth(qMin(300, viewport()->width() - 16));
    m_dropMessage->setFixedHeight(m_dropMessage->heightForWidth(m_dropMessage->width()));
    const QPoint at = mapFromScene(scenePosition) + QPoint(14, 14);
    m_dropMessage->move(qBound(8, at.x(), qMax(8, viewport()->width() - m_dropMessage->width() - 8)),
                        qBound(8, at.y(), qMax(8, viewport()->height() - m_dropMessage->height() - 8)));
    m_dropMessage->show();
    m_dropMessage->raise();
    m_dropTimer->start();
}

void BlueprintView::contextMenuEvent(QContextMenuEvent *event) {
    const bool keyboard = event->reason() == QContextMenuEvent::Keyboard;
    const QPoint at = keyboard ? QPoint(viewport()->width() / 2, viewport()->height() / 2) : event->pos();
    const QPoint global = keyboard ? viewport()->mapToGlobal(at) : event->globalPos();
    NodeItem *node = keyboard ? m_scene->node(m_selected) : nodeAt(at);
    if (node) {
        if (node->id() != m_selected) {
            m_scene->clearSelection();
            node->setSelected(true);
        }
        emit nodeMenuRequested(node->id(), global, mapToScene(at));
    } else emit canvasMenuRequested(global, mapToScene(at));
    event->accept();
}

void BlueprintView::requestQuickAdd() {
    QPoint at = viewport()->mapFromGlobal(QCursor::pos());
    if (!viewport()->rect().contains(at)) at = QPoint(viewport()->width() / 2, viewport()->height() / 2);
    emit quickAddRequested(viewport()->mapToGlobal(at), mapToScene(at));
}

bool BlueprintView::event(QEvent *event) {
    // Tab adds a node here; Ctrl+Tab and Shift+Tab move focus out of the canvas.
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier) {
            requestQuickAdd();
            key->accept();
            return true;
        }
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::ControlModifier) {
            focusNextChild();
            key->accept();
            return true;
        }
    }
    return QGraphicsView::event(event);
}

void BlueprintView::keyPressEvent(QKeyEvent *event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    switch (event->key()) {
    case Qt::Key_Escape:
        if (!m_source) { QGraphicsView::keyPressEvent(event); return; }
        cancelConnect();
        break;
    case Qt::Key_Plus: case Qt::Key_Equal: zoomBy(1.2); break;
    case Qt::Key_Minus: zoomBy(1 / 1.2); break;
    case Qt::Key_Home: case Qt::Key_F: fit(); break;
    case Qt::Key_Delete: case Qt::Key_Backspace:
        if (m_wire) {
            emit disconnectRequested(m_wire->edge());
            break;
        }
        if (m_selected.isEmpty()) { event->ignore(); return; }
        emit deleteRequested(m_selected);
        break;
    case Qt::Key_Space: requestQuickAdd(); break;
    case Qt::Key_I:
        if (m_selected.isEmpty()) { event->ignore(); return; }
        emit detailsRequested(m_selected);
        break;
    case Qt::Key_Left: panBy({keyPan, 0}); break;
    case Qt::Key_Right: panBy({-keyPan, 0}); break;
    case Qt::Key_Up: panBy({0, keyPan}); break;
    case Qt::Key_Down: panBy({0, -keyPan}); break;
    default: QGraphicsView::keyPressEvent(event); return;
    }
    event->accept();
}

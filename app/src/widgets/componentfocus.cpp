#include "componentfocus.h"
#include "motion.h"
#include "theme.h"
#include <QApplication>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>

class FocusLayer : public QWidget {
public:
    explicit FocusLayer(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus); setObjectName("componentFocusHalo");
        setGeometry(parent->rect());
        m_motion=new MotionValue(this,[this](qreal) { update(); });
        show(); raise();
    }
    void focused(bool value) { m_motion->to(value?1:0,isVisible()); }
    void stop() { m_motion->snap(0); }
protected:
    void paintEvent(QPaintEvent *) override {
        if (m_motion->value()<=0 || !parentWidget()->isEnabled()) return;
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        QColor color(Theme::accentLabel()); color.setAlphaF(0.22*m_motion->value());
        p.setBrush(Qt::NoBrush); p.setPen(QPen(color,4));
        p.drawRoundedRect(QRectF(rect()).adjusted(2,2,-2,-2),7,7);
    }
private:
    MotionValue *m_motion;
};

namespace {
QWidget *fieldHost(QWidget *widget) {
    if (!widget) return nullptr;
    if (qobject_cast<QLineEdit *>(widget)) {
        if (qobject_cast<QAbstractSpinBox *>(widget->parentWidget()) || qobject_cast<QComboBox *>(widget->parentWidget()))
            return widget->parentWidget();
        return widget;
    }
    if (qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QComboBox *>(widget) ||
        qobject_cast<QTextEdit *>(widget) || qobject_cast<QPlainTextEdit *>(widget)) return widget;
    return nullptr;
}
}
ComponentFocus::ComponentFocus(QObject *parent) : QObject(parent) { qApp->installEventFilter(this); }
bool ComponentFocus::eventFilter(QObject *watched,QEvent *event) {
    auto *widget=qobject_cast<QWidget *>(watched);
    if (!widget) return false;
    if (event->type()==QEvent::FocusIn || event->type()==QEvent::FocusOut) {
        auto *host=fieldHost(widget);
        if (!host) return false;
        if (!m_layers.contains(host)) {
            m_layers.insert(host,new FocusLayer(host));
            connect(host,&QObject::destroyed,this,[this,host] { m_layers.remove(host); });
        }
        const QPointer<FocusLayer> layer=m_layers.value(host);
        if (event->type()==QEvent::FocusIn) { layer->raise(); layer->focused(true); }
        else QTimer::singleShot(0,layer,[layer,host] {
            if (!layer) return;
            auto *focus=QApplication::focusWidget();
            layer->focused(focus && (focus==host || host->isAncestorOf(focus)));
        });
    } else if (m_layers.contains(widget)) {
        if (auto layer=m_layers.value(widget)) {
            if (event->type()==QEvent::Resize) layer->setGeometry(widget->rect());
            else if (event->type()==QEvent::Hide) layer->stop();
        }
    }
    return false;
}

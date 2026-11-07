#pragma once
#include <QObject>
#include <QHash>
#include <QPointer>
class QWidget;
class FocusLayer;

// A paint-only focus halo: it never takes focus or intercepts input.
class ComponentFocus : public QObject {
public:
    explicit ComponentFocus(QObject *parent);
protected:
    bool eventFilter(QObject *watched,QEvent *event) override;
private:
    QHash<QWidget *,QPointer<FocusLayer>> m_layers;
};

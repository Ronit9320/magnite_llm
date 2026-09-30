#pragma once
#include <QObject>

// Adds visual feedback without intercepting native button or keyboard actions.
class ClickFeedback : public QObject {
public:
    explicit ClickFeedback(QObject *parent);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

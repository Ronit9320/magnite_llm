#pragma once

#include <QComboBox>

// Native combo interaction with a consistent list popup and theme-aware chevron.
class ComboBox : public QComboBox {
public:
    explicit ComboBox(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};

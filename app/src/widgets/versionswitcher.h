#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

// "‹ 2 / 3 ›": steps between the versions of an edited message or a retried reply.
// Hides itself when there is only one version.
class VersionSwitcher : public QWidget {
    Q_OBJECT

public:
    explicit VersionSwitcher(QWidget *parent = nullptr);

    // `index` is 0-based. The previous arrow is off at the first version, the next at the last.
    void setVersion(int index, int total);
    // false turns both arrows off whatever the position (while a reply is streaming, for example).
    void setEnabledForUse(bool enabled);

signals:
    void step(int delta);   // -1 or +1

private:
    void refresh();

    QPushButton *m_prev;
    QPushButton *m_next;
    QLabel *m_label;
    int m_index = 0;
    int m_total = 1;
    bool m_enabled = true;
};

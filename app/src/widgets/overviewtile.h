#pragma once

#include <QPushButton>
#include <QString>

class QLabel;

// A compact live-state tile for the workbench home panel (design doc §12).
// Shows a numbered kicker, a value drawn from live backend state and a caption;
// clicking the tile deep-links to the workspace it represents (the panel index
// is forwarded as `activated`).
class OverviewTile : public QPushButton {
    Q_OBJECT

public:
    OverviewTile(int panel, const QString &kicker, const QString &value,
                 const QString &caption, QWidget *parent = nullptr);

    int panel() const { return m_panel; }
    QString tone() const { return m_tone; }

    // `tone`: "" neutral, "live" for an active/ready state (accent cue in QSS).
    void setState(const QString &value, const QString &caption,
                  const QString &tone = QString());

signals:
    void activated(int panel);

private:
    int m_panel;
    QString m_tone;
    QLabel *m_value;
    QLabel *m_caption;
};

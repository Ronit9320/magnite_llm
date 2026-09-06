#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QVBoxLayout;

// Abstract base class for all main panels.
// Provides a consistent title header and a persisted-settings pathway.
class BasePanel : public QWidget {
    Q_OBJECT

public:
    explicit BasePanel(const QString &title, QWidget *parent = nullptr);

    QString title() const { return m_title; }

    // Implementations may return a JSON object describing their persistent
    // settings. Default implementation returns an empty object.
    virtual QString settingsJson() const { return QString(); }

    // Called when settings should be (re)applied from the given JSON.
    virtual void applySettingsJson(const QString &) {}

protected:
    QVBoxLayout *contentLayout() const { return m_contentLayout; }
    void setHeaderSubtitle(const QString &subtitle);

private:
    QString m_title;
    QVBoxLayout *m_contentLayout;
    QLabel *m_subtitleLabel;
};

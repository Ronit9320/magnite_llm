#pragma once

#include "widgets/floatingpanel.h"

#include <QStringList>

class Button;
class QButtonGroup;
class QHBoxLayout;
class QLabel;

// The Blueprints editor's top bar: back, name and revision, Draft and fixture pills, the Build · Test cases ·
// Run history segmented control, live validation, the revision actions, a ⋯ menu and Run ▾. It only reports
// clicks; BlueprintPanel owns every action. In Build it floats over the canvas.
class BlueprintTopBar : public FloatingPanel {
    Q_OBJECT
public:
    enum class View { Build, TestCases, RunHistory };

    explicit BlueprintTopBar(QWidget *parent = nullptr);

    void setView(View view);  // selects a segment without emitting viewChanged
    View view() const { return m_view; }
    void setBlueprint(const QString &name, const QString &revisionText, bool draft, bool fixture);
    // "Ready to run", or the issue count; the list is the button's tooltip.
    void setValidation(const QStringList &issues);
    void setActions(bool canPublish, bool canReload, bool canOpenInConversation, bool canGoBackToConversation);
    // Publish and Reload need the local service.
    void setConnected(bool connected);
    void setRunEnabled(bool enabled);
    // False while showing a run whose blueprint is not open: only Test cases, no blueprint actions.
    void setEditable(bool editable);
    void setFullscreen(bool fullscreen);

    // The bar never asks its parent layout for more than the room it is given; it folds instead.
    QSize minimumSizeHint() const override;

signals:
    void backClicked();
    void viewChanged(BlueprintTopBar::View view);
    // index is the first issue, or -1 when there are none (the panel then checks readiness).
    void issueClicked(int index);
    void publishClicked();
    void reloadClicked();
    void openInConversationClicked();
    void runClicked();
    void runMenuRequested(const QPoint &globalPosition);
    void editDetailsClicked();
    void duplicateClicked();
    void deleteClicked();
    void backToConversationClicked();
    void fullscreenClicked();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void showMoreMenu();
    void refreshIcons();
    // Folds the least important controls (validation text, Reload, Open in Conversation, the Run label)
    // until the bar fits its width.
    void fit();

    View m_view = View::Build;
    QHBoxLayout *m_row;
    Button *m_back;
    QWidget *m_title;
    QLabel *m_name;
    QLabel *m_meta;
    QLabel *m_draft;
    QLabel *m_fixture;
    QButtonGroup *m_segments;
    Button *m_build;
    Button *m_tests;
    Button *m_history;
    Button *m_validation;
    Button *m_publish;
    Button *m_reload;
    Button *m_open;
    Button *m_fullscreen;
    Button *m_more;
    Button *m_run;
    Button *m_runMenu;
    QString m_validationText;
    int m_issues = 0;
    bool m_canReload = false;
    bool m_canOpen = false;
    bool m_canGoBack = false;
    bool m_editable = true;
    bool m_isFullscreen = false;
    bool m_runEnabled = true;
    bool m_fitting = false;
    int m_folded = 0;  // how many fold steps fit() applied
};

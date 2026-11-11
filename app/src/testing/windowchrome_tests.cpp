#include "testing/windowchrome_tests.h"
#include "widgets/windowchrome.h"
#include "widgets/button.h"
#include "widgets/settingswindow.h"
#include "theme.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QVBoxLayout>
#include <cstdio>

int runWindowChromeChecks(const QString &captures) {
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) ++failures;
        fprintf(stderr, "[window-check] %s %s\n", ok ? "ok:" : "FAIL:", name);
    };
    auto pump = [] {
        QElapsedTimer timer;
        timer.start();
        do { QApplication::processEvents(); QThread::msleep(2); } while (timer.elapsed() < 60);
    };
    const bool originalDark = Theme::isDark();
    QMainWindow main;
    auto *body = new QWidget(&main);
    auto *layout = new QVBoxLayout(body);
    auto *masthead = new QFrame(body);
    masthead->setFixedHeight(52);
    new QHBoxLayout(masthead);
    layout->addWidget(masthead);
    layout->addWidget(new QLabel("Workspace", body));
    main.setCentralWidget(body);
    WindowChrome::attachMainWindow(&main, masthead);
    main.resize(700, 450);
    main.show(); pump();
    check(main.windowFlags().testFlag(Qt::FramelessWindowHint), "main frame omits system decoration");
    check(main.contentsMargins().top() == 0 && masthead->height() == 52,
          "main controls reuse the masthead without adding a title strip");
    auto *maximize = main.findChild<Button *>("windowMaximize");
    maximize->click(); pump();
    check(main.isMaximized() && maximize->accessibleName() == "Restore window", "maximize updates state and accessible action");
    maximize->click(); pump();
    check(!main.isMaximized() && maximize->accessibleName() == "Maximize window", "restore returns the normal window");
    main.findChild<Button *>("windowMinimize")->click(); pump();
    // xdg-shell does not report a minimized state back to Qt:
    // https://github.com/qt/qtwayland/blob/6.8/src/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp
    if (!QApplication::platformName().startsWith("wayland"))
        check(main.isMinimized(), "minimize control minimizes the window");
    else
        fprintf(stderr, "[window-check] info: minimized state is verified offscreen; Wayland does not report it\n");
    main.showNormal(); pump();
    const QPointF headerPoint(masthead->rect().center());
    const QPointF screenPoint(masthead->mapToGlobal(headerPoint.toPoint()));
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, headerPoint, screenPoint,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(masthead, &doubleClick); pump();
    check(main.isMaximized(), "double-clicking the masthead maximizes the window");
    maximize->click(); pump();
    const QPointF edge(1, main.height() / 2);
    QMouseEvent edgeHover(QEvent::MouseMove, edge, main.mapToGlobal(edge.toPoint()),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&main, &edgeHover);
    check(main.cursor().shape() == Qt::SizeHorCursor, "window edges expose the resize cursor");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&main, &leave);
    check(main.cursor().shape() == Qt::ArrowCursor, "leaving a resize edge restores the cursor");

    for (bool dark : {false, true}) {
        Theme::setDark(dark);
        QDialog dialog(&main);
        dialog.setWindowTitle("Training graph");
        auto *content = new QVBoxLayout(&dialog);
        auto *reading = new QLabel("Measured training values", &dialog);
        content->addWidget(reading);
        dialog.resize(480, 300);
        dialog.show(); pump();
        auto *header = dialog.findChild<QWidget *>("appWindowHeader");
        check(header && dialog.windowFlags().testFlag(Qt::FramelessWindowHint), "dialogs use a compact app-owned header");
        check(header && reading->geometry().top() >= header->geometry().bottom(), "dialog contents stay below window controls");
        dialog.setWindowTitle("Updated graph");
        check(dialog.findChild<QLabel *>("appWindowTitle")->text() == "Updated graph", "dialog title follows window title changes");
        dialog.resize(640, 360); pump();
        check(header->width() == dialog.width() && header->height() == 36, "dialog header tracks resizing");
        dialog.hide(); dialog.show(); pump();
        check(dialog.findChildren<QWidget *>("appWindowHeader").size() == 1 && dialog.contentsMargins().top() == 36,
              "reopening a dialog does not duplicate controls or padding");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            check(dialog.grab().save(captures + (dark ? "/window-dialog-dark.png" : "/window-dialog-light.png")), "capture themed dialog");
        }
        dialog.findChild<Button *>("windowClose")->click(); pump();
        check(!dialog.isVisible() && dialog.result() == QDialog::Rejected, "close rejects a dialog safely");
        dialog.show(); pump();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&dialog, &escape);
        check(!dialog.isVisible(), "Escape still dismisses dialogs");
    }
    QMessageBox confirm(QMessageBox::Question, "Delete item", "Remove this item?", QMessageBox::Yes | QMessageBox::Cancel, &main);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.show(); pump();
    check(confirm.findChild<Button *>("windowClose") && confirm.windowFlags().testFlag(Qt::FramelessWindowHint), "message boxes share the app controls");
    confirm.findChild<Button *>("windowClose")->click(); pump();
    check(confirm.result() == QMessageBox::Cancel, "closing a confirmation preserves cancellation");
    QFileDialog files(&main, "Choose a file");
    files.show(); pump();
    check(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs) && files.findChild<Button *>("windowClose"),
          "file pickers use the same compact app frame");
    files.reject();
    SettingsDetailDialog settings(&main);
    new QVBoxLayout(&settings);
    settings.show(); pump();
    check(!settings.findChild<QWidget *>("appWindowHeader"), "settings keeps its existing controls without a duplicate header");
    settings.reject();
    main.findChild<Button *>("windowClose")->click(); pump();
    check(!main.isVisible(), "main close control closes the workspace");
    Theme::setDark(originalDark);
    fprintf(stderr, "[window-check] %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}

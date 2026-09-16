#include <QApplication>
#include "mainwindow.h"
#include "theme.h"

#include <QDir>
#include <QFont>
#include <QSettings>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Magnite");
    QCoreApplication::setOrganizationDomain("magnite.local");
    QCoreApplication::setApplicationName("Magnite LLM");

    // Phase 5.5.0 infrastructure check: validates the theme tokens, the
    // embedded qrc assets (theme/fonts/icons) and the registered font
    // families, then exits. No window is shown.
    if (argc > 1 && QString(argv[1]) == "--theme-check")
        return Theme::selfCheck();

    // Phase 5.5.1: bundled fonts + global stylesheet active, default typeface
    // set to DM Sans (falls back to the system font if DM Sans can't load).
    Theme::setupApplicationTheme();
    QFont appFont = app.font();
    appFont.setFamily(Theme::bodyFont());
    app.setFont(appFont);

    MainWindow window;
    window.show();

    // Dev tool: capture every panel to <dir>/panel-<n>.png, then exit.
    if (argc > 2 && QString(argv[1]) == "--screenshot") {
        QTimer::singleShot(400, &window, [&window, dir = QString(argv[2])]() {
            window.captureScreenshots(dir);
        });
    }

    if (argc > 1 && QString(argv[1]) == "--selftest") {
        window.setSelfTestMode(true);
        // Redirect settings to a throwaway file so selftests never touch real config.
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QDir::tempPath());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QTimer::singleShot(2000, &window, &MainWindow::runSelfTest);
    }

    return app.exec();
}
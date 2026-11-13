#include <QApplication>
#include "mainwindow.h"
#include "theme.h"
#include "testing/restart_tests.h"
#include "widgets/windowchrome.h"
#include "testing/uicheck.h"
#include "testing/windowchrome_tests.h"

#include <QDir>
#include <QFont>
#include <QSettings>
#include <QTimer>
#include <QFileInfo>
#include <QFile>
#include "widgets/confirmation.h"
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

int main(int argc, char *argv[]) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    WindowChrome::install(app);
    QCoreApplication::setOrganizationName("Magnite");
    QCoreApplication::setOrganizationDomain("magnite.local");
    QCoreApplication::setApplicationName("Magnite LLM");
    QCoreApplication::setApplicationVersion(QStringLiteral(MAGNITE_BUILD_LABEL));

#ifdef Q_OS_LINUX
    // Coordinate all packaged instances and the independent update helper.
    // A stable lock beside the image survives atomic replacement of its inode.
    int installationLock = -1;
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")) {
        const auto lockPath = QFile::encodeName(QFileInfo(qEnvironmentVariable("APPIMAGE")).absoluteFilePath() + ".update-lock");
        installationLock = open(lockPath.constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (installationLock >= 0 && flock(installationLock, LOCK_EX | LOCK_NB) != 0) {
            close(installationLock);
            Theme::setupApplicationTheme();
            Ui::showNotice(nullptr, "Magnite", "This AppImage is already running or being updated.");
            return 1;
        }
        // A read-only location can still run; its in-app installer is disabled.
    }
#endif

    const bool selftest = argc > 1 && QString(argv[1]) == "--selftest";
    const bool uiCheck = argc > 1 && QString(argv[1]) == "--ui-check";
    const bool windowCheck = argc > 1 && QString(argv[1]) == "--window-check";
    if (selftest || uiCheck || windowCheck) {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                          QDir::tempPath() + "/magnite-selftest-" + QString::number(QCoreApplication::applicationPid()));
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

    // Phase 5.5.0 infrastructure check: validates the theme tokens, the
    // embedded qrc assets (theme/fonts/icons) and the registered font
    // families, then exits. No window is shown.
    if (argc > 1 && QString(argv[1]) == "--theme-check")
        return Theme::selfCheck();
    if (argc > 1 && QString(argv[1]) == "--restart-policy-check")
        return runRestartPolicyCheck();

    // Phase 5.5.1: bundled fonts + global stylesheet active, default typeface
    // set to DM Sans (falls back to the system font if DM Sans can't load).
    Theme::setupApplicationTheme();
    QFont appFont = app.font();
    appFont.setFamily(Theme::bodyFont());
    app.setFont(appFont);

    if (windowCheck) return runWindowChromeChecks(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString()) ? 1 : 0;
    if (uiCheck) return runUiCheck(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());

    MainWindow window;
    window.show();

    // Dev tool: capture every panel to <dir>/panel-<n>.png, then exit.
    if (argc > 2 && QString(argv[1]) == "--screenshot") {
        if (qEnvironmentVariableIsSet("MAGNITE_SCREENSHOT_COMPACT")) window.resize(1024,680);
        // MAGNITE_SCREENSHOT_DELAY_MS waits for the backend so panels capture live data.
        const int delay = qBound(400, qEnvironmentVariable("MAGNITE_SCREENSHOT_DELAY_MS", "400").toInt(), 120000);
        QTimer::singleShot(delay, &window, [&window, dir = QString(argv[2])]() {
            window.captureScreenshots(dir);
        });
    }

    if (selftest) {
        window.setSelfTestMode(true);
        QTimer::singleShot(2000, &window, &MainWindow::runSelfTest);
    }

    const int result = app.exec();
    // MainWindow and BackendManager must be destroyed before releasing the lock.
    // The process exit closes the descriptor after these automatic objects.
    return result;
}

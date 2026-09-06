#include <QApplication>
#include "mainwindow.h"

#include <QDir>
#include <QSettings>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Magnite");
    QCoreApplication::setOrganizationDomain("magnite.local");
    QCoreApplication::setApplicationName("Magnite LLM");

    MainWindow window;
    window.show();

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
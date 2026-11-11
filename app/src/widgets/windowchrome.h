#pragma once

class QApplication;
class QWidget;

// Install before creating windows. Dialogs receive compact app-owned controls;
// the main window can place its controls in the existing workspace masthead.
namespace WindowChrome {
void install(QApplication &application);
void attachMainWindow(QWidget *window, QWidget *masthead);
}

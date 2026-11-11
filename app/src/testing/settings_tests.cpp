#include "testing/settings_tests.h"
#include "panels/chatpanel.h"
#include "panels/chatstore.h"
#include "panels/settingspanel.h"
#include "theme.h"
#include "widgets/settingssummary.h"
#include "widgets/settingswindow.h"
#include "widgets/shortcuts.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>

namespace {
void pump(int ms = 50) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
}
void key(QWidget* target, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(target, &event);
}
void answer(const QString& text) {
    QTimer::singleShot(0, qApp, [text] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            for (auto* button : box->buttons())
                if (button->text() == text)
                    button->click();
    });
}
} // namespace
int runSettingsChecks(BackendClient* client, const QString& captures) {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok)
            ++failures;
        fprintf(stderr, "[settings-check] %s %s\n", ok ? "ok:" : "FAIL:", name);
    };
    {
        // Storage capacity bar: Models | Workspace | Other used | Free, all measured, never guessed.
        constexpr double GiB = 1073741824.0;
        auto size = [](double bytes) { return QLocale().formattedDataSize(qint64(bytes)); };
        auto fixture = [&](bool onVolume, bool complete, QJsonValue free, QJsonValue total, double workspace = 2 * GiB) {
            return QJsonObject{{"workspace", QJsonObject{{"bytes", workspace}, {"complete", complete}}},
                               {"models", QJsonObject{{"bytes", 6 * GiB}, {"complete", true}}},
                               {"combined_locations", false}, {"cache_on_data_volume", onVolume},
                               {"free_bytes", free}, {"disk_total_bytes", total}};
        };
        StorageSummary full;
        full.setMeasurements(fixture(true, true, 20 * GiB, 100 * GiB));
        check(full.segments() == QList<double>({6 * GiB, 2 * GiB, 72 * GiB, 20 * GiB}),
              "storage bar splits the disk into models, workspace, other used and free");
        check(full.freeText() == size(20 * GiB) + " of " + size(100 * GiB), "storage shows free space against disk capacity");
        check(full.otherText() == size(72 * GiB) && full.otherTitle().contains("Other used") && !full.otherHidden(),
              "storage shows what else uses the disk");
        StorageSummary elsewhere;
        elsewhere.setMeasurements(fixture(false, true, 20 * GiB, 100 * GiB));
        check(elsewhere.segments() == QList<double>({0, 2 * GiB, 78 * GiB, 20 * GiB}),
              "a model cache on another volume is not subtracted from this disk");
        StorageSummary partial;
        partial.setMeasurements(fixture(true, false, 20 * GiB, 100 * GiB));
        check(partial.otherTitle().contains("unscanned") && partial.otherText().startsWith("≤"),
              "an incomplete scan labels other used as an upper bound");
        StorageSummary unknown;
        unknown.setMeasurements(fixture(true, true, QJsonValue(), QJsonValue()));
        check(unknown.segments() == QList<double>({6 * GiB, 2 * GiB, 0, 0}) && unknown.freeText() == "Unknown" && unknown.otherHidden(),
              "unknown capacity falls back to the size split without claiming free space");
        StorageSummary racing;
        racing.setMeasurements(fixture(true, true, 95 * GiB, 100 * GiB));
        check(racing.segments()[2] == 0, "measured files larger than disk usage never produce negative other");
    }
    QSettings preferences;
    QMap<QString, QVariant> saved;
    for (const auto& key : preferences.allKeys())
        saved.insert(key, preferences.value(key));
    const bool wasDark = Theme::isDark();
    QTemporaryDir data;
    ChatStore store(data.path() + "/chats");
    ChatTree tree;
    auto user = tree.addChild(ChatTree::kRoot, "user", "hello");
    tree.addChild(user, "assistant", "original");
    tree.addChild(user, "assistant", "alternative");
    const QString id = store.create();
    check(store.save(id, tree), "prepare a branched chat");
    check(store.rename(id, "Custom export title"), "prepare chat metadata");
    QJsonObject exported;
    QString error;
    check(store.exportAll(&exported, &error), "export all chats succeeds");
    const auto record = exported["chats"].toArray().first().toObject();
    check(record["tree"].toObject() == tree.toJson() && record["title"] == "Custom export title" &&
              record["customTitle"] == true,
          "export preserves inactive branches and metadata");
    QFile broken(store.directory() + "/" + id + ".json");
    check(broken.open(QIODevice::WriteOnly), "open chat corruption fixture");
    broken.write("{broken");
    broken.close();
    check(!store.exportAll(&exported, &error) && !error.isEmpty(),
          "unreadable listed chats fail export instead of silently disappearing");
    check(store.save(id, tree), "restore chat fixture");
    ChatPanel chat;
    chat.setChatStore(&store);
    check(chat.openChat(id), "open the chat before testing bulk deletion");
    SettingsPanel panel;
    panel.setBackendClient(client);
    panel.setChatContext(&store, &chat);
    panel.setRuntimeLogs([] { return QString("Local diagnostic test message"); });
    QWidget workspace;
    workspace.resize(1280, 820);
    workspace.show();
    SettingsWindow window(&panel, &workspace);
    window.openSettings();
    pump();
    check(window.isModal() && window.isVisible() && panel.isVisible(),
          "settings opens as a modal native utility window");
    auto* pages = panel.findChild<QStackedWidget*>("settingsPages");
    check(pages && pages->count() == 8, "all eight reviewed categories exist");
    for (bool dark : {false, true}) {
        panel.findChild<QComboBox*>("settingsTheme")->setCurrentIndex(dark ? 1 : 0);
        Theme::setDark(dark);
        for (int category = 0; category < 8; ++category) {
            panel.selectCategory(category);
            pump(180);
            auto* scroll = qobject_cast<QScrollArea*>(pages->currentWidget());
            check(scroll && scroll->horizontalScrollBar()->maximum() == 0,
                  "category fits without horizontal scrolling");
            if (!captures.isEmpty()) {
                QDir().mkpath(captures);
                check(
                    window.grab().save(
                        captures +
                        QString("/settings-%1-%2.png").arg(dark ? "dark" : "light").arg(category)),
                    "capture native settings window");
            }
        }
    }
    for (bool dark : {false, true}) {
        panel.findChild<QComboBox*>("settingsTheme")->setCurrentIndex(dark ? 1 : 0);
        panel.selectCategory(0);
        QTimer::singleShot(0, qApp, [&, dark] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog && dialog->objectName() == "settingsDetailDialog" &&
                      dialog->windowFlags().testFlag(Qt::FramelessWindowHint),
                  "shortcuts uses the settings frame instead of a system title bar");
            if (dialog) {
                const auto frame = dialog->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                check(qAlpha(frame.pixel(8, frame.height() / 2)) == 255 &&
                          qAlpha(frame.pixel(frame.width() / 2, frame.height() / 2)) == 255,
                      "shortcut dialog surface is opaque in both themes");
                check(qAlpha(frame.pixel(0, 0)) == 0 &&
                          qAlpha(frame.pixel(frame.width() - 1, 0)) == 0,
                      "only rounded exterior corners remain transparent");
                QStringList keycaps;
                for (auto* key : dialog->findChildren<QLabel*>("settingsKeycap"))
                    keycaps << key->text();
                check(keycaps.contains(Shortcuts::settings().toString(QKeySequence::NativeText)) &&
                          keycaps.contains(Shortcuts::newChat().toString(QKeySequence::NativeText)),
                      "reference labels use the same native key sequences as activation");
#ifndef Q_OS_MAC
                check(!keycaps.join(' ').contains(QString::fromUtf8("⌘")),
                      "Windows and Linux shortcut labels omit the Mac Command symbol");
#endif
                if (!captures.isEmpty())
                    frame.save(captures + (dark ? "/settings-shortcuts-dark.png"
                                                : "/settings-shortcuts-light.png"));
                dialog->reject();
            }
        });
        panel.findChild<QPushButton*>("settingsShortcuts")->click();
    }
    panel.selectCategory(1);
    auto* size = panel.findChild<QComboBox*>("settingsInterfaceSize");
    const int defaultSize = panel.findChild<QLabel*>("settingsTitle")->font().pixelSize();
    size->setCurrentIndex(2);
    pump();
    check(preferences.value("appearance/interfaceSize").toInt() == 2 &&
              panel.findChild<QLabel*>("settingsTitle")->font().pixelSize() > defaultSize,
          "large interface size changes actual rendered text and persists");
    size->setCurrentIndex(1);
    auto* theme = panel.findChild<QComboBox*>("settingsTheme");
    theme->setCurrentIndex(0);
    check(!Theme::isDark(), "Light selection applies globally");
    theme->setCurrentIndex(1);
    check(Theme::isDark(), "Dark selection applies globally");
    auto* motion = panel.findChild<QCheckBox*>("reducedMotionToggle");
    motion->setChecked(true);
    panel.selectCategory(2);
    check(panel.findChild<QGraphicsOpacityEffect*>()->opacity() == 1,
          "reduced motion snaps the category transition");
    auto* send = panel.findChild<QComboBox*>("sendKeyCombo");
    send->setCurrentIndex(1);
    auto* input = chat.findChild<QPlainTextEdit*>("chatInput");
    input->setPlainText("hello");
    key(input, Qt::Key_Return);
    check(input->toPlainText().contains('\n'),
          "modified-send preference leaves plain Enter as a new line");
    input->setPlainText("hello");
    key(input, Qt::Key_Return, Qt::ControlModifier);
    check(!input->toPlainText().contains('\n'), "Ctrl/Command+Enter is consumed as a send action");
    panel.selectCategory(4);
    auto* remove = panel.findChild<QPushButton*>("deleteAllChats");
    answer("Cancel");
    remove->click();
    check(store.list().size() == 1, "cancelling bulk deletion preserves chats");
    answer("Delete all chats");
    remove->click();
    check(store.list().isEmpty() && chat.currentChatId().isEmpty(),
          "confirmed bulk deletion updates the store and current chat");
    preferences.setValue("storage/dataDir", data.path());
    preferences.setValue("updates/highestSequence", 88);
    panel.selectCategory(0);
    answer("Cancel");
    panel.findChild<QPushButton*>("resetPreferencesButton")->click();
    check(preferences.value("chat/sendKey") == "modified",
          "cancelling preference reset preserves values");
    answer("Reset preferences");
    panel.findChild<QPushButton*>("resetPreferencesButton")->click();
    check(!preferences.contains("chat/sendKey") &&
              preferences.value("storage/dataDir") == data.path() &&
              preferences.value("updates/highestSequence").toInt() == 88,
          "reset preserves storage and updater trust state");
    key(&window, Qt::Key_Escape);
    pump(220);
    check(!window.isVisible(), "Escape closes settings");
    window.openSettings();
    pump();
    check(window.isVisible(), "settings can be reopened");
    window.reject();
    pump(220);
    // The panel is stack-owned; detach before the QDialog is destroyed.
    panel.setParent(nullptr);
    panel.hide();
    preferences.clear();
    for (auto it = saved.begin(); it != saved.end(); ++it)
        preferences.setValue(it.key(), it.value());
    Theme::applyPreferences();
    Theme::setDark(wasDark);
    fprintf(stderr, "[settings-check] %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}

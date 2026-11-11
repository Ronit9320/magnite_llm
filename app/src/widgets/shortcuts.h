#pragma once
#include <QKeySequence>

// Use these sequences for both activation and platform-native reference text.
namespace Shortcuts {
inline QKeySequence settings() {
    return QKeySequence(Qt::CTRL | Qt::Key_Comma);
}
inline QKeySequence newChat() {
    return QKeySequence(QKeySequence::New);
}
inline QKeySequence workspace(int index) {
    return QKeySequence(Qt::CTRL | (Qt::Key_1 + index));
}
inline QKeySequence collapseSidebar() {
    return QKeySequence(Qt::CTRL | Qt::Key_B);
}
inline QKeySequence modifiedSend() {
    return QKeySequence(Qt::CTRL | Qt::Key_Enter);
}
inline QKeySequence newLine() {
    return QKeySequence(Qt::SHIFT | Qt::Key_Enter);
}
inline QString label(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::NativeText);
}
} // namespace Shortcuts

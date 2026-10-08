#pragma once

#include <QColor>
#include <QString>

class QIcon;

// Studio 03 design tokens (design doc §13). This is the single source of
// truth for every color, font, radius and spacing value in the UI. Panels and
// the global QSS (Theme::qss) read tokens from here — never hardcode hex
// literals in widget code.
namespace Theme {

bool isDark();
void setDark(bool dark); // Apply immediately; persistence is owned by the switch.

// ---- Paper, ink, orange and violet palette ----
inline QString bgVoid()        { return isDark() ? QStringLiteral("#161719") : QStringLiteral("#F1EFE7"); }
inline QString bgDeep()        { return isDark() ? QStringLiteral("#111214") : QStringLiteral("#E9E6DC"); }
// Keep existing semantic names for consumers; the secondary accent is lavender.
inline QString brandOrange() { return QStringLiteral("#FF4A0E"); }
inline QString brandInk() { return QStringLiteral("#201411"); }
inline QString brandText() { return isDark() ? QStringLiteral("#F1EFE7") : QStringLiteral("#1B1311"); }
inline QString accentPrimary() { return QStringLiteral("#FF5C28"); }
inline QString accentBlue()    { return isDark() ? QStringLiteral("#B4A7FF") : QStringLiteral("#5145B7"); }
inline QString accentIce()    { return QStringLiteral("#FF5C28"); }
// Derived accent shades used by QSS interaction states (hover/pressed) and the
// light-on-accent text/thumb fill. Centralized so the QSS never hardcodes hex.
inline QString accentHover()   { return QStringLiteral("#FF784E"); }
inline QString accentActive()  { return QStringLiteral("#E1491B"); }
inline QString accentLabel()   { return isDark() ? QStringLiteral("#FF784E") : QStringLiteral("#B83D16"); }
inline QString accentText()    { return QStringLiteral("#201D18"); }
// Destructive-button hover + text-on-danger fills (status-error family).
inline QString dangerHover()   { return isDark() ? QStringLiteral("#512D25") : QStringLiteral("#FFD0BD"); }

inline QString glassFill()          { return isDark() ? QStringLiteral("#202225") : QStringLiteral("#FFFEF8"); }
inline QString glassFillHover()     { return isDark() ? QStringLiteral("#2A2D31") : QStringLiteral("#E5E8D5"); }
inline QString glassFillActive()    { return isDark() ? QStringLiteral("#34383D") : QStringLiteral("#E5E8D5"); }
inline QString glassBorder()        { return isDark() ? QStringLiteral("#383C42") : QStringLiteral("#CFD2C5"); }
inline QString glassBorderHover()   { return QStringLiteral("#FF784E"); }

inline QString textPrimary()   { return isDark() ? QStringLiteral("#F1EFE7") : QStringLiteral("#20221F"); }
inline QString textSecondary() { return isDark() ? QStringLiteral("#B6BAC2") : QStringLiteral("#555D54"); }
inline QString textMuted()     { return isDark() ? QStringLiteral("#888F9A") : QStringLiteral("#62685F"); }

inline QString statusSuccess() { return isDark() ? QStringLiteral("#8ED5AD") : QStringLiteral("#276749"); }
inline QString statusWarning() { return isDark() ? QStringLiteral("#E8C478") : QStringLiteral("#855C14"); }
inline QString statusError()   { return isDark() ? QStringLiteral("#FF9E82") : QStringLiteral("#B13D24"); }

// ---- Typography (design doc §3) ----
inline QString headingFont() { return QStringLiteral("Space Grotesk"); }
inline QString bodyFont()    { return QStringLiteral("DM Sans"); }
// Mono is a system-family fallback resolved at runtime (nothing bundled).
QString monoFont();

// ---- Layout & spacing (design doc §4) ----
inline int spacingBase()           { return 6; }
inline int panelPadding()          { return 16; }
inline int minWindowWidth()        { return 1024; }
inline int minWindowHeight()       { return 680; }
inline int cornerRadiusSmall()     { return 8; }

// ---- Runtime helpers ----

// Bind the Token <-> qss() substitution map. Every @token in theme.qss is
// replaced here; unknown tokens would survive into the parsed stylesheet and
// are caught by selfCheck().
QString qss();

// Load the bundled OFL fonts into QFontDatabase (silent no-op if a file is
// missing), then install qss() as the app-wide stylesheet. Called from main().
void setupApplicationTheme();

// Load the bundled OFL fonts (design doc §3 typography). Safe to call twice.
void loadFonts();

// A stroke-tinted Lucide-style line icon rendered from a bundled SVG. The SVG
// must use stroke="currentColor"; we substitute the requested color so a single
// source icon works for every state (accent-primary active / text-secondary idle).
QIcon coloredIcon(const QString &lucideName, const QColor &color, int size = 18);

// Theme verification: checks active design tokens, the embedded qrc assets
// and the registered font families to stderr. Returns the number of failures
// (0 = the infrastructure is sound). Exits via main()'s --theme-check flag.
int selfCheck();

}  // namespace Theme

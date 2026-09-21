#pragma once

#include <QColor>
#include <QString>

class QIcon;

// Studio design tokens (design doc §11). This is the single source of
// truth for every color, font, radius and spacing value in the UI. Panels and
// the global QSS (Theme::qss) read tokens from here — never hardcode hex
// literals in widget code.
namespace Theme {

// ---- Graphite, mint, cream and lavender palette ----
inline QString bgVoid()        { return QStringLiteral("#101716"); }
inline QString bgDeep()        { return QStringLiteral("#151E1C"); }
// Keep existing semantic names for consumers; the secondary accent is lavender.
inline QString accentPrimary() { return QStringLiteral("#BEF4CF"); }
inline QString accentBlue()    { return QStringLiteral("#B6AEF2"); }
inline QString accentGlow()    { return QStringLiteral("rgba(190, 244, 207, 0.18)"); }
inline QString accentIce()    { return QStringLiteral("#BEF4CF"); }
// Derived accent shades used by QSS interaction states (hover/pressed) and the
// light-on-accent text/thumb fill. Centralized so the QSS never hardcodes hex.
inline QString accentHover()   { return QStringLiteral("#D8FFE4"); }
inline QString accentActive()  { return QStringLiteral("#98DCAE"); }
inline QString accentText()    { return QStringLiteral("#152A1C"); }
// Destructive-button hover + text-on-danger fills (status-error family).
inline QString dangerHover()   { return QStringLiteral("#FFD0BD"); }
inline QString onDanger()      { return QStringLiteral("#301F19"); }

inline QString glassFill()          { return QStringLiteral("#1B2623"); }
inline QString glassFillHover()     { return QStringLiteral("#273832"); }
inline QString glassFillActive()    { return QStringLiteral("#273832"); }
inline QString glassBorder()        { return QStringLiteral("#33453D"); }
inline QString glassBorderHover()   { return QStringLiteral("#D8FFE4"); }
inline QString glassBorderActive()  { return QStringLiteral("#BEF4CF"); }

inline QString textPrimary()   { return QStringLiteral("#F4F1E8"); }
inline QString textSecondary() { return QStringLiteral("#B6C5BB"); }
inline QString textMuted()     { return QStringLiteral("#80988A"); }

inline QString statusSuccess() { return QStringLiteral("#BEF4CF"); }
inline QString statusWarning() { return QStringLiteral("#EBC98C"); }
inline QString statusError()   { return QStringLiteral("#F3AF98"); }
inline QString statusInfo()    { return QStringLiteral("#B6AEF2"); }

// ---- Typography (design doc §3) ----
inline QString headingFont() { return QStringLiteral("Space Grotesk"); }
inline QString bodyFont()    { return QStringLiteral("DM Sans"); }
// Mono is a system-family fallback resolved at runtime (nothing bundled).
QString monoFont();

// Font pixel sizes (design doc §3 scale).
inline int fontH1()       { return 28; }
inline int fontH2()       { return 20; }
inline int fontH3()       { return 16; }
inline int fontBody()     { return 14; }
inline int fontBodySm()   { return 12; }
inline int fontMono()     { return 13; }

// Heading weights (design doc §3).
inline QString fontWeightSemibold() { return QStringLiteral("600"); }
inline QString fontWeightMedium()    { return QStringLiteral("500"); }

// ---- Layout & spacing (design doc §4) ----
inline int spacingBase()           { return 8; }
inline int panelPadding()          { return 32; }
inline int sectionGap()            { return 24; }
inline int formGap()               { return 16; }
inline int sidebarWidthExpanded()  { return 240; }
inline int sidebarWidthCollapsed() { return 64; }
inline int minWindowWidth()        { return 1024; }
inline int minWindowHeight()       { return 680; }
inline int cornerRadius()          { return 12; }
inline int cornerRadiusSmall()     { return 8; }

// ---- Runtime helpers ----

// Bind the Token <-> qss() substitution map. Every @token in theme.qss is
// replaced here; unknown tokens would survive into the parsed stylesheet and
// are caught by selfCheck().
QString qss();

// Load the bundled OFL fonts into QFontDatabase (silent no-op if a file is
// missing), then install qss() as the app-wide stylesheet. Called once from
// main() in Phase 5.5.1; not part of Phase 5.5.0's visible scope.
void setupApplicationTheme();

// Load the bundled OFL fonts (design doc §3 typography). Safe to call twice.
void loadFonts();

// A stroke-tinted Lucide-style line icon rendered from a bundled SVG. The SVG
// must use stroke="currentColor"; we substitute the requested color so a single
// source icon works for every state (accent-primary active / text-secondary idle).
QIcon coloredIcon(const QString &lucideName, const QColor &color, int size = 18);

// Phase 5.5.0 verification: prints every design token, the embedded qrc assets
// and the registered font families to stderr. Returns the number of failures
// (0 = the infrastructure is sound). Exits via main()'s --theme-check flag.
int selfCheck();

}  // namespace Theme

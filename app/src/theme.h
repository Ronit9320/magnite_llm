#pragma once

#include <QColor>
#include <QString>

class QIcon;
class QPixmap;

// Current design tokens. This is the single source of
// truth for every color, font, radius and spacing value in the UI. Panels and
// the global QSS (Theme::qss) read tokens from here — never hardcode hex
// literals in widget code.
namespace Theme {

bool isDark();
// animate crossfades open windows when light/dark changes (user actions; never at startup).
void applyPreferences(bool animate = false);
inline QString previewPaper() { return QStringLiteral("#FFFEF8"); }
inline QString previewGraphite() { return QStringLiteral("#111113"); }
void setDark(bool dark, bool animate = false); // Persistence is owned by the switch.
inline int motionThemeMs() { return 380; }   // light/dark crossfade

// ---- Paper, graphite and orange palette ----
inline QString canvasBackground()    { return isDark() ? QStringLiteral("#09090B") : QStringLiteral("#F1EFE7"); }
inline QString insetBackground()     { return isDark() ? QStringLiteral("#050506") : QStringLiteral("#E9E6DC"); }
// Supplied brand colors.
inline QString brandOrange() { return QStringLiteral("#FF4A0E"); }
inline QString brandInk() { return QStringLiteral("#201411"); }
inline QString brandText() { return isDark() ? QStringLiteral("#F1EFE7") : QStringLiteral("#1B1311"); }
inline QString accentPrimary() { return QStringLiteral("#FF5C28"); }
// Derived accent shades used by QSS interaction states (hover/pressed) and the
// light-on-accent text/thumb fill. Centralized so the QSS never hardcodes hex.
inline QString accentHover()   { return QStringLiteral("#FF784E"); }
inline QString accentActive()  { return QStringLiteral("#E1491B"); }
inline QString accentLabel()   { return isDark() ? QStringLiteral("#FF784E") : QStringLiteral("#B83D16"); }
inline QString accentText()    { return QStringLiteral("#201D18"); }
// Destructive-button hover + text-on-danger fills (status-error family).
inline QString dangerHover()   { return isDark() ? QStringLiteral("#512D25") : QStringLiteral("#FFD0BD"); }

inline QString surfaceFill()         { return isDark() ? QStringLiteral("#111113") : QStringLiteral("#FFFEF8"); }
inline QString surfaceFillHover()    { return isDark() ? QStringLiteral("#1A1A1D") : QStringLiteral("#E5E8D5"); }
inline QString surfaceFillActive()   { return isDark() ? QStringLiteral("#222225") : QStringLiteral("#E5E8D5"); }
inline QString surfaceBorder()       { return isDark() ? QStringLiteral("#36363C") : QStringLiteral("#C4C8B9"); }
// Fields and buttons have no outline: their fill sets them apart, and focus draws an accent ring.
inline QString fieldFill()           { return isDark() ? QStringLiteral("#1A1A1D") : QStringLiteral("#E8E6DB"); }
inline QString fieldFillHover()      { return isDark() ? QStringLiteral("#222225") : QStringLiteral("#DEDCD0"); }
// Scroll handles and other drawn strokes that must reach 3:1 against their fill (WCAG 1.4.11).
inline QString controlBorder()       { return isDark() ? QStringLiteral("#63636B") : QStringLiteral("#878C7C"); }

// Unfilled part of the context meter ring and bar.
inline QString meterTrack()          { return surfaceBorder(); }

inline QString textPrimary()   { return isDark() ? QStringLiteral("#F1EFE7") : QStringLiteral("#20221F"); }
inline QString textSecondary() { return isDark() ? QStringLiteral("#B6BAC2") : QStringLiteral("#555D54"); }
inline QString textMuted()     { return isDark() ? QStringLiteral("#888F9A") : QStringLiteral("#62685F"); }

inline QString statusSuccess() { return isDark() ? QStringLiteral("#8ED5AD") : QStringLiteral("#276749"); }
inline QString statusWarning() { return isDark() ? QStringLiteral("#E8C478") : QStringLiteral("#855C14"); }
inline QString statusError()   { return isDark() ? QStringLiteral("#FF9E82") : QStringLiteral("#B13D24"); }
inline QString comparisonSeries() { return isDark() ? QStringLiteral("#85B9F4") : QStringLiteral("#245B92"); }

// ---- Blueprints editor: wire (port type) colours, node family colours, floating panels ----
inline QString blueprintTypeText() { return isDark() ? QStringLiteral("#85B9F4") : QStringLiteral("#245B92"); }
inline QString blueprintTypeNumber() { return isDark() ? QStringLiteral("#7FD8E0") : QStringLiteral("#0B6B75"); }
inline QString blueprintTypeBoolean() { return isDark() ? QStringLiteral("#FF9E82") : QStringLiteral("#B13D24"); }
inline QString blueprintTypeObject() { return isDark() ? QStringLiteral("#C3A6FF") : QStringLiteral("#6B47B8"); }
inline QString blueprintTypeArray() { return isDark() ? QStringLiteral("#8ED5AD") : QStringLiteral("#276749"); }
inline QString blueprintTypeReceipt() { return isDark() ? QStringLiteral("#B6BAC2") : QStringLiteral("#5B6370"); }
inline QString blueprintTypeAdapter() { return isDark() ? QStringLiteral("#E8C478") : QStringLiteral("#855C14"); }
inline QString blueprintFamilySources() { return isDark() ? QStringLiteral("#6FCF97") : QStringLiteral("#2E7D4F"); }
inline QString blueprintFamilyTransform() { return isDark() ? QStringLiteral("#9AA3B2") : QStringLiteral("#5B6370"); }
inline QString blueprintFamilyKnowledge() { return isDark() ? QStringLiteral("#85B9F4") : QStringLiteral("#245B92"); }
inline QString blueprintFamilyModels() { return isDark() ? QStringLiteral("#FF784E") : QStringLiteral("#B8420F"); }
inline QString blueprintFamilyTools() { return isDark() ? QStringLiteral("#C3A6FF") : QStringLiteral("#6B47B8"); }
inline QString blueprintFamilyOutput() { return isDark() ? QStringLiteral("#8ED5AD") : QStringLiteral("#276749"); }
inline QString blueprintPanelFill() { return isDark() ? QStringLiteral("#111113") : QStringLiteral("#FFFEF8"); }
// Soft drop shadow colour for floating panels, #AARRGGBB.
inline QString blueprintPanelShadow() { return isDark() ? QStringLiteral("#73000000") : QStringLiteral("#33281E14"); }
inline QString blueprintCanvasDot() { return isDark() ? QStringLiteral("#1A1A1C") : QStringLiteral("#D3CFC0"); }

// Graph readouts stay dark and compact in both appearances.
inline QString chartHoverFill() { return brandInk(); }
inline QString chartHoverText() { return isDark() ? textPrimary() : canvasBackground(); }

// Subtle accent surface for callouts and selected Hub controls.
inline QString accentSurface() {
    const QColor base(surfaceFill()), accent(accentPrimary());
    return QColor::fromRgbF(base.redF() * 0.92 + accent.redF() * 0.08,
                           base.greenF() * 0.92 + accent.greenF() * 0.08,
                           base.blueF() * 0.92 + accent.blueF() * 0.08).name();
}

// Tinted surface for inline warning banners (service reconnecting, etc.).
inline QString warningSurface() {
    const QColor base(surfaceFill()), warning(statusWarning());
    return QColor::fromRgbF(base.redF() * 0.9 + warning.redF() * 0.1,
                           base.greenF() * 0.9 + warning.greenF() * 0.1,
                           base.blueF() * 0.9 + warning.blueF() * 0.1).name();
}

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
inline int motionFastMs() { return 140; }
inline int motionStandardMs() { return 180; }
inline int motionScrollMs() { return 220; }   // base wheel-glide duration
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

// The pixmap behind coloredIcon(): `size` logical pixels square, backed by
// size*dpr device pixels. Exposed with an explicit ratio so HiDPI rendering can
// be verified on any machine (the offscreen test platform always reports 1x).
QPixmap iconPixmap(const QString &lucideName, const QColor &color, int size, qreal dpr);

// Theme verification: checks active design tokens, the embedded qrc assets
// and the registered font families to stderr. Returns the number of failures
// (0 = the infrastructure is sound). Exits via main()'s --theme-check flag.
int selfCheck();

}  // namespace Theme

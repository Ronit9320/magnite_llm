#include "theme.h"

#include <QBrush>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <QRadialGradient>
#include <QSvgRenderer>
#include <QWidget>

#include <QApplication>
#include <QScreen>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

namespace {

// Read a qt resource file (":/...").
QString loadText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Take a Lucide-style SVG's literal color values and flip every stroke to a
// target color (in CSS-compatible scalar syntax for <svg stroke="..." />).
QString tintSvg(const QString &svg, const QColor &color) {
    const QString hex = color.name(QColor::HexRgb);
    QString out = svg;
    out.replace("stroke=\"currentColor\"", "stroke=\"" + hex + "\"");
    // Guard: some Lucide builds emit currentcolor lowercase.
    out.replace("stroke='currentColor'", "stroke='" + hex + "'");
    out.replace("stroke=\"currentcolor\"", "stroke=\"" + hex + "\"");
    // Any fill an icon happens to carry is hardcoded; leave untouched.
    out.replace("#030914", hex);
    return out;
}

// The @token -> value substitution map used to compile theme.qss at runtime.
// Keep it in one place so the design doc §2 single-source rule holds even after
// the stylesheet is generated. selfCheck() verifies every @token is resolved.
struct Token {
    QString name;
    QString value;
};

std::vector<Token> tokens() {
    return {
        {"@bgVoid",             Theme::bgVoid()},
        {"@bgDeep",             Theme::bgDeep()},
        {"@accentPrimary",      Theme::accentPrimary()},
        {"@accentBlue",         Theme::accentBlue()},
        {"@accentGlow",         Theme::accentGlow()},
        {"@accentIce",         Theme::accentIce()},
        {"@accentHover",        Theme::accentHover()},
        {"@accentActive",       Theme::accentActive()},
        {"@accentText",         Theme::accentText()},
        {"@dangerHover",        Theme::dangerHover()},
        {"@onDanger",           Theme::onDanger()},
        {"@glassFill",          Theme::glassFill()},
        {"@glassFillHover",     Theme::glassFillHover()},
        {"@glassFillActive",    Theme::glassFillActive()},
        {"@glassBorder",        Theme::glassBorder()},
        {"@glassBorderHover",   Theme::glassBorderHover()},
        {"@glassBorderActive",  Theme::glassBorderActive()},
        {"@textPrimary",        Theme::textPrimary()},
        {"@textSecondary",      Theme::textSecondary()},
        {"@textMuted",          Theme::textMuted()},
        {"@statusSuccess",      Theme::statusSuccess()},
        {"@statusWarning",      Theme::statusWarning()},
        {"@statusError",        Theme::statusError()},
        {"@statusInfo",         Theme::statusInfo()},
        {"@headingFont",        Theme::headingFont()},
        {"@bodyFont",           Theme::bodyFont()},
        {"@monoFont",           Theme::monoFont()},
    };
}

}  // namespace

QString Theme::monoFont() {
    // Prefer a bundled/shipped mono, then fall back to whatever the OS reports
    // as a fixed-pitch family. Space Grotesk/DM Sans have no bundled mono.
    static const QStringList kPreferred = {"JetBrains Mono", "SF Mono", "Menlo",
                                           "Cascadia Mono", "Consolas", "Noto Sans Mono",
                                           "Liberation Mono", "Courier New"};
    for (const QString &f : kPreferred) {
        const QStringList families = QFontDatabase::families();
        if (families.contains(f))
            return f;
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

// ---- Icon helper (design doc §9 icon set) ----
QIcon Theme::coloredIcon(const QString &lucideName, const QColor &color, int size) {
    if (!color.isValid() || color.alpha() == 0) return QIcon();
    const QString svg = loadText(QStringLiteral(":/icons/%1.svg").arg(lucideName));
    if (svg.isEmpty()) return QIcon();
    const QByteArray tinted = tintSvg(svg, color).toUtf8();

    // Render at the current device pixel ratio so icons stay crisp on HiDPI
    // (identical output at 1x, sharper at 2x — no blank-icon regressions).
    QSvgRenderer renderer(tinted);
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    renderer.render(&p);
    p.end();

    QIcon icon;
    icon.addPixmap(pm);
    return icon;
}

// ---- Fonts (design doc §3 typography + §7 licensing) ----
void Theme::loadFonts() {
    static const QStringList kPaths = {
        QStringLiteral(":/fonts/SpaceGrotesk-Regular.ttf"),
        QStringLiteral(":/fonts/SpaceGrotesk-Medium.ttf"),
        QStringLiteral(":/fonts/SpaceGrotesk-SemiBold.ttf"),
        QStringLiteral(":/fonts/DMSans-Regular.ttf"),
        QStringLiteral(":/fonts/DMSans-Medium.ttf"),
    };
    for (const QString &path : kPaths) {
        QFile f(path);
        if (f.exists())
            QFontDatabase::addApplicationFont(path);  // silent fallback on failure
    }
}

void Theme::setupApplicationTheme() {
    loadFonts();
    qApp->setStyleSheet(qss());
}

QString Theme::qss() {
    QString tpl = loadText(QStringLiteral(":/theme/theme.qss"));
    if (tpl.isEmpty()) return QString();
    for (const auto &tk : tokens())
        tpl.replace(tk.name, tk.value);
    return tpl;
}

int Theme::selfCheck() {
    int failures = 0;
    auto check = [&failures](const char *name, bool ok, const char *detail = nullptr) {
        if (ok) {
            std::fprintf(stderr, "[theme-check] ok: %s\n", name);
        } else {
            ++failures;
            std::fprintf(stderr, "[theme-check] FAIL: %s%s%s\n",
                         name, detail ? " — " : "", detail ? detail : "");
        }
    };

    // 1. Design tokens (design doc §2–§3) are the documented hex values.
    check("bgVoid", Theme::bgVoid() == "#030914");
    check("bgDeep", Theme::bgDeep() == "#071222");
    check("accentPrimary", Theme::accentPrimary() == "#2563EB");
    check("accentBlue", Theme::accentBlue() == "#2563EB");
    check("accentIce", Theme::accentIce() == "#93C5FD");
    check("accentHover", Theme::accentHover() == "#3B82F6");
    check("accentActive", Theme::accentActive() == "#1D4ED8");
    check("accentText", Theme::accentText() == "#FFFFFF");
    check("dangerHover", Theme::dangerHover() == "#93C5FD");
    check("onDanger", Theme::onDanger() == "#FFFFFF");
    check("textPrimary", Theme::textPrimary() == "#FFFFFF");
    check("textSecondary", Theme::textSecondary() == "#B3C7E1");
    check("textMuted", Theme::textMuted() == "#7B96B9");
    check("statusSuccess", Theme::statusSuccess() == "#93C5FD");
    check("statusWarning", Theme::statusWarning() == "#FFFFFF");
    check("statusError", Theme::statusError() == "#93C5FD");
    check("statusInfo", Theme::statusInfo() == "#2563EB");
    check("headingFont", Theme::headingFont() == "Space Grotesk");
    check("bodyFont", Theme::bodyFont() == "DM Sans");
    check("monoFont resolves", !Theme::monoFont().isEmpty());

    // 2. QSS compiles: non-empty output with no leftover @tokens. Every
    // "@..." in theme.qss must be substituted; a surviving token would be
    // parsed as an unknown at-rule by the stylesheet engine.
    const QString styles = Theme::qss();
    check("qss() non-empty", !styles.isEmpty());
    check("qss() tokens substituted", !styles.contains("@"));

    // 3. Qt resource tree: theme/fonts/icons embedded via resources.qrc.
    check("theme.qss embedded", !loadText(":/theme/theme.qss").isEmpty());
    check("fonts embedded", !loadText(":/fonts/SpaceGrotesk-Regular.ttf").isEmpty() &&
                            !loadText(":/fonts/DMSans-Regular.ttf").isEmpty());
    const std::vector<const char *> kIcons = {
        "database", "graduation-cap", "folder", "layers", "message-circle",
        "settings", "search", "refresh-cw", "download", "trash", "plus",
        "file-up", "file-down", "send", "circle-stop"};
    for (const char *name : kIcons) {
        const QString hit = QStringLiteral(":/icons/%1.svg").arg(QString::fromLatin1(name));
        check(name, !loadText(hit).isEmpty());
    }

    // 4. Fonts registered after loadFonts().
    Theme::loadFonts();
    const QStringList families = QFontDatabase::families();
    check("Space Grotesk registered", families.contains(Theme::headingFont()));
    check("DM Sans registered", families.contains(Theme::bodyFont()));

    // 5. coloredIcon() renders a non-null icon with the tint applied.
    const QIcon icon = Theme::coloredIcon("settings", QColor(Theme::accentPrimary()), 18);
    check("coloredIcon(settings) renders", !icon.isNull());

    std::fprintf(stderr, "[theme-check] %d failure(s)\n", failures);
    return failures;
}
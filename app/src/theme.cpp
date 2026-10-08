#include "theme.h"

#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QWidget>

#include <QApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QPalette>
#include <QLabel>
#include <QAbstractButton>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

namespace {
bool darkMode = false;

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
    out.replace("#F1EFE7", hex);
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
        {"@brandOrange", Theme::brandOrange()},
        {"@brandInk", Theme::brandInk()},
        {"@brandText", Theme::brandText()},
        {"@bgVoid",             Theme::bgVoid()},
        {"@bgDeep",             Theme::bgDeep()},
        {"@accentLabel",        Theme::accentLabel()},
        {"@accentPrimary",      Theme::accentPrimary()},
        {"@accentBlue",         Theme::accentBlue()},
        {"@accentIce",         Theme::accentIce()},
        {"@accentHover",        Theme::accentHover()},
        {"@accentActive",       Theme::accentActive()},
        {"@accentText",         Theme::accentText()},
        {"@dangerHover",        Theme::dangerHover()},
        {"@glassFill",          Theme::glassFill()},
        {"@glassFillHover",     Theme::glassFillHover()},
        {"@glassFillActive",    Theme::glassFillActive()},
        {"@glassBorder",        Theme::glassBorder()},
        {"@glassBorderHover",   Theme::glassBorderHover()},
        {"@textPrimary",        Theme::textPrimary()},
        {"@textSecondary",      Theme::textSecondary()},
        {"@textMuted",          Theme::textMuted()},
        {"@statusSuccess",      Theme::statusSuccess()},
        {"@statusWarning",      Theme::statusWarning()},
        {"@statusError",        Theme::statusError()},
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
    // Start the refined dark-first design in dark mode once, then remember the switch.
    QSettings appearance;
    if (!appearance.value("appearance/darkRefinementApplied", false).toBool()) {
        appearance.setValue("appearance/studio3Dark", true);
        appearance.setValue("appearance/darkRefinementApplied", true);
    }
    setDark(appearance.value("appearance/studio3Dark", true).toBool());
}

bool Theme::isDark() { return darkMode; }

void Theme::setDark(bool dark) {
    darkMode = dark;
    QPalette palette = qApp->palette();
    palette.setColor(QPalette::Window, QColor(bgVoid()));
    palette.setColor(QPalette::Base, QColor(glassFill()));
    palette.setColor(QPalette::AlternateBase, QColor(bgDeep()));
    palette.setColor(QPalette::Button, QColor(glassFill()));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(role, QColor(textPrimary()));
    palette.setColor(QPalette::PlaceholderText, QColor(textMuted()));
    palette.setColor(QPalette::Highlight, QColor(accentPrimary()));
    palette.setColor(QPalette::HighlightedText, QColor(accentText()));
    qApp->setPalette(palette);
    qApp->setStyleSheet(qss());
    for (auto *widget : QApplication::allWidgets()) {
        if (auto *label = qobject_cast<QLabel *>(widget);
            label && label->property("themeIcon").isValid())
            label->setPixmap(coloredIcon(label->property("themeIcon").toString(),
                                        QColor(textSecondary()),14).pixmap(14,14));
        if (auto *button = qobject_cast<QAbstractButton *>(widget);
            button && button->property("themeIcon").isValid())
            button->setIcon(coloredIcon(button->property("themeIcon").toString(),
                QColor(button->property("themeIconTone")=="error" ? statusError() : textSecondary())));
        widget->update();
    }
}

QString Theme::qss() {
    QString tpl = loadText(QStringLiteral(":/theme/theme.qss"));
    if (tpl.isEmpty()) return QString();
    // Replace longer token names first so @glassFill is not consumed inside
    // @glassFillHover or @glassFillActive.
    auto replacements = tokens();
    std::sort(replacements.begin(), replacements.end(), [](const Token &a, const Token &b) {
        return a.name.size() > b.name.size();
    });
    for (const auto &tk : replacements)
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
    check("bgVoid", Theme::bgVoid() == "#F1EFE7");
    check("bgDeep", Theme::bgDeep() == "#E9E6DC");
    check("accentLabel", QColor(Theme::accentLabel()).isValid());
    check("accentPrimary", Theme::accentPrimary() == "#FF5C28");
    check("accentBlue", Theme::accentBlue() == "#5145B7");
    check("accentIce", Theme::accentIce() == "#FF5C28");
    check("accentHover", Theme::accentHover() == "#FF784E");
    check("accentActive", Theme::accentActive() == "#E1491B");
    check("accentText", Theme::accentText() == "#201D18");
    check("dangerHover", Theme::dangerHover() == "#FFD0BD");
    check("textPrimary", Theme::textPrimary() == "#20221F");
    check("textSecondary", Theme::textSecondary() == "#555D54");
    check("textMuted", Theme::textMuted() == "#62685F");
    check("statusSuccess", Theme::statusSuccess() == "#276749");
    check("statusWarning", Theme::statusWarning() == "#855C14");
    check("statusError", Theme::statusError() == "#B13D24");
    check("headingFont", Theme::headingFont() == "Space Grotesk");
    check("bodyFont", Theme::bodyFont() == "DM Sans");
    check("monoFont resolves", !Theme::monoFont().isEmpty());

    check("brand orange", Theme::brandOrange() == "#FF4A0E");
    check("brand ink", Theme::brandInk() == "#201411");
    check("brand text valid", QColor(Theme::brandText()).isValid());

    // 2. QSS compiles: non-empty output with no leftover @tokens. Every
    // "@..." in theme.qss must be substituted; a surviving token would be
    // parsed as an unknown at-rule by the stylesheet engine.
    const QString styles = Theme::qss();
    check("qss() non-empty", !styles.isEmpty());
    check("qss() tokens substituted", !styles.contains("@"));
    // Prefix collisions used to leave strings like #0C1B30Hover behind.
    // Validate the resulting color literals, not only the absence of tokens.
    const QRegularExpression colorLiteral(QStringLiteral("#[A-Za-z0-9]+"));
    auto matches = colorLiteral.globalMatch(styles);
    bool validColors = true;
    while (matches.hasNext()) {
        const QString value = matches.next().captured();
        // QSS object selectors are also #names; only inspect digit-leading
        // or hexadecimal-looking words with a six-digit color prefix.
        if (QRegularExpression("^#[0-9A-Fa-f]{6}").match(value).hasMatch())
            validColors = validColors && QColor(value).isValid();
    }
    check("qss() color literals valid", validColors);

    // 3. Qt resource tree: theme/fonts/icons embedded via resources.qrc.
    check("theme.qss embedded", !loadText(":/theme/theme.qss").isEmpty());
    check("fonts embedded", !loadText(":/fonts/SpaceGrotesk-Regular.ttf").isEmpty() &&
                            !loadText(":/fonts/DMSans-Regular.ttf").isEmpty());
    const std::vector<const char *> kIcons = {
        "database", "graduation-cap", "folder", "layers", "message-circle",
        "settings", "search", "refresh-cw", "download", "trash", "plus",
        "file-up", "file-down", "send", "circle-stop", "chevron-up", "chevron-down"};
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

    const bool originalDark = isDark();
    setDark(true);
    check("refined dark canvas", bgVoid() == "#161719");
    check("refined dark surface", glassFill() == "#202225");
    check("refined dark border", glassBorder() == "#383C42");
    check("dark palette uses dark surfaces", QColor(bgVoid()).lightness() < 40);
    check("dark palette uses light text", QColor(textPrimary()).lightness() > 220);
    check("dark stylesheet tokens substituted", !qss().contains('@'));
    bool validDark = true;
    for (const auto &token : tokens())
        if (token.value.startsWith('#')) validDark &= QColor(token.value).isValid();
    check("dark color tokens valid", validDark);
    // Verify actual text contrast in both appearances, including secondary copy.
    auto luminance = [](const QString &value) {
        const QColor c(value);
        auto linear = [](qreal channel) { return channel <= 0.04045 ? channel/12.92 : std::pow((channel+0.055)/1.055,2.4); };
        return 0.2126*linear(c.redF())+0.7152*linear(c.greenF())+0.0722*linear(c.blueF());
    };
    auto contrast = [&luminance](const QString &a,const QString &b) {
        const qreal x=luminance(a),y=luminance(b);
        return (qMax(x,y)+0.05)/(qMin(x,y)+0.05);
    };
    for (bool dark : {false,true}) {
        setDark(dark);
        bool readable=true;
        for(const auto &ink:{textPrimary(),textSecondary(),textMuted(),statusSuccess(),statusError(),statusWarning(),accentLabel()})
            for(const auto &surface:{bgVoid(),glassFill()}) readable &= contrast(ink,surface)>=4.5;
        check(dark ? "dark text contrast >= 4.5" : "light text contrast >= 4.5",readable);
        check("primary button text contrast >= 4.5",contrast(accentText(),accentPrimary())>=4.5);
    }
    setDark(originalDark);
    std::fprintf(stderr, "[theme-check] %d failure(s)\n", failures);
    return failures;
}

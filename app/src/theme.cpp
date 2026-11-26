#include "widgets/wheelguard.h"
#include "theme.h"
#include "systemappearance.h"

#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QImage>
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
#include <QStyleHints>
#include <QStyle>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

namespace {
bool darkMode = false;
bool themeApplied = false;
bool systemDark = false;
bool startupPaletteDark = false;

// System mode: the desktop portal's stated preference, then Qt's colour scheme, then the startup palette.
bool detectSystemDark() {
    if (const auto portal = SystemAppearance::instance()->prefersDark()) return *portal;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = qApp->styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) return scheme == Qt::ColorScheme::Dark;
#endif
    return startupPaletteDark;
}

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
        {"@canvasBackground",     Theme::canvasBackground()},
        {"@blueprintPanelFill",   Theme::blueprintPanelFill()},
        {"@blueprintCanvasDot",   Theme::blueprintCanvasDot()},
        {"@insetBackground",      Theme::insetBackground()},
        {"@accentSurface", Theme::accentSurface()},
        {"@accentLabel",        Theme::accentLabel()},
        {"@accentPrimary",      Theme::accentPrimary()},
        {"@accentHover",        Theme::accentHover()},
        {"@accentActive",       Theme::accentActive()},
        {"@accentText",         Theme::accentText()},
        {"@dangerHover",        Theme::dangerHover()},
        {"@surfaceFill",          Theme::surfaceFill()},
        {"@surfaceFillHover",     Theme::surfaceFillHover()},
        {"@surfaceFillActive",    Theme::surfaceFillActive()},
        {"@surfaceBorder",        Theme::surfaceBorder()},
        {"@controlBorder",        Theme::controlBorder()},
        {"@fieldFillHover",       Theme::fieldFillHover()},
        {"@fieldFill",            Theme::fieldFill()},
        {"@warningSurface",       Theme::warningSurface()},
        {"@successSurface",       Theme::statusSurface(Theme::statusSuccess())},
        {"@errorSurface",         Theme::statusSurface(Theme::statusError())},
        {"@runningSurface",       Theme::statusSurface(Theme::comparisonSeries())},
        {"@textPrimary",        Theme::textPrimary()},
        {"@textSecondary",      Theme::textSecondary()},
        {"@textMuted",          Theme::textMuted()},
        {"@statusSuccess",      Theme::statusSuccess()},
        {"@comparisonSeries",   Theme::comparisonSeries()},
        {"@chartHoverFill",     Theme::chartHoverFill()},
        {"@chartHoverText",     Theme::chartHoverText()},
        {"@statusWarning",      Theme::statusWarning()},
        {"@statusError",        Theme::statusError()},
        {"@headingFont",        Theme::headingFont()},
        {"@bodyFont",           Theme::bodyFont()},
        {"@monoFont",           Theme::monoFont()},
    };
}

}  // namespace

QString Theme::monoFont() {
    // JetBrains Mono is bundled (see loadFonts); the rest only cover a stripped
    // resource build, then whatever the OS reports as a fixed-pitch family.
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
QPixmap Theme::iconPixmap(const QString &lucideName, const QColor &color, int size, qreal dpr) {
    if (!color.isValid() || color.alpha() == 0) return QPixmap();
    const QString svg = loadText(QStringLiteral(":/icons/%1.svg").arg(lucideName));
    if (svg.isEmpty()) return QPixmap();
    const QByteArray tinted = tintSvg(svg, color).toUtf8();

    QSvgRenderer renderer(tinted);
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    // A painter on a pixmap with a device pixel ratio already scales logical
    // units to device pixels. Without an explicit target, render() fills the
    // device-pixel viewport and the dpr scaling is applied twice, which on
    // Retina left only the top-left quarter of each glyph visible.
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();
    return pm;
}

QIcon Theme::coloredIcon(const QString &lucideName, const QColor &color, int size) {
    // Render at the current device pixel ratio so icons stay crisp on HiDPI.
    const QPixmap pm = iconPixmap(lucideName, color, size, qApp->devicePixelRatio());
    if (pm.isNull()) return QIcon();
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
        QStringLiteral(":/fonts/JetBrainsMono-Regular.ttf"),
        QStringLiteral(":/fonts/JetBrainsMono-Medium.ttf"),
    };
    for (const QString &path : kPaths) {
        QFile f(path);
        if (f.exists())
            QFontDatabase::addApplicationFont(path);  // silent fallback on failure
    }
}

void Theme::setupApplicationTheme() {
    loadFonts();
    WheelGuard::install();
    // Start the refined dark-first design in dark mode once, then remember the switch.
    QSettings appearance;
    if (!appearance.value("appearance/darkRefinementApplied", false).toBool()) {
        appearance.setValue("appearance/studio3Dark", true);
        appearance.setValue("appearance/darkRefinementApplied", true);
    }
    // The palette Qt starts with reflects the desktop; ours replaces it once a theme is applied.
    startupPaletteDark=qApp->palette().color(QPalette::Window).lightness()<128;
    systemDark=detectSystemDark();
    const auto followSystem=[] {
        const bool dark=detectSystemDark();
        if (dark==systemDark) return;
        systemDark=dark;
        if(QSettings().value("appearance/themeMode").toString()=="system") Theme::applyPreferences(true);
    };
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(qApp->styleHints(),&QStyleHints::colorSchemeChanged,qApp,followSystem);
#endif
    QObject::connect(SystemAppearance::instance(),&SystemAppearance::changed,qApp,followSystem);
    applyPreferences();
}

bool Theme::isDark() { return darkMode; }

namespace {
// A still of a window as it looked before the theme changed. It sits on top while the new
// stylesheet is applied underneath (the slow part), then fades away to reveal the new theme.
class ThemeCrossfade : public QWidget {
public:
    ThemeCrossfade(QWidget *window, const QPixmap &still) : QWidget(window), m_still(still) {
        setObjectName("themeCrossfade");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        setFocusPolicy(Qt::NoFocus);
        setGeometry(window->rect());
        m_fade.setDuration(Theme::motionThemeMs());
        m_fade.setEasingCurve(QEasingCurve::InOutQuad);
        m_fade.setStartValue(1.0);
        m_fade.setEndValue(0.0);
        QObject::connect(&m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_opacity = value.toReal();
            // Floating bars and overlays raise themselves as the new style lays them out; stay above them.
            if (parentWidget() && parentWidget()->children().constLast() != this) raise();
            update();
        });
        QObject::connect(&m_fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show();
        raise();
    }
    void start() { raise(); m_fade.start(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setOpacity(m_opacity);
        painter.drawPixmap(rect(), m_still);
    }

private:
    QPixmap m_still;
    QVariantAnimation m_fade;
    qreal m_opacity = 1.0;
};

bool crossfadeWanted(QWidget *window) {
    const auto type = window->windowType();
    return window->isVisible() && !window->isMinimized() && type != Qt::Popup && type != Qt::ToolTip &&
           type != Qt::SplashScreen && !window->size().isEmpty();
}
}  // namespace

void Theme::setDark(bool dark, bool animate) {
    if (themeApplied && darkMode==dark) return;
    QList<ThemeCrossfade *> fades;
    if (animate && themeApplied && darkMode != dark &&
        !QSettings().value("appearance/studio3ReducedMotion", false).toBool()) {
        for (auto *window : QApplication::topLevelWidgets()) {
            if (!crossfadeWanted(window)) continue;
            // A switch during a fade captures that fade too, so the change continues from what is on screen.
            const QPixmap still = window->grab();
            const auto previous = window->findChildren<QWidget *>("themeCrossfade", Qt::FindDirectChildrenOnly);
            fades << new ThemeCrossfade(window, still);
            qDeleteAll(previous);
        }
    }
    themeApplied=true;
    // Apply palette and stylesheet as one visual change, including open dialogs.
    QList<QWidget *> updatingWindows;
    for (auto *window : QApplication::topLevelWidgets()) if (window->updatesEnabled()) {
        window->setUpdatesEnabled(false); updatingWindows.append(window);
    }
    darkMode = dark;
    QPalette palette = qApp->palette();
    palette.setColor(QPalette::Window, QColor(canvasBackground()));
    palette.setColor(QPalette::Base, QColor(surfaceFill()));
    palette.setColor(QPalette::AlternateBase, QColor(insetBackground()));
    palette.setColor(QPalette::Button, QColor(surfaceFill()));
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
        {
            const int size = label->property("themeIconSize").toInt() > 0 ? label->property("themeIconSize").toInt() : 14;
            const QColor tone(label->property("themeIconTone") == "muted" ? textMuted() : textSecondary());
            label->setPixmap(iconPixmap(label->property("themeIcon").toString(), tone, size, label->devicePixelRatioF()));
        }
        if (auto *button = qobject_cast<QAbstractButton *>(widget);
            button && button->property("themeIcon").isValid())
            button->setIcon(coloredIcon(button->property("themeIcon").toString(),
                QColor(button->property("themeIconTone")=="error" ? statusError() : textSecondary())));
    }
    for (auto *window : updatingWindows) { window->setUpdatesEnabled(true); window->update(); }
    for (auto *fade : fades) fade->start();
}

void Theme::applyPreferences(bool animate) {
    QSettings settings;
    const auto mode=settings.value("appearance/themeMode").toString();
    const bool dark=mode=="system"?systemDark:mode=="light"?false:mode=="dark"?true:settings.value("appearance/studio3Dark",true).toBool();
    settings.setValue("appearance/studio3Dark",dark);
    // Re-apply even when light/dark is unchanged (interface size); only a real switch animates.
    const bool switching = themeApplied && darkMode != dark;
    themeApplied=false;
    if (switching && animate) {
        themeApplied=true;
        setDark(dark, true);
        return;
    }
    setDark(dark);
}

QString Theme::qss() {
    QString tpl = loadText(QStringLiteral(":/theme/theme.qss"));
    if (tpl.isEmpty()) return QString();
    // Replace longer token names first so @surfaceFill is not consumed inside
    // @surfaceFillHover or @surfaceFillActive.
    auto replacements = tokens();
    std::sort(replacements.begin(), replacements.end(), [](const Token &a, const Token &b) {
        return a.name.size() > b.name.size();
    });
    for (const auto &tk : replacements)
        tpl.replace(tk.name, tk.value);
    const int size=qBound(0,QSettings().value("appearance/interfaceSize",1).toInt(),2);
    const qreal scale=size==0?.92:size==2?1.14:1.;
    if(size!=1) {
        QRegularExpression expression("font-size:\\s*(\\d+)px");
        auto matches=expression.globalMatch(tpl);
        QList<QRegularExpressionMatch> replacements;
        while(matches.hasNext()) replacements.prepend(matches.next());
        for(const auto &match:replacements) tpl.replace(match.capturedStart(),match.capturedLength(),QString("font-size: %1px").arg(qRound(match.captured(1).toInt()*scale)));
    }
    return tpl;
}

static double contrastRatio(const QString &a, const QString &b) {
    auto luminance = [](const QColor &c) {
        auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
    };
    const double la = luminance(QColor(a)), lb = luminance(QColor(b));
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
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
    check("canvasBackground", Theme::canvasBackground() == "#F1EFE7");
    check("insetBackground", Theme::insetBackground() == "#E9E6DC");
    check("accentSurface", QColor(Theme::accentSurface()).isValid() && Theme::accentSurface() != Theme::surfaceFill());
    check("accentLabel", QColor(Theme::accentLabel()).isValid());
    check("warningSurface keeps warning text readable", contrastRatio(Theme::statusWarning(), Theme::warningSurface()) >= 4.5 && contrastRatio(Theme::textPrimary(), Theme::warningSurface()) >= 4.5);
    check("accentPrimary", Theme::accentPrimary() == "#FF5C28");
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
    check("meterTrack valid", QColor(Theme::meterTrack()).isValid() && Theme::meterTrack() == Theme::surfaceBorder());
    check("motion timings", Theme::motionFastMs()>0 && Theme::motionFastMs()<=Theme::motionStandardMs() && Theme::motionStandardMs()<=200);
    check("preview palette",QColor(Theme::previewPaper()).isValid() && QColor(Theme::previewGraphite()).isValid());
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
                            !loadText(":/fonts/DMSans-Regular.ttf").isEmpty() &&
                            !loadText(":/fonts/JetBrainsMono-Regular.ttf").isEmpty());
    Theme::loadFonts();   // --theme-check runs before setupApplicationTheme()
    check("monoFont is bundled JetBrains Mono", Theme::monoFont() == "JetBrains Mono");
    check("light fields stand apart by fill", isDark() || (fieldFill() != surfaceFill() && fieldFill() != canvasBackground() && fieldFillHover() != fieldFill()));
    check("light focus ring reaches 3:1 on fields", isDark() || contrastRatio(accentLabel(), fieldFill()) >= 3.0);
    check("light control border reaches 3:1", isDark() || (contrastRatio(controlBorder(), surfaceFill()) >= 3.0 && contrastRatio(controlBorder(), canvasBackground()) >= 3.0));
    {
        // Type scale: every QSS font-size sits on the shared scale, never below 11px.
        static const QList<int> kScale = {11, 12, 13, 14, 16, 18, 20, 24, 28, 32, 36};
        const QString raw = loadText(QStringLiteral(":/theme/theme.qss"));
        QStringList offScale;
        auto sizes = QRegularExpression(QStringLiteral("font-size:\\s*(\\d+)px")).globalMatch(raw);
        while (sizes.hasNext()) {
            const int px = sizes.next().captured(1).toInt();
            if (!kScale.contains(px)) offScale << QString::number(px);
        }
        offScale.removeDuplicates();
        const QByteArray detail = offScale.join(", ").toUtf8();
        check("qss font sizes on type scale (min 11px)", offScale.isEmpty(), detail.constData());
        // DM Sans is bundled as Regular and Medium only: heavier body text would be synthesized.
        QStringList fauxBold;
        for (const QString &rule : raw.split(QLatin1Char('}'))) {
            const auto weight = QRegularExpression(QStringLiteral("font-weight:\\s*(\\d+|bold)")).match(rule);
            if (!weight.hasMatch()) continue;
            const bool heavy = weight.captured(1) == "bold" || weight.captured(1).toInt() > 500;
            if (heavy && !rule.contains(QStringLiteral("@headingFont")))
                fauxBold << rule.section(QLatin1Char('{'), 0, 0).trimmed().section(QLatin1Char('\n'), -1);
        }
        const QByteArray fauxDetail = fauxBold.join(", ").toUtf8();
        check("qss weights above 500 only on the heading font", fauxBold.isEmpty(), fauxDetail.constData());
    }
    const std::vector<const char *> kIcons = {
        "database", "graduation-cap", "folder", "layers", "message-circle",
        "settings", "search", "refresh-cw", "download", "trash", "plus",
        "file-up", "file-down", "send", "circle-stop", "chevron-up", "chevron-down",
        "pencil", "chevron-left", "chevron-right", "copy", "check", "expand", "shrink", "brain", "user", "info"};
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

    // 5b. Icons must render complete at every pixel ratio. Regression guard for
    // the Retina bug where the SVG was drawn at device-pixel size into a
    // dpr-scaled painter, leaving only the glyph's top-left corner visible.
    // The glyph's opaque bounding box (in logical px) must match the 1x render
    // and fill most of the icon box, centred, at 1x/2x/3x.
    {
        auto opaqueBounds = [](const QPixmap &pm) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            QRect box;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 16)
                        box = box.isNull() ? QRect(x, y, 1, 1) : box.united(QRect(x, y, 1, 1));
            return box;
        };
        // Overlap (IoU) of the glyph mask after scaling a hi-dpi render down to
        // the logical size. A cropped or enlarged glyph overlaps the 1x render
        // poorly; the same glyph at a higher resolution overlaps it well.
        auto maskIou = [](const QPixmap &hi, const QPixmap &lo, int logicalSize) {
            const QImage a = hi.toImage().scaled(logicalSize, logicalSize,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                 .convertToFormat(QImage::Format_ARGB32);
            const QImage b = lo.toImage().convertToFormat(QImage::Format_ARGB32);
            int inter = 0, uni = 0;
            for (int y = 0; y < logicalSize; ++y)
                for (int x = 0; x < logicalSize; ++x) {
                    const bool pa = qAlpha(a.pixel(x, y)) > 100, pb = qAlpha(b.pixel(x, y)) > 100;
                    inter += pa && pb;
                    uni += pa || pb;
                }
            return uni ? double(inter) / uni : 0.0;
        };
        const int logical = 18;
        const std::vector<const char *> kGlyphs = {
            "box", "graduation-cap", "folder", "layers", "message-circle",
            "settings", "database", "download", "play", "circle-stop", "brain"};
        for (const char *glyph : kGlyphs) {
            const QString name = QString::fromLatin1(glyph);
            const QPixmap base = Theme::iconPixmap(name, QColor(Theme::accentPrimary()), logical, 1.0);
            const QRectF ref = QRectF(opaqueBounds(base));
            check(qPrintable(name + " renders at 1x"), !ref.isNull() && ref.width() >= logical * 0.45
                                                      && ref.height() >= logical * 0.45
                                                      && ref.center().x() >= logical * 0.35
                                                      && ref.center().x() <= logical * 0.65
                                                      && ref.center().y() >= logical * 0.35
                                                      && ref.center().y() <= logical * 0.65);
            for (qreal dpr : {2.0, 3.0}) {
                const QPixmap pm = Theme::iconPixmap(name, QColor(Theme::accentPrimary()), logical, dpr);
                const int device = qRound(logical * dpr);
                const QRectF got = QRectF(opaqueBounds(pm));
                const QRectF logicalGot(got.x() / dpr, got.y() / dpr, got.width() / dpr, got.height() / dpr);
                const bool sized = pm.width() == device && pm.height() == device &&
                                   qFuzzyCompare(pm.devicePixelRatio(), dpr);
                const bool matches1x = !got.isNull() &&
                    std::abs(logicalGot.left() - ref.left()) <= 1.5 &&
                    std::abs(logicalGot.top() - ref.top()) <= 1.5 &&
                    std::abs(logicalGot.right() - ref.right()) <= 1.5 &&
                    std::abs(logicalGot.bottom() - ref.bottom()) <= 1.5;
                const double iou = maskIou(pm, base, logical);
                check(qPrintable(QStringLiteral("%1 at %2x matches the 1x render").arg(name).arg(dpr)), iou > 0.6);
                check(qPrintable(QStringLiteral("%1 pixmap at %2x has device size").arg(name).arg(dpr)), sized);
                check(qPrintable(QStringLiteral("%1 at %2x is not cropped/shifted").arg(name).arg(dpr)), matches1x);
            }
        }
        check("iconPixmap rejects unknown glyph", Theme::iconPixmap("no-such-icon", QColor("#fff"), 18, 2.0).isNull());
        check("iconPixmap rejects invalid colour", Theme::iconPixmap("box", QColor(), 18, 2.0).isNull());
        check("coloredIcon(unknown) is null", Theme::coloredIcon("no-such-icon", QColor("#fff")).isNull());
    }

    const bool originalDark = isDark();
    setDark(true);
    check("refined dark canvas", canvasBackground() == "#09090B");
    check("refined dark surface", surfaceFill() == "#111113");
    check("refined dark border", surfaceBorder() == "#36363C");
    check("dark control border reaches 3:1", contrastRatio(controlBorder(), surfaceFill()) >= 3.0 && contrastRatio(controlBorder(), canvasBackground()) >= 3.0);
    check("dark fields stand apart by fill", fieldFill() != surfaceFill() && fieldFill() != canvasBackground() && fieldFillHover() != fieldFill());
    check("dark focus ring reaches 3:1 on fields", contrastRatio(accentLabel(), fieldFill()) >= 3.0);
    check("dark palette uses dark surfaces", QColor(canvasBackground()).lightness() <= 12);
    check("dark surfaces retain elevation", QColor(insetBackground()).lightness()<QColor(canvasBackground()).lightness() && QColor(canvasBackground()).lightness()<QColor(surfaceFill()).lightness() && QColor(surfaceFill()).lightness()<QColor(surfaceFillHover()).lightness());
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
        for(const auto &ink:{textPrimary(),textSecondary(),textMuted(),statusSuccess(),statusError(),statusWarning(),accentLabel(),comparisonSeries()})
            for(const auto &surface:{canvasBackground(),surfaceFill()}) readable &= contrast(ink,surface)>=4.5;
        check(dark ? "dark text contrast >= 4.5" : "light text contrast >= 4.5",readable);
        check("primary button text contrast >= 4.5",contrast(accentText(),accentPrimary())>=4.5);
        check("chart hover colours valid",QColor(chartHoverFill()).isValid() && QColor(chartHoverText()).isValid());
        check("chart hover text contrast >= 4.5",contrast(chartHoverText(),chartHoverFill())>=4.5);
        // Blueprints editor tokens.
        bool editorValid=true, wiresVisible=true, headersReadable=true;
        for(const auto &value:{blueprintTypeText(),blueprintTypeNumber(),blueprintTypeBoolean(),blueprintTypeObject(),blueprintTypeArray(),blueprintTypeReceipt(),blueprintTypeAdapter(),
                               blueprintFamilySources(),blueprintFamilyTransform(),blueprintFamilyKnowledge(),blueprintFamilyModels(),blueprintFamilyTools(),blueprintFamilyOutput(),
                               blueprintPanelFill(),blueprintPanelShadow(),blueprintCanvasDot()})
            editorValid &= QColor(value).isValid();
        for(const auto &value:{blueprintTypeText(),blueprintTypeNumber(),blueprintTypeBoolean(),blueprintTypeObject(),blueprintTypeArray(),blueprintTypeReceipt(),blueprintTypeAdapter()})
            wiresVisible &= contrast(value,canvasBackground())>=3.0;
        for(const auto &family:{blueprintFamilySources(),blueprintFamilyTransform(),blueprintFamilyKnowledge(),blueprintFamilyModels(),blueprintFamilyTools(),blueprintFamilyOutput()}) {
            const QColor tint(family),base(blueprintPanelFill());
            const qreal alpha=0.22;  // BlueprintStyle::headerTintAlpha
            const QColor blended=QColor::fromRgbF(base.redF()+(tint.redF()-base.redF())*alpha,
                                                  base.greenF()+(tint.greenF()-base.greenF())*alpha,
                                                  base.blueF()+(tint.blueF()-base.blueF())*alpha);
            headersReadable &= contrast(textPrimary(),blended.name())>=4.5;
        }
        check(dark ? "dark Blueprints editor tokens valid" : "light Blueprints editor tokens valid",editorValid);
        check(dark ? "dark wire colours reach 3:1 on the canvas" : "light wire colours reach 3:1 on the canvas",wiresVisible);
        check(dark ? "dark node header text contrast >= 4.5" : "light node header text contrast >= 4.5",headersReadable);
        check("Blueprints canvas dot differs from the canvas",QColor(blueprintCanvasDot())!=QColor(canvasBackground()));
    }
    setDark(originalDark);
    std::fprintf(stderr, "[theme-check] %d failure(s)\n", failures);
    return failures;
}

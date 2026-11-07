#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>

class QWidget;
class QLabel;
class QPushButton;

// Pure threshold/parsing logic for the context meter, kept free of widgets so the
// UI check can exercise it directly.
namespace ContextMeterLogic {

enum class Level { Ok, Warn, Critical };
enum class Style { Ring, Bar, RingBanner, Off };

// Green below 70%, amber from 70% to 90%, red above 90%.
Level levelFor(double fill);
// True when auto-compact is on (70..90 in steps of 5) and the fill has reached it.
bool shouldCompact(double fill, int autoCompactPercent);

Style parseStyle(const QString &key);   // unknown or empty -> Ring
QString styleKey(Style style);          // "ring" | "bar" | "ringBanner" | "off"
int parseAutoCompact(const QVariant &stored);   // 0 (off) or 70..90 step 5; anything else -> 85

// "1,390 / 4,096 tokens", prefixed with "~" when the figure is an estimate.
// `reserved` is accepted for symmetry with setUsage(); the reply reserve is shown separately.
QString usageText(int used, int reserved, int limit, bool exact);

}  // namespace ContextMeterLogic

// Shows how full the model's context is. The three visual pieces live in different
// places of the chat layout, so the meter owns them as separate widgets and the
// panel places them: ring() beside Send, bar() under the composer, banner() above it.
// Only the pieces the chosen style needs are visible, and only once usage is known.
class ContextMeter : public QObject {
    Q_OBJECT

public:
    using Level = ContextMeterLogic::Level;
    using Style = ContextMeterLogic::Style;

    explicit ContextMeter(QObject *parent = nullptr);
    ~ContextMeter() override;

    QWidget *ring() const;
    QWidget *bar() const;
    QWidget *banner() const;

    void setStyle(Style style);
    Style style() const { return m_style; }
    // 0 = off, otherwise the auto-compact percentage; only changes the warning wording.
    void setAutoCompact(int percent);

    void setUsage(int used, int reserved, int limit, bool exact);
    void clearUsage();

    bool hasUsage() const { return m_hasUsage; }
    double fill() const;
    Level level() const;
    // Empty below 70%; "Reduce Response tokens" when the reserve alone fills the window.
    QString warningText() const;

signals:
    void newChatRequested();

private:
    void refresh();

    QPointer<QWidget> m_ring;
    QPointer<QWidget> m_bar;
    QPointer<QWidget> m_banner;
    QPointer<QLabel> m_barText;
    QPointer<QLabel> m_bannerText;
    Style m_style = Style::Ring;
    int m_autoCompact = 85;
    bool m_hasUsage = false;
    bool m_exact = true;
    int m_used = 0;
    int m_reserved = 0;
    int m_limit = 0;
};

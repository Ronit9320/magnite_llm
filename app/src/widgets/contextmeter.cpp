#include "widgets/contextmeter.h"

#include "theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include "widgets/button.h"
#include <QtGlobal>

namespace ContextMeterLogic {

Level levelFor(double fill) {
    if (fill < 0.70) return Level::Ok;
    if (fill <= 0.90) return Level::Warn;
    return Level::Critical;
}

bool shouldCompact(double fill, int autoCompactPercent) {
    if (autoCompactPercent < 70 || autoCompactPercent > 90 || autoCompactPercent % 5 != 0) return false;
    return fill >= autoCompactPercent / 100.0;
}

Style parseStyle(const QString &key) {
    if (key == "bar") return Style::Bar;
    if (key == "ringBanner") return Style::RingBanner;
    if (key == "off") return Style::Off;
    return Style::Ring;
}

QString styleKey(Style style) {
    switch (style) {
    case Style::Bar: return "bar";
    case Style::RingBanner: return "ringBanner";
    case Style::Off: return "off";
    case Style::Ring: break;
    }
    return "ring";
}

int parseAutoCompact(const QVariant &stored) {
    bool ok = false;
    const int value = stored.toInt(&ok);
    if (ok && (value == 0 || (value >= 70 && value <= 90 && value % 5 == 0))) return value;
    return 85;
}

static QString grouped(int value) { return QLocale(QLocale::English).toString(value); }

QString usageText(int used, int reserved, int limit, bool exact) {
    Q_UNUSED(reserved);
    return QString("%1%2 / %3 tokens").arg(exact ? "" : "~", grouped(used), grouped(limit));
}

}  // namespace ContextMeterLogic

namespace {

using ContextMeterLogic::Level;

QColor levelColor(Level level) {
    switch (level) {
    case Level::Warn: return QColor(Theme::statusWarning());
    case Level::Critical: return QColor(Theme::statusError());
    case Level::Ok: break;
    }
    return QColor(Theme::statusSuccess());
}

// Fill indicator shared by ring and bar: paints from the current theme every time.
class FillWidget : public QWidget {
public:
    FillWidget(bool ring, QWidget *parent = nullptr) : QWidget(parent), m_ring(ring) {
        if (ring) setFixedSize(22, 22);
        else setFixedHeight(4);
    }
    void setFill(double fill, Level level) { m_fill = qBound(0.0, fill, 1.0); m_level = level; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor track(Theme::meterTrack());
        const QColor color = levelColor(m_level);
        if (m_ring) {
            const QRectF box = QRectF(rect()).adjusted(2, 2, -2, -2);
            QPen pen(track, 3);
            pen.setCapStyle(Qt::FlatCap);
            p.setPen(pen);
            p.drawEllipse(box);
            pen.setColor(color);
            p.setPen(pen);
            p.drawArc(box, 90 * 16, -qRound(m_fill * 360 * 16));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(track);
            p.drawRoundedRect(rect(), 2, 2);
            p.setBrush(color);
            p.drawRoundedRect(QRectF(0, 0, width() * m_fill, height()), 2, 2);
        }
    }

private:
    bool m_ring;
    double m_fill = 0.0;
    Level m_level = Level::Ok;
};

class BannerFrame : public QFrame {
public:
    using QFrame::QFrame;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor border(Theme::statusError());
        QColor fill = border;
        fill.setAlpha(36);
        p.setPen(border);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Theme::cornerRadiusSmall(), Theme::cornerRadiusSmall());
    }
};

}  // namespace

ContextMeter::ContextMeter(QObject *parent) : QObject(parent) {
    m_ring = new FillWidget(true);
    m_ring->setObjectName("contextRing");

    m_bar = new QWidget;
    m_bar->setObjectName("contextBar");
    auto *barLayout = new QHBoxLayout(m_bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(Theme::spacingBase() * 2);
    auto *track = new FillWidget(false, m_bar);
    track->setObjectName("contextBarTrack");
    barLayout->addWidget(track, 1, Qt::AlignVCenter);
    m_barText = new QLabel(m_bar);
    m_barText->setObjectName("contextBarText");
    barLayout->addWidget(m_barText);

    auto *banner = new BannerFrame;
    banner->setObjectName("contextBanner");
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(12, 8, 12, 8);
    m_bannerText = new QLabel(banner);
    m_bannerText->setObjectName("contextBannerText");
    m_bannerText->setWordWrap(true);
    bannerLayout->addWidget(m_bannerText, 1);
    auto *newChat = new Button("Start new chat", banner);
    newChat->setObjectName("contextBannerNewChat");
    connect(newChat, &QPushButton::clicked, this, &ContextMeter::newChatRequested);
    bannerLayout->addWidget(newChat);
    m_banner = banner;

    refresh();
}

ContextMeter::~ContextMeter() {
    // Pieces placed in a layout belong to their parent now; delete only the ones never placed.
    for (const QPointer<QWidget> &widget : {m_ring, m_bar, m_banner})
        if (widget && !widget->parentWidget()) delete widget;
}

QWidget *ContextMeter::ring() const { return m_ring; }
QWidget *ContextMeter::bar() const { return m_bar; }
QWidget *ContextMeter::banner() const { return m_banner; }

void ContextMeter::setStyle(Style style) {
    m_style = style;
    refresh();
}

void ContextMeter::setAutoCompact(int percent) {
    m_autoCompact = percent;
    refresh();
}

void ContextMeter::setUsage(int used, int reserved, int limit, bool exact) {
    if (limit <= 0) {
        clearUsage();
        return;
    }
    m_used = used;
    m_reserved = reserved;
    m_limit = limit;
    m_exact = exact;
    m_hasUsage = true;
    refresh();
}

void ContextMeter::clearUsage() {
    m_hasUsage = false;
    m_used = m_reserved = m_limit = 0;
    refresh();
}

double ContextMeter::fill() const {
    return m_hasUsage && m_limit > 0 ? double(m_used + m_reserved) / m_limit : 0.0;
}

ContextMeter::Level ContextMeter::level() const { return ContextMeterLogic::levelFor(fill()); }

QString ContextMeter::warningText() const {
    if (!m_hasUsage) return QString();
    if (m_reserved >= m_limit) return "Reduce Response tokens";
    switch (level()) {
    case Level::Ok: return QString();
    case Level::Warn: return "Context is getting full.";
    case Level::Critical:
        return m_autoCompact > 0 ? "Context is almost full. Older messages will be compacted when you send."
                                 : "Context is almost full. The next message may fail.";
    }
    return QString();
}

void ContextMeter::refresh() {
    const bool ringShown = m_hasUsage && (m_style == Style::Ring || m_style == Style::RingBanner);
    const bool barShown = m_hasUsage && m_style == Style::Bar;
    const bool bannerShown = m_hasUsage && m_style == Style::RingBanner && level() == Level::Critical;

    if (m_hasUsage) {
        const double f = fill();
        const Level lv = level();
        static_cast<FillWidget *>(m_ring.data())->setFill(f, lv);
        if (auto *track = m_bar->findChild<QWidget *>("contextBarTrack")) static_cast<FillWidget *>(track)->setFill(f, lv);
        const QString estimate = m_exact ? QString() : QStringLiteral("~");
        m_ring->setToolTip(QString("%1%2% of context used\n%1%3 of %4 tokens\n%1%5 reserved for the reply")
                               .arg(estimate, QString::number(qRound(f * 100)),
                                    QLocale(QLocale::English).toString(m_used),
                                    QLocale(QLocale::English).toString(m_limit),
                                    QLocale(QLocale::English).toString(m_reserved)));
        m_barText->setText(ContextMeterLogic::usageText(m_used, m_reserved, m_limit, m_exact) +
                           QString(" · %1 reserved for reply").arg(QLocale(QLocale::English).toString(m_reserved)));
        m_bannerText->setText(warningText());
    } else {
        m_ring->setToolTip(QString());
        m_barText->clear();
        m_bannerText->clear();
    }
    m_ring->setVisible(ringShown);
    m_bar->setVisible(barShown);
    m_banner->setVisible(bannerShown);
}

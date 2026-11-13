#include "widgets/button.h"
#include "widgets/settingswindow.h"
#include "panels/settingspanel.h"
#include "theme.h"
#include "widgets/motion.h"
#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QVBoxLayout>

namespace {
void paintSurface(QWidget* widget) {
    QPainter painter(widget);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(widget->rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(Theme::surfaceFill()));
    painter.setPen(QPen(QColor(Theme::surfaceBorder()), 1));
    painter.drawRoundedRect(QRectF(widget->rect()).adjusted(.5, .5, -.5, -.5), 14, 14);
}
class Scrim : public QWidget {
  public:
    explicit Scrim(QWidget* parent) : QWidget(parent) { setAttribute(Qt::WA_NoSystemBackground); }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QColor color(Theme::brandInk());
        color.setAlpha(82);
        painter.fillRect(rect(), color);
    }
};
} // namespace
Button *Ui::dialogCloseButton(QDialog *dialog, QWidget *parent) {
    auto *close = new Button(parent ? parent : dialog);
    close->setObjectName("hubDialogClose");
    close->setFlat(true);
    close->setFixedSize(30, 30);
    close->setIconSize(QSize(16, 16));
    close->setIcon(Theme::coloredIcon("window-close", QColor(Theme::textSecondary()), 16));
    close->setAccessibleName(QObject::tr("Close dialog"));
    close->setToolTip(QObject::tr("Close"));
    close->setAutoDefault(false);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
    return close;
}

SettingsDetailDialog::SettingsDetailDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
    setObjectName("settingsDetailDialog");
    setAttribute(Qt::WA_TranslucentBackground);
}
void SettingsDetailDialog::paintEvent(QPaintEvent*) {
    paintSurface(this);
}
void SettingsWindow::paintEvent(QPaintEvent*) {
    paintSurface(this);
}

SettingsWindow::SettingsWindow(SettingsPanel* panel, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint), m_scrim(new Scrim(parent)),
      m_fade(new QPropertyAnimation(this, "windowOpacity", this)) {
    setObjectName("settingsWindow");
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle("Settings — Magnite Space");
    setWindowModality(Qt::WindowModal);
    setMinimumSize(760, 560);
    resize(960, 662);
    auto* body = new QVBoxLayout(this);
    body->setContentsMargins(1, 1, 1, 1);
    body->addWidget(panel);
    m_scrim->hide();
    parent->installEventFilter(this);
    m_fade->setDuration(Theme::motionFastMs());
    m_fade->setEasingCurve(QEasingCurve::OutCubic);
    connect(panel, &SettingsPanel::closeRequested, this, &QDialog::reject);
    connect(panel, &SettingsPanel::settingsChanged, this, [this] {
        if (MotionValue::reduced()) {
            m_fade->stop();
            setWindowOpacity(1);
            if (m_finishing)
                QDialog::done(m_finishResult);
        }
    });
    connect(this, &QDialog::finished, this, [this] {
        m_scrim->hide();
        setWindowOpacity(1);
        m_finishing = false;
    });
}
void SettingsWindow::positionWindow() {
    const auto parent = parentWidget();
    const auto available = parent->screen()->availableGeometry();
    const QSize wanted(qMin(960, qMin(parent->width() - 40, available.width() - 32)),
                       qMin(720, qMin(parent->height() - 40, available.height() - 32)));
    resize(wanted.expandedTo(minimumSize()).boundedTo(available.size()));
    move(parent->frameGeometry().center() - rect().center());
}
void SettingsWindow::openSettings() {
    if (isVisible()) {
        raise();
        activateWindow();
        return;
    }
    m_finishing = false;
    positionWindow();
    m_scrim->setGeometry(parentWidget()->rect());
    m_scrim->show();
    m_scrim->raise();
    open();
    raise();
    activateWindow();
    animate(false);
}
bool SettingsWindow::motionRunning() const {
    return m_fade->state() == QAbstractAnimation::Running;
}
void SettingsWindow::animate(bool closing, int result) {
    m_fade->stop();
    disconnect(m_fade, &QPropertyAnimation::finished, this, nullptr);
    if (MotionValue::reduced() || QApplication::platformName() == "offscreen") {
        setWindowOpacity(1);
        if (closing)
            QDialog::done(result);
        return;
    }
    m_fade->setStartValue(closing ? windowOpacity() : .9);
    m_fade->setEndValue(closing ? .9 : 1.);
    connect(m_fade, &QPropertyAnimation::finished, this, [this, closing, result] {
        if (closing)
            QDialog::done(result);
    });
    m_fade->start();
}
void SettingsWindow::done(int result) {
    if (m_finishing)
        return;
    m_finishing = true;
    m_finishResult = result;
    animate(true, result);
}
bool SettingsWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && isVisible() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        m_scrim->setGeometry(parentWidget()->rect());
        positionWindow();
    }
    return QDialog::eventFilter(watched, event);
}

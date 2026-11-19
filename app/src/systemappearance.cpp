#include "systemappearance.h"

#include <QCoreApplication>

#ifdef MAGNITE_DESKTOP_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>

namespace {
const char *const kService = "org.freedesktop.portal.Desktop";
const char *const kPath = "/org/freedesktop/portal/desktop";
const char *const kInterface = "org.freedesktop.portal.Settings";
const char *const kGroup = "org.freedesktop.appearance";
const char *const kKey = "color-scheme";

// Portal replies wrap the value in one or two variants depending on the method and version.
QVariant unwrap(QVariant value) {
    while (value.canConvert<QDBusVariant>() && value.userType() == qMetaTypeId<QDBusVariant>())
        value = qvariant_cast<QDBusVariant>(value).variant();
    return value;
}

std::optional<uint> readColorScheme() {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return std::nullopt;
    // ReadOne (portal v2) first, then the older Read. A short timeout keeps startup quick without a portal.
    for (const char *method : {"ReadOne", "Read"}) {
        auto call = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
        call << QString::fromLatin1(kGroup) << QString::fromLatin1(kKey);
        const auto reply = bus.call(call, QDBus::Block, 500);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) continue;
        bool ok = false;
        const uint value = unwrap(reply.arguments().first()).toUInt(&ok);
        if (ok) return value;
    }
    return std::nullopt;
}
}
#endif

std::optional<bool> SystemAppearance::fromColorScheme(uint value) {
    if (value == 1) return true;
    if (value == 2) return false;
    return std::nullopt;
}

SystemAppearance *SystemAppearance::instance() {
    static SystemAppearance *appearance = new SystemAppearance(QCoreApplication::instance());
    return appearance;
}

SystemAppearance::SystemAppearance(QObject *parent) : QObject(parent) {
#ifdef MAGNITE_DESKTOP_DBUS
    if (qEnvironmentVariableIsSet("MAGNITE_IGNORE_PORTAL_APPEARANCE")) return;   // tests: Qt's answer only
    if (const auto scheme = readColorScheme()) m_prefersDark = fromColorScheme(*scheme);
    QDBusConnection::sessionBus().connect(kService, kPath, kInterface, "SettingChanged", this,
                                          SLOT(settingChanged(QString,QString,QDBusVariant)));
#endif
}

#ifdef MAGNITE_DESKTOP_DBUS
void SystemAppearance::settingChanged(const QString &group, const QString &key, const QDBusVariant &value) {
    if (group != QLatin1String(kGroup) || key != QLatin1String(kKey)) return;
    bool ok = false;
    const uint scheme = unwrap(value.variant()).toUInt(&ok);
    const auto next = ok ? fromColorScheme(scheme) : std::nullopt;
    if (next == m_prefersDark) return;
    m_prefersDark = next;
    emit changed();
}
#endif

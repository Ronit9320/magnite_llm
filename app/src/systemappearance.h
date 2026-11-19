#pragma once

#include <QObject>
#include <optional>

#ifdef MAGNITE_DESKTOP_DBUS
#include <QDBusVariant>
#endif

// The desktop's light/dark preference as Linux desktops publish it to apps: the XDG desktop portal's
// org.freedesktop.appearance color-scheme (GNOME, KDE and others). Qt's GTK platform theme only sees
// the GTK theme name ("Adwaita"), so on GNOME with the dark style it reports light; this fills that gap.
// Elsewhere (and without a portal) there is no preference and Qt's own answer is used.
class SystemAppearance : public QObject {
    Q_OBJECT
public:
    static SystemAppearance *instance();
    // true = dark, false = light, nullopt = no stated preference (or no portal).
    std::optional<bool> prefersDark() const { return m_prefersDark; }
    // Maps the portal's color-scheme value: 1 prefers dark, 2 prefers light, anything else has no preference.
    static std::optional<bool> fromColorScheme(uint value);

signals:
    void changed();

private:
    explicit SystemAppearance(QObject *parent);
#ifdef MAGNITE_DESKTOP_DBUS
private slots:
    void settingChanged(const QString &group, const QString &key, const QDBusVariant &value);
#endif
private:
    std::optional<bool> m_prefersDark;
};

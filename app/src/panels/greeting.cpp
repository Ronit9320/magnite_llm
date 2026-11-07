#include "panels/greeting.h"

#include <QSettings>

namespace Greeting {

namespace {
constexpr int kNameLength = 40;

struct Wording {
    const char *withName;      // "%1" is the name
    const char *withoutName;
};

const Wording kMorning[] = {{"Good morning, %1", "Good morning"}, {"Morning, %1", "Morning"}, {"Rise and shine, %1", "Rise and shine"}};
const Wording kAfternoon[] = {{"Good afternoon, %1", "Good afternoon"}, {"Afternoon, %1", "Afternoon"},
                              {"Hope your day is going well, %1", "Hope your day is going well"}};
const Wording kEvening[] = {{"Good evening, %1", "Good evening"}, {"Evening, %1", "Evening"}, {"Winding down, %1?", "Winding down?"}};
const Wording kLate[] = {{"Up late, %1?", "Up late?"}, {"Burning the midnight oil, %1?", "Burning the midnight oil?"}, {"Still up, %1?", "Still up?"}};
}  // namespace

QStringList variants(int hour, const QString &name) {
    const Wording *band = kLate;
    if (hour >= 5 && hour <= 11) band = kMorning;
    else if (hour >= 12 && hour <= 16) band = kAfternoon;
    else if (hour >= 17 && hour <= 21) band = kEvening;
    const QString clean = name.trimmed();
    QStringList out;
    for (int i = 0; i < 3; ++i)
        out.append(clean.isEmpty() ? QString::fromUtf8(band[i].withoutName) : QString::fromUtf8(band[i].withName).arg(clean));
    return out;
}

QString pick(const QStringList &options, const QString &previous, int random) {
    if (options.isEmpty()) return QString();
    const int n = options.size();
    int index = int(((qint64(random) % n) + n) % n);
    if (n > 1 && options.at(index) == previous) index = (index + 1) % n;
    return options.at(index);
}

QString cleanName(const QString &raw) {
    QString name = raw.trimmed();
    if (name.size() > kNameLength) {
        name = name.left(kNameLength);
        if (name.at(name.size() - 1).isHighSurrogate()) name.chop(1);
        name = name.trimmed();
    }
    return name;
}

QString userName() { return cleanName(QSettings().value("profile/name").toString()); }

void setUserName(const QString &name) {
    const QString clean = cleanName(name);
    if (clean.isEmpty()) QSettings().remove("profile/name");
    else QSettings().setValue("profile/name", clean);
}

bool nameAsked() { return QSettings().value("profile/nameAsked", false).toBool(); }
void setNameAsked(bool asked) { QSettings().setValue("profile/nameAsked", asked); }

QString accountNameSuggestion() {
    QString name = qEnvironmentVariable("USER");
    if (name.isEmpty()) name = qEnvironmentVariable("USERNAME");
    name = name.trimmed();
    if (name.isEmpty()) return QString();
    name[0] = name.at(0).toUpper();
    return cleanName(name);
}

}  // namespace Greeting

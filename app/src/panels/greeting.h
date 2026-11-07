#pragma once

#include <QString>
#include <QStringList>

// The friendly line on a blank chat ("Good morning, Archit") and the name behind it.
// Wording is pure so it can be tested without a clock; the name lives in QSettings.
namespace Greeting {

// Every wording for that hour's band (05-11 morning, 12-16 afternoon, 17-21 evening, 22-04 late night),
// with the name applied; a blank name drops it. Hours outside 0-23 count as late night.
QStringList variants(int hour, const QString &name);
// options[random mod n]; steps on to the next one rather than repeat `previous` (when there is a choice).
QString pick(const QStringList &options, const QString &previous, int random);

// "Ada" from "  Ada  "; at most 40 characters, never cut inside a character; blank stays blank.
QString cleanName(const QString &raw);
QString userName();                       // QSettings profile/name
void setUserName(const QString &name);
bool nameAsked();                         // QSettings profile/nameAsked: the first-launch question was answered or skipped
void setNameAsked(bool asked);
// The operating system's account name with a capital first letter, or empty when unknown.
QString accountNameSuggestion();

}  // namespace Greeting

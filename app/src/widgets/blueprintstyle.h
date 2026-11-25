#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

// Pure mappings from graph vocabulary (port types, node kinds) to the editor's colours, labels,
// icons and node metrics. Colours come from the Blueprints tokens in theme.h.
namespace BlueprintStyle {

// Node geometry in scene units.
constexpr qreal nodeWidth = 220;
constexpr qreal headerHeight = 36;
constexpr qreal pinRowHeight = 24;
constexpr qreal previewHeight = 22;
constexpr qreal cornerRadius = 10;
constexpr qreal pinRadius = 5;
// Opacity of the family colour washed over the node header.
constexpr qreal headerTintAlpha = 0.22;

// One of: text number boolean object array receipt adapter.
QString typeFamily(const QString &portType);
QColor typeColor(const QString &portType);

// One of families(): sources transform knowledge models tools output.
QString nodeFamily(const QString &kind);
QString familyLabel(const QString &family);
QStringList families();
QColor familyColor(const QString &family);

QString nodeIcon(const QString &kind);

}  // namespace BlueprintStyle

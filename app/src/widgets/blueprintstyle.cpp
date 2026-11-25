#include "widgets/blueprintstyle.h"

#include "theme.h"

namespace BlueprintStyle {

QString typeFamily(const QString &portType) {
    if (portType == "integer" || portType == "number" || portType == "index") return "number";
    if (portType == "object" || portType == "fields" || portType == "order") return "object";
    if (portType == "array") return "array";
    if (portType == "receipt") return "receipt";
    if (portType == "adapter-ref") return "adapter";
    if (portType == "boolean") return "boolean";
    return "text";
}

QColor typeColor(const QString &portType) {
    const QString family = typeFamily(portType);
    if (family == "number") return QColor(Theme::blueprintTypeNumber());
    if (family == "object") return QColor(Theme::blueprintTypeObject());
    if (family == "array") return QColor(Theme::blueprintTypeArray());
    if (family == "receipt") return QColor(Theme::blueprintTypeReceipt());
    if (family == "adapter") return QColor(Theme::blueprintTypeAdapter());
    if (family == "boolean") return QColor(Theme::blueprintTypeBoolean());
    return QColor(Theme::blueprintTypeText());
}

QString nodeFamily(const QString &kind) {
    if (kind == "input" || kind == "dataset") return "sources";
    if (kind == "knowledge") return "knowledge";
    if (kind == "model" || kind == "reasoning" || kind == "adapter" || kind == "agent" || kind == "selector"
        || kind == "extractor") return "models";
    if (kind == "tool") return "tools";
    if (kind == "output") return "output";
    return "transform";
}

QString familyLabel(const QString &family) {
    if (family == "sources") return "Sources";
    if (family == "knowledge") return "Knowledge";
    if (family == "models") return "Models";
    if (family == "tools") return "Tools";
    if (family == "output") return "Output";
    return "Transform";
}

QStringList families() {
    return {"sources", "transform", "knowledge", "models", "tools", "output"};
}

QColor familyColor(const QString &family) {
    if (family == "sources") return QColor(Theme::blueprintFamilySources());
    if (family == "knowledge") return QColor(Theme::blueprintFamilyKnowledge());
    if (family == "models") return QColor(Theme::blueprintFamilyModels());
    if (family == "tools") return QColor(Theme::blueprintFamilyTools());
    if (family == "output") return QColor(Theme::blueprintFamilyOutput());
    return QColor(Theme::blueprintFamilyTransform());
}

QString nodeIcon(const QString &kind) {
    if (kind == "input") return "message-circle";
    if (kind == "dataset") return "database";
    if (kind == "model") return "layers";
    if (kind == "adapter") return "box";
    if (kind == "knowledge") return "search";
    if (kind == "reasoning") return "brain";
    if (kind == "agent") return "message-circle";
    if (kind == "output") return "file-down";
    if (kind == "selector") return "search";
    if (kind == "extractor") return "layers";
    if (kind == "pricing") return "check";
    if (kind == "tool") return "settings";
    return "cpu";
}

}  // namespace BlueprintStyle

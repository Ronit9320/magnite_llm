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

// Blurs the alpha of a black image in place with a few box-blur passes.
void blurAlpha(QImage &image, int radius) {
    const int w = image.width(), h = image.height();
    QVector<quint8> a(w * h), b(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) a[y * w + x] = qAlpha(image.pixel(x, y));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            int sum = 0;
            for (int x = -radius; x <= radius; ++x) sum += a[y * w + qBound(0, x, w - 1)];
            for (int x = 0; x < w; ++x) {
                b[y * w + x] = sum / (2 * radius + 1);
                sum += a[y * w + qMin(w - 1, x + radius + 1)] - a[y * w + qMax(0, x - radius)];
            }
        }
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int y = -radius; y <= radius; ++y) sum += b[qBound(0, y, h - 1) * w + x];
            for (int y = 0; y < h; ++y) {
                a[y * w + x] = sum / (2 * radius + 1);
                sum += b[qMin(h - 1, y + radius + 1) * w + x] - b[qMax(0, y - radius) * w + x];
            }
        }
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) image.setPixel(x, y, qRgba(0, 0, 0, a[y * w + x]));
}

}  // namespace BlueprintStyle

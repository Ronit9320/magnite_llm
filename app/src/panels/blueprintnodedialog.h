#pragma once

#include <QJsonObject>
#include <optional>

class BlueprintPreview;
class QWidget;

std::optional<QJsonObject> configureBlueprintNode(BlueprintPreview &preview, QWidget *parent,
    const QString &kind, const QJsonObject &existing = {});

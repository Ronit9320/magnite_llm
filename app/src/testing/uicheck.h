#pragma once
#include <QString>
// Deterministic UI checks against a loopback fixture server, without ML downloads.
int runUiCheck(const QString &captureDirectory = {});

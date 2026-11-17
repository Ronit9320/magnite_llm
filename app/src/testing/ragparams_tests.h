#pragma once

#include <functional>

// Pure checks of RagParams (status/embedders parsing and the indexing-time hint), run by --selftest.
void ragParamsChecks(const std::function<void(bool, const char *)> &check);

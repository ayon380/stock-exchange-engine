/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#include "EngineConfig.h"

namespace engine_config {
namespace {
std::atomic<bool> g_dev_mode{false};
} // namespace

void setDevMode(bool enabled) {
    g_dev_mode.store(enabled, std::memory_order_relaxed);
}

bool isDevMode() {
    return g_dev_mode.load(std::memory_order_relaxed);
}

} // namespace engine_config

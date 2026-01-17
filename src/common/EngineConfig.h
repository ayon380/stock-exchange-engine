/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#pragma once

#include <atomic>

namespace engine_config {

// Sets whether the engine is running in developer (verbose) mode.
void setDevMode(bool enabled);

// Returns true if developer mode is enabled. Developer mode enables
// verbose logging that is helpful during local debugging sessions.
bool isDevMode();

}

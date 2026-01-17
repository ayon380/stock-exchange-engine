/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#pragma once

#include <iostream>
#include "EngineConfig.h"

// Helper macro to guard verbose logging so it only runs while the engine
// is launched with the -dev flag.
#define ENGINE_LOG_DEV(expr)                                                        \
    do {                                                                            \
        if (engine_config::isDevMode()) {                                           \
            expr;                                                                   \
        }                                                                           \
    } while (0)

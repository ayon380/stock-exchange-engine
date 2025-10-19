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

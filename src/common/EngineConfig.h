#pragma once

#include <atomic>

namespace engine_config {

// Sets whether the engine is running in developer (verbose) mode.
void setDevMode(bool enabled);

// Returns true if developer mode is enabled. Developer mode enables
// verbose logging that is helpful during local debugging sessions.
bool isDevMode();

}

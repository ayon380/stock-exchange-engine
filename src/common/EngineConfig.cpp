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

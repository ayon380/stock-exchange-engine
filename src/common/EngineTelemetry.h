#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

struct TelemetrySnapshot {
    uint64_t totalOrders{0};
    double averageLatencyUs{0.0};
    double ordersPerSecond{0.0};
    double cpuPercent{0.0};
    double memoryMb{0.0};
};

// Thread-safe singleton that keeps track of high-level engine telemetry.
class EngineTelemetry {
public:
    static EngineTelemetry& instance();

    // Records an order. If latencyUs is >= 0, it is included in the
    // rolling average latency calculation. Latency is expected to be in microseconds.
    void recordOrder(long long latencyUs = -1);

    // Returns the latest metrics snapshot. This call is inexpensive and can be
    // performed frequently (e.g. once per second) from a display thread.
    TelemetrySnapshot snapshot();

private:
    EngineTelemetry();

    std::atomic<uint64_t> order_count_;
    std::atomic<long long> latency_sum_us_;
    std::atomic<uint64_t> latency_samples_;

    std::mutex usage_mutex_;
    long long last_user_us_;
    long long last_system_us_;
    uint64_t last_order_count_;
    std::chrono::steady_clock::time_point last_wall_ts_;
    std::chrono::steady_clock::time_point last_order_ts_;

    double queryMemoryMb() const;
};

#include "EngineTelemetry.h"

#include <sys/resource.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

EngineTelemetry& EngineTelemetry::instance() {
    static EngineTelemetry telemetry;
    return telemetry;
}

EngineTelemetry::EngineTelemetry()
    : order_count_(0),
      latency_sum_us_(0),
      latency_samples_(0),
      last_user_us_(0),
      last_system_us_(0),
      last_order_count_(0) {}

void EngineTelemetry::recordOrder(long long latencyUs) {
    order_count_.fetch_add(1, std::memory_order_relaxed);

    if (latencyUs >= 0) {
        latency_sum_us_.fetch_add(latencyUs, std::memory_order_relaxed);
        latency_samples_.fetch_add(1, std::memory_order_relaxed);
    }
}

TelemetrySnapshot EngineTelemetry::snapshot() {
    TelemetrySnapshot snapshot;

    snapshot.totalOrders = order_count_.load(std::memory_order_relaxed);
    const auto samples = latency_samples_.load(std::memory_order_relaxed);
    const auto latency_sum = latency_sum_us_.load(std::memory_order_relaxed);
    if (samples > 0) {
        snapshot.averageLatencyUs = static_cast<double>(latency_sum) / static_cast<double>(samples);
    }

    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(usage_mutex_);

        struct rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            const long long user_us = static_cast<long long>(usage.ru_utime.tv_sec) * 1'000'000LL + usage.ru_utime.tv_usec;
            const long long system_us = static_cast<long long>(usage.ru_stime.tv_sec) * 1'000'000LL + usage.ru_stime.tv_usec;

            if (last_wall_ts_.time_since_epoch().count() != 0) {
                const auto wall_delta_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_wall_ts_).count();
                const long long cpu_delta_us = (user_us - last_user_us_) + (system_us - last_system_us_);
                if (wall_delta_us > 0) {
                    snapshot.cpuPercent = static_cast<double>(cpu_delta_us) / static_cast<double>(wall_delta_us) * 100.0;
                }
            }

            last_user_us_ = user_us;
            last_system_us_ = system_us;
            last_wall_ts_ = now;
        }

        if (last_order_ts_.time_since_epoch().count() != 0) {
            const auto wall_delta_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_order_ts_).count();
            if (wall_delta_us > 0) {
                const auto order_delta = snapshot.totalOrders - last_order_count_;
                snapshot.ordersPerSecond = static_cast<double>(order_delta) * 1'000'000.0 / static_cast<double>(wall_delta_us);
            }
        }

        last_order_count_ = snapshot.totalOrders;
        last_order_ts_ = now;
    }

    snapshot.memoryMb = queryMemoryMb();

    return snapshot;
}

double EngineTelemetry::queryMemoryMb() const {
#ifdef __APPLE__
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<double>(usage.ru_maxrss) / 1024.0; // ru_maxrss is in KB on Linux
    }
    return 0.0;
#endif
}

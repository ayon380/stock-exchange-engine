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
#include <chrono>
#include <thread>
#include <algorithm>

/**
 * Adaptive Load Manager - Dynamically adjusts CPU usage based on order flow
 * 
 * Strategy:
 * - IDLE mode: Long sleeps (1-10ms) when no orders
 * - WARMING mode: Progressively shorter sleeps as orders increase
 * - ACTIVE mode: Minimal sleep (yield only) during moderate load
 * - PEAK mode: Full busy-wait (no sleep) during high-frequency trading
 * 
 * Benefits:
 * - Near-zero CPU usage when idle
 * - Sub-millisecond latency during active trading
 * - Automatic adaptation to load patterns
 */
class AdaptiveLoadManager {
public:
    enum class LoadLevel {
        IDLE,      // 0% load: sleep 5ms
        LOW,       // 1-10% load: sleep 1ms
        WARMING,   // 10-30% load: sleep 100μs
        ACTIVE,    // 30-70% load: yield only
        PEAK       // 70%+ load: busy-wait (no sleep/yield)
    };

private:
    // Tunable parameters
    static constexpr int MEASUREMENT_WINDOW = 1000;     // Measure load over N iterations
    static constexpr int IDLE_THRESHOLD = 0;            // 0 messages = IDLE
    static constexpr int LOW_THRESHOLD = 10;            // <1% busy
    static constexpr int WARMING_THRESHOLD = 100;       // 10% busy
    static constexpr int ACTIVE_THRESHOLD = 500;        // 50% busy
    
    // Sleep durations per level
    static constexpr auto SLEEP_IDLE = std::chrono::milliseconds(5);
    static constexpr auto SLEEP_LOW = std::chrono::milliseconds(1);
    static constexpr auto SLEEP_WARMING = std::chrono::microseconds(100);
    static constexpr auto SLEEP_ACTIVE = std::chrono::microseconds(1);
    
    std::atomic<LoadLevel> current_level_{LoadLevel::IDLE};
    std::atomic<int> iteration_count_{0};
    std::atomic<int> work_count_{0};
    
    // Hysteresis to prevent rapid mode switching
    int level_switch_delay_{0};
    static constexpr int SWITCH_DELAY_CYCLES = 100;

public:
    AdaptiveLoadManager() = default;
    
    /**
     * Call this after attempting to process work
     * @param did_work true if work was processed, false if queue was empty
     */
    void recordIteration(bool did_work) {
        int iter = iteration_count_.fetch_add(1, std::memory_order_relaxed);
        
        if (did_work) {
            work_count_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Re-evaluate load level periodically
        if (iter % MEASUREMENT_WINDOW == 0) {
            updateLoadLevel();
        }
    }
    
    /**
     * Sleep/yield based on current load level
     */
    void waitForWork() {
        LoadLevel level = current_level_.load(std::memory_order_relaxed);
        
        switch (level) {
            case LoadLevel::IDLE:
                std::this_thread::sleep_for(SLEEP_IDLE);
                break;
            case LoadLevel::LOW:
                std::this_thread::sleep_for(SLEEP_LOW);
                break;
            case LoadLevel::WARMING:
                std::this_thread::sleep_for(SLEEP_WARMING);
                break;
            case LoadLevel::ACTIVE:
                std::this_thread::sleep_for(SLEEP_ACTIVE);
                break;
            case LoadLevel::PEAK:
                // Busy-wait - no sleep or yield
                break;
        }
    }
    
    /**
     * Get current load level for monitoring
     */
    LoadLevel getLoadLevel() const {
        return current_level_.load(std::memory_order_relaxed);
    }
    
    /**
     * Get human-readable load level name
     */
    const char* getLoadLevelName() const {
        switch (getLoadLevel()) {
            case LoadLevel::IDLE: return "IDLE";
            case LoadLevel::LOW: return "LOW";
            case LoadLevel::WARMING: return "WARMING";
            case LoadLevel::ACTIVE: return "ACTIVE";
            case LoadLevel::PEAK: return "PEAK";
            default: return "UNKNOWN";
        }
    }
    
    /**
     * Get current work percentage (0-100)
     */
    int getWorkPercentage() const {
        int iter = iteration_count_.load(std::memory_order_relaxed);
        int work = work_count_.load(std::memory_order_relaxed);
        
        if (iter == 0) return 0;
        return (work * 100) / iter;
    }
    
    /**
     * Reset statistics (call when starting new trading session)
     */
    void reset() {
        iteration_count_.store(0, std::memory_order_relaxed);
        work_count_.store(0, std::memory_order_relaxed);
        current_level_.store(LoadLevel::IDLE, std::memory_order_relaxed);
        level_switch_delay_ = 0;
    }

private:
    void updateLoadLevel() {
        int work = work_count_.exchange(0, std::memory_order_relaxed);
        iteration_count_.store(0, std::memory_order_relaxed);
        
        // Determine new load level based on work count
        LoadLevel new_level;
        
        if (work <= IDLE_THRESHOLD) {
            new_level = LoadLevel::IDLE;
        } else if (work <= LOW_THRESHOLD) {
            new_level = LoadLevel::LOW;
        } else if (work <= WARMING_THRESHOLD) {
            new_level = LoadLevel::WARMING;
        } else if (work <= ACTIVE_THRESHOLD) {
            new_level = LoadLevel::ACTIVE;
        } else {
            new_level = LoadLevel::PEAK;
        }
        
        // Apply hysteresis to prevent oscillation
        LoadLevel current = current_level_.load(std::memory_order_relaxed);
        
        if (new_level != current) {
            if (level_switch_delay_ > 0) {
                level_switch_delay_--;
            } else {
                current_level_.store(new_level, std::memory_order_release);
                level_switch_delay_ = SWITCH_DELAY_CYCLES;
            }
        } else {
            level_switch_delay_ = 0;
        }
    }
};

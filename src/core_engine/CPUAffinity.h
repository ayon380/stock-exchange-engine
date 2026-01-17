/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#pragma once
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>
#elif defined(__APPLE__)
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

class CPUAffinity {
public:
    // Set CPU affinity for current thread
    static bool setThreadAffinity(int cpu_core) {
#ifdef _WIN32
        DWORD_PTR mask = 1ULL << cpu_core;
        return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__APPLE__)
        // macOS implementation using thread affinity policy
        thread_affinity_policy_data_t policy = {cpu_core};
        mach_port_t mach_thread = pthread_mach_thread_np(pthread_self());
        return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
#endif
    }
    
    // Set CPU affinity for specific thread
    static bool setThreadAffinity(std::thread& thread, int cpu_core) {
#ifdef _WIN32
        DWORD_PTR mask = 1ULL << cpu_core;
        return SetThreadAffinityMask(thread.native_handle(), mask) != 0;
#elif defined(__APPLE__)
        // macOS implementation using thread affinity policy
        thread_affinity_policy_data_t policy = {cpu_core};
        mach_port_t mach_thread = pthread_mach_thread_np(thread.native_handle());
        return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        return pthread_setaffinity_np(thread.native_handle(), sizeof(cpuset), &cpuset) == 0;
#endif
    }
    
    // Get number of CPU cores
    static int getCoreCount() {
        return std::thread::hardware_concurrency();
    }
    
    // Get available CPU cores (excluding system reserved cores)
    static std::vector<int> getAvailableCores() {
        std::vector<int> cores;
        int total_cores = getCoreCount();
        
        // Reserve first core for system/OS, use remaining cores
        for (int i = 1; i < total_cores; ++i) {
            cores.push_back(i);
        }
        
        return cores;
    }
    
    // Set thread priority (high priority for matching engine)
    static bool setHighPriority(std::thread& thread) {
#ifdef _WIN32
        return SetThreadPriority(thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#elif defined(__APPLE__)
        // macOS doesn't have the same priority system, return true for compatibility
        return true;
#else
        struct sched_param param;
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        return pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &param) == 0;
#endif
    }
    
    // Set current thread to high priority
    static bool setCurrentThreadHighPriority() {
#ifdef _WIN32
        return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#elif defined(__APPLE__)
        // macOS doesn't have the same priority system, return true for compatibility
        return true;
#else
        struct sched_param param;
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#endif
    }
};

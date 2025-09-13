#pragma once
#include <atomic>
#include <memory>
#include <array>
#include <thread>

// Single Producer Single Consumer (SPSC) lock-free queue
template<typename T, size_t Size>
class SPSCQueue {
private:
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static constexpr size_t MASK = Size - 1;
    
    struct alignas(64) Slot {
        std::atomic<T*> data{nullptr};
    };
    
    alignas(64) std::array<Slot, Size> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    
public:
    SPSCQueue() = default;
    ~SPSCQueue() = default;
    
    // Non-copyable, non-movable
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;
    
    // Producer side (single thread only)
    bool enqueue(T* item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & MASK;
        
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue is full
        }
        
        buffer_[head].data.store(item, std::memory_order_relaxed);
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    // Consumer side (single thread only)
    T* dequeue() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        
        if (tail == head_.load(std::memory_order_acquire)) {
            return nullptr; // Queue is empty
        }
        
        T* item = buffer_[tail].data.load(std::memory_order_relaxed);
        buffer_[tail].data.store(nullptr, std::memory_order_relaxed);
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return item;
    }
    
    // Check if queue is empty (not thread-safe, for diagnostic only)
    bool empty() const {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }
    
    // Get approximate size (not thread-safe, for diagnostic only)
    size_t size() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) & MASK;
    }
};

// Multi-Producer Single Consumer (MPSC) lock-free queue
template<typename T, size_t Size>
class MPSCQueue {
private:
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static constexpr size_t MASK = Size - 1;
    
    struct alignas(64) Slot {
        std::atomic<T*> data{nullptr};
        std::atomic<bool> ready{false};
    };
    
    alignas(64) std::array<Slot, Size> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    
public:
    MPSCQueue() = default;
    ~MPSCQueue() = default;
    
    // Non-copyable, non-movable
    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;
    MPSCQueue(MPSCQueue&&) = delete;
    MPSCQueue& operator=(MPSCQueue&&) = delete;
    
    // Producer side (multiple threads)
    bool enqueue(T* item) {
        const size_t head = head_.fetch_add(1, std::memory_order_acq_rel) & MASK;
        
        // Wait for slot to be available
        while (buffer_[head].ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        buffer_[head].data.store(item, std::memory_order_relaxed);
        buffer_[head].ready.store(true, std::memory_order_release);
        return true;
    }
    
    // Consumer side (single thread only)
    T* dequeue() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        
        if (!buffer_[tail].ready.load(std::memory_order_acquire)) {
            return nullptr; // Nothing ready
        }
        
        T* item = buffer_[tail].data.load(std::memory_order_relaxed);
        buffer_[tail].data.store(nullptr, std::memory_order_relaxed);
        buffer_[tail].ready.store(false, std::memory_order_release);
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return item;
    }
};

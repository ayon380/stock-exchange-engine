/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// Single Producer Single Consumer (SPSC) lock-free queue
template <typename T, size_t Size> class SPSCQueue {
private:
  static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
  static constexpr size_t MASK = Size - 1;

  struct alignas(64) Slot {
    std::atomic<T *> data{nullptr};
  };

  alignas(64) std::array<Slot, Size> buffer_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};

public:
  SPSCQueue() = default;
  ~SPSCQueue() = default;

  // Non-copyable, non-movable
  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;
  SPSCQueue(SPSCQueue &&) = delete;
  SPSCQueue &operator=(SPSCQueue &&) = delete;

  // Producer side (single thread only)
  bool enqueue(T *item) {
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
  T *dequeue() {
    const size_t tail = tail_.load(std::memory_order_relaxed);

    if (tail == head_.load(std::memory_order_acquire)) {
      return nullptr; // Queue is empty
    }

    T *item = buffer_[tail].data.load(std::memory_order_relaxed);
    buffer_[tail].data.store(nullptr, std::memory_order_relaxed);
    tail_.store((tail + 1) & MASK, std::memory_order_release);
    return item;
  }

  // Check if queue is empty (not thread-safe, for diagnostic only)
  bool empty() const {
    return head_.load(std::memory_order_relaxed) ==
           tail_.load(std::memory_order_relaxed);
  }

  // Get approximate size (not thread-safe, for diagnostic only)
  size_t size() const {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    return (head - tail) & MASK;
  }
};

// Multi-Producer Single Consumer (MPSC) lock-free queue
template <typename T, size_t Size> class MPSCQueue {
private:
  static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
  static constexpr size_t MASK = Size - 1;

  struct alignas(64) Slot {
    std::atomic<size_t> sequence{0};
    T *data{nullptr};
  };

  alignas(64) std::array<Slot, Size> buffer_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};

public:
  MPSCQueue() {
    for (size_t i = 0; i < Size; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }
  ~MPSCQueue() = default;

  // Non-copyable, non-movable
  MPSCQueue(const MPSCQueue &) = delete;
  MPSCQueue &operator=(const MPSCQueue &) = delete;
  MPSCQueue(MPSCQueue &&) = delete;
  MPSCQueue &operator=(MPSCQueue &&) = delete;

  // Producer side (multiple threads)
  bool enqueue(T *item) {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = buffer_[pos & MASK];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          slot.data = item;
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        // Queue is full, wait until consumer advances
        std::this_thread::yield();
        pos = head_.load(std::memory_order_relaxed);
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  // Producer side (multiple threads) - non-blocking attempt
  bool try_enqueue(T *item) {
    size_t pos = head_.load(std::memory_order_relaxed);
    Slot &slot = buffer_[pos & MASK];
    size_t seq = slot.sequence.load(std::memory_order_acquire);
    intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

    if (diff == 0) {
      if (head_.compare_exchange_strong(pos, pos + 1, std::memory_order_relaxed)) {
        slot.data = item;
        slot.sequence.store(pos + 1, std::memory_order_release);
        return true;
      }
    }

    return false;
  }

  // Consumer side (single thread only)
  T *dequeue() {
    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = buffer_[pos & MASK];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          T *item = slot.data;
          slot.data = nullptr;
          slot.sequence.store(pos + Size, std::memory_order_release);
          return item;
        }
      } else if (diff < 0) {
        return nullptr; // Queue is empty
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }
};

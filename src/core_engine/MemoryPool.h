#pragma once
#include <memory>
#include <vector>
#include <atomic>
#include <cstddef>
#include <cstddef>  // for offsetof
#include <new>

// Memory pool for fixed-size allocations
template<typename T, size_t PoolSize = 1024>
class MemoryPool {
private:
    // Use union to safely store either T or a pointer for free list
    // Ensure block is large enough for both T and a pointer
    struct Block {
        union {
            alignas(T) char data[sizeof(T)];
            Block* next;  // For free list when block is not in use
        };
    };
    
    // Ensure Block can hold a pointer
    static_assert(sizeof(Block) >= sizeof(Block*), 
                  "Block must be large enough to hold a pointer");
    static_assert(alignof(Block) >= alignof(Block*),
                  "Block must be properly aligned for pointer storage");
    
    std::unique_ptr<Block[]> pool_;
    std::atomic<Block*> free_list_;
    std::atomic<size_t> allocated_count_{0};
    size_t pool_size_;
    
    void initializeFreeList() {
        // Initialize the free list in reverse order for better cache locality
        // Now using the union's 'next' member instead of reinterpret_cast
        for (size_t i = pool_size_ - 1; i > 0; --i) {
            pool_[i].next = &pool_[i - 1];
        }
        pool_[0].next = nullptr;
        free_list_.store(&pool_[pool_size_ - 1], std::memory_order_relaxed);
    }
    
public:
    explicit MemoryPool(size_t size = PoolSize) : pool_size_(size) {
        pool_ = std::make_unique<Block[]>(pool_size_);
        initializeFreeList();
    }
    
    ~MemoryPool() = default;
    
    // Non-copyable, non-movable
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;
    
    // Allocate object from pool
    template<typename... Args>
    T* allocate(Args&&... args) {
        Block* block = free_list_.load(std::memory_order_acquire);
        
        while (block != nullptr) {
            // Use union's next member instead of reinterpret_cast
            Block* next = block->next;
            if (free_list_.compare_exchange_weak(block, next, 
                                               std::memory_order_release, 
                                               std::memory_order_acquire)) {
                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                // Construct T in the block's data area (placement new)
                return new(&block->data) T(std::forward<Args>(args)...);
            }
        }
        
        // Pool exhausted, fallback to regular allocation
        // NOTE: This is tracked separately and must be handled in deallocate
        return new T(std::forward<Args>(args)...);
    }
    
    // Deallocate object back to pool
    void deallocate(T* ptr) {
        if (!ptr) return;
        
        // Calculate the Block* from T* (ptr points to the data member in the union)
        Block* block = reinterpret_cast<Block*>(
            reinterpret_cast<char*>(ptr) - offsetof(Block, data)
        );
        
        // Check if pointer is within our pool range
        if (block >= pool_.get() && block < pool_.get() + pool_size_) {
            // Call destructor
            ptr->~T();
            
            // Add back to free list using union's next member
            Block* head = free_list_.load(std::memory_order_acquire);
            do {
                block->next = head;
            } while (!free_list_.compare_exchange_weak(head, block,
                                                     std::memory_order_release,
                                                     std::memory_order_acquire));
            
            allocated_count_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            // Was allocated outside pool, use regular delete
            delete ptr;
        }
    }
    
    // Statistics
    size_t allocated_count() const {
        return allocated_count_.load(std::memory_order_relaxed);
    }
    
    size_t capacity() const {
        return pool_size_;
    }
    
    size_t available() const {
        return pool_size_ - allocated_count();
    }
};

// RAII wrapper for memory pool allocation
template<typename T, size_t PoolSize = 1024>
class PoolPtr {
private:
    T* ptr_;
    MemoryPool<T, PoolSize>* pool_;
    
public:
    template<typename... Args>
    PoolPtr(MemoryPool<T, PoolSize>* pool, Args&&... args) 
        : pool_(pool) {
        ptr_ = pool_->allocate(std::forward<Args>(args)...);
    }
    
    ~PoolPtr() {
        if (ptr_ && pool_) {
            pool_->deallocate(ptr_);
        }
    }
    
    // Move constructor
    PoolPtr(PoolPtr&& other) noexcept 
        : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }
    
    // Move assignment
    PoolPtr& operator=(PoolPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_ && pool_) {
                pool_->deallocate(ptr_);
            }
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    // No copy
    PoolPtr(const PoolPtr&) = delete;
    PoolPtr& operator=(const PoolPtr&) = delete;
    
    T* get() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }
    
    explicit operator bool() const { return ptr_ != nullptr; }
    
    T* release() {
        T* tmp = ptr_;
        ptr_ = nullptr;
        pool_ = nullptr;
        return tmp;
    }
};

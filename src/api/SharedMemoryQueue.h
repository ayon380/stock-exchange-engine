#pragma once
#include <thread>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <atomic>
#include <string>
#include <memory>
#include <cstring>
#include <mutex>
#include "../core_engine/StockExchange.h"

// Shared memory ring buffer for ultra-low latency order submission
class SharedMemoryQueue {
private:
    struct SharedData {
        // Ring buffer metadata
        std::atomic<size_t> head{0};
        std::atomic<size_t> tail{0};
        size_t capacity;
        size_t message_size;

        // Synchronization
        boost::interprocess::interprocess_mutex mutex;
        boost::interprocess::interprocess_condition not_empty;
        boost::interprocess::interprocess_condition not_full;

        // Ring buffer data follows
        char data[0];
    };

    std::string shm_name_;
    boost::interprocess::shared_memory_object shm_;
    boost::interprocess::mapped_region region_;
    SharedData* shared_data_;
    bool is_server_;
    size_t capacity_;
    size_t message_size_;

public:
    // Server-side constructor (creates shared memory)
    SharedMemoryQueue(const std::string& name, size_t capacity, size_t message_size);

    // Client-side constructor (connects to existing shared memory)
    SharedMemoryQueue(const std::string& name);

    ~SharedMemoryQueue();

    // Producer interface
    bool enqueue(const void* message, size_t size);
    bool try_enqueue(const void* message, size_t size);

    // Consumer interface
    bool dequeue(void* message, size_t& size);
    bool try_dequeue(void* message, size_t& size);

    // Status
    bool is_connected() const { return shared_data_ != nullptr; }
    size_t size() const;
    bool empty() const { return size() == 0; }
    bool full() const;

    // Cleanup (server only)
    static void remove(const std::string& name);
};

// Order message structure for shared memory
#pragma pack(push, 1)
struct SharedOrderMessage {
    uint32_t message_length;
    uint32_t order_id_len;
    uint32_t user_id_len;
    uint32_t symbol_len;
    uint8_t side;
    uint8_t order_type;
    uint64_t quantity;
    double price;
    uint64_t timestamp_ms;
    // Variable-length strings follow
};
#pragma pack(pop)

// High-performance shared memory order client
class SharedMemoryOrderClient {
private:
    std::unique_ptr<SharedMemoryQueue> queue_;
    std::string shm_name_;

public:
    SharedMemoryOrderClient(const std::string& shm_name);
    ~SharedMemoryOrderClient();

    bool connect();
    void disconnect();

    bool submitOrder(const std::string& order_id, const std::string& user_id,
                    const std::string& symbol, uint8_t side, uint8_t order_type,
                    uint64_t quantity, double price);

    bool isConnected() const { return queue_ && queue_->is_connected(); }
};

// Shared memory order server (integrates with StockExchange)
class SharedMemoryOrderServer {
private:
    std::unique_ptr<SharedMemoryQueue> queue_;
    std::string shm_name_;
    StockExchange* exchange_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

public:
    SharedMemoryOrderServer(const std::string& shm_name, StockExchange* exchange);
    ~SharedMemoryOrderServer();

    bool start();
    void stop();

private:
    void processOrders();
};

#include "SharedMemoryQueue.h"
#include "../core_engine/StockExchange.h"
#include "common/EngineTelemetry.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <future>

using namespace boost::interprocess;

// SharedMemoryQueue implementation
SharedMemoryQueue::SharedMemoryQueue(const std::string& name, size_t capacity, size_t message_size)
    : shm_name_(name), capacity_(capacity), message_size_(message_size), is_server_(true) {

    // Remove any existing shared memory
    shared_memory_object::remove(shm_name_.c_str());

    // Create shared memory
    shm_ = shared_memory_object(create_only, shm_name_.c_str(), read_write);

    // Calculate total size needed
    size_t total_size = sizeof(SharedData) + (capacity * message_size);
    shm_.truncate(total_size);

    // Map the shared memory
    region_ = mapped_region(shm_, read_write);

    // Initialize shared data
    shared_data_ = new (region_.get_address()) SharedData();
    shared_data_->capacity = capacity;
    shared_data_->message_size = message_size;
    shared_data_->head.store(0);
    shared_data_->tail.store(0);
}

SharedMemoryQueue::SharedMemoryQueue(const std::string& name)
    : shm_name_(name), is_server_(false) {

    try {
        // Open existing shared memory
        shm_ = shared_memory_object(open_only, shm_name_.c_str(), read_write);
        region_ = mapped_region(shm_, read_write);
        shared_data_ = static_cast<SharedData*>(region_.get_address());

        capacity_ = shared_data_->capacity;
        message_size_ = shared_data_->message_size;
    } catch (const interprocess_exception&) {
        shared_data_ = nullptr;
    }
}

SharedMemoryQueue::~SharedMemoryQueue() {
    if (is_server_) {
        // Server cleans up shared memory
        shared_memory_object::remove(shm_name_.c_str());
    }
}

bool SharedMemoryQueue::enqueue(const void* message, size_t size) {
    if (!shared_data_ || size > message_size_) return false;

    scoped_lock<interprocess_mutex> lock(shared_data_->mutex);

    while (full()) {
        shared_data_->not_full.wait(lock);
    }

    size_t head = shared_data_->head.load();
    char* slot = shared_data_->data + (head * message_size_);

    // Copy message with size prefix
    *reinterpret_cast<size_t*>(slot) = size;
    std::memcpy(slot + sizeof(size_t), message, size);

    shared_data_->head.store((head + 1) % capacity_);
    shared_data_->not_empty.notify_one();

    return true;
}

bool SharedMemoryQueue::try_enqueue(const void* message, size_t size) {
    if (!shared_data_ || size > message_size_) return false;

    scoped_lock<interprocess_mutex> lock(shared_data_->mutex, try_to_lock);
    if (!lock.owns()) return false;

    if (full()) return false;

    size_t head = shared_data_->head.load();
    char* slot = shared_data_->data + (head * message_size_);

    // Copy message with size prefix
    *reinterpret_cast<size_t*>(slot) = size;
    std::memcpy(slot + sizeof(size_t), message, size);

    shared_data_->head.store((head + 1) % capacity_);
    shared_data_->not_empty.notify_one();

    return true;
}

bool SharedMemoryQueue::dequeue(void* message, size_t& size) {
    if (!shared_data_) return false;

    scoped_lock<interprocess_mutex> lock(shared_data_->mutex);

    while (empty()) {
        shared_data_->not_empty.wait(lock);
    }

    size_t tail = shared_data_->tail.load();
    char* slot = shared_data_->data + (tail * message_size_);

    // Read message size and data
    size_t msg_size = *reinterpret_cast<size_t*>(slot);
    std::memcpy(message, slot + sizeof(size_t), msg_size);
    size = msg_size;

    shared_data_->tail.store((tail + 1) % capacity_);
    shared_data_->not_full.notify_one();

    return true;
}

bool SharedMemoryQueue::try_dequeue(void* message, size_t& size) {
    if (!shared_data_) return false;

    scoped_lock<interprocess_mutex> lock(shared_data_->mutex, try_to_lock);
    if (!lock.owns()) return false;

    if (empty()) return false;

    size_t tail = shared_data_->tail.load();
    char* slot = shared_data_->data + (tail * message_size_);

    // Read message size and data
    size_t msg_size = *reinterpret_cast<size_t*>(slot);
    std::memcpy(message, slot + sizeof(size_t), msg_size);
    size = msg_size;

    shared_data_->tail.store((tail + 1) % capacity_);
    shared_data_->not_full.notify_one();

    return true;
}

size_t SharedMemoryQueue::size() const {
    if (!shared_data_) return 0;

    size_t head = shared_data_->head.load();
    size_t tail = shared_data_->tail.load();

    if (head >= tail) {
        return head - tail;
    } else {
        return capacity_ + head - tail;
    }
}

bool SharedMemoryQueue::full() const {
    return size() == capacity_ - 1; // Leave one slot empty to distinguish full from empty
}

void SharedMemoryQueue::remove(const std::string& name) {
    shared_memory_object::remove(name.c_str());
}

// SharedMemoryOrderClient implementation
SharedMemoryOrderClient::SharedMemoryOrderClient(const std::string& shm_name)
    : shm_name_(shm_name) {}

SharedMemoryOrderClient::~SharedMemoryOrderClient() {
    disconnect();
}

bool SharedMemoryOrderClient::connect() {
    try {
        queue_ = std::make_unique<SharedMemoryQueue>(shm_name_);
        return queue_->is_connected();
    } catch (const std::exception&) {
        return false;
    }
}

void SharedMemoryOrderClient::disconnect() {
    queue_.reset();
}

bool SharedMemoryOrderClient::submitOrder(const std::string& order_id, const std::string& user_id,
                                         const std::string& symbol, uint8_t side, uint8_t order_type,
                                         uint64_t quantity, double price) {
    if (!queue_) return false;

    // Calculate message size
    size_t message_size = sizeof(SharedOrderMessage) + order_id.size() + user_id.size() + symbol.size();
    std::vector<char> message(message_size);

    SharedOrderMessage* order_msg = reinterpret_cast<SharedOrderMessage*>(message.data());
    order_msg->message_length = message_size;
    order_msg->order_id_len = order_id.size();
    order_msg->user_id_len = user_id.size();
    order_msg->symbol_len = symbol.size();
    order_msg->side = side;
    order_msg->order_type = order_type;
    order_msg->quantity = quantity;
    order_msg->price = price;
    order_msg->timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Copy strings
    char* string_data = message.data() + sizeof(SharedOrderMessage);
    std::memcpy(string_data, order_id.data(), order_id.size());
    string_data += order_id.size();
    std::memcpy(string_data, user_id.data(), user_id.size());
    string_data += user_id.size();
    std::memcpy(string_data, symbol.data(), symbol.size());

    return queue_->enqueue(message.data(), message_size);
}

// SharedMemoryOrderServer implementation
SharedMemoryOrderServer::SharedMemoryOrderServer(const std::string& shm_name, StockExchange* exchange)
    : shm_name_(shm_name), exchange_(exchange) {}

SharedMemoryOrderServer::~SharedMemoryOrderServer() {
    stop();
}

bool SharedMemoryOrderServer::start() {
    if (running_.exchange(true)) return false;

    try {
        // Create shared memory queue
        queue_ = std::make_unique<SharedMemoryQueue>(shm_name_, 1024, 1024); // 1K capacity, 1KB messages

        // Start processing thread
        worker_thread_ = std::thread(&SharedMemoryOrderServer::processOrders, this);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to start shared memory server: " << e.what() << std::endl;
        running_.store(false);
        return false;
    }
}

void SharedMemoryOrderServer::stop() {
    if (!running_.exchange(false)) return;

    // Wait for worker thread with timeout to prevent hang
    if (worker_thread_.joinable()) {
        // Give worker thread 200ms to finish processing and exit
        auto future = std::async(std::launch::async, [this]() {
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        });
        
        if (future.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout) {
            std::cerr << "Warning: SharedMemory worker thread timeout, detaching" << std::endl;
            worker_thread_.detach();
        }
    }

    queue_.reset();
}

void SharedMemoryOrderServer::processOrders() {
    std::vector<char> buffer(1024); // 1KB buffer

    while (running_.load()) {
        size_t message_size = 0;
        if (queue_->try_dequeue(buffer.data(), message_size)) {
            if (message_size >= sizeof(SharedOrderMessage)) {
                SharedOrderMessage* order_msg = reinterpret_cast<SharedOrderMessage*>(buffer.data());

                // Extract strings
                size_t offset = sizeof(SharedOrderMessage);
                std::string order_id(buffer.data() + offset, order_msg->order_id_len);
                offset += order_msg->order_id_len;

                std::string user_id(buffer.data() + offset, order_msg->user_id_len);
                offset += order_msg->user_id_len;

                std::string symbol(buffer.data() + offset, order_msg->symbol_len);

                // Convert to core Order
                Order core_order{
                    order_id,
                    user_id,
                    symbol,
                    static_cast<int>(order_msg->side),
                    static_cast<int>(order_msg->order_type),
                    static_cast<int64_t>(order_msg->quantity),
                    static_cast<Price>(order_msg->price * 100.0 + 0.5), // Convert double to Price
                    static_cast<int64_t>(order_msg->timestamp_ms)
                };

                // Submit order
                exchange_->submitOrder(symbol, core_order);
                EngineTelemetry::instance().recordOrder();
            }
        } else {
            if (!running_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

}

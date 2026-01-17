/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#include "SharedMemoryQueue.h"
#include "../core_engine/StockExchange.h"
#include "common/EngineTelemetry.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <future>
#include <limits>

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
    if (!shared_data_) return false;
    if (size + sizeof(size_t) > message_size_) {
        std::cerr << "SharedMemoryQueue: message too large for slot (" << size << " bytes)" << std::endl;
        return false;
    }

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
    if (!shared_data_) return false;
    if (size + sizeof(size_t) > message_size_) {
        std::cerr << "SharedMemoryQueue: message too large for slot (" << size << " bytes)" << std::endl;
        return false;
    }

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
    if (msg_size + sizeof(size_t) > message_size_) {
        std::cerr << "SharedMemoryQueue: corrupt message size (" << msg_size << " bytes)" << std::endl;
        shared_data_->tail.store((tail + 1) % capacity_);
        shared_data_->not_full.notify_one();
        return false;
    }
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
    if (msg_size + sizeof(size_t) > message_size_) {
        std::cerr << "SharedMemoryQueue: corrupt message size (" << msg_size << " bytes)" << std::endl;
        shared_data_->tail.store((tail + 1) % capacity_);
        shared_data_->not_full.notify_one();
        return false;
    }
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
                                         uint64_t quantity, double price, const std::string& auth_token) {
    if (!queue_) return false;

    if (auth_token.empty()) {
        std::cerr << "SharedMemoryQueue: auth token is required for order submission" << std::endl;
        return false;
    }

    auto fits_u32 = [](size_t value) {
        return value <= static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    };

    if (!fits_u32(order_id.size()) || !fits_u32(user_id.size()) ||
        !fits_u32(symbol.size()) || !fits_u32(auth_token.size())) {
        std::cerr << "SharedMemoryQueue: field size exceeds protocol limits" << std::endl;
        return false;
    }

    // Calculate message size
    auto safe_add = [](size_t base, size_t value, bool& ok) {
        if (!ok || value > std::numeric_limits<size_t>::max() - base) {
            ok = false;
            return base;
        }
        return base + value;
    };

    bool add_ok = true;
    size_t variable_size = 0;
    variable_size = safe_add(variable_size, order_id.size(), add_ok);
    variable_size = safe_add(variable_size, user_id.size(), add_ok);
    variable_size = safe_add(variable_size, symbol.size(), add_ok);
    variable_size = safe_add(variable_size, auth_token.size(), add_ok);

    if (!add_ok || variable_size > std::numeric_limits<size_t>::max() - sizeof(SharedOrderMessage)) {
        std::cerr << "SharedMemoryQueue: message too large" << std::endl;
        return false;
    }

    size_t message_size = sizeof(SharedOrderMessage) + variable_size;
    std::vector<char> message(message_size);

    SharedOrderMessage* order_msg = reinterpret_cast<SharedOrderMessage*>(message.data());
    order_msg->message_length = message_size;
    order_msg->order_id_len = static_cast<uint32_t>(order_id.size());
    order_msg->user_id_len = static_cast<uint32_t>(user_id.size());
    order_msg->symbol_len = static_cast<uint32_t>(symbol.size());
    order_msg->auth_token_len = static_cast<uint32_t>(auth_token.size());
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
    string_data += symbol.size();
    std::memcpy(string_data, auth_token.data(), auth_token.size());

    return queue_->enqueue(message.data(), message_size);
}

// SharedMemoryOrderServer implementation
SharedMemoryOrderServer::SharedMemoryOrderServer(const std::string& shm_name, StockExchange* exchange,
                                               AuthenticationManager* auth_manager)
    : shm_name_(shm_name), exchange_(exchange), auth_manager_(auth_manager) {}

SharedMemoryOrderServer::~SharedMemoryOrderServer() {
    stop();
}

bool SharedMemoryOrderServer::start() {
    if (running_.exchange(true)) return false;

    try {
        if (!auth_manager_) {
            std::cerr << "SharedMemory: Authentication manager is required for shared memory order intake" << std::endl;
            running_.store(false);
            return false;
        }

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

                if (order_msg->message_length != message_size) {
                    std::cerr << "SharedMemory: message length mismatch, dropping order" << std::endl;
                    continue;
                }

                auto safe_accumulate = [](size_t base, size_t value, bool& ok) {
                    if (!ok || value > std::numeric_limits<size_t>::max() - base) {
                        ok = false;
                        return base;
                    }
                    return base + value;
                };

                bool len_ok = true;
                size_t payload_len = 0;
                payload_len = safe_accumulate(payload_len, order_msg->order_id_len, len_ok);
                payload_len = safe_accumulate(payload_len, order_msg->user_id_len, len_ok);
                payload_len = safe_accumulate(payload_len, order_msg->symbol_len, len_ok);
                payload_len = safe_accumulate(payload_len, order_msg->auth_token_len, len_ok);

                if (!len_ok) {
                    std::cerr << "SharedMemory: message field lengths overflow, dropping order" << std::endl;
                    continue;
                }

                if (payload_len > message_size - sizeof(SharedOrderMessage)) {
                    std::cerr << "SharedMemory: message truncated, dropping order" << std::endl;
                    continue;
                }

                // Extract strings
                size_t offset = sizeof(SharedOrderMessage);
                std::string order_id(buffer.data() + offset, order_msg->order_id_len);
                offset += order_msg->order_id_len;

                std::string user_id(buffer.data() + offset, order_msg->user_id_len);
                offset += order_msg->user_id_len;

                std::string symbol(buffer.data() + offset, order_msg->symbol_len);
                offset += order_msg->symbol_len;

                std::string auth_token(buffer.data() + offset, order_msg->auth_token_len);

                if (auth_token.empty()) {
                    std::cerr << "SharedMemory: missing auth token, order rejected" << std::endl;
                    continue;
                }

                if (!auth_manager_) {
                    std::cerr << "SharedMemory: no authentication manager configured, order rejected" << std::endl;
                    continue;
                }

                ConnectionId conn_id = automation_connection_id_.fetch_sub(1);
                AuthResult auth_result = auth_manager_->authenticateConnection(conn_id, auth_token);

                if (auth_result != AuthResult::SUCCESS && auth_result != AuthResult::ALREADY_AUTHENTICATED) {
                    std::cerr << "SharedMemory: token authentication failed for order " << order_id << std::endl;
                    auth_manager_->removeSession(conn_id);
                    continue;
                }

                UserId authenticated_user = auth_manager_->getUserId(conn_id);
                auth_manager_->removeSession(conn_id);

                if (authenticated_user.empty()) {
                    std::cerr << "SharedMemory: authenticated user not found for order " << order_id << std::endl;
                    continue;
                }

                if (!user_id.empty() && user_id != authenticated_user) {
                    std::cerr << "SharedMemory: user mismatch for order " << order_id
                              << ", token belongs to " << authenticated_user << std::endl;
                    continue;
                }

                // Convert to core Order
                Order core_order{
                    order_id,
                    authenticated_user,
                    symbol,
                    static_cast<int>(order_msg->side),
                    static_cast<int>(order_msg->order_type),
                    static_cast<int64_t>(order_msg->quantity),
                    static_cast<Price>(order_msg->price * 100.0 + 0.5), // Convert double to Price
                    static_cast<int64_t>(order_msg->timestamp_ms)
                };

                // CRITICAL FIX: Check the result of submitOrder to detect rejections
                // Previously we ignored the return value, causing silent failures for
                // shared memory clients (risk rejections, validation errors, etc.)
                std::string result = exchange_->submitOrder(symbol, core_order);
                if (result != "accepted") {
                    std::cerr << "SharedMemory: Order " << order_id << " rejected: " << result << std::endl;
                    // TODO: Consider implementing a response queue for shared memory clients
                    // to receive rejection notifications (currently there's no ack mechanism)
                }
                
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

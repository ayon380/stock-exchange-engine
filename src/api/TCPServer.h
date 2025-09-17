#pragma once
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "../core_engine/StockExchange.h"
#include "../core_engine/MemoryPool.h"
#include "../core_engine/LockFreeQueue.h"
#include "AuthenticationManager.h"

// Binary protocol message types
enum class MessageType : uint8_t {
    LOGIN_REQUEST = 1,
    LOGIN_RESPONSE = 2,
    SUBMIT_ORDER = 3,
    ORDER_RESPONSE = 4,
    HEARTBEAT = 5,
    HEARTBEAT_ACK = 6
};

// Binary order request structure (packed for network efficiency)
#pragma pack(push, 1)
struct BinaryOrderRequest {
    uint32_t message_length;  // Total message length
    MessageType type;         // Message type
    uint32_t order_id_len;    // Length of order_id string
    uint32_t user_id_len;     // Length of user_id string
    uint32_t symbol_len;      // Length of symbol string
    uint8_t side;             // 0=BUY, 1=SELL
    uint8_t order_type;       // 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    uint64_t quantity;        // Order quantity
    double price;             // Order price
    uint64_t timestamp_ms;    // Timestamp
    // Variable-length strings follow: order_id, user_id, symbol
};

// Binary order request body (without message_length field, used after reading header)
struct BinaryOrderRequestBody {
    MessageType type;         // Message type
    uint32_t order_id_len;    // Length of order_id string
    uint32_t user_id_len;     // Length of user_id string
    uint32_t symbol_len;      // Length of symbol string
    uint8_t side;             // 0=BUY, 1=SELL
    uint8_t order_type;       // 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    uint64_t quantity;        // Order quantity
    double price;             // Order price
    uint64_t timestamp_ms;    // Timestamp
    // Variable-length strings follow: order_id, user_id, symbol
};
#pragma pack(pop)

// Binary order response structure
#pragma pack(push, 1)
struct BinaryOrderResponse {
    uint32_t message_length;  // Total message length
    MessageType type;         // Message type
    uint32_t order_id_len;    // Length of order_id string
    uint8_t accepted;         // 1=accepted, 0=rejected
    uint32_t message_len;     // Length of message string
    // Variable-length strings follow: order_id, message
};
#pragma pack(pop)

// TCP connection for handling client sessions
class TCPConnection : public std::enable_shared_from_this<TCPConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    std::array<char, 8192> buffer_;
    std::vector<char> message_buffer_;
    StockExchange* exchange_;
    AuthenticationManager* auth_manager_;
    std::atomic<bool> connected_{true};
    ConnectionId connection_id_;

    // For batched responses
    std::vector<std::vector<char>> pending_responses_;
    std::mutex response_mutex_;

    // Batching for order processing
    std::vector<std::vector<char>> pending_orders_;
    std::mutex batch_mutex_;
    std::condition_variable batch_cv_;
    static constexpr size_t MAX_BATCH_SIZE = 64;
    static constexpr std::chrono::microseconds BATCH_TIMEOUT{100};

public:
    TCPConnection(boost::asio::ip::tcp::socket socket, StockExchange* exchange, AuthenticationManager* auth_manager);
    ~TCPConnection();

    void start();
    void stop();
    ConnectionId getConnectionId() const { return connection_id_; }

private:
    void readHeader();
    void readBody(size_t expected_length);
    void processMessage(const std::vector<char>& data);
    void processLoginRequest(const std::vector<char>& data);
    void processOrderRequest(const std::vector<char>& data);
    void sendResponse(const std::vector<char>& response, uint64_t start_cycles = 0);
    void handleError(const boost::system::error_code& error);
};

// High-performance TCP server for order submission
class TCPServer {
private:
    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> running_{false};
    StockExchange* exchange_;
    AuthenticationManager* auth_manager_;

    // Work guard to keep io_context alive
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;

    // Connection management
    std::unordered_map<std::string, std::shared_ptr<TCPConnection>> connections_;
    std::mutex connections_mutex_;

    // Connection ID counter
    std::atomic<ConnectionId> next_connection_id_{1};

    // Batching configuration
    static constexpr size_t MAX_BATCH_SIZE = 64;
    static constexpr std::chrono::microseconds BATCH_TIMEOUT{100};
    
    // Batch processing
    std::vector<std::vector<char>> pending_orders_;
    std::mutex batch_mutex_;
    std::condition_variable batch_cv_;
    std::thread batch_processor_;
    std::atomic<bool> batch_processor_running_{false};

public:
    TCPServer(const std::string& address, uint16_t port, StockExchange* exchange, AuthenticationManager* auth_manager);
    ~TCPServer();

    void start();
    void stop();

private:
    void acceptConnection();
    void workerThread();
    void batchProcessorThread();
    void processOrderBatch(const std::vector<char>& data);
    void cleanupConnections();
};

#include "TCPServer.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <boost/bind/bind.hpp>

// Helper function for 64-bit network to host byte order conversion
uint64_t ntohll(uint64_t value) {
#if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(_WIN32)
    uint32_t high = ntohl(static_cast<uint32_t>(value >> 32));
    uint32_t low = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFF));
    return (static_cast<uint64_t>(low) << 32) | high;
#else
    return value;
#endif
}

// Helper function to convert uint64 bits (network order) to double (host order)
double uint64_to_double(uint64_t value) {
    uint64_t host_value = ntohll(value);
    double d;
    std::memcpy(&d, &host_value, sizeof(d));
    return d;
}

// TCPConnection implementation
TCPConnection::TCPConnection(boost::asio::ip::tcp::socket socket, StockExchange* exchange)
    : socket_(std::move(socket)), exchange_(exchange) {
}

TCPConnection::~TCPConnection() {
    stop();
}

void TCPConnection::start() {
    connected_.store(true);
    readHeader();
}

void TCPConnection::stop() {
    if (!connected_.exchange(false)) return;

    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

void TCPConnection::readHeader() {
    if (!connected_.load()) return;

    auto self = shared_from_this();
    boost::asio::async_read(socket_,
        boost::asio::buffer(buffer_.data(), sizeof(uint32_t)),
        [this, self](const boost::system::error_code& error, std::size_t bytes_transferred) {
            if (error) {
                handleError(error);
                return;
            }

            if (bytes_transferred != sizeof(uint32_t)) {
                handleError(boost::asio::error::make_error_code(boost::asio::error::misc_errors::eof));
                return;
            }

            uint32_t message_length;
            std::memcpy(&message_length, buffer_.data(), sizeof(uint32_t));
            message_length = ntohl(message_length); // Network to host byte order

            // The message_length includes the 4-byte length header, so subtract it
            size_t body_length = message_length - sizeof(uint32_t);

            if (message_length > 8192 || body_length < sizeof(BinaryOrderRequestBody)) {
                std::cerr << "TCP Connection: Invalid message length: " << message_length 
                         << " (body: " << body_length << ")" << std::endl;
                handleError(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
                return;
            }

            readBody(body_length);
        });
}

void TCPConnection::readBody(size_t expected_length) {
    if (!connected_.load()) return;

    message_buffer_.resize(expected_length);
    auto self = shared_from_this();

    boost::asio::async_read(socket_,
        boost::asio::buffer(message_buffer_.data(), expected_length),
        [=](const boost::system::error_code& error, std::size_t bytes_transferred) {
            if (error) {
                handleError(error);
                return;
            }

            if (bytes_transferred != expected_length) {
                handleError(boost::asio::error::make_error_code(boost::asio::error::misc_errors::eof));
                return;
            }

            // Process message immediately instead of batching
            processMessage(message_buffer_);
            
            readHeader(); // Continue reading next message
        });
}

void TCPConnection::processMessage(const std::vector<char>& data) {
    if (data.size() < sizeof(BinaryOrderRequestBody)) {
        std::cerr << "TCP Connection: Message too small: " << data.size() 
                  << " bytes, expected at least " << sizeof(BinaryOrderRequestBody) << std::endl;
        return; // Invalid message
    }

    const BinaryOrderRequestBody* request = reinterpret_cast<const BinaryOrderRequestBody*>(data.data());

    // Convert from network byte order
    uint32_t order_id_len = ntohl(request->order_id_len);
    uint32_t user_id_len = ntohl(request->user_id_len); 
    uint32_t symbol_len = ntohl(request->symbol_len);
    
    std::cout << "TCP Connection: Processing message - type: " << static_cast<int>(request->type)
              << ", order_id_len: " << order_id_len 
              << ", user_id_len: " << user_id_len
              << ", symbol_len: " << symbol_len << std::endl;

    if (request->type != MessageType::SUBMIT_ORDER) {
        // Handle other message types (heartbeat, etc.)
        if (request->type == MessageType::HEARTBEAT) {
            // Send heartbeat ack
            std::vector<char> response(sizeof(BinaryOrderResponse) + 1);
            BinaryOrderResponse* resp = reinterpret_cast<BinaryOrderResponse*>(response.data());
            resp->message_length = htonl(sizeof(BinaryOrderResponse) + 1);
            resp->type = MessageType::HEARTBEAT_ACK;
            resp->order_id_len = htonl(1);
            resp->accepted = 1;
            resp->message_len = htonl(1);
            response[sizeof(BinaryOrderResponse)] = 'P'; // Pong
            sendResponse(response);
        }
        return;
    }

    // Validate string lengths
    size_t expected_size = sizeof(BinaryOrderRequestBody) + order_id_len + user_id_len + symbol_len;
    if (data.size() < expected_size) {
        std::cerr << "TCP Connection: Message size mismatch - got: " << data.size() 
                  << ", expected: " << expected_size << std::endl;
        return;
    }

    // Extract strings from variable-length data
    size_t offset = sizeof(BinaryOrderRequestBody);
    std::string order_id(data.data() + offset, order_id_len);
    offset += order_id_len;

    std::string user_id(data.data() + offset, user_id_len);
    offset += user_id_len;

    std::string symbol(data.data() + offset, symbol_len);

    // Convert price from network byte order to host byte order
    uint64_t price_bits;
    std::memcpy(&price_bits, &request->price, sizeof(double));
    uint64_t host_price_bits = ntohll(price_bits);
    double correct_price;
    std::memcpy(&correct_price, &host_price_bits, sizeof(double));

    // Convert to core Order
    Order core_order{
        order_id,
        user_id,
        symbol,
        static_cast<int>(request->side),
        static_cast<int>(request->order_type),
        static_cast<int64_t>(ntohll(request->quantity)),
        correct_price,
        static_cast<int64_t>(ntohll(request->timestamp_ms))
    };

    // Submit order
    std::string result = exchange_->submitOrder(symbol, core_order);

    // Prepare response
    bool accepted = (result == "accepted");
    std::string message = accepted ? "Order accepted" : result;

    size_t response_size = sizeof(BinaryOrderResponse) + order_id.size() + message.size();
    std::vector<char> response(response_size);

    BinaryOrderResponse* resp = reinterpret_cast<BinaryOrderResponse*>(response.data());
    resp->message_length = htonl(response_size);
    resp->type = MessageType::ORDER_RESPONSE;
    resp->order_id_len = htonl(order_id.size());
    resp->accepted = accepted ? 1 : 0;
    resp->message_len = htonl(message.size());

    // Copy strings
    char* string_data = response.data() + sizeof(BinaryOrderResponse);
    std::memcpy(string_data, order_id.data(), order_id.size());
    string_data += order_id.size();
    std::memcpy(string_data, message.data(), message.size());

    sendResponse(response);
}

void TCPConnection::sendResponse(const std::vector<char>& response) {
    if (!connected_.load()) return;

    auto self = shared_from_this();
    boost::asio::async_write(socket_,
        boost::asio::buffer(response.data(), response.size()),
        [this, self](const boost::system::error_code& error, std::size_t /*bytes_transferred*/) {
            if (error) {
                handleError(error);
            }
        });
}

void TCPConnection::handleError(const boost::system::error_code& error) {
    if (error != boost::asio::error::eof &&
        error != boost::asio::error::connection_reset &&
        error != boost::asio::error::operation_aborted) {
        std::cerr << "TCP connection error: " << error.message() << " (code: " << error.value() << ")" << std::endl;
    }
    stop();
}

// TCPServer implementation
TCPServer::TCPServer(const std::string& address, uint16_t port, StockExchange* exchange)
    : acceptor_(io_context_), exchange_(exchange) {
    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(address, ec);
    if (ec) {
        std::cerr << "TCP Server: Invalid address '" << address << "': " << ec.message() << std::endl;
        throw std::runtime_error("Invalid address: " + address);
    }
    
    // Open the acceptor
    acceptor_.open(boost::asio::ip::tcp::v4(), ec);
    if (ec) {
        std::cerr << "TCP Server: Failed to open acceptor: " << ec.message() << std::endl;
        throw std::runtime_error("Failed to open acceptor: " + ec.message());
    }
    
    // Set socket options
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        std::cerr << "TCP Server: Warning - Failed to set reuse_address: " << ec.message() << std::endl;
    }
    
    // Bind to address and port
    boost::asio::ip::tcp::endpoint endpoint(addr, port);
    acceptor_.bind(endpoint, ec);
    if (ec) {
        std::cerr << "TCP Server: Failed to bind to " << address << ":" << port << " - " << ec.message() << std::endl;
        throw std::runtime_error("Failed to bind to " + address + ":" + std::to_string(port) + " - " + ec.message());
    }
    
    // Start listening
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "TCP Server: Failed to listen: " << ec.message() << std::endl;
        throw std::runtime_error("Failed to listen: " + ec.message());
    }
    
    std::cout << "TCP Server: Successfully initialized on " << address << ":" << port << std::endl;
}

TCPServer::~TCPServer() {
    stop();
}

void TCPServer::start() {
    if (running_.exchange(true)) return;

    std::cout << "Starting TCP order server..." << std::endl;
    
    try {
        // Reset io_context in case it was stopped before
        io_context_.restart();

        // Create work guard to keep io_context alive
        work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(io_context_));

        // Start accepting connections BEFORE starting worker threads
        std::cout << "TCP Server: Starting to accept connections" << std::endl;
        acceptConnection();

        // Start worker threads
        unsigned int num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;

        std::cout << "TCP Server: Starting " << num_threads << " worker threads" << std::endl;

        for (unsigned int i = 0; i < num_threads; ++i) {
            worker_threads_.emplace_back(&TCPServer::workerThread, this);
        }
        
        std::cout << "TCP Server: Successfully started and accepting connections" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "TCP Server: Failed to start: " << e.what() << std::endl;
        running_.store(false);
        throw;
    }
}

void TCPServer::stop() {
    if (!running_.exchange(false)) return;

    std::cout << "Stopping TCP order server..." << std::endl;

    // Release work guard first to allow io_context to stop
    work_guard_.reset();
    
    io_context_.stop();

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    cleanupConnections();
}

void TCPServer::acceptConnection() {
    if (!running_.load()) return;
    
    acceptor_.async_accept(
        [this](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket) {
            if (error) {
                if (error != boost::asio::error::operation_aborted && running_.load()) {
                    std::cerr << "TCP Server: Accept error: " << error.message() << std::endl;
                }
            } else if (running_.load()) {
                std::cout << "TCP Server: New client connected from " 
                         << socket.remote_endpoint().address().to_string() 
                         << ":" << socket.remote_endpoint().port() << std::endl;
                         
                auto connection = std::make_shared<TCPConnection>(std::move(socket), exchange_);
                connection->start();

                // Store connection for cleanup
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_[std::to_string(reinterpret_cast<uintptr_t>(connection.get()))] = connection;
                }
            }

            if (running_.load()) {
                acceptConnection(); // Continue accepting
            }
        });
}

void TCPServer::workerThread() {
    try {
        io_context_.run();
    } catch (const std::exception& e) {
        std::cerr << "TCP server worker thread error: " << e.what() << std::endl;
    }
}

void TCPServer::cleanupConnections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& pair : connections_) {
        pair.second->stop();
    }
    connections_.clear();
}

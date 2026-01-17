/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#include "TCPServer.h"
#include "common/EngineTelemetry.h"
#include "common/EngineLogging.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <future>
#include <boost/bind/bind.hpp>
#ifdef _WIN32
#include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif
#include <thread>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include "AuthenticationManager.h"

// CPU frequency calibration for RDTSC timing
static uint64_t get_cpu_freq_hz() {
    static uint64_t freq = 0;
    if (freq == 0) {
#if defined(__x86_64__) || defined(_M_X64) || defined(_WIN32)
        auto chrono_start = std::chrono::high_resolution_clock::now();
        uint64_t rdtsc_start = get_cycles();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t rdtsc_end = get_cycles();
        auto chrono_end = std::chrono::high_resolution_clock::now();
        auto time_us = std::chrono::duration_cast<std::chrono::microseconds>(chrono_end - chrono_start).count();
        freq = (rdtsc_end - rdtsc_start) * 1000000ULL / time_us;
#else
        // For non-x86 platforms, use a reasonable default (3 GHz)
        freq = 3000000000ULL;
#endif
    }
    return freq;
}

// Cross-platform cycle counter
static inline uint64_t get_cycles() {
#if defined(__x86_64__) || defined(_M_X64) || defined(_WIN32)
    return __rdtsc();
#else
    // For non-x86 platforms, use high_resolution_clock as cycle approximation
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}

// Get performance log file
static std::ofstream& get_perf_file() {
    static std::ofstream perf_file("performance.txt", std::ios::app);
    return perf_file;
}

static std::mutex& get_perf_mutex() {
    static std::mutex perf_mutex;
    return perf_mutex;
}

static void append_perf_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(get_perf_mutex());
    auto& file = get_perf_file();
    file << line << '\n';
    file.flush();
}

// Helper function for 64-bit network to host byte order conversion
#ifndef ntohll
uint64_t ntohll(uint64_t value) {
#if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(_WIN32)
    uint32_t high = ntohl(static_cast<uint32_t>(value >> 32));
    uint32_t low = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFF));
    return (static_cast<uint64_t>(low) << 32) | high;
#else
    return value;
#endif
}
#endif

// Helper function to convert uint64 bits (network order) to double (host order)
double uint64_to_double(uint64_t value) {
    uint64_t host_value = ntohll(value);
    double d;
    std::memcpy(&d, &host_value, sizeof(d));
    return d;
}

// TCPConnection implementation
TCPConnection::TCPConnection(boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket, 
                             StockExchange* exchange, 
                             AuthenticationManager* auth_manager)
    : ssl_socket_(std::move(socket)), exchange_(exchange), auth_manager_(auth_manager) {
    // Generate unique connection ID (using socket native handle or a counter)
    connection_id_ = static_cast<ConnectionId>(ssl_socket_.lowest_layer().native_handle());
}

TCPConnection::~TCPConnection() {
    if (auth_manager_) {
        auth_manager_->removeSession(connection_id_);
    }
    stop();
}

void TCPConnection::start() {
    connected_.store(true);
    
    // Perform SSL/TLS handshake before reading data
    auto self = shared_from_this();
    ssl_socket_.async_handshake(boost::asio::ssl::stream_base::server,
        [this, self](const boost::system::error_code& error) {
            if (!error) {
                ENGINE_LOG_DEV(std::cout << "TCP Connection: SSL handshake successful for connection " 
                                         << connection_id_ << std::endl;);
                readHeader();  // Now all communication is encrypted!
            } else {
                std::cerr << "TCP Connection: SSL handshake failed: " << error.message() << std::endl;
                handleError(error);
            }
        });
}

void TCPConnection::stop() {
    if (!connected_.exchange(false)) return;

    boost::system::error_code ec;
    
    // Shutdown SSL stream first
    ssl_socket_.async_shutdown([](const boost::system::error_code&) {
        // SSL shutdown complete (ignore errors during shutdown)
    });
    
    // Then shutdown the underlying socket
    ssl_socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    ssl_socket_.lowest_layer().close(ec);
}

void TCPConnection::readHeader() {
    if (!connected_.load()) return;

    auto self = shared_from_this();
    
    try {
        boost::asio::async_read(ssl_socket_,
            boost::asio::buffer(buffer_.data(), sizeof(uint32_t)),
            [this, self](const boost::system::error_code& error, std::size_t bytes_transferred) {
                try {
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

                    // Guard against malformed frames that are smaller than the header itself
                    if (message_length < sizeof(uint32_t) + sizeof(MessageType)) {
                        std::cerr << "TCP Connection: Invalid message length header: " << message_length << std::endl;
                        handleError(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
                        return;
                    }

                    // The message_length includes the 4-byte length header, so subtract it
                    size_t body_length = message_length - sizeof(uint32_t);

                    if (message_length > buffer_.size() || message_length > 8192 || body_length < sizeof(MessageType)) {
                        std::cerr << "TCP Connection: Invalid message length: " << message_length 
                                 << " (body: " << body_length << ")" << std::endl;
                        handleError(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
                        return;
                    }

                    readBody(body_length);
                } catch (const std::exception& e) {
                    std::cerr << "TCP Connection: Exception in readHeader callback: " << e.what() << std::endl;
                    stop();
                } catch (...) {
                    std::cerr << "TCP Connection: Unknown exception in readHeader callback" << std::endl;
                    stop();
                }
            });
    } catch (const std::exception& e) {
        std::cerr << "TCP Connection: Exception in readHeader: " << e.what() << std::endl;
        stop();
    } catch (...) {
        std::cerr << "TCP Connection: Unknown exception in readHeader" << std::endl;
        stop();
    }
}

void TCPConnection::readBody(size_t expected_length) {
    if (!connected_.load()) return;

    try {
        message_buffer_.resize(expected_length);
        auto self = shared_from_this();

        boost::asio::async_read(ssl_socket_,
            boost::asio::buffer(message_buffer_.data(), expected_length),
            [this, self, expected_length](const boost::system::error_code& error, std::size_t bytes_transferred) {
                try {
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
                } catch (const std::exception& e) {
                    std::cerr << "TCP Connection: Exception in readBody callback: " << e.what() << std::endl;
                    stop();
                } catch (...) {
                    std::cerr << "TCP Connection: Unknown exception in readBody callback" << std::endl;
                    stop();
                }
            });
    } catch (const std::exception& e) {
        std::cerr << "TCP Connection: Exception in readBody: " << e.what() << std::endl;
        stop();
    } catch (...) {
        std::cerr << "TCP Connection: Unknown exception in readBody" << std::endl;
        stop();
    }
}

void TCPConnection::processMessage(const std::vector<char>& data) {
    try {
        if (data.empty()) {
            std::cerr << "TCP Connection: Empty message received" << std::endl;
            return;
        }

        // Peek at message type
        if (data.size() < sizeof(MessageType)) {
            std::cerr << "TCP Connection: Message too small to determine type" << std::endl;
            return;
        }

        MessageType msg_type = *reinterpret_cast<const MessageType*>(data.data());
        
        switch (msg_type) {
            case MessageType::LOGIN_REQUEST:
                processLoginRequest(data);
            break;
            
        case MessageType::SUBMIT_ORDER:
            // Check if user is authenticated first
            if (!auth_manager_->isAuthenticated(connection_id_)) {
                std::cerr << "TCP Connection: Order received from unauthenticated connection " << connection_id_ << std::endl;
                // Send rejection response
                std::vector<char> response(sizeof(BinaryOrderResponse) + 20);
                BinaryOrderResponse* resp = reinterpret_cast<BinaryOrderResponse*>(response.data());
                resp->message_length = htonl(sizeof(BinaryOrderResponse) + 20);
                resp->type = MessageType::ORDER_RESPONSE;
                resp->order_id_len = htonl(0);
                resp->accepted = 0;
                resp->message_len = htonl(20);
                std::memcpy(response.data() + sizeof(BinaryOrderResponse), "Not authenticated", 17);
                sendResponse(response);
                return;
            }
            processOrderRequest(data);
            break;
            
        case MessageType::HEARTBEAT:
            // Update last activity and send heartbeat ack
            auth_manager_->updateLastActivity(connection_id_);
            {
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
            break;
            
        default:
            std::cerr << "TCP Connection: Unknown message type: " << static_cast<int>(msg_type) << std::endl;
            break;
        }
    } catch (const std::exception& e) {
        std::cerr << "TCP Connection: Exception in processMessage: " << e.what() << std::endl;
        stop();
    } catch (...) {
        std::cerr << "TCP Connection: Unknown exception in processMessage" << std::endl;
        stop();
    }
}

void TCPConnection::sendResponse(const std::vector<char>& response, uint64_t start_cycles) {
    if (!connected_.load()) return;

    try {
        auto self = shared_from_this();
        boost::asio::async_write(ssl_socket_,
            boost::asio::buffer(response.data(), response.size()),
            [this, self, start_cycles](const boost::system::error_code& error, std::size_t /*bytes_transferred*/) {
                try {
                    if (error) {
                        handleError(error);
                    } else if (start_cycles != 0) {
                        uint64_t end_cycles = get_cycles();
                        uint64_t total_cycles = end_cycles - start_cycles;
                        long long total_us = total_cycles * 1000000LL / get_cpu_freq_hz();
                        append_perf_log("Total trip time: " + std::to_string(total_us) + " microseconds");
                        
                        // Log slow total trip
                        if (total_us > 10000) {
                            append_perf_log("Slow total trip: " + std::to_string(total_us) + "us");
                        }
                        
                        // CRITICAL FIX: Use thread_local to prevent data race across worker threads
                        // Each thread maintains its own latency collection
                        thread_local std::vector<long long> total_latencies;
                        total_latencies.push_back(total_us);
                        if (total_latencies.size() >= 50) {
                            std::sort(total_latencies.begin(), total_latencies.end());
                            long long p50 = total_latencies[25];
                            long long p90 = total_latencies[45];
                            long long p99 = total_latencies[49];
                            long long max_lat = total_latencies.back();
                            std::ostringstream oss;
                            oss << "Total trip latency percentiles: p50=" << p50
                                << "us, p90=" << p90
                                << "us, p99=" << p99
                                << "us, max=" << max_lat << "us";
                            append_perf_log(oss.str());
                            total_latencies.clear();
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "TCP Connection: Exception in sendResponse callback: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "TCP Connection: Unknown exception in sendResponse callback" << std::endl;
                }
            });
    } catch (const std::exception& e) {
        std::cerr << "TCP Connection: Exception in sendResponse: " << e.what() << std::endl;
        stop();
    } catch (...) {
        std::cerr << "TCP Connection: Unknown exception in sendResponse" << std::endl;
        stop();
    }
}

void TCPConnection::handleError(const boost::system::error_code& error) {
    // Silently handle expected disconnection errors
    bool is_expected_disconnect = (
        error == boost::asio::error::eof ||
        error == boost::asio::error::connection_reset ||
        error == boost::asio::error::operation_aborted ||
        error == boost::asio::error::broken_pipe ||
        error.value() == 1 ||  // stream truncated (SSL short read)
        error.category() == boost::asio::error::get_ssl_category()
    );
    
    if (!is_expected_disconnect) {
        std::cerr << "TCP connection error: " << error.message() 
                  << " (code: " << error.value() << ")" << std::endl;
    } else {
        ENGINE_LOG_DEV(std::cout << "TCP connection closed normally for connection " 
                                  << connection_id_ << std::endl;);
    }
    
    stop();
    
    // CRITICAL FIX: Remove connection from server's connection map when an error occurs
    // This ensures the shared_ptr is released and the TCPConnection destructor runs,
    // which calls auth_manager_->removeSession() to prevent session hijacking
    // (Without this, stale connections stay in the map and sessions persist across reconnects)
    if (server_cleanup_callback_) {
        server_cleanup_callback_(connection_id_);
    }
}

void TCPConnection::processLoginRequest(const std::vector<char>& data) {
    try {
        if (data.size() < sizeof(BinaryLoginRequestBody)) {
            std::cerr << "TCP Connection: Login message too small" << std::endl;
            return;
        }

        const BinaryLoginRequestBody* request = reinterpret_cast<const BinaryLoginRequestBody*>(data.data());
        uint32_t token_len = ntohl(request->token_len);
        
        if (data.size() < sizeof(BinaryLoginRequestBody) + token_len) {
            std::cerr << "TCP Connection: Login message size mismatch" << std::endl;
            return;
        }

        // Extract JWT token
        std::string jwt_token(data.data() + sizeof(BinaryLoginRequestBody), token_len);
        
        ENGINE_LOG_DEV(std::cout << "TCP Connection: Processing login request for connection "
                                 << connection_id_ << std::endl;);
        
        // Authenticate with AuthenticationManager
        AuthResult auth_result = auth_manager_->authenticateConnection(connection_id_, jwt_token);
        
        bool success = (auth_result == AuthResult::SUCCESS);
        std::string message;
        
        switch (auth_result) {
            case AuthResult::SUCCESS:
                message = "Login successful";
                break;
            case AuthResult::INVALID_TOKEN:
                message = "Invalid or expired token";
                break;
            case AuthResult::REDIS_ERROR:
                message = "Authentication service error";
                break;
            case AuthResult::USER_NOT_FOUND:
                message = "User account not found";
                break;
            case AuthResult::ALREADY_AUTHENTICATED:
                message = "Already authenticated";
                success = true; // Treat as success
                break;
            default:
                message = "Unknown authentication error";
                break;
        }
        
        // Prepare response
        size_t response_size = sizeof(BinaryLoginResponse) + message.size();
        std::vector<char> response(response_size);
        
        BinaryLoginResponse* resp = reinterpret_cast<BinaryLoginResponse*>(response.data());
        resp->message_length = htonl(response_size);
        resp->type = static_cast<uint8_t>(MessageType::LOGIN_RESPONSE);
        resp->success = success ? 1 : 0;
        resp->message_len = htonl(message.size());
        
        // Copy message
        std::memcpy(response.data() + sizeof(BinaryLoginResponse), message.data(), message.size());
        
        sendResponse(response);
        
        if (!success) {
            // Close connection on failed authentication
            std::cerr << "TCP Connection: Authentication failed for connection " << connection_id_ << ": " << message << std::endl;
            stop();
        } else {
        ENGINE_LOG_DEV(std::cout << "TCP Connection: Authentication successful for connection "
                     << connection_id_ << std::endl;);
        }
    } catch (const std::exception& e) {
        std::cerr << "TCP Connection: Exception in processLoginRequest: " << e.what() << std::endl;
        stop();
    } catch (...) {
        std::cerr << "TCP Connection: Unknown exception in processLoginRequest" << std::endl;
        stop();
    }
}

void TCPConnection::processOrderRequest(const std::vector<char>& data) {
    try {
        // Sample metrics every 1000 orders to minimize overhead
        static std::atomic<uint64_t> metrics_counter{0};
        uint64_t current_count = metrics_counter.fetch_add(1, std::memory_order_relaxed);
        bool should_measure = (current_count % 1000 == 0);

        uint64_t total_start_cycles = 0;
        if (should_measure) {
            total_start_cycles = get_cycles();
        }

        if (data.size() < sizeof(BinaryOrderRequestBody)) {
            std::cerr << "TCP Connection: Order message too small: " << data.size() 
                      << " bytes, expected at least " << sizeof(BinaryOrderRequestBody) << std::endl;
            return;
        }

    const BinaryOrderRequestBody* request = reinterpret_cast<const BinaryOrderRequestBody*>(data.data());

    // Convert from network byte order
    uint32_t order_id_len = ntohl(request->order_id_len);
    uint32_t user_id_len = ntohl(request->user_id_len); 
    uint32_t symbol_len = ntohl(request->symbol_len);
    
    // Validate string lengths
    size_t expected_size = sizeof(BinaryOrderRequestBody) + order_id_len + user_id_len + symbol_len;
    if (data.size() < expected_size) {
        std::cerr << "TCP Connection: Order message size mismatch - got: " << data.size() 
                  << ", expected: " << expected_size << std::endl;
        return;
    }

    // Extract strings from variable-length data
    size_t offset = sizeof(BinaryOrderRequestBody);
    std::string order_id(data.data() + offset, order_id_len);
    offset += order_id_len;

    std::string received_user_id(data.data() + offset, user_id_len);
    offset += user_id_len;

    std::string symbol(data.data() + offset, symbol_len);

    // Get authenticated user ID (this is the authoritative source)
    UserId authenticated_user_id = auth_manager_->getUserId(connection_id_);
    if (authenticated_user_id.empty()) {
        std::cerr << "TCP Connection: No authenticated user for connection " << connection_id_ << std::endl;
        return;
    }

    // Update last activity
    auth_manager_->updateLastActivity(connection_id_);

    // Convert price from network byte order to host byte order
    uint64_t price_bits;
    std::memcpy(&price_bits, &request->price, sizeof(double));
    uint64_t host_price_bits = ntohll(price_bits);
    double correct_price;
    std::memcpy(&correct_price, &host_price_bits, sizeof(double));

    int64_t quantity = static_cast<int64_t>(ntohll(request->quantity));
    
    // Check buying power using AuthenticationManager
    if (request->side == 0) { // BUY order
        CashAmount required_cash = quantity * static_cast<CashAmount>(correct_price * 100.0 + 0.5);
        if (!auth_manager_->checkBuyingPower(authenticated_user_id, required_cash)) {
            std::string message = "Insufficient buying power";
            
            size_t response_size = sizeof(BinaryOrderResponse) + order_id.size() + message.size();
            std::vector<char> response(response_size);

            BinaryOrderResponse* resp = reinterpret_cast<BinaryOrderResponse*>(response.data());
            resp->message_length = htonl(response_size);
            resp->type = MessageType::ORDER_RESPONSE;
            resp->order_id_len = htonl(order_id.size());
            resp->accepted = 0;
            resp->message_len = htonl(message.size());

            // Copy strings
            char* string_data = response.data() + sizeof(BinaryOrderResponse);
            std::memcpy(string_data, order_id.data(), order_id.size());
            string_data += order_id.size();
            std::memcpy(string_data, message.data(), message.size());

            sendResponse(response);
            return;
        }
    }

    // Convert to core Order (using authenticated user ID, not the one from message)
    Order core_order{
        order_id,
        authenticated_user_id,  // Use authenticated user ID
        symbol,
        static_cast<int>(request->side),
        static_cast<int>(request->order_type),
        quantity,
        static_cast<Price>(correct_price * 100.0 + 0.5), // Convert double to Price
        static_cast<int64_t>(ntohll(request->timestamp_ms))
    };

    // Submit order with timing (only when sampling)
    uint64_t engine_start_cycles = 0;
    if (should_measure) {
        engine_start_cycles = get_cycles();
    }
    
    std::string result = exchange_->submitOrder(symbol, core_order);
    
    long long engine_us = 0;
    if (should_measure) {
        uint64_t engine_end_cycles = get_cycles();
        uint64_t engine_cycles = engine_end_cycles - engine_start_cycles;
        engine_us = engine_cycles * 1000000LL / get_cpu_freq_hz();
        append_perf_log("Order execution time: " + std::to_string(engine_us) + " microseconds for order " + order_id);
        
        // Log slow orders
        if (engine_us > 1000) {
            append_perf_log("Slow order: " + order_id + " engine time " + std::to_string(engine_us) + "us");
        }
        
        // CRITICAL FIX: Use thread_local to prevent data race across worker threads
        // Each thread maintains its own latency collection
        thread_local std::vector<long long> engine_latencies;
        engine_latencies.push_back(engine_us);
        if (engine_latencies.size() >= 50) {
            std::sort(engine_latencies.begin(), engine_latencies.end());
            long long p50 = engine_latencies[25];
            long long p90 = engine_latencies[45];
            long long p99 = engine_latencies[49];
            long long max_lat = engine_latencies.back();
            std::ostringstream oss;
            oss << "Engine latency percentiles: p50=" << p50
                << "us, p90=" << p90
                << "us, p99=" << p99
                << "us, max=" << max_lat << "us";
            append_perf_log(oss.str());
            engine_latencies.clear();
        }
    }

    EngineTelemetry::instance().recordOrder(should_measure ? engine_us : -1);

    // Update account position if order was accepted
    bool accepted = (result == "accepted");
    // Prepare response
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

    sendResponse(response, should_measure ? total_start_cycles : 0);
    
    } catch (const std::exception& e) {
        std::cerr << "TCP Connection: Exception in processOrderRequest: " << e.what() << std::endl;
        // Don't stop connection on order processing error, just skip this order
    } catch (...) {
        std::cerr << "TCP Connection: Unknown exception in processOrderRequest" << std::endl;
        // Don't stop connection on order processing error, just skip this order
    }
}

// TCPServer implementation
TCPServer::TCPServer(const std::string& address, uint16_t port, 
                     StockExchange* exchange, 
                     AuthenticationManager* auth_manager,
                     const std::string& cert_file,
                     const std::string& key_file)
    : acceptor_(io_context_), 
      ssl_context_(boost::asio::ssl::context::tlsv13),
      exchange_(exchange), 
      auth_manager_(auth_manager) {
    
    try {
        // Configure SSL context with strong security settings
        ssl_context_.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 |
            boost::asio::ssl::context::single_dh_use);
        
        // Load server certificate and private key
        ssl_context_.use_certificate_chain_file(cert_file);
        ssl_context_.use_private_key_file(key_file, boost::asio::ssl::context::pem);
        
        std::cout << "✅ SSL/TLS enabled with certificate: " << cert_file << std::endl;
        std::cout << "🔒 All TCP connections will be encrypted" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "⚠️  SSL certificate not found. Creating self-signed certificate..." << std::endl;
        std::cerr << "   Run this command to generate certificates:" << std::endl;
        std::cerr << "   openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes" << std::endl;
        std::cerr << "   Error: " << e.what() << std::endl;
        throw std::runtime_error("SSL initialization failed: " + std::string(e.what()));
    }
    
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
    
    ENGINE_LOG_DEV(std::cout << "TCP Server: Successfully initialized on " << address << ":" << port << std::endl;);
}

TCPServer::~TCPServer() {
    stop();
}

void TCPServer::start() {
    if (running_.exchange(true)) return;

    ENGINE_LOG_DEV(std::cout << "Starting TCP order server..." << std::endl;);
    
    try {
        // Reset io_context in case it was stopped before
        io_context_.restart();

        // Create work guard to keep io_context alive
        work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(io_context_));

        // Start accepting connections BEFORE starting worker threads
    ENGINE_LOG_DEV(std::cout << "TCP Server: Starting to accept connections" << std::endl;);
        acceptConnection();

        // Start worker threads
        unsigned int num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;

    ENGINE_LOG_DEV(std::cout << "TCP Server: Starting " << num_threads << " worker threads" << std::endl;);

        for (unsigned int i = 0; i < num_threads; ++i) {
            worker_threads_.emplace_back(&TCPServer::workerThread, this);
        }
        
    ENGINE_LOG_DEV(std::cout << "TCP Server: Successfully started and accepting connections" << std::endl;);
        
    } catch (const std::exception& e) {
        std::cerr << "TCP Server: Failed to start: " << e.what() << std::endl;
        running_.store(false);
        throw;
    }
}

void TCPServer::stop() {
    if (!running_.exchange(false)) return;

    ENGINE_LOG_DEV(std::cout << "Stopping TCP order server..." << std::endl;);

    // Release work guard first to allow io_context to stop
    work_guard_.reset();
    
    io_context_.stop();

    // Join worker threads with timeout protection
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            auto future = std::async(std::launch::async, [&thread]() {
                thread.join();
            });
            
            if (future.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout) {
                std::cerr << "Warning: TCP worker thread timeout, detaching" << std::endl;
                thread.detach();
            }
        }
    }

    cleanupConnections();
}

void TCPServer::acceptConnection() {
    if (!running_.load()) return;
    
    // Create a new SSL socket for the incoming connection
    auto ssl_socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
        io_context_, ssl_context_);
    
    acceptor_.async_accept(ssl_socket->lowest_layer(),
        [this, ssl_socket](const boost::system::error_code& error) {
            if (error) {
                if (error != boost::asio::error::operation_aborted && running_.load()) {
                    std::cerr << "TCP Server: Accept error: " << error.message() << std::endl;
                }
            } else if (running_.load()) {
                ENGINE_LOG_DEV(std::cout << "TCP Server: New client connected from "
                                         << ssl_socket->lowest_layer().remote_endpoint().address().to_string()
                                         << ":" << ssl_socket->lowest_layer().remote_endpoint().port() 
                                         << " (SSL handshake pending...)" << std::endl;);
                           
                auto connection = std::make_shared<TCPConnection>(std::move(*ssl_socket), exchange_, auth_manager_);
                  
                // CRITICAL FIX: Set cleanup callback so connection can remove itself from map
                // This prevents stale connections from persisting and enables proper session cleanup
                ConnectionId conn_id = connection->getConnectionId();
                connection->setCleanupCallback([this, conn_id](ConnectionId id) {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_.erase(std::to_string(id));
                });
                  
                connection->start();

                // Store connection for cleanup
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_[std::to_string(conn_id)] = connection;
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

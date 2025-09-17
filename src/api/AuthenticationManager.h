#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <sw/redis++/redis++.h>
#include "absl/container/flat_hash_map.h"
#include "../core_engine/DatabaseManager.h"

// The string-based UUID for user identification
using UserId = std::string;
using ConnectionId = int; // Socket file descriptor or connection identifier

// Represents a user's financial state in memory
struct Account {
    std::atomic<double> cash;
    std::atomic<long> goog_position;
    std::atomic<long> aapl_position;
    std::atomic<long> tsla_position;
    std::atomic<long> msft_position;
    std::atomic<long> amzn_position;
    // Add more positions as needed
    
    // Risk metrics
    std::atomic<double> buying_power;
    std::atomic<double> day_trading_buying_power;
    std::atomic<int> day_trades_count;
    
    // Constructor to initialize atomic values
    Account(double initial_cash = 0.0) 
        : cash(initial_cash)
        , goog_position(0)
        , aapl_position(0)
        , tsla_position(0)
        , msft_position(0)
        , amzn_position(0)
        , buying_power(initial_cash)
        , day_trading_buying_power(initial_cash)
        , day_trades_count(0) {
    }
    
    // Copy constructor for atomic types
    Account(const Account& other)
        : cash(other.cash.load())
        , goog_position(other.goog_position.load())
        , aapl_position(other.aapl_position.load())
        , tsla_position(other.tsla_position.load())
        , msft_position(other.msft_position.load())
        , amzn_position(other.amzn_position.load())
        , buying_power(other.buying_power.load())
        , day_trading_buying_power(other.day_trading_buying_power.load())
        , day_trades_count(other.day_trades_count.load()) {
    }
};

// Represents a client's live connection state
struct Session {
    UserId userId;
    bool isAuthenticated = false;
    std::chrono::steady_clock::time_point lastActivity;
    
    Session() : lastActivity(std::chrono::steady_clock::now()) {}
    Session(const UserId& id) : userId(id), isAuthenticated(true), lastActivity(std::chrono::steady_clock::now()) {}
};

// Authentication result
enum class AuthResult {
    SUCCESS,
    INVALID_TOKEN,
    REDIS_ERROR,
    USER_NOT_FOUND,
    ALREADY_AUTHENTICATED
};

// Binary login request structure
#pragma pack(push, 1)
struct BinaryLoginRequest {
    uint32_t message_length;    // Total message length
    uint8_t type;               // Message type (LOGIN_REQUEST = 1)
    uint32_t token_len;         // Length of JWT token string
    // Variable-length JWT token follows
};

// Binary login request body (without message_length field, used after reading header)
struct BinaryLoginRequestBody {
    uint8_t type;               // Message type (LOGIN_REQUEST = 1)
    uint32_t token_len;         // Length of JWT token string
    // Variable-length JWT token follows
};

struct BinaryLoginResponse {
    uint32_t message_length;    // Total message length
    uint8_t type;               // Message type (LOGIN_RESPONSE = 2)
    uint8_t success;            // 1=success, 0=failure
    uint32_t message_len;       // Length of message string
    // Variable-length message follows
};
#pragma pack(pop)

// Main authentication and account management class
class AuthenticationManager {
private:
    // Redis connection for token validation
    std::unique_ptr<sw::redis::Redis> redis_client_;
    
    // Database manager for loading account data
    DatabaseManager* db_manager_;
    
    // The primary, high-performance map for all user accounts
    absl::flat_hash_map<UserId, std::unique_ptr<Account>> accounts_;
    std::shared_mutex accounts_mutex_; // Allow concurrent reads, exclusive writes
    
    // Maps a live connection to its authenticated session
    std::unordered_map<ConnectionId, Session> sessions_;
    std::mutex sessions_mutex_; // Protects the sessions map from concurrent access
    
    // Redis key prefix for trading tokens
    static constexpr const char* TRADING_TOKEN_PREFIX = "trading:";

public:
    AuthenticationManager(const std::string& redis_host, int redis_port, DatabaseManager* db_manager);
    ~AuthenticationManager();
    
    // Initialize Redis connection
    bool initialize();
    
    // Authentication methods
    AuthResult authenticateConnection(ConnectionId conn_id, const std::string& jwt_token);
    bool isAuthenticated(ConnectionId conn_id);
    UserId getUserId(ConnectionId conn_id);
    
    // Account management
    Account* getAccount(const UserId& user_id);
    bool loadAccountFromDatabase(const UserId& user_id);
    
    // Session management
    void removeSession(ConnectionId conn_id);
    void updateLastActivity(ConnectionId conn_id);
    void cleanupInactiveSessions(std::chrono::minutes timeout = std::chrono::minutes(30));
    
    // Risk management helpers
    bool checkBuyingPower(const UserId& user_id, double required_cash);
    bool updatePosition(const UserId& user_id, const std::string& symbol, long quantity_change, double price);
    
    // Statistics
    size_t getActiveSessionCount();
    size_t getLoadedAccountCount();

private:
    // Helper methods
    bool validateJWTWithRedis(const std::string& jwt_token, UserId& user_id);
    bool createAccountEntry(const UserId& user_id, const Account& account_data);
};

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
#include "../core_engine/Stock.h"

struct Trade;
struct Order;

// The string-based UUID for user identification
using UserId = std::string;
using ConnectionId = int; // Socket file descriptor or connection identifier

// Fixed-point arithmetic for financial calculations
using CashAmount = int64_t;

// Represents a user's financial state in memory
struct Account {
    std::atomic<CashAmount> cash;
    std::atomic<long> aapl_qty;
    std::atomic<long> googl_qty;
    std::atomic<long> msft_qty;
    std::atomic<long> amzn_qty;
    std::atomic<long> tsla_qty;
    // Add more positions as needed
    
    // Risk metrics
    std::atomic<CashAmount> buying_power;
    std::atomic<CashAmount> day_trading_buying_power;
    std::atomic<long long> total_trades;
    std::atomic<long long> realized_pnl;
    std::atomic<bool> is_active;
    std::atomic<CashAmount> reserved_cash;
    std::atomic<long> reserved_aapl;
    std::atomic<long> reserved_googl;
    std::atomic<long> reserved_msft;
    std::atomic<long> reserved_amzn;
    std::atomic<long> reserved_tsla;
    mutable std::mutex account_mutex;
    
    // Constructor to initialize atomic values
    Account(CashAmount initial_cash = 0) 
        : cash(initial_cash)
        , aapl_qty(0)
        , googl_qty(0)
        , msft_qty(0)
        , amzn_qty(0)
        , tsla_qty(0)
        , buying_power(initial_cash)
        , day_trading_buying_power(initial_cash)
        , total_trades(0)
        , realized_pnl(0)
        , is_active(true)
        , reserved_cash(0)
        , reserved_aapl(0)
        , reserved_googl(0)
        , reserved_msft(0)
        , reserved_amzn(0)
        , reserved_tsla(0) {
    }
    
    // Copy constructor for atomic types
    Account(const Account& other)
        : cash(other.cash.load())
        , aapl_qty(other.aapl_qty.load())
        , googl_qty(other.googl_qty.load())
        , msft_qty(other.msft_qty.load())
        , amzn_qty(other.amzn_qty.load())
        , tsla_qty(other.tsla_qty.load())
        , buying_power(other.buying_power.load())
        , day_trading_buying_power(other.day_trading_buying_power.load())
        , total_trades(other.total_trades.load())
        , realized_pnl(other.realized_pnl.load())
        , is_active(other.is_active.load())
        , reserved_cash(other.reserved_cash.load())
        , reserved_aapl(other.reserved_aapl.load())
        , reserved_googl(other.reserved_googl.load())
        , reserved_msft(other.reserved_msft.load())
        , reserved_amzn(other.reserved_amzn.load())
        , reserved_tsla(other.reserved_tsla.load()) {
    }

    // Helper functions for conversion
    static CashAmount fromDouble(double dollars) { return static_cast<CashAmount>(dollars * 100.0 + 0.5); }
    double cashToDouble() const { return static_cast<double>(cash.load()) / 100.0; }
    double buyingPowerToDouble() const { return static_cast<double>(buying_power.load()) / 100.0; }
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
    bool reserveForOrder(const Order& order, Price effective_price, std::string& reason);
    void releaseForOrder(const Order& order, const std::string& reason);
    void applyTrade(const Trade& trade);
    UserId getUserId(ConnectionId conn_id);
    
    // Account management
    Account* getAccount(const UserId& user_id);
    bool loadAccountFromDatabase(const UserId& user_id);
    
    // Session management
    void removeSession(ConnectionId conn_id);
    void updateLastActivity(ConnectionId conn_id);
    void cleanupInactiveSessions(std::chrono::minutes timeout = std::chrono::minutes(30));
    bool checkBuyingPower(const UserId& user_id, CashAmount required_cash);
    bool updatePosition(const UserId& user_id, const std::string& symbol, long quantity_change, Price price);
    
    // Database synchronization (called every 30 seconds)
    void syncAllAccountsToDatabase();
    
    // Statistics
    size_t getActiveSessionCount();
    size_t getLoadedAccountCount();

private:
    // Risk management helpers
    struct Reservation {
        CashAmount reserved_cash{0};
        int64_t reserved_quantity{0};
        std::string symbol;
        int side{0};
        Price price{0};
    };

    std::atomic<long>* locatePosition(Account* account, const std::string& symbol);
    std::atomic<long>* locateReservedPosition(Account* account, const std::string& symbol);
    void consumeReservationForTrade(const std::string& order_id,
                                    const UserId& user_id,
                                    Price execution_price,
                                    int64_t quantity,
                                    int side);
    absl::flat_hash_map<std::string, Reservation> order_reservations_;
    std::mutex reservations_mutex_;

    // Helper methods
    bool validateJWTWithRedis(const std::string& jwt_token, UserId& user_id);
    bool createAccountEntry(const UserId& user_id, const Account& account_data);
};

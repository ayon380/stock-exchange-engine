#pragma once
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <iostream>

// Fixed-point arithmetic types
using Price = int64_t;
using CashAmount = int64_t;

struct StockData {
    std::string symbol;
    Price last_price;
    Price open_price;
    int64_t volume;
    int64_t timestamp_ms;
    
    StockData() : symbol(""), last_price(0), open_price(0), volume(0), timestamp_ms(0) {}
    StockData(const std::string& sym, Price last, Price open, int64_t vol, int64_t ts)
        : symbol(sym), last_price(last), open_price(open), volume(vol), timestamp_ms(ts) {}

    // Helper functions for conversion
    static Price fromDouble(double dollars) { return static_cast<Price>(dollars * 100.0 + 0.5); }
    double lastPriceToDouble() const { return static_cast<double>(last_price) / 100.0; }
    double openPriceToDouble() const { return static_cast<double>(open_price) / 100.0; }
};

// Connection pool for thread-safe database access
class ConnectionPool {
private:
    std::queue<std::unique_ptr<pqxx::connection>> available_connections_;
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    std::string connection_string_;
    size_t pool_size_;
    std::atomic<size_t> active_connections_{0};
    
public:
    ConnectionPool(const std::string& connection_string, size_t pool_size = 5)
        : connection_string_(connection_string), pool_size_(pool_size) {}
    
    ~ConnectionPool() {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        while (!available_connections_.empty()) {
            available_connections_.pop();
        }
    }
    
    bool initialize() {
        try {
            for (size_t i = 0; i < pool_size_; ++i) {
                auto conn = std::make_unique<pqxx::connection>(connection_string_);
                if (!conn->is_open()) {
                    return false;
                }
                available_connections_.push(std::move(conn));
            }
            active_connections_ = pool_size_;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to initialize connection pool: " << e.what() << std::endl;
            return false;
        }
    }
    
    std::unique_ptr<pqxx::connection> acquire() {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        
        // Wait for an available connection
        pool_cv_.wait(lock, [this]() { return !available_connections_.empty(); });
        
        auto conn = std::move(available_connections_.front());
        available_connections_.pop();
        return conn;
    }
    
    void release(std::unique_ptr<pqxx::connection> conn) {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        available_connections_.push(std::move(conn));
        pool_cv_.notify_one();
    }
    
    size_t available_count() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(pool_mutex_));
        return available_connections_.size();
    }
};

// RAII wrapper for connection acquisition
class ScopedConnection {
private:
    ConnectionPool& pool_;
    std::unique_ptr<pqxx::connection> conn_;
    
public:
    ScopedConnection(ConnectionPool& pool) : pool_(pool), conn_(pool.acquire()) {}
    
    ~ScopedConnection() {
        if (conn_) {
            pool_.release(std::move(conn_));
        }
    }
    
    pqxx::connection* operator->() { return conn_.get(); }
    pqxx::connection& get() { return *conn_; }
    
    // Delete copy operations
    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
};

class DatabaseManager {
private:
    std::unique_ptr<ConnectionPool> connection_pool_;
    std::atomic<bool> running_;
    std::thread sync_thread_;
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;
    
    std::string connection_string_;
    std::chrono::seconds sync_interval_;
    size_t pool_size_;
    
    void initializeTables();
    void syncWorker();
    
public:
    DatabaseManager(const std::string& connection_string, 
                   std::chrono::seconds sync_interval = std::chrono::seconds(30),
                   size_t pool_size = 5);
    ~DatabaseManager();
    
    bool connect();
    void disconnect();
    
    void startBackgroundSync();
    void stopBackgroundSync();
    
    // Stock data operations
    bool saveStockData(const StockData& data);
    bool saveStockDataBatch(const std::vector<StockData>& data_batch);
    std::vector<StockData> loadStockData();
    StockData getLatestStockData(const std::string& symbol);
    
    // Order and trade persistence (can be extended)
    bool saveOrder(const std::string& order_data);
    bool saveTrade(const std::string& trade_data);
    
    // User account operations
    struct UserAccount {
        std::string user_id;
        CashAmount cash;
        long aapl_qty;
        long googl_qty;
        long msft_qty;
        long amzn_qty;
        long tsla_qty;
        CashAmount buying_power;
        CashAmount day_trading_buying_power;
        int64_t total_trades;
        int64_t realized_pnl;
        bool is_active;

        // Helper functions for conversion
        static CashAmount fromDouble(double dollars) { return static_cast<CashAmount>(dollars * 100.0 + 0.5); }
        double cashToDouble() const { return static_cast<double>(cash) / 100.0; }
        double buyingPowerToDouble() const { return static_cast<double>(buying_power) / 100.0; }
        
        UserAccount() : user_id(""), cash(0), aapl_qty(0), googl_qty(0), msft_qty(0), 
                       amzn_qty(0), tsla_qty(0), buying_power(0), day_trading_buying_power(0),
                       total_trades(0), realized_pnl(0), is_active(true) {}
    };
    
    bool loadUserAccount(const std::string& user_id, UserAccount& account);
    bool saveUserAccount(const UserAccount& account);
    bool createUserAccount(const std::string& user_id, CashAmount initial_cash = 10000000); // $100,000.00 in fixed-point
    UserAccount getUserAccount(const std::string& user_id); // Returns account or empty if not found
    bool updateUserAccount(const UserAccount& account); // Update existing account
    
    // Health check
    bool isConnected() const;
};

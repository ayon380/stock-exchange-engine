#pragma once
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

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

class DatabaseManager {
private:
    std::unique_ptr<pqxx::connection> conn_;
    std::atomic<bool> running_;
    std::thread sync_thread_;
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;
    
    std::string connection_string_;
    std::chrono::seconds sync_interval_;
    
    void initializeTables();
    void syncWorker();
    
public:
    DatabaseManager(const std::string& connection_string, 
                   std::chrono::seconds sync_interval = std::chrono::seconds(30));
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

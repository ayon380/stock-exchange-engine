#pragma once
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

struct StockData {
    std::string symbol;
    double last_price;
    double open_price;
    int64_t volume;
    int64_t timestamp_ms;
    
    StockData() : symbol(""), last_price(0.0), open_price(0.0), volume(0), timestamp_ms(0) {}
    StockData(const std::string& sym, double last, double open, int64_t vol, int64_t ts)
        : symbol(sym), last_price(last), open_price(open), volume(vol), timestamp_ms(ts) {}
};

class DatabaseManager {
private:
    std::unique_ptr<pqxx::connection> conn_;
    std::atomic<bool> running_;
    std::thread sync_thread_;
    
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
        double cash;
        long goog_position;
        long aapl_position;
        long tsla_position;
        long msft_position;
        long amzn_position;
        double buying_power;
        double day_trading_buying_power;
        int day_trades_count;
    };
    
    bool loadUserAccount(const std::string& user_id, UserAccount& account);
    bool saveUserAccount(const UserAccount& account);
    bool createUserAccount(const std::string& user_id, double initial_cash = 100000.0);
    
    // Health check
    bool isConnected() const;
};

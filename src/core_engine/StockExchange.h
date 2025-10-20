#pragma once
#include "Stock.h"
#include "DatabaseManager.h"
#include <map>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>

// Fixed-point arithmetic types (imported from Stock.h)
using Price = int64_t;
using CashAmount = int64_t;

class AuthenticationManager;

// Keep the old IndexEntry for backward compatibility
struct IndexEntry {
    std::string symbol;
    Price last_price;
    double change_pct;  // Keep as double for percentage
    int64_t volume;
    
    IndexEntry() : symbol(""), last_price(0), change_pct(0.0), volume(0) {}
    IndexEntry(const std::string& sym, Price price, double change, int64_t vol)
        : symbol(sym), last_price(price), change_pct(change), volume(vol) {}

    // Helper for conversion
    double priceToDouble() const { return static_cast<double>(last_price) / 100.0; }
};

struct IndexConstituent {
    std::string symbol;
    Price last_price;
    double weight;        // Weight in index calculation (0.0 to 1.0)
    double contribution;  // Points contributed to index
    double change_percent;
    
    IndexConstituent() : symbol(""), last_price(0), weight(0.0), contribution(0.0), change_percent(0.0) {}
    IndexConstituent(const std::string& sym, Price price, double w, double contrib, double change)
        : symbol(sym), last_price(price), weight(w), contribution(contrib), change_percent(change) {}

    // Helper for conversion
    double priceToDouble() const { return static_cast<double>(last_price) / 100.0; }
};

struct MarketIndex {
    std::string index_name;
    double index_value;
    double change_points;
    double change_percent;
    double day_high;
    double day_low;
    double day_open;
    int64_t timestamp_ms;
    std::vector<IndexConstituent> constituents;
    
    MarketIndex() : index_name(""), index_value(0.0), change_points(0.0), 
                   change_percent(0.0), day_high(0.0), day_low(0.0), 
                   day_open(0.0), timestamp_ms(0) {}
};

struct StockSnapshot {
    std::string symbol;
    Price last_price;
    Price change_points;
    double change_percent;
    Price day_high;
    Price day_low;
    Price day_open;
    int64_t volume;
    Price vwap;
    std::vector<PriceLevel> top_bids;
    std::vector<PriceLevel> top_asks;
    
    StockSnapshot() : symbol(""), last_price(0), change_points(0), 
                     change_percent(0.0), day_high(0), day_low(0), 
                     day_open(0), volume(0), vwap(0) {}

    // Helper functions for conversion
    double lastPriceToDouble() const { return static_cast<double>(last_price) / 100.0; }
    double changePointsToDouble() const { return static_cast<double>(change_points) / 100.0; }
    double dayHighToDouble() const { return static_cast<double>(day_high) / 100.0; }
    double dayLowToDouble() const { return static_cast<double>(day_low) / 100.0; }
    double dayOpenToDouble() const { return static_cast<double>(day_open) / 100.0; }
    double vwapToDouble() const { return static_cast<double>(vwap) / 100.0; }
};

struct MarketDataUpdate {
    std::string symbol;
    Price last_price;
    int64_t last_qty;
    std::vector<PriceLevel> top_bids;
    std::vector<PriceLevel> top_asks;
    int64_t timestamp_ms;
    
    MarketDataUpdate() : symbol(""), last_price(0), last_qty(0), timestamp_ms(0) {}

    // Helper function for conversion
    double priceToDouble() const { return static_cast<double>(last_price) / 100.0; }
};

// Callback types for streaming
using MarketDataCallback = std::function<void(const MarketDataUpdate&)>;
using IndexUpdateCallback = std::function<void(const std::vector<IndexEntry>&)>;
using MarketIndexCallback = std::function<void(const MarketIndex&)>;
using AllStocksCallback = std::function<void(const std::vector<StockSnapshot>&)>;

class StockExchange {
private:
    // Stock symbols to initialize (temporarily reduced for performance tuning)
    const std::vector<std::string> STOCK_SYMBOLS = {
        "AAPL", "MSFT"
    };
    
    std::map<std::string, std::unique_ptr<Stock>> stocks_;
    std::unique_ptr<DatabaseManager> db_manager_;
    
    // Threading
    std::atomic<bool> running_;
    std::thread index_thread_;
    std::thread db_sync_thread_;
    std::mutex db_sync_mutex_;
    std::condition_variable db_sync_cv_;
    
    // Market data streaming
    mutable std::mutex subscribers_mutex_;
    std::map<std::string, std::vector<MarketDataCallback>> market_data_subscribers_;
    std::vector<IndexUpdateCallback> index_subscribers_;
    std::vector<MarketIndexCallback> market_index_subscribers_;
    std::vector<AllStocksCallback> all_stocks_subscribers_;
    mutable std::mutex trade_subscribers_mutex_;
    std::vector<TradeCallback> trade_subscribers_;
    AuthenticationManager* auth_manager_{nullptr};
    OrderReserveCallback reserve_callback_;
    OrderReleaseCallback release_callback_;
    bool trade_observer_registered_{false};
    
    // Index calculation
    mutable std::mutex index_mutex_;
    std::vector<IndexEntry> current_index_;
    
    // Market Index calculation (like S&P 500)
    mutable std::mutex market_index_mutex_;
    MarketIndex current_market_index_;
    double market_index_base_value_;  // Base value for index calculation
    
    void indexWorker();
    void databaseSyncWorker();
    void calculateIndex();
    void calculateMarketIndex();
    void broadcastMarketData(const std::string& symbol);
    void broadcastIndex();
    void broadcastMarketIndex();
    void broadcastAllStocks();
    void dispatchTrade(const Trade& trade);
    
public:
    StockExchange(const std::string& db_connection_string = "");
    ~StockExchange();
    
    bool initialize();
    void start();
    void stop();
    
    // Order management
    std::string submitOrder(const std::string& symbol, const Order& order);
    Order getOrderStatus(const std::string& symbol, const std::string& order_id);
    
    // Market data
    MarketDataUpdate getMarketData(const std::string& symbol);
    std::vector<IndexEntry> getTopIndex(const std::string& criterion = "volume", int top_n = 5);
    
    // Market Index (like S&P 500, Sensex)
    MarketIndex getMarketIndex(const std::string& index_name = "TECH500");
    
    // All stocks snapshot
    std::vector<StockSnapshot> getAllStocksSnapshot(bool include_order_book = false);
    
    // Streaming subscriptions
    void subscribeToMarketData(const std::string& symbol, MarketDataCallback callback);
    void subscribeToIndex(IndexUpdateCallback callback);
    void subscribeToMarketIndex(MarketIndexCallback callback);
    void subscribeToAllStocks(AllStocksCallback callback);
    void registerTradeObserver(TradeCallback callback);
    void setAuthenticationManager(AuthenticationManager* manager);
    void unsubscribeFromMarketData(const std::string& symbol);
    void unsubscribeFromIndex();
    void unsubscribeFromMarketIndex();
    void unsubscribeFromAllStocks();
    
    // Database operations
    void loadFromDatabase();
    void saveToDatabase();
    
    // Health check
    bool isHealthy() const;
    std::vector<std::string> getSymbols() const { return STOCK_SYMBOLS; }
    DatabaseManager* getDatabaseManager() const { return db_manager_.get(); }
};

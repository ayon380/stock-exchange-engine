#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <queue>
#include <map>
#include <thread>
#include <chrono>
#include "LockFreeQueue.h"
#include "MemoryPool.h"
#include "CPUAffinity.h"

// Fixed-point arithmetic: prices stored as 1/100th of currency unit
// $123.45 becomes 12345 (integer)
using Price = int64_t;
using CashAmount = int64_t;

struct PriceLevel {
    Price price;
    int64_t quantity;
    
    PriceLevel() : price(0), quantity(0) {}
    PriceLevel(Price p, int64_t q) : price(p), quantity(q) {}

    // Helper function for conversion
    double priceToDouble() const { return static_cast<double>(price) / 100.0; }
};

struct Order {
    std::string order_id;
    std::string user_id;
    std::string symbol;
    int side;  // 0=BUY, 1=SELL
    int type;  // 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    int64_t quantity;
    int64_t remaining_qty;
    Price price;  // Fixed-point: multiply by 10000 for dollars
    int64_t timestamp_ms;
    std::string status; // "open", "filled", "cancelled", "partial"
    Order* next_at_price; // For linking orders at same price level
    
    Order() = default;
    Order(const std::string& id, const std::string& uid, const std::string& sym, 
          int s, int t, int64_t qty, Price p, int64_t ts)
        : order_id(id), user_id(uid), symbol(sym), side(s), type(t), 
          quantity(qty), remaining_qty(qty), price(p), timestamp_ms(ts), 
          status("open"), next_at_price(nullptr) {}

    // Helper functions for conversion
    static Price fromDouble(double dollars) { return static_cast<Price>(dollars * 100.0 + 0.5); }
    double toDouble() const { return static_cast<double>(price) / 100.0; }
};

struct Trade {
    std::string buy_order_id;
    std::string sell_order_id;
    std::string symbol;
    Price price;
    int64_t quantity;
    int64_t timestamp_ms;
    
    Trade() = default;
    Trade(const std::string& buy_id, const std::string& sell_id, const std::string& sym, 
          Price p, int64_t qty, int64_t ts)
        : buy_order_id(buy_id), sell_order_id(sell_id), symbol(sym), 
          price(p), quantity(qty), timestamp_ms(ts) {}

    // Helper functions for conversion
    static Price fromDouble(double dollars) { return static_cast<Price>(dollars * 100.0 + 0.5); }
    double toDouble() const { return static_cast<double>(price) / 100.0; }
};

// Messages for lock-free communication
struct OrderMessage {
    enum Type { NEW_ORDER, CANCEL_ORDER, MARKET_DATA_REQUEST };
    Type type;
    Order order;
    std::string cancel_order_id;
    
    OrderMessage() = default;
    OrderMessage(Type t, const Order& o) : type(t), order(o) {}
    OrderMessage(Type t, const std::string& cancel_id) : type(t), cancel_order_id(cancel_id) {}
};

struct TradeMessage {
    Trade trade;
    bool ack_required;
    
    TradeMessage() : ack_required(false) {}
    TradeMessage(const Trade& t, bool ack = false) : trade(t), ack_required(ack) {}
};

struct MarketDataMessage {
    std::string symbol;
    double last_price;
    int64_t last_qty;
    std::vector<PriceLevel> top_bids;
    std::vector<PriceLevel> top_asks;
    int64_t timestamp_ms;
    
    MarketDataMessage() = default;
};

// Fast order book using price-time priority with linked lists
struct PriceLevelNode {
    Price price;
    int64_t total_quantity;
    Order* first_order;
    Order* last_order;
    PriceLevelNode* next_level;
    
    PriceLevelNode(Price p) : price(p), total_quantity(0), first_order(nullptr), 
                              last_order(nullptr), next_level(nullptr) {}
    
    void addOrder(Order* order) {
        if (!first_order) {
            first_order = last_order = order;
        } else {
            last_order->next_at_price = order;
            last_order = order;
        }
        total_quantity += order->remaining_qty;
        order->next_at_price = nullptr;
    }
    
    void removeOrder(Order* order) {
        total_quantity -= order->remaining_qty;
        // Simple removal - in production, maintain doubly-linked list
    }
};

class Stock {
private:
    // Core data
    std::string symbol_;
    std::atomic<double> last_price_;
    std::atomic<int64_t> volume_;
    std::atomic<double> open_price_;
    std::atomic<double> day_high_;
    std::atomic<double> day_low_;
    std::atomic<double> vwap_;
    std::atomic<double> total_value_traded_;
    std::atomic<bool> running_;
    
    // Lock-free queues for communication
    static constexpr size_t QUEUE_SIZE = 4096; // Must be power of 2
    SPSCQueue<OrderMessage, QUEUE_SIZE> order_queue_;           // Ingress -> Matching Engine
    SPSCQueue<TradeMessage, QUEUE_SIZE> trade_queue_;           // Matching Engine -> Trade Publisher  
    SPSCQueue<MarketDataMessage, QUEUE_SIZE> market_data_queue_; // Matching Engine -> Market Data Publisher
    
    // Memory pools for zero-allocation trading
    MemoryPool<Order, 1024> order_pool_;
    MemoryPool<PriceLevelNode, 256> price_level_pool_;
    MemoryPool<OrderMessage, 2048> order_message_pool_;
    MemoryPool<TradeMessage, 1024> trade_message_pool_;
    MemoryPool<MarketDataMessage, 512> market_data_message_pool_;
    
    // Fast order book (no locks in matching thread)
    std::map<std::string, Order*> orders_; // Only accessed by matching thread
    PriceLevelNode* best_bid_;
    PriceLevelNode* best_ask_;
    
    // Worker threads
    std::thread matching_thread_;
    std::thread market_data_thread_;
    std::thread trade_publisher_thread_;
    
    // CPU affinity assignment
    int matching_engine_core_;
    int market_data_core_;
    int trade_publisher_core_;
    
    // Statistics for monitoring
    std::atomic<uint64_t> orders_processed_;
    std::atomic<uint64_t> trades_executed_;
    std::atomic<uint64_t> messages_sent_;
    
    // Core engine methods (no locks, single writer)
    void matchingEngineWorker();
    void marketDataWorker();
    void tradePublisherWorker();
    
    void processNewOrder(const Order& order);
    void processCancelOrder(const std::string& order_id);
    std::vector<Trade> matchOrder(Order* incoming_order);
    void updateMarketData();
    void updateDailyStats(double price, int64_t quantity);
    
    // Order book management (lockless, single thread)
    void addOrderToBook(Order* order);
    void removeOrderFromBook(Order* order);
    PriceLevelNode* findOrCreatePriceLevel(Price price, bool is_buy);
    
public:
    explicit Stock(const std::string& symbol, double initial_price = 100.0, 
                   int matching_core = 1, int md_core = 2, int trade_core = 3);
    ~Stock();
    
    void start();
    void stop();
    
    // Lock-free order submission (called from any thread)
    std::string submitOrder(const Order& order);
    std::string cancelOrder(const std::string& order_id);
    Order getOrderStatus(const std::string& order_id);
    
    // Market data (lock-free reads)
    double getLastPrice() const { return last_price_.load(std::memory_order_relaxed); }
    int64_t getVolume() const { return volume_.load(std::memory_order_relaxed); }
    double getChangePercent() const;
    double getChangePoints() const;
    double getDayHigh() const { return day_high_.load(std::memory_order_relaxed); }
    double getDayLow() const { return day_low_.load(std::memory_order_relaxed); }
    double getDayOpen() const { return open_price_.load(std::memory_order_relaxed); }
    double getVWAP() const { return vwap_.load(std::memory_order_relaxed); }
    
    std::vector<PriceLevel> getTopBids(int count = 5) const;
    std::vector<PriceLevel> getTopAsks(int count = 5) const;
    
    // Performance monitoring
    uint64_t getOrdersProcessed() const { return orders_processed_.load(std::memory_order_relaxed); }
    uint64_t getTradesExecuted() const { return trades_executed_.load(std::memory_order_relaxed); }
    uint64_t getMessagesSent() const { return messages_sent_.load(std::memory_order_relaxed); }
    
    // For database persistence
    std::string getSymbol() const { return symbol_; }
    void setLastPrice(double price) { last_price_.store(price, std::memory_order_relaxed); }
    void setOpenPrice(double price) { open_price_.store(price, std::memory_order_relaxed); }
    void setVolume(int64_t vol) { volume_.store(vol, std::memory_order_relaxed); }
    void setDayHigh(double high) { day_high_.store(high, std::memory_order_relaxed); }
    void setDayLow(double low) { day_low_.store(low, std::memory_order_relaxed); }
};

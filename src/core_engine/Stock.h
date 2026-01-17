/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <map>
#include <thread>
#include <chrono>
#include <functional>
#include "LockFreeQueue.h"
#include "MemoryPool.h"
#include "CPUAffinity.h"
#include "AdaptiveLoadManager.h"

// Fixed-point arithmetic: prices stored as 1/100th of currency unit
// $123.45 becomes 12345 (integer)
using Price = int64_t;
using CashAmount = int64_t;

// Forward declaration
struct PriceLevelNode;

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
    Price price;  // Fixed-point: multiply by 100 for dollars
    int64_t timestamp_ms;
    std::string status; // "open", "filled", "cancelled", "partial"
    Order* next_at_price; // For linking orders at same price level
    Order* prev_at_price; // For O(1) removal from price level
    PriceLevelNode* price_level; // Track which level this order is in
    
    Order() = default;
    Order(const std::string& id, const std::string& uid, const std::string& sym, 
          int s, int t, int64_t qty, Price p, int64_t ts)
        : order_id(id), user_id(uid), symbol(sym), side(s), type(t), 
          quantity(qty), remaining_qty(qty), price(p), timestamp_ms(ts), 
          status("open"), next_at_price(nullptr), prev_at_price(nullptr), price_level(nullptr) {}

    // Helper functions for conversion
    static Price fromDouble(double dollars) { return static_cast<Price>(dollars * 100.0 + 0.5); }
    double toDouble() const { return static_cast<double>(price) / 100.0; }
};

// Reservation callbacks used by higher-level risk/account managers
using OrderReserveCallback = std::function<bool(const Order&, Price, std::string&)>;
using OrderReleaseCallback = std::function<void(const Order&, const std::string&)>;

struct Trade {
    std::string buy_order_id;
    std::string sell_order_id;
    std::string symbol;
    Price price;
    int64_t quantity;
    int64_t timestamp_ms;
        std::string buy_user_id;
        std::string sell_user_id;
    
    Trade() = default;
    Trade(const std::string& buy_id, const std::string& sell_id, const std::string& sym, 
                    Price p, int64_t qty, int64_t ts, std::string buy_uid, std::string sell_uid)
                : buy_order_id(buy_id), sell_order_id(sell_id), symbol(sym), 
                    price(p), quantity(qty), timestamp_ms(ts),
                    buy_user_id(std::move(buy_uid)), sell_user_id(std::move(sell_uid)) {}

    // Helper functions for conversion
    static Price fromDouble(double dollars) { return static_cast<Price>(dollars * 100.0 + 0.5); }
    double toDouble() const { return static_cast<double>(price) / 100.0; }
};

// Callback invoked when a trade is executed. The parameter carries trade metadata in fixed-point cents.
using TradeCallback = std::function<void(const Trade&)>;

// Callback invoked when an order status changes (for SEC compliance persistence)
using OrderStatusCallback = std::function<void(const Order&)>;

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
    Price last_price;
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
            order->prev_at_price = nullptr;
            order->next_at_price = nullptr;
        } else {
            last_order->next_at_price = order;
            order->prev_at_price = last_order;
            order->next_at_price = nullptr;
            last_order = order;
        }
        total_quantity += order->remaining_qty;
        order->price_level = this; // Track which level this order is in
    }
    
    void removeOrder(Order* order) {
        total_quantity -= order->remaining_qty;
        
        // Update links
        if (order->prev_at_price) {
            order->prev_at_price->next_at_price = order->next_at_price;
        } else {
            // This was the first order
            first_order = order->next_at_price;
        }
        
        if (order->next_at_price) {
            order->next_at_price->prev_at_price = order->prev_at_price;
        } else {
            // This was the last order
            last_order = order->prev_at_price;
        }
        
        order->prev_at_price = nullptr;
        order->next_at_price = nullptr;
    }
};

class Stock {
private:
    // Core data
    std::string symbol_;
    std::atomic<Price> last_price_;
    std::atomic<int64_t> volume_;
    std::atomic<Price> open_price_;
    std::atomic<Price> day_high_;
    std::atomic<Price> day_low_;
    std::atomic<double> vwap_;
    std::atomic<double> total_value_traded_;
    std::atomic<bool> running_;
    
    // Lock-free queues for communication
    static constexpr size_t QUEUE_SIZE = 4096; // Must be power of 2
    MPSCQueue<OrderMessage, QUEUE_SIZE> order_queue_;           // Ingress -> Matching Engine
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
    
    // Thread-safe order status cache (for getOrderStatus queries)
    mutable std::mutex order_status_mutex_;
    mutable std::map<std::string, Order> order_status_cache_;
    
    // Shared mutex for safe concurrent reading of order book (readers don't block readers)
    mutable std::shared_mutex orderbook_mutex_;
    
    // Cached snapshot for high-frequency reads (reduces lock contention)
    mutable std::vector<PriceLevel> cached_bids_;
    mutable std::vector<PriceLevel> cached_asks_;
    mutable std::atomic<int64_t> last_bids_snapshot_time_ms_;
    mutable std::atomic<int64_t> last_asks_snapshot_time_ms_;
    mutable std::mutex snapshot_mutex_;
    static constexpr int64_t SNAPSHOT_CACHE_MS = 100; // Cache for 100ms
    
    // Worker threads
    std::thread matching_thread_;
    std::thread market_data_thread_;
    std::thread trade_publisher_thread_;
    
    // CPU affinity assignment
    int matching_engine_core_;
    int market_data_core_;
    int trade_publisher_core_;
    
    // Market order protection (max 10% deviation from last price)
    static constexpr double MAX_MARKET_ORDER_DEVIATION = 0.10; // 10%
    
    // Order book depth limits (prevent memory exhaustion)
    static constexpr size_t MAX_ORDER_BOOK_DEPTH = 10000; // Max orders per side
    std::atomic<size_t> total_buy_orders_{0};
    std::atomic<size_t> total_sell_orders_{0};
    
    // VWAP calculation synchronization
    mutable std::mutex vwap_mutex_;
    double vwap_numerator_ = 0.0;  // Sum of (price * quantity)
    int64_t vwap_denominator_ = 0;  // Sum of quantity
    
    // Statistics for monitoring
    std::atomic<uint64_t> orders_processed_;
    std::atomic<uint64_t> trades_executed_;
    std::atomic<uint64_t> messages_sent_;
    
    // Per-instance counter for market data updates (fixes data race from static counter)
    uint64_t market_data_update_counter_;

    // Adaptive load management for dynamic CPU scaling
    AdaptiveLoadManager matching_load_manager_;
    AdaptiveLoadManager market_data_load_manager_;
    AdaptiveLoadManager trade_publisher_load_manager_;

    // Optional trade observer for account settlement and analytics
    TradeCallback trade_callback_;
    mutable std::mutex trade_callback_mutex_;
    
    // SEC Compliance: Order status observer for database persistence
    OrderStatusCallback order_status_callback_;
    mutable std::mutex order_status_callback_mutex_;
    
    OrderReserveCallback reserve_callback_;
    OrderReleaseCallback release_callback_;
    
    // Core engine methods (no locks, single writer)
    void matchingEngineWorker();
    void marketDataWorker();
    void tradePublisherWorker();
    
    void processNewOrder(const Order& order);
    void processCancelOrder(const std::string& order_id);
    std::vector<Trade> matchOrder(Order* incoming_order);
    void updateMarketData();
    void updateDailyStats(Price price, int64_t quantity);
    
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
    // Prepare stock for shutdown: stop accepting new orders and drain queues
    void prepareForShutdown();
    
    // Lock-free order submission (called from any thread)
    std::string submitOrder(const Order& order);
    std::string cancelOrder(const std::string& order_id);
    Order getOrderStatus(const std::string& order_id);
    
    // Market data (lock-free reads)
    double getLastPrice() const { return static_cast<double>(last_price_.load(std::memory_order_relaxed)) / 100.0; }
    Price getLastPriceFixed() const { return last_price_.load(std::memory_order_relaxed); }
    int64_t getVolume() const { return volume_.load(std::memory_order_relaxed); }
    double getChangePercent() const;
    double getChangePoints() const;
    Price getChangePointsFixed() const;
    double getDayHigh() const { return static_cast<double>(day_high_.load(std::memory_order_relaxed)) / 100.0; }
    double getDayLow() const { return static_cast<double>(day_low_.load(std::memory_order_relaxed)) / 100.0; }
    double getDayOpen() const { return static_cast<double>(open_price_.load(std::memory_order_relaxed)) / 100.0; }
    Price getDayHighFixed() const { return day_high_.load(std::memory_order_relaxed); }
    Price getDayLowFixed() const { return day_low_.load(std::memory_order_relaxed); }
    Price getDayOpenFixed() const { return open_price_.load(std::memory_order_relaxed); }
    double getVWAP() const { return vwap_.load(std::memory_order_relaxed); }
    Price getVWAPFixed() const;
    
    std::vector<PriceLevel> getTopBids(int count = 5) const;
    std::vector<PriceLevel> getTopAsks(int count = 5) const;
    
    // Performance monitoring
    uint64_t getOrdersProcessed() const { return orders_processed_.load(std::memory_order_relaxed); }
    uint64_t getTradesExecuted() const { return trades_executed_.load(std::memory_order_relaxed); }
    uint64_t getMessagesSent() const { return messages_sent_.load(std::memory_order_relaxed); }
    
    // Load level monitoring
    const char* getMatchingLoadLevel() const { return matching_load_manager_.getLoadLevelName(); }
    const char* getMarketDataLoadLevel() const { return market_data_load_manager_.getLoadLevelName(); }
    const char* getTradePublisherLoadLevel() const { return trade_publisher_load_manager_.getLoadLevelName(); }
    int getMatchingWorkPercentage() const { return matching_load_manager_.getWorkPercentage(); }
    int getMarketDataWorkPercentage() const { return market_data_load_manager_.getWorkPercentage(); }
    int getTradePublisherWorkPercentage() const { return trade_publisher_load_manager_.getWorkPercentage(); }
    
    // For database persistence
    std::string getSymbol() const { return symbol_; }
    void setLastPrice(double price) { last_price_.store(static_cast<Price>(price * 100.0), std::memory_order_relaxed); }
    void setOpenPrice(double price) { open_price_.store(static_cast<Price>(price * 100.0), std::memory_order_relaxed); }
    void setVolume(int64_t vol) { volume_.store(vol, std::memory_order_relaxed); }
    void setDayHigh(double high) { day_high_.store(static_cast<Price>(high * 100.0), std::memory_order_relaxed); }
    void setDayLow(double low) { day_low_.store(static_cast<Price>(low * 100.0), std::memory_order_relaxed); }

    // SEC Compliance: Set callbacks for persistence
    void setTradeCallback(TradeCallback callback);
    void setOrderStatusCallback(OrderStatusCallback callback);
    void setReservationHandlers(OrderReserveCallback reserve_cb, OrderReleaseCallback release_cb);
};

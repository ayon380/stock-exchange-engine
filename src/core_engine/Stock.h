#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <queue>
#include <map>
#include <thread>
#include <chrono>

struct PriceLevel {
    double price;
    int64_t quantity;
    
    PriceLevel() : price(0.0), quantity(0) {}
    PriceLevel(double p, int64_t q) : price(p), quantity(q) {}
};

struct Order {
    std::string order_id;
    std::string user_id;
    std::string symbol;
    int side;  // 0=BUY, 1=SELL
    int type;  // 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    int64_t quantity;
    int64_t remaining_qty;
    double price;
    int64_t timestamp_ms;
    std::string status; // "open", "filled", "cancelled", "partial"
    
    Order() = default;
    Order(const std::string& id, const std::string& uid, const std::string& sym, 
          int s, int t, int64_t qty, double p, int64_t ts)
        : order_id(id), user_id(uid), symbol(sym), side(s), type(t), 
          quantity(qty), remaining_qty(qty), price(p), timestamp_ms(ts), status("open") {}
};

struct Trade {
    std::string buy_order_id;
    std::string sell_order_id;
    std::string symbol;
    double price;
    int64_t quantity;
    int64_t timestamp_ms;
    
    Trade(const std::string& buy_id, const std::string& sell_id, const std::string& sym, 
          double p, int64_t qty, int64_t ts)
        : buy_order_id(buy_id), sell_order_id(sell_id), symbol(sym), 
          price(p), quantity(qty), timestamp_ms(ts) {}
};

class Stock {
private:
    std::string symbol_;
    std::atomic<double> last_price_;
    std::atomic<int64_t> volume_;
    std::atomic<double> open_price_;
    std::atomic<double> day_high_;
    std::atomic<double> day_low_;
    std::atomic<double> vwap_;  // Volume Weighted Average Price
    std::atomic<double> total_value_traded_;
    std::atomic<bool> running_;
    
    mutable std::mutex orders_mutex_;
    mutable std::mutex book_mutex_;
    
    std::map<std::string, Order> orders_;
    std::multimap<double, Order*> buy_orders_;   // sorted by price desc
    std::multimap<double, Order*> sell_orders_;  // sorted by price asc
    
    std::vector<Trade> trades_;
    std::thread worker_thread_;
    
    void processOrders();
    std::vector<Trade> matchOrders();
    void updateMarketData();
    void updateDailyStats(double price, int64_t quantity);
    
public:
    explicit Stock(const std::string& symbol, double initial_price = 100.0);
    ~Stock();
    
    void start();
    void stop();
    
    std::string submitOrder(const Order& order);
    Order getOrderStatus(const std::string& order_id);
    
    // Market data
    double getLastPrice() const { return last_price_.load(); }
    int64_t getVolume() const { return volume_.load(); }
    double getChangePercent() const;
    double getChangePoints() const;
    double getDayHigh() const { return day_high_.load(); }
    double getDayLow() const { return day_low_.load(); }
    double getDayOpen() const { return open_price_.load(); }
    double getVWAP() const { return vwap_.load(); }
    
    std::vector<PriceLevel> getTopBids(int count = 5) const;
    std::vector<PriceLevel> getTopAsks(int count = 5) const;
    
    // For database persistence
    std::string getSymbol() const { return symbol_; }
    void setLastPrice(double price) { last_price_.store(price); }
    void setOpenPrice(double price) { open_price_.store(price); }
    void setVolume(int64_t vol) { volume_.store(vol); }
    void setDayHigh(double high) { day_high_.store(high); }
    void setDayLow(double low) { day_low_.store(low); }
};

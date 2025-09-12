#include "Stock.h"
#include <random>
#include <algorithm>
#include <iostream>

Stock::Stock(const std::string& symbol, double initial_price) 
    : symbol_(symbol), last_price_(initial_price), volume_(0), 
      open_price_(initial_price), day_high_(initial_price), day_low_(initial_price),
      vwap_(initial_price), total_value_traded_(0.0), running_(false) {
}

Stock::~Stock() {
    stop();
}

void Stock::start() {
    running_.store(true);
    worker_thread_ = std::thread(&Stock::processOrders, this);
}

void Stock::stop() {
    running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::string Stock::submitOrder(const Order& order) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    
    if (orders_.find(order.order_id) != orders_.end()) {
        return "Order ID already exists";
    }
    
    orders_[order.order_id] = order;
    Order* order_ptr = &orders_[order.order_id];
    
    if (order.side == 0) { // BUY
        buy_orders_.emplace(-order.price, order_ptr); // negative for desc sort
    } else { // SELL
        sell_orders_.emplace(order.price, order_ptr);
    }
    
    std::cout << "[" << symbol_ << "] New order: " << order.order_id 
              << " " << (order.side == 0 ? "BUY" : "SELL") 
              << " " << order.quantity << "@" << order.price << std::endl;
    
    return "accepted";
}

Order Stock::getOrderStatus(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
        return it->second;
    }
    return Order{}; // empty order if not found
}

void Stock::processOrders() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> price_change(-0.02, 0.02); // ±2% change
    
    while (running_.load()) {
        // Simulate price movements
        double current_price = last_price_.load();
        double change = price_change(gen);
        double new_price = current_price * (1 + change);
        new_price = std::max(1.0, new_price); // Minimum price of $1
        
        last_price_.store(new_price);
        
        // Process order matching
        auto trades = matchOrders();
        for (const auto& trade : trades) {
            volume_.fetch_add(trade.quantity);
            last_price_.store(trade.price);
            updateDailyStats(trade.price, trade.quantity);
        }
        
        updateMarketData();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Stock::updateDailyStats(double price, int64_t quantity) {
    // Update daily high/low
    double current_high = day_high_.load();
    while (price > current_high && !day_high_.compare_exchange_weak(current_high, price)) {
        current_high = day_high_.load();
    }
    
    double current_low = day_low_.load();
    while (price < current_low && !day_low_.compare_exchange_weak(current_low, price)) {
        current_low = day_low_.load();
    }
    
    // Update VWAP (Volume Weighted Average Price)
    double trade_value = price * quantity;
    double current_total = total_value_traded_.load();
    double new_total = current_total + trade_value;
    total_value_traded_.store(new_total);
    
    int64_t total_volume = volume_.load();
    if (total_volume > 0) {
        double new_vwap = total_value_traded_.load() / total_volume;
        vwap_.store(new_vwap);
    }
}

std::vector<Trade> Stock::matchOrders() {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    std::vector<Trade> new_trades;
    
    while (!buy_orders_.empty() && !sell_orders_.empty()) {
        auto buy_it = buy_orders_.begin();
        auto sell_it = sell_orders_.begin();
        
        double buy_price = -buy_it->first; // Convert back from negative
        double sell_price = sell_it->first;
        
        if (buy_price < sell_price) {
            break; // No match possible
        }
        
        Order* buy_order = buy_it->second;
        Order* sell_order = sell_it->second;
        
        if (buy_order->remaining_qty == 0 || sell_order->remaining_qty == 0) {
            if (buy_order->remaining_qty == 0) {
                buy_orders_.erase(buy_it);
            }
            if (sell_order->remaining_qty == 0) {
                sell_orders_.erase(sell_it);
            }
            continue;
        }
        
        int64_t trade_qty = std::min(buy_order->remaining_qty, sell_order->remaining_qty);
        double trade_price = sell_price; // Price discovery: seller's price
        
        // Create trade
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        new_trades.emplace_back(buy_order->order_id, sell_order->order_id, 
                               symbol_, trade_price, trade_qty, now);
        
        // Update orders
        buy_order->remaining_qty -= trade_qty;
        sell_order->remaining_qty -= trade_qty;
        
        if (buy_order->remaining_qty == 0) {
            buy_order->status = "filled";
            buy_orders_.erase(buy_it);
        } else {
            buy_order->status = "partial";
        }
        
        if (sell_order->remaining_qty == 0) {
            sell_order->status = "filled";
            sell_orders_.erase(sell_it);
        } else {
            sell_order->status = "partial";
        }
        
        std::cout << "[" << symbol_ << "] Trade executed: " << trade_qty 
                  << "@" << trade_price << std::endl;
    }
    
    trades_.insert(trades_.end(), new_trades.begin(), new_trades.end());
    return new_trades;
}

void Stock::updateMarketData() {
    // This method can be extended to notify subscribers
    // For now, it's a placeholder for market data updates
}

double Stock::getChangePercent() const {
    double open = open_price_.load();
    double current = last_price_.load();
    if (open == 0) return 0.0;
    return ((current - open) / open) * 100.0;
}

double Stock::getChangePoints() const {
    double open = open_price_.load();
    double current = last_price_.load();
    return current - open;
}

std::vector<PriceLevel> Stock::getTopBids(int count) const {
    std::lock_guard<std::mutex> lock(book_mutex_);
    std::vector<PriceLevel> bids;
    
    std::map<double, int64_t> aggregated_bids;
    for (const auto& entry : buy_orders_) {
        double price = -entry.first;
        if (entry.second->remaining_qty > 0) {
            aggregated_bids[price] += entry.second->remaining_qty;
        }
    }
    
    int added = 0;
    for (auto it = aggregated_bids.rbegin(); it != aggregated_bids.rend() && added < count; ++it, ++added) {
        bids.emplace_back(it->first, it->second);
    }
    
    return bids;
}

std::vector<PriceLevel> Stock::getTopAsks(int count) const {
    std::lock_guard<std::mutex> lock(book_mutex_);
    std::vector<PriceLevel> asks;
    
    std::map<double, int64_t> aggregated_asks;
    for (const auto& entry : sell_orders_) {
        double price = entry.first;
        if (entry.second->remaining_qty > 0) {
            aggregated_asks[price] += entry.second->remaining_qty;
        }
    }
    
    int added = 0;
    for (const auto& entry : aggregated_asks) {
        if (added >= count) break;
        asks.emplace_back(entry.first, entry.second);
        added++;
    }
    
    return asks;
}

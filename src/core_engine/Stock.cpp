#define NOMINMAX
#include "Stock.h"
#include <random>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <limits>

Stock::Stock(const std::string& symbol, double initial_price, 
             int matching_core, int md_core, int trade_core) 
    : symbol_(symbol), last_price_(initial_price), volume_(0), 
      open_price_(initial_price), day_high_(initial_price), day_low_(initial_price),
      vwap_(initial_price), total_value_traded_(0.0), running_(false),
      best_bid_(nullptr), best_ask_(nullptr),
      matching_engine_core_(matching_core), market_data_core_(md_core), 
      trade_publisher_core_(trade_core),
      orders_processed_(0), trades_executed_(0), messages_sent_(0) {
}

Stock::~Stock() {
    stop();
}

void Stock::start() {
    running_.store(true, std::memory_order_release);
    
    // Start worker threads with CPU affinity
    matching_thread_ = std::thread(&Stock::matchingEngineWorker, this);
    market_data_thread_ = std::thread(&Stock::marketDataWorker, this);
    trade_publisher_thread_ = std::thread(&Stock::tradePublisherWorker, this);
    
    // Set CPU affinity and high priority
    CPUAffinity::setThreadAffinity(matching_thread_, matching_engine_core_);
    CPUAffinity::setThreadAffinity(market_data_thread_, market_data_core_);
    CPUAffinity::setThreadAffinity(trade_publisher_thread_, trade_publisher_core_);
    
    CPUAffinity::setHighPriority(matching_thread_);
    CPUAffinity::setHighPriority(market_data_thread_);
    CPUAffinity::setHighPriority(trade_publisher_thread_);
    
    std::cout << "[" << symbol_ << "] Started with cores: matching=" << matching_engine_core_ 
              << " md=" << market_data_core_ << " trade=" << trade_publisher_core_ << std::endl;
}

void Stock::stop() {
    running_.store(false, std::memory_order_release);
    
    if (matching_thread_.joinable()) {
        matching_thread_.join();
    }
    if (market_data_thread_.joinable()) {
        market_data_thread_.join();
    }
    if (trade_publisher_thread_.joinable()) {
        trade_publisher_thread_.join();
    }
}

std::string Stock::submitOrder(const Order& order) {
    // Allocate message from pool (lock-free)
    OrderMessage* msg = order_message_pool_.allocate(OrderMessage::NEW_ORDER, order);
    
    if (!order_queue_.enqueue(msg)) {
        order_message_pool_.deallocate(msg);
        return "Queue full - order rejected";
    }
    
    return "accepted";
}

std::string Stock::cancelOrder(const std::string& order_id) {
    OrderMessage* msg = order_message_pool_.allocate(OrderMessage::CANCEL_ORDER, order_id);
    
    if (!order_queue_.enqueue(msg)) {
        order_message_pool_.deallocate(msg);
        return "Queue full - cancel rejected";
    }
    
    return "cancel submitted";
}

Order Stock::getOrderStatus(const std::string& order_id) {
    // Note: In production, this would need a separate query mechanism
    // For now, return empty order - orders are managed in matching thread
    return Order{}; 
}

void Stock::matchingEngineWorker() {
    // Set CPU affinity and priority for this thread
    CPUAffinity::setCurrentThreadHighPriority();
    
    std::cout << "[" << symbol_ << "] Matching engine started on core " 
              << matching_engine_core_ << std::endl;
    
    while (running_.load(std::memory_order_acquire)) {
        // Process incoming orders (lock-free)
        OrderMessage* msg = order_queue_.dequeue();
        if (msg) {
            switch (msg->type) {
                case OrderMessage::NEW_ORDER:
                    processNewOrder(msg->order);
                    break;
                case OrderMessage::CANCEL_ORDER:
                    processCancelOrder(msg->cancel_order_id);
                    break;
                default:
                    break;
            }
            
            order_message_pool_.deallocate(msg);
            orders_processed_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Update market data periodically
        static uint64_t counter = 0;
        if (++counter % 1000 == 0) {
            updateMarketData();
        }
        
        // Yield CPU if no work (busy-wait for ultra-low latency)
        if (!msg) {
            std::this_thread::yield();
        }
    }
}

void Stock::marketDataWorker() {
    CPUAffinity::setCurrentThreadHighPriority();
    
    std::cout << "[" << symbol_ << "] Market data worker started on core " 
              << market_data_core_ << std::endl;
    
    while (running_.load(std::memory_order_acquire)) {
        MarketDataMessage* msg = market_data_queue_.dequeue();
        if (msg) {
            // Broadcast market data to subscribers
            // In production, this would publish to market data feeds
            messages_sent_.fetch_add(1, std::memory_order_relaxed);
            market_data_message_pool_.deallocate(msg);
        } else {
            std::this_thread::yield();
        }
    }
}

void Stock::tradePublisherWorker() {
    CPUAffinity::setCurrentThreadHighPriority();
    
    std::cout << "[" << symbol_ << "] Trade publisher started on core " 
              << trade_publisher_core_ << std::endl;
    
    while (running_.load(std::memory_order_acquire)) {
        TradeMessage* msg = trade_queue_.dequeue();
        if (msg) {
            // Publish trade to external systems
            std::cout << "[" << symbol_ << "] Trade: " 
                      << msg->trade.quantity << "@" << msg->trade.price << std::endl;
            
            trades_executed_.fetch_add(1, std::memory_order_relaxed);
            trade_message_pool_.deallocate(msg);
        } else {
            std::this_thread::yield();
        }
    }
}

void Stock::processNewOrder(const Order& incoming_order) {
    // Allocate order from pool
    Order* order = order_pool_.allocate(incoming_order);
    
    if (orders_.find(order->order_id) != orders_.end()) {
        order_pool_.deallocate(order);
        return; // Duplicate order ID
    }
    
    orders_[order->order_id] = order;
    
    // Try to match immediately
    auto trades = matchOrder(order);
    
    // If order still has remaining quantity, add to book
    if (order->remaining_qty > 0) {
        addOrderToBook(order);
    }
    
    // Send trades to publisher
    for (const auto& trade : trades) {
        TradeMessage* trade_msg = trade_message_pool_.allocate(trade, true);
        if (!trade_queue_.enqueue(trade_msg)) {
            trade_message_pool_.deallocate(trade_msg);
        }
        
        // Update market data
        last_price_.store(trade.price, std::memory_order_relaxed);
        volume_.fetch_add(trade.quantity, std::memory_order_relaxed);
        updateDailyStats(trade.price, trade.quantity);
    }
}

void Stock::processCancelOrder(const std::string& order_id) {
    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
        Order* order = it->second;
        removeOrderFromBook(order);
        order->status = "cancelled";
        orders_.erase(it);
        order_pool_.deallocate(order);
    }
}

std::vector<Trade> Stock::matchOrder(Order* incoming_order) {
    std::vector<Trade> trades;
    
    if (incoming_order->side == 0) { // BUY order
        // Match against sell orders (asks)
        while (incoming_order->remaining_qty > 0 && best_ask_) {
            if (incoming_order->price < best_ask_->price) {
                break; // No more matches possible
            }
            
            PriceLevelNode* ask_level = best_ask_;
            Order* sell_order = ask_level->first_order;
            
            if (!sell_order || sell_order->remaining_qty == 0) {
                // Remove empty level
                best_ask_ = ask_level->next_level;
                price_level_pool_.deallocate(ask_level);
                continue;
            }
            
            // Execute trade
            int64_t trade_qty = (std::min)(incoming_order->remaining_qty, sell_order->remaining_qty);
            double trade_price = sell_order->price; // Price discovery: taker pays maker's price
            
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            trades.emplace_back(incoming_order->order_id, sell_order->order_id, 
                               symbol_, trade_price, trade_qty, now);
            
            // Update orders
            incoming_order->remaining_qty -= trade_qty;
            sell_order->remaining_qty -= trade_qty;
            ask_level->total_quantity -= trade_qty;
            
            if (incoming_order->remaining_qty == 0) {
                incoming_order->status = "filled";
            } else {
                incoming_order->status = "partial";
            }
            
            if (sell_order->remaining_qty == 0) {
                sell_order->status = "filled";
                // Remove order from level
                ask_level->first_order = sell_order->next_at_price;
                if (!ask_level->first_order) {
                    ask_level->last_order = nullptr;
                }
            } else {
                sell_order->status = "partial";
            }
        }
    } else { // SELL order
        // Match against buy orders (bids)
        while (incoming_order->remaining_qty > 0 && best_bid_) {
            if (incoming_order->price > best_bid_->price) {
                break; // No more matches possible
            }
            
            PriceLevelNode* bid_level = best_bid_;
            Order* buy_order = bid_level->first_order;
            
            if (!buy_order || buy_order->remaining_qty == 0) {
                // Remove empty level
                best_bid_ = bid_level->next_level;
                price_level_pool_.deallocate(bid_level);
                continue;
            }
            
            // Execute trade
            int64_t trade_qty = (std::min)(incoming_order->remaining_qty, buy_order->remaining_qty);
            double trade_price = buy_order->price; // Price discovery: taker pays maker's price
            
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            trades.emplace_back(buy_order->order_id, incoming_order->order_id, 
                               symbol_, trade_price, trade_qty, now);
            
            // Update orders
            incoming_order->remaining_qty -= trade_qty;
            buy_order->remaining_qty -= trade_qty;
            bid_level->total_quantity -= trade_qty;
            
            if (incoming_order->remaining_qty == 0) {
                incoming_order->status = "filled";
            } else {
                incoming_order->status = "partial";
            }
            
            if (buy_order->remaining_qty == 0) {
                buy_order->status = "filled";
                // Remove order from level
                bid_level->first_order = buy_order->next_at_price;
                if (!bid_level->first_order) {
                    bid_level->last_order = nullptr;
                }
            } else {
                buy_order->status = "partial";
            }
        }
    }
    
    return trades;
}

void Stock::addOrderToBook(Order* order) {
    PriceLevelNode* level = findOrCreatePriceLevel(order->price, order->side == 0);
    level->addOrder(order);
}

void Stock::removeOrderFromBook(Order* order) {
    // Simple implementation - in production, use doubly-linked lists for O(1) removal
    // For now, we'll mark the order as removed by setting remaining_qty to 0
    order->remaining_qty = 0;
}

PriceLevelNode* Stock::findOrCreatePriceLevel(Price price, bool is_buy) {
    if (is_buy) {
        // Find insertion point in bid side (descending order)
        if (!best_bid_ || price > best_bid_->price) {
            PriceLevelNode* new_level = price_level_pool_.allocate(price);
            new_level->next_level = best_bid_;
            best_bid_ = new_level;
            return new_level;
        }
        
        PriceLevelNode* current = best_bid_;
        while (current) {
            if (current->price == price) {
                return current;
            }
            if (current->next_level == nullptr || current->next_level->price < price) {
                PriceLevelNode* new_level = price_level_pool_.allocate(price);
                new_level->next_level = current->next_level;
                current->next_level = new_level;
                return new_level;
            }
            current = current->next_level;
        }
    } else {
        // Find insertion point in ask side (ascending order)
        if (!best_ask_ || price < best_ask_->price) {
            PriceLevelNode* new_level = price_level_pool_.allocate(price);
            new_level->next_level = best_ask_;
            best_ask_ = new_level;
            return new_level;
        }
        
        PriceLevelNode* current = best_ask_;
        while (current) {
            if (current->price == price) {
                return current;
            }
            if (current->next_level == nullptr || current->next_level->price > price) {
                PriceLevelNode* new_level = price_level_pool_.allocate(price);
                new_level->next_level = current->next_level;
                current->next_level = new_level;
                return new_level;
            }
            current = current->next_level;
        }
    }
    
    return nullptr; // Should never reach here
}

void Stock::updateDailyStats(double price, int64_t quantity) {
    // Update daily high/low using compare_exchange for lock-free updates
    double current_high = day_high_.load(std::memory_order_relaxed);
    while (price > current_high && 
           !day_high_.compare_exchange_weak(current_high, price, 
                                          std::memory_order_relaxed)) {
        current_high = day_high_.load(std::memory_order_relaxed);
    }
    
    double current_low = day_low_.load(std::memory_order_relaxed);
    while (price < current_low && 
           !day_low_.compare_exchange_weak(current_low, price, 
                                         std::memory_order_relaxed)) {
        current_low = day_low_.load(std::memory_order_relaxed);
    }
    
    // Update VWAP (Volume Weighted Average Price)
    double trade_value = price * quantity;
    double current_total = total_value_traded_.fetch_add(trade_value, std::memory_order_relaxed);
    
    int64_t total_volume = volume_.load(std::memory_order_relaxed);
    if (total_volume > 0) {
        double new_vwap = (current_total + trade_value) / total_volume;
        vwap_.store(new_vwap, std::memory_order_relaxed);
    }
}

void Stock::updateMarketData() {
    MarketDataMessage* msg = market_data_message_pool_.allocate();
    msg->symbol = symbol_;
    msg->last_price = last_price_.load(std::memory_order_relaxed);
    msg->timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Build top bids and asks
    int count = 0;
    PriceLevelNode* level = best_bid_;
    while (level && count < 5) {
        if (level->total_quantity > 0) {
            msg->top_bids.emplace_back(level->price, level->total_quantity);
            count++;
        }
        level = level->next_level;
    }
    
    count = 0;
    level = best_ask_;
    while (level && count < 5) {
        if (level->total_quantity > 0) {
            msg->top_asks.emplace_back(level->price, level->total_quantity);
            count++;
        }
        level = level->next_level;
    }
    
    if (!market_data_queue_.enqueue(msg)) {
        market_data_message_pool_.deallocate(msg);
    }
}
double Stock::getChangePercent() const {
    double open = open_price_.load(std::memory_order_relaxed);
    double current = last_price_.load(std::memory_order_relaxed);
    if (open == 0) return 0.0;
    return ((current - open) / open) * 100.0;
}

double Stock::getChangePoints() const {
    double open = open_price_.load(std::memory_order_relaxed);
    double current = last_price_.load(std::memory_order_relaxed);
    return current - open;
}

std::vector<PriceLevel> Stock::getTopBids(int count) const {
    std::vector<PriceLevel> bids;
    
    // Note: In production, this would need proper synchronization or snapshots
    // For now, we'll provide a simple implementation
    PriceLevelNode* level = best_bid_;
    int added = 0;
    
    while (level && added < count) {
        if (level->total_quantity > 0) {
            bids.emplace_back(level->price, level->total_quantity);
            added++;
        }
        level = level->next_level;
    }
    
    return bids;
}

std::vector<PriceLevel> Stock::getTopAsks(int count) const {
    std::vector<PriceLevel> asks;
    
    PriceLevelNode* level = best_ask_;
    int added = 0;
    
    while (level && added < count) {
        if (level->total_quantity > 0) {
            asks.emplace_back(level->price, level->total_quantity);
            added++;
        }
        level = level->next_level;
    }
    
    return asks;
}

#define NOMINMAX
#include "Stock.h"
#include "../common/EngineLogging.h"
#include <random>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <limits>
#include <future>

Stock::Stock(const std::string& symbol, double initial_price, 
             int matching_core, int md_core, int trade_core) 
    : symbol_(symbol), last_price_(static_cast<Price>(initial_price * 100.0)), volume_(0), 
      open_price_(static_cast<Price>(initial_price * 100.0)), 
      day_high_(static_cast<Price>(initial_price * 100.0)), 
      day_low_(static_cast<Price>(initial_price * 100.0)),
      vwap_(initial_price), total_value_traded_(0.0), running_(false),
      best_bid_(nullptr), best_ask_(nullptr),
      matching_engine_core_(matching_core), market_data_core_(md_core), 
      trade_publisher_core_(trade_core),
      orders_processed_(0), trades_executed_(0), messages_sent_(0),
      last_snapshot_time_ms_(0) {
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
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Started with cores: matching=" << matching_engine_core_
                             << " md=" << market_data_core_ << " trade=" << trade_publisher_core_ << std::endl;);
}

void Stock::stop() {
    running_.store(false, std::memory_order_seq_cst);  // Stronger memory ordering
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Stopping threads..." << std::flush;);
    
    // Join threads with timeout protection to prevent indefinite hangs
    auto joinWithTimeout = [](std::thread& t, const char* name, int timeout_ms) {
        if (t.joinable()) {
            ENGINE_LOG_DEV(std::cout << " " << name << std::flush;);
            auto future = std::async(std::launch::async, [&t]() { t.join(); });
            if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::timeout) {
                std::cerr << "\nWarning: " << name << " thread timeout, detaching" << std::endl;
                t.detach();
            }
        }
    };
    
    // Increased timeout to 500ms per thread to handle heavy load
    joinWithTimeout(matching_thread_, "matching", 500);
    joinWithTimeout(market_data_thread_, "md", 500);
    joinWithTimeout(trade_publisher_thread_, "trade", 500);
    
    ENGINE_LOG_DEV(std::cout << " done" << std::endl;);
}

void Stock::prepareForShutdown() {
    // Prevent new orders from being accepted
    running_.store(false, std::memory_order_release);

    // Aggressively drain queues so consumer threads don't block on heavy backlog
    OrderMessage* om = nullptr;
    while ((om = order_queue_.dequeue()) != nullptr) {
        order_message_pool_.deallocate(om);
    }

    TradeMessage* tm = nullptr;
    while ((tm = trade_queue_.dequeue()) != nullptr) {
        trade_message_pool_.deallocate(tm);
    }

    MarketDataMessage* mm = nullptr;
    while ((mm = market_data_queue_.dequeue()) != nullptr) {
        market_data_message_pool_.deallocate(mm);
    }
}

std::string Stock::submitOrder(const Order& order) {
    if (!running_.load(std::memory_order_acquire)) {
        return "rejected: engine not running";
    }

    // Comprehensive order validation
    
    // 1. Validate order ID
    if (order.order_id.empty()) {
        return "rejected: order_id cannot be empty";
    }
    
    // 2. Validate user ID
    if (order.user_id.empty()) {
        return "rejected: user_id cannot be empty";
    }
    
    // 3. Validate quantity
    if (order.quantity <= 0) {
        return "rejected: quantity must be positive";
    }
    
    // 4. Validate price (for non-MARKET orders)
    if (order.type != 0) { // Not a MARKET order
        if (order.price <= 0) {
            return "rejected: price must be positive";
        }
        // Reasonable price range check (between $0.01 and $1,000,000.00)
        if (order.price < 1 || order.price > 100000000) {
            return "rejected: price out of valid range";
        }
    }
    
    // 5. Validate side
    if (order.side != 0 && order.side != 1) {
        return "rejected: invalid side (must be 0=BUY or 1=SELL)";
    }
    
    // 6. Validate order type
    if (order.type < 0 || order.type > 3) {
        return "rejected: invalid order type (must be 0-3)";
    }
    
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
    std::lock_guard<std::mutex> lock(order_status_mutex_);
    
    auto it = order_status_cache_.find(order_id);
    if (it != order_status_cache_.end()) {
        return it->second;
    }
    
    return Order{}; // Order not found
}

void Stock::matchingEngineWorker() {
    // Set CPU affinity and priority for this thread
    CPUAffinity::setCurrentThreadHighPriority();
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Matching engine started on core "
                             << matching_engine_core_ << std::endl;);
    
    int idle_count = 0;
    while (true) {
        if (!running_.load(std::memory_order_seq_cst)) {
            // Fast drain of any remaining work without processing to honour shutdown
            int drained = 0;
            OrderMessage* pending;
            while ((pending = order_queue_.dequeue()) != nullptr) {
                order_message_pool_.deallocate(pending);
                drained++;
                // Check periodically if we need to exit immediately
                if (drained % 100 == 0 && drained > 0) {
                    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Drained " << drained << " orders..." << std::endl;);
                }
            }
            if (drained > 0) {
                ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Drained total " << drained << " orders on shutdown" << std::endl;);
            }
            break;
        }

        // Process incoming orders (lock-free)
        OrderMessage* msg = order_queue_.dequeue();
        if (msg) {
            idle_count = 0;
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
        
        // Yield CPU if no work - sleep briefly every 1000 idle cycles
        if (!msg) {
            if (++idle_count % 1000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                std::this_thread::yield();
            }
        }
    }
}

void Stock::marketDataWorker() {
    CPUAffinity::setCurrentThreadHighPriority();
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Market data worker started on core "
                             << market_data_core_ << std::endl;);
    
    int idle_count = 0;
    while (true) {
        if (!running_.load(std::memory_order_seq_cst)) {
            if (MarketDataMessage* pending = market_data_queue_.dequeue()) {
                market_data_message_pool_.deallocate(pending);
                continue;
            }
            break;
        }

        MarketDataMessage* msg = market_data_queue_.dequeue();
        if (msg) {
            idle_count = 0;
            // Broadcast market data to subscribers
            // In production, this would publish to market data feeds
            messages_sent_.fetch_add(1, std::memory_order_relaxed);
            market_data_message_pool_.deallocate(msg);
        } else {
            if (++idle_count % 1000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                std::this_thread::yield();
            }
        }
    }
}

void Stock::tradePublisherWorker() {
    CPUAffinity::setCurrentThreadHighPriority();
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Trade publisher started on core "
                             << trade_publisher_core_ << std::endl;);
    
    int idle_count = 0;
    while (true) {
        if (!running_.load(std::memory_order_seq_cst)) {
            if (TradeMessage* pending = trade_queue_.dequeue()) {
                trade_message_pool_.deallocate(pending);
                continue;
            }
            break;
        }

        TradeMessage* msg = trade_queue_.dequeue();
        if (msg) {
            idle_count = 0;
            // Publish trade to external systems
            ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Trade: "
                                     << msg->trade.quantity << "@" << msg->trade.price << std::endl;);
            
            trades_executed_.fetch_add(1, std::memory_order_relaxed);
            trade_message_pool_.deallocate(msg);
        } else {
            if (++idle_count % 1000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                std::this_thread::yield();
            }
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
    
    // Update order status cache (thread-safe)
    {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        order_status_cache_[order->order_id] = *order;
    }
    
    // Try to match immediately
    auto trades = matchOrder(order);
    
    // If order still has remaining quantity, add to book
    if (order->remaining_qty > 0) {
        addOrderToBook(order);
    }
    
    // Update order status cache after matching
    {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        order_status_cache_[order->order_id] = *order;
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
        
        // Update order status cache
        {
            std::lock_guard<std::mutex> lock(order_status_mutex_);
            order_status_cache_[order_id] = *order;
        }
        
        orders_.erase(it);
        order_pool_.deallocate(order);
    }
}

std::vector<Trade> Stock::matchOrder(Order* incoming_order) {
    std::vector<Trade> trades;
    
    // Order types: 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    
    // For FOK (Fill-or-Kill), check if order can be fully filled first
    if (incoming_order->type == 3) {
        int64_t available_qty = 0;
        
        if (incoming_order->side == 0) { // BUY FOK
            PriceLevelNode* level = best_ask_;
            while (level && available_qty < incoming_order->quantity) {
                // MARKET orders match at any price, LIMIT orders check price
                if (incoming_order->type != 0 && incoming_order->price < level->price) {
                    break;
                }
                available_qty += level->total_quantity;
                level = level->next_level;
            }
        } else { // SELL FOK
            PriceLevelNode* level = best_bid_;
            while (level && available_qty < incoming_order->quantity) {
                // MARKET orders match at any price, LIMIT orders check price
                if (incoming_order->type != 0 && incoming_order->price > level->price) {
                    break;
                }
                available_qty += level->total_quantity;
                level = level->next_level;
            }
        }
        
        // If cannot fill completely, cancel the order
        if (available_qty < incoming_order->quantity) {
            incoming_order->status = "cancelled";
            incoming_order->remaining_qty = 0;
            return trades; // Empty - no trades executed
        }
    }
    
    if (incoming_order->side == 0) { // BUY order
        // Match against sell orders (asks)
        while (incoming_order->remaining_qty > 0 && best_ask_) {
            // For LIMIT and IOC orders, check price constraint
            // For MARKET orders (type == 0), match at any price BUT with protection
            if (incoming_order->type == 0) {
                // Market order protection: prevent execution at unreasonable prices
                Price current_price = last_price_.load(std::memory_order_relaxed);
                if (current_price > 0) {
                    Price max_buy_price = static_cast<Price>(current_price * (1.0 + MAX_MARKET_ORDER_DEVIATION));
                    if (best_ask_->price > max_buy_price) {
                        // Price too high, stop matching and cancel unfilled portion
                        incoming_order->status = "cancelled";
                        incoming_order->remaining_qty = 0;
                        break;
                    }
                }
            } else if (incoming_order->price < best_ask_->price) {
                break; // No more matches possible for LIMIT orders
            }
            
            PriceLevelNode* ask_level = best_ask_;
            Order* sell_order = ask_level->first_order;
            
            if (!sell_order || sell_order->remaining_qty == 0) {
                // Remove empty level
                best_ask_ = ask_level->next_level;
                price_level_pool_.deallocate(ask_level);
                continue;
            }
            
            // Self-trade prevention: skip orders from same user
            if (incoming_order->user_id == sell_order->user_id) {
                // Move to next order at this level
                if (sell_order->next_at_price) {
                    sell_order = sell_order->next_at_price;
                    continue;
                } else {
                    // No more orders at this level, move to next level
                    best_ask_ = ask_level->next_level;
                    continue;
                }
            }
            
            // Execute trade
            int64_t trade_qty = (std::min)(incoming_order->remaining_qty, sell_order->remaining_qty);
            Price trade_price = sell_order->price; // Price discovery: taker pays maker's price
            
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
            // For LIMIT and IOC orders, check price constraint
            // For MARKET orders (type == 0), match at any price BUT with protection
            if (incoming_order->type == 0) {
                // Market order protection: prevent execution at unreasonable prices
                Price current_price = last_price_.load(std::memory_order_relaxed);
                if (current_price > 0) {
                    Price min_sell_price = static_cast<Price>(current_price * (1.0 - MAX_MARKET_ORDER_DEVIATION));
                    if (best_bid_->price < min_sell_price) {
                        // Price too low, stop matching and cancel unfilled portion
                        incoming_order->status = "cancelled";
                        incoming_order->remaining_qty = 0;
                        break;
                    }
                }
            } else if (incoming_order->price > best_bid_->price) {
                break; // No more matches possible for LIMIT orders
            }
            
            PriceLevelNode* bid_level = best_bid_;
            Order* buy_order = bid_level->first_order;
            
            if (!buy_order || buy_order->remaining_qty == 0) {
                // Remove empty level
                best_bid_ = bid_level->next_level;
                price_level_pool_.deallocate(bid_level);
                continue;
            }
            
            // Self-trade prevention: skip orders from same user
            if (incoming_order->user_id == buy_order->user_id) {
                // Move to next order at this level
                if (buy_order->next_at_price) {
                    buy_order = buy_order->next_at_price;
                    continue;
                } else {
                    // No more orders at this level, move to next level
                    best_bid_ = bid_level->next_level;
                    continue;
                }
            }
            
            // Execute trade
            int64_t trade_qty = (std::min)(incoming_order->remaining_qty, buy_order->remaining_qty);
            Price trade_price = buy_order->price; // Price discovery: taker pays maker's price
            
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
    
    // Handle IOC (Immediate-or-Cancel): Cancel any unfilled portion
    if (incoming_order->type == 2 && incoming_order->remaining_qty > 0) {
        incoming_order->status = "cancelled";
        incoming_order->remaining_qty = 0;
    }
    
    // Handle MARKET orders: If not filled, cancel (shouldn't happen with liquidity)
    if (incoming_order->type == 0 && incoming_order->remaining_qty > 0) {
        incoming_order->status = "cancelled";
        incoming_order->remaining_qty = 0;
    }
    
    return trades;
}

void Stock::addOrderToBook(Order* order) {
    // No lock needed - only called from matching thread which has exclusive write access
    PriceLevelNode* level = findOrCreatePriceLevel(order->price, order->side == 0);
    level->addOrder(order);
}

void Stock::removeOrderFromBook(Order* order) {
    if (!order || !order->price_level) {
        return; // Order not in book
    }
    
    // No lock needed - only called from matching thread which has exclusive write access
    PriceLevelNode* level = order->price_level;
    
    // Remove order from the price level's linked list
    level->removeOrder(order);
    
    // If the price level is now empty, remove it from the book
    if (level->total_quantity == 0 && level->first_order == nullptr) {
        if (order->side == 0) { // BUY side
            // Remove from bid side
            if (best_bid_ == level) {
                best_bid_ = level->next_level;
                price_level_pool_.deallocate(level);
            } else {
                // Find and remove from middle of list
                PriceLevelNode* current = best_bid_;
                while (current && current->next_level != level) {
                    current = current->next_level;
                }
                if (current) {
                    current->next_level = level->next_level;
                    price_level_pool_.deallocate(level);
                }
            }
        } else { // SELL side
            // Remove from ask side
            if (best_ask_ == level) {
                best_ask_ = level->next_level;
                price_level_pool_.deallocate(level);
            } else {
                // Find and remove from middle of list
                PriceLevelNode* current = best_ask_;
                while (current && current->next_level != level) {
                    current = current->next_level;
                }
                if (current) {
                    current->next_level = level->next_level;
                    price_level_pool_.deallocate(level);
                }
            }
        }
    }
    
    order->price_level = nullptr;
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

void Stock::updateDailyStats(Price price, int64_t quantity) {
    // Update daily high/low using compare_exchange for lock-free updates
    Price current_high = day_high_.load(std::memory_order_relaxed);
    while (price > current_high && 
           !day_high_.compare_exchange_weak(current_high, price, 
                                          std::memory_order_relaxed)) {
        current_high = day_high_.load(std::memory_order_relaxed);
    }
    
    Price current_low = day_low_.load(std::memory_order_relaxed);
    while (price < current_low && 
           !day_low_.compare_exchange_weak(current_low, price, 
                                         std::memory_order_relaxed)) {
        current_low = day_low_.load(std::memory_order_relaxed);
    }
    
    // Update VWAP (Volume Weighted Average Price)
    // Convert to double for calculation
    double price_double = static_cast<double>(price) / 100.0;
    double trade_value = price_double * quantity;
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
    msg->last_price = static_cast<double>(last_price_.load(std::memory_order_relaxed)) / 100.0;
    msg->timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Build top bids and asks with shared lock (allows concurrent readers)
    {
        std::shared_lock<std::shared_mutex> lock(orderbook_mutex_);
        
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
    }
    
    if (!market_data_queue_.enqueue(msg)) {
        market_data_message_pool_.deallocate(msg);
    }
}
double Stock::getChangePercent() const {
    Price open = open_price_.load(std::memory_order_relaxed);
    Price current = last_price_.load(std::memory_order_relaxed);
    if (open == 0) return 0.0;
    return ((static_cast<double>(current - open)) / static_cast<double>(open)) * 100.0;
}

double Stock::getChangePoints() const {
    Price open = open_price_.load(std::memory_order_relaxed);
    Price current = last_price_.load(std::memory_order_relaxed);
    return static_cast<double>(current - open) / 100.0;
}

std::vector<PriceLevel> Stock::getTopBids(int count) const {
    // Check if we can use cached snapshot
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    int64_t last_snapshot = last_snapshot_time_ms_.load(std::memory_order_relaxed);
    
    if (now - last_snapshot < SNAPSHOT_CACHE_MS && !cached_bids_.empty()) {
        // Use cached snapshot (no lock needed!)
        std::vector<PriceLevel> result;
        int items = (std::min)(count, static_cast<int>(cached_bids_.size()));
        result.assign(cached_bids_.begin(), cached_bids_.begin() + items);
        return result;
    }
    
    // Cache expired, rebuild snapshot with lock
    std::vector<PriceLevel> bids;
    {
        std::shared_lock<std::shared_mutex> lock(orderbook_mutex_);
        
        PriceLevelNode* level = best_bid_;
        int added = 0;
        
        while (level && added < count) {
            if (level->total_quantity > 0) {
                bids.emplace_back(level->price, level->total_quantity);
                added++;
            }
            level = level->next_level;
        }
    }
    
    // Update cache
    cached_bids_ = bids;
    last_snapshot_time_ms_.store(now, std::memory_order_relaxed);
    
    return bids;
}

std::vector<PriceLevel> Stock::getTopAsks(int count) const {
    // Check if we can use cached snapshot
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    int64_t last_snapshot = last_snapshot_time_ms_.load(std::memory_order_relaxed);
    
    if (now - last_snapshot < SNAPSHOT_CACHE_MS && !cached_asks_.empty()) {
        // Use cached snapshot (no lock needed!)
        std::vector<PriceLevel> result;
        int items = (std::min)(count, static_cast<int>(cached_asks_.size()));
        result.assign(cached_asks_.begin(), cached_asks_.begin() + items);
        return result;
    }
    
    // Cache expired, rebuild snapshot with lock
    std::vector<PriceLevel> asks;
    {
        std::shared_lock<std::shared_mutex> lock(orderbook_mutex_);
        
        PriceLevelNode* level = best_ask_;
        int added = 0;
        
        while (level && added < count) {
            if (level->total_quantity > 0) {
                asks.emplace_back(level->price, level->total_quantity);
                added++;
            }
            level = level->next_level;
        }
    }
    
    // Update cache
    cached_asks_ = asks;
    last_snapshot_time_ms_.store(now, std::memory_order_relaxed);
    
    return asks;
}

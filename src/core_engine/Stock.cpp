/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#define NOMINMAX
#include "Stock.h"
#include "../common/EngineLogging.h"
#include <random>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <limits>
#include <future>
#include <cmath>
#include <utility>

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
    last_bids_snapshot_time_ms_(0), last_asks_snapshot_time_ms_(0),
    market_data_update_counter_(0) {
}

void Stock::setTradeCallback(TradeCallback callback) {
    std::lock_guard<std::mutex> lock(trade_callback_mutex_);
    trade_callback_ = std::move(callback);
}

void Stock::setOrderStatusCallback(OrderStatusCallback callback) {
    std::lock_guard<std::mutex> lock(order_status_callback_mutex_);
    order_status_callback_ = std::move(callback);
}

void Stock::setReservationHandlers(OrderReserveCallback reserve_cb, OrderReleaseCallback release_cb) {
    reserve_callback_ = std::move(reserve_cb);
    release_callback_ = std::move(release_cb);
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
    
    // CRITICAL FIX: Do NOT drain queues here - they are single-consumer queues
    // and only their respective worker threads should dequeue from them.
    // The worker threads will drain their own queues when they see running_=false.
    // Draining from this thread violates single-consumer semantics and can corrupt
    // the lock-free ring buffer state.
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
    
    // 4. Validate maximum quantity to prevent overflow
    static constexpr int64_t MAX_ORDER_QUANTITY = 1000000000; // 1 billion shares
    if (order.quantity > MAX_ORDER_QUANTITY) {
        return "rejected: quantity exceeds maximum allowed";
    }
    
    // 5. Validate price (for non-MARKET orders)
    if (order.type != 0) { // Not a MARKET order
        if (order.price <= 0) {
            return "rejected: price must be positive";
        }
        // Reasonable price range check (between $0.01 and $1,000,000.00)
        if (order.price < 1 || order.price > 100000000) {
            return "rejected: price out of valid range";
        }
        
        // 6. CRITICAL: Check for multiplication overflow (quantity * price)
        // This prevents integer overflow in cash calculations
        // Max safe value: INT64_MAX / price must be >= quantity
        if (order.quantity > INT64_MAX / order.price) {
            return "rejected: order value too large (overflow risk)";
        }
    }
    
    // 7. Validate side
    if (order.side != 0 && order.side != 1) {
        return "rejected: invalid side (must be 0=BUY or 1=SELL)";
    }
    
    // 8. Validate order type
    if (order.type < 0 || order.type > 3) {
        return "rejected: invalid order type (must be 0-3)";
    }

    // Reject duplicate order IDs that are still active
    {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        auto it = order_status_cache_.find(order.order_id);
        if (it != order_status_cache_.end()) {
            const std::string& status = it->second.status;
            const bool is_rejected = status.rfind("rejected", 0) == 0;
            if (!is_rejected && status != "filled" && status != "cancelled") {
                return "rejected: duplicate order_id";
            }
        }
    }

    // Pre-check order book depth to avoid async rejection
    if (order.side == 0) {
        if (total_buy_orders_.load(std::memory_order_relaxed) >= MAX_ORDER_BOOK_DEPTH) {
            return "rejected: buy book depth limit reached";
        }
    } else {
        if (total_sell_orders_.load(std::memory_order_relaxed) >= MAX_ORDER_BOOK_DEPTH) {
            return "rejected: sell book depth limit reached";
        }
    }
    
    bool reservation_acquired = false;
    Price reservation_price = order.price;
    if (order.type == 0) {
        reservation_price = last_price_.load(std::memory_order_relaxed);
        if (reservation_price <= 0) {
            reservation_price = (order.price > 0) ? order.price : Order::fromDouble(1.0);
        }
    }
    std::string reservation_error;
    if (reserve_callback_) {
        if (!reserve_callback_(order, reservation_price, reservation_error)) {
            return reservation_error.empty() ? "rejected: insufficient capacity" : reservation_error;
        }
        reservation_acquired = true;
    }
    
    // Allocate message from pool (lock-free)
    OrderMessage* msg = order_message_pool_.allocate(OrderMessage::NEW_ORDER, order);

    // Track pending status so cancels can find the order immediately
    {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        Order pending_copy = order;
        pending_copy.status = "pending";
        pending_copy.remaining_qty = order.quantity;
        order_status_cache_[order.order_id] = pending_copy;
    }
    
    if (!order_queue_.enqueue(msg)) {
        order_message_pool_.deallocate(msg);
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        auto it = order_status_cache_.find(order.order_id);
        if (it != order_status_cache_.end() && it->second.status == "pending") {
            order_status_cache_.erase(it);
        }
        if (reservation_acquired && release_callback_) {
            release_callback_(order, "queue_full");
        }
        return "Queue full - order rejected";
    }
    
    return "accepted";
}

std::string Stock::cancelOrder(const std::string& order_id) {
    // Check if order exists before submitting cancel
    {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        auto it = order_status_cache_.find(order_id);
        if (it == order_status_cache_.end()) {
            return "Order not found - cancel rejected";
        }
        // Check if order is already in a terminal state
        if (it->second.status == "filled" || it->second.status == "cancelled") {
            return "Order already " + it->second.status + " - cancel rejected";
        }
        it->second.status = "cancel_pending";
    }
    
    OrderMessage* msg = order_message_pool_.allocate(OrderMessage::CANCEL_ORDER, order_id);
    
    if (!order_queue_.enqueue(msg)) {
        order_message_pool_.deallocate(msg);
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        auto it = order_status_cache_.find(order_id);
        if (it != order_status_cache_.end() && it->second.status == "cancel_pending") {
            it->second.status = "open";
        }
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
                             << matching_engine_core_ << " with adaptive load management" << std::endl;);
    
    matching_load_manager_.reset();
    
    while (true) {
        if (!running_.load(std::memory_order_seq_cst)) {
            // Fast drain of any remaining work without processing to honour shutdown
            int drained = 0;
            OrderMessage* pending;
            while ((pending = order_queue_.dequeue()) != nullptr) {
                if (pending->type == OrderMessage::NEW_ORDER) {
                    if (release_callback_) {
                        release_callback_(pending->order, "engine_shutdown");
                    }
                    std::lock_guard<std::mutex> lock(order_status_mutex_);
                    order_status_cache_.erase(pending->order.order_id);
                }
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
        bool did_work = (msg != nullptr);
        
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
        
        // Update market data periodically (using per-instance counter to avoid data race)
        if (++market_data_update_counter_ % 1000 == 0) {
            updateMarketData();
        }
        
        // Adaptive load-based CPU management
        matching_load_manager_.recordIteration(did_work);
        if (!did_work) {
            matching_load_manager_.waitForWork();
        }
    }
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Matching engine stopped at load level: "
                             << matching_load_manager_.getLoadLevelName() << std::endl;);
}

void Stock::marketDataWorker() {
    CPUAffinity::setCurrentThreadHighPriority();
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Market data worker started on core "
                             << market_data_core_ << " with adaptive load management" << std::endl;);
    
    market_data_load_manager_.reset();
    
    while (true) {
        if (!running_.load(std::memory_order_seq_cst)) {
            if (MarketDataMessage* pending = market_data_queue_.dequeue()) {
                market_data_message_pool_.deallocate(pending);
                continue;
            }
            break;
        }

        MarketDataMessage* msg = market_data_queue_.dequeue();
        bool did_work = (msg != nullptr);
        
        if (msg) {
            // Broadcast market data to subscribers
            // In production, this would publish to market data feeds
            messages_sent_.fetch_add(1, std::memory_order_relaxed);
            market_data_message_pool_.deallocate(msg);
        }
        
        // Adaptive load-based CPU management
        market_data_load_manager_.recordIteration(did_work);
        if (!did_work) {
            market_data_load_manager_.waitForWork();
        }
    }
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Market data worker stopped at load level: "
                             << market_data_load_manager_.getLoadLevelName() << std::endl;);
}

void Stock::tradePublisherWorker() {
    CPUAffinity::setCurrentThreadHighPriority();
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Trade publisher started on core "
                             << trade_publisher_core_ << " with adaptive load management" << std::endl;);
    
    trade_publisher_load_manager_.reset();
    
    while (true) {
        if (!running_.load(std::memory_order_seq_cst)) {
            if (TradeMessage* pending = trade_queue_.dequeue()) {
                trade_message_pool_.deallocate(pending);
                continue;
            }
            break;
        }

        TradeMessage* msg = trade_queue_.dequeue();
        bool did_work = (msg != nullptr);
        
        if (msg) {
            // Publish trade to external systems
            ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Trade: "
                                     << msg->trade.quantity << "@" << msg->trade.price << std::endl;);
            
            trades_executed_.fetch_add(1, std::memory_order_relaxed);
            
            // Notify trade observer (e.g., for account settlement)
            {
                std::lock_guard<std::mutex> lock(trade_callback_mutex_);
                if (trade_callback_) {
                    trade_callback_(msg->trade);
                }
            }
            
            trade_message_pool_.deallocate(msg);
        }
        
        // Adaptive load-based CPU management
        trade_publisher_load_manager_.recordIteration(did_work);
        if (!did_work) {
            trade_publisher_load_manager_.waitForWork();
        }
    }
    
    ENGINE_LOG_DEV(std::cout << "[" << symbol_ << "] Trade publisher stopped at load level: "
                             << trade_publisher_load_manager_.getLoadLevelName() << std::endl;);
}

void Stock::processNewOrder(const Order& incoming_order) {
    // Check order book depth limits
    size_t current_buys = total_buy_orders_.load(std::memory_order_relaxed);
    size_t current_sells = total_sell_orders_.load(std::memory_order_relaxed);
    
    if (incoming_order.side == 0 && current_buys >= MAX_ORDER_BOOK_DEPTH) {
        // Buy side full, reject order
        std::cerr << "Order book depth limit reached for buy side: " << symbol_ << std::endl;
        {
            std::lock_guard<std::mutex> lock(order_status_mutex_);
            Order rejected = incoming_order;
            rejected.status = "rejected: buy book depth limit reached";
            rejected.remaining_qty = 0;
            order_status_cache_[incoming_order.order_id] = rejected;
        }
        if (release_callback_) {
            release_callback_(incoming_order, "rejected: buy book depth limit reached");
        }
        return;
    }
    if (incoming_order.side == 1 && current_sells >= MAX_ORDER_BOOK_DEPTH) {
        // Sell side full, reject order
        std::cerr << "Order book depth limit reached for sell side: " << symbol_ << std::endl;
        {
            std::lock_guard<std::mutex> lock(order_status_mutex_);
            Order rejected = incoming_order;
            rejected.status = "rejected: sell book depth limit reached";
            rejected.remaining_qty = 0;
            order_status_cache_[incoming_order.order_id] = rejected;
        }
        if (release_callback_) {
            release_callback_(incoming_order, "rejected: sell book depth limit reached");
        }
        return;
    }
    
    // Allocate order from pool
    Order* order = order_pool_.allocate(incoming_order);
    
    if (orders_.find(order->order_id) != orders_.end()) {
        {
            std::lock_guard<std::mutex> lock(order_status_mutex_);
            Order rejected = incoming_order;
            rejected.status = "rejected: duplicate order_id";
            rejected.remaining_qty = 0;
            order_status_cache_[incoming_order.order_id] = rejected;
        }
        if (release_callback_) {
            release_callback_(incoming_order, "rejected: duplicate order_id");
        }
        order_pool_.deallocate(order);
        return; // Duplicate order ID
    }
    
    orders_[order->order_id] = order;
    
    // Update order status cache (thread-safe)
    {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        order_status_cache_[order->order_id] = *order;
    }
    
    std::vector<Trade> trades;
    {
        std::unique_lock<std::shared_mutex> book_lock(orderbook_mutex_);

        bool skip_matching = false;
        std::string release_reason;

        if (order->type == 3) {
            auto can_fulfill_fok = [this, order]() {
                int64_t remaining = order->quantity;
                if (order->side == 0) {
                    PriceLevelNode* level = best_ask_;
                    while (level && remaining > 0) {
                        if (order->price > 0 && order->price < level->price) {
                            break;
                        }
                        Order* maker = level->first_order;
                        while (maker && remaining > 0) {
                            if (maker->user_id != order->user_id) {
                                remaining -= maker->remaining_qty;
                            }
                            maker = maker->next_at_price;
                        }
                        level = level->next_level;
                    }
                } else {
                    PriceLevelNode* level = best_bid_;
                    while (level && remaining > 0) {
                        if (order->price > 0 && order->price > level->price) {
                            break;
                        }
                        Order* maker = level->first_order;
                        while (maker && remaining > 0) {
                            if (maker->user_id != order->user_id) {
                                remaining -= maker->remaining_qty;
                            }
                            maker = maker->next_at_price;
                        }
                        level = level->next_level;
                    }
                }
                return remaining <= 0;
            };

            if (!can_fulfill_fok()) {
                skip_matching = true;
                order->status = "cancelled";
                order->remaining_qty = 0;
                release_reason = "fok_not_filled";
            }
        }

        if (!skip_matching) {
            trades = matchOrder(order);
        }

        if (order->remaining_qty > 0) {
            if (order->type == 1) {
                addOrderToBook(order);
            } else {
                order->status = "cancelled";
                order->remaining_qty = 0;
                if (release_reason.empty()) {
                    release_reason = (order->type == 3) ? "fok_not_filled" : "cancelled";
                }
            }
        }

        if (order != nullptr && order->remaining_qty == 0) {
            {
                std::lock_guard<std::mutex> lock(order_status_mutex_);
                order_status_cache_[order->order_id] = *order;
            }
            Order release_snapshot = *order;
            if (release_reason.empty()) {
                release_reason = release_snapshot.status;
            }
            orders_.erase(order->order_id);
            if (release_callback_) {
                release_callback_(release_snapshot, release_reason);
            }
            order_pool_.deallocate(order);
            order = nullptr;
        }
    }
    
    // Update order status cache after matching (only if order still exists)
    if (order != nullptr) {
        std::lock_guard<std::mutex> lock(order_status_mutex_);
        order_status_cache_[order->order_id] = *order;
        
        // SEC Compliance: Notify about order status change for persistence
        if (order_status_callback_) {
            std::lock_guard<std::mutex> cb_lock(order_status_callback_mutex_);
            if (order_status_callback_) {
                order_status_callback_(*order);
            }
        }
    }
    
    // Send trades to publisher
    for (const auto& trade : trades) {
        TradeMessage* trade_msg = trade_message_pool_.allocate(trade, true);
        while (!trade_queue_.enqueue(trade_msg)) {
            if (!running_.load(std::memory_order_acquire)) {
                trade_message_pool_.deallocate(trade_msg);
                return;
            }
            std::this_thread::yield();
        }
        
        // Update market data
        last_price_.store(trade.price, std::memory_order_relaxed);
        volume_.fetch_add(trade.quantity, std::memory_order_relaxed);
        updateDailyStats(trade.price, trade.quantity);

        // CRITICAL FIX: Removed duplicate trade callback execution from here
        // The trade callback is already invoked in tradePublisherWorker (line ~404)
        // Calling it here causes double-execution, leading to double accounting
        // (cash/positions adjusted twice per trade, resulting in incorrect balances)
        // Trade callbacks should only be invoked once per trade, by the trade publisher thread
    }
}

void Stock::processCancelOrder(const std::string& order_id) {
    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
        Order* order = it->second;
        {
            std::unique_lock<std::shared_mutex> book_lock(orderbook_mutex_);
            removeOrderFromBook(order);
        }
        order->status = "cancelled";
        
        // Update order status cache
        {
            std::lock_guard<std::mutex> lock(order_status_mutex_);
            order_status_cache_[order_id] = *order;
        }
        
        if (release_callback_) {
            release_callback_(*order, "cancelled");
        }
        
        orders_.erase(it);
        order_pool_.deallocate(order);
    }
}

std::vector<Trade> Stock::matchOrder(Order* incoming_order) {
    std::vector<Trade> trades;
    
    // Order types: 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    
    // For FOK (Fill-or-Kill), check if order can be fully filled first
    // CRITICAL FIX: Must exclude self-liquidity from the availability check
    if (incoming_order->type == 3) {
        int64_t available_qty = 0;
        
        if (incoming_order->side == 0) { // BUY FOK
            PriceLevelNode* level = best_ask_;
            while (level && available_qty < incoming_order->quantity) {
                // MARKET orders match at any price, LIMIT orders check price
                if (incoming_order->type != 0 && incoming_order->price < level->price) {
                    break;
                }
                // Count only non-self orders
                Order* maker = level->first_order;
                while (maker) {
                    if (maker->user_id != incoming_order->user_id) {
                        available_qty += maker->remaining_qty;
                    }
                    maker = maker->next_at_price;
                }
                level = level->next_level;
            }
        } else { // SELL FOK
            PriceLevelNode* level = best_bid_;
            while (level && available_qty < incoming_order->quantity) {
                // MARKET orders match at any price, LIMIT orders check price
                if (incoming_order->type != 0 && incoming_order->price > level->price) {
                    break;
                }
                // Count only non-self orders
                Order* maker = level->first_order;
                while (maker) {
                    if (maker->user_id != incoming_order->user_id) {
                        available_qty += maker->remaining_qty;
                    }
                    maker = maker->next_at_price;
                }
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
            // Keep searching through orders at this price level until we find one from a different user
            while (sell_order && incoming_order->user_id == sell_order->user_id) {
                sell_order = sell_order->next_at_price;
            }
            
            // CRITICAL FIX: If all orders at this level are from the same user, we must properly
            // clean up the level instead of just skipping it. Otherwise, those orders become
            // unreachable and the price level leaks memory.
            if (!sell_order) {
                // All orders at this level belong to the incoming user - skip entire level
                PriceLevelNode* level_to_skip = ask_level;
                best_ask_ = ask_level->next_level;
                
                // Cancel all orders at this level (they're all from the same user)
                Order* order_at_level = level_to_skip->first_order;
                while (order_at_level) {
                    Order* next_order = order_at_level->next_at_price;
                    
                    // Mark as cancelled and update cache
                    order_at_level->status = "cancelled";
                    order_at_level->remaining_qty = 0;
                    {
                        std::lock_guard<std::mutex> lock(order_status_mutex_);
                        order_status_cache_[order_at_level->order_id] = *order_at_level;
                    }
                    if (release_callback_) {
                        release_callback_(*order_at_level, "cancelled");
                    }
                    // Clean up
                    orders_.erase(order_at_level->order_id);
                    total_sell_orders_.fetch_sub(1, std::memory_order_relaxed);
                    order_at_level->price_level = nullptr;  // Prevent use-after-free
                    order_pool_.deallocate(order_at_level);
                    
                    order_at_level = next_order;
                }
                
                // Deallocate the empty level
                price_level_pool_.deallocate(level_to_skip);
                continue;
            }
            
            // Execute trade
            int64_t trade_qty = (std::min)(incoming_order->remaining_qty, sell_order->remaining_qty);
            Price trade_price = sell_order->price; // Price discovery: taker pays maker's price
            
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            trades.emplace_back(
                incoming_order->order_id,
                sell_order->order_id,
                symbol_,
                trade_price,
                trade_qty,
                now,
                incoming_order->user_id,
                sell_order->user_id);
            
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
                
                // CRITICAL FIX: Use removeOrder to properly unlink from the price level
                // This handles all the pointer updates (first_order, last_order, prev/next)
                ask_level->removeOrder(sell_order);
                
                // CRITICAL FIX: Decrement counter and clean up filled order
                total_sell_orders_.fetch_sub(1, std::memory_order_relaxed);
                
                // Update order status cache before cleanup
                {
                    std::lock_guard<std::mutex> lock(order_status_mutex_);
                    order_status_cache_[sell_order->order_id] = *sell_order;
                }
                if (release_callback_) {
                    release_callback_(*sell_order, sell_order->status);
                }
                
                // Clean up filled order
                orders_.erase(sell_order->order_id);
                order_pool_.deallocate(sell_order);
            } else {
                sell_order->status = "partial";
                
                // Update order status cache for the matched order
                {
                    std::lock_guard<std::mutex> lock(order_status_mutex_);
                    order_status_cache_[sell_order->order_id] = *sell_order;
                }
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
            // Keep searching through orders at this price level until we find one from a different user
            while (buy_order && incoming_order->user_id == buy_order->user_id) {
                buy_order = buy_order->next_at_price;
            }
            
            // CRITICAL FIX: If all orders at this level are from the same user, we must properly
            // clean up the level instead of just skipping it. Otherwise, those orders become
            // unreachable and the price level leaks memory.
            if (!buy_order) {
                // All orders at this level belong to the incoming user - skip entire level
                PriceLevelNode* level_to_skip = bid_level;
                best_bid_ = bid_level->next_level;
                
                // Cancel all orders at this level (they're all from the same user)
                Order* order_at_level = level_to_skip->first_order;
                while (order_at_level) {
                    Order* next_order = order_at_level->next_at_price;
                    
                    // Mark as cancelled and update cache
                    order_at_level->status = "cancelled";
                    order_at_level->remaining_qty = 0;
                    {
                        std::lock_guard<std::mutex> lock(order_status_mutex_);
                        order_status_cache_[order_at_level->order_id] = *order_at_level;
                    }
                    if (release_callback_) {
                        release_callback_(*order_at_level, "cancelled");
                    }
                    
                    // Clean up
                    orders_.erase(order_at_level->order_id);
                    total_buy_orders_.fetch_sub(1, std::memory_order_relaxed);
                    order_at_level->price_level = nullptr;  // Prevent use-after-free
                    order_pool_.deallocate(order_at_level);
                    
                    order_at_level = next_order;
                }
                
                // Deallocate the empty level
                price_level_pool_.deallocate(level_to_skip);
                continue;
            }
            
            // Execute trade
            int64_t trade_qty = (std::min)(incoming_order->remaining_qty, buy_order->remaining_qty);
            Price trade_price = buy_order->price; // Price discovery: taker pays maker's price
            
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            trades.emplace_back(
                buy_order->order_id,
                incoming_order->order_id,
                symbol_,
                trade_price,
                trade_qty,
                now,
                buy_order->user_id,
                incoming_order->user_id);
            
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
                
                // CRITICAL FIX: Use removeOrder to properly unlink from the price level
                // This handles all the pointer updates (first_order, last_order, prev/next)
                bid_level->removeOrder(buy_order);
                
                // CRITICAL FIX: Decrement counter and clean up filled order
                total_buy_orders_.fetch_sub(1, std::memory_order_relaxed);
                
                // Update order status cache before cleanup
                {
                    std::lock_guard<std::mutex> lock(order_status_mutex_);
                    order_status_cache_[buy_order->order_id] = *buy_order;
                }
                if (release_callback_) {
                    release_callback_(*buy_order, buy_order->status);
                }
                
                // Clean up filled order
                orders_.erase(buy_order->order_id);
                order_pool_.deallocate(buy_order);
            } else {
                buy_order->status = "partial";
                
                // Update order status cache for the matched order
                {
                    std::lock_guard<std::mutex> lock(order_status_mutex_);
                    order_status_cache_[buy_order->order_id] = *buy_order;
                }
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
    
    // Update order book depth counters
    if (order->side == 0) {
        total_buy_orders_.fetch_add(1, std::memory_order_relaxed);
    } else {
        total_sell_orders_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Stock::removeOrderFromBook(Order* order) {
    if (!order || !order->price_level) {
        return; // Order not in book
    }
    
    // Update order book depth counters
    if (order->side == 0) {
        total_buy_orders_.fetch_sub(1, std::memory_order_relaxed);
    } else {
        total_sell_orders_.fetch_sub(1, std::memory_order_relaxed);
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
    
    // Update VWAP (Volume Weighted Average Price) with proper synchronization
    {
        std::lock_guard<std::mutex> lock(vwap_mutex_);
        double price_double = static_cast<double>(price) / 100.0;
        double trade_value = price_double * quantity;
        
        // CRITICAL: Prevent overflow after many trades
        // If denominator gets too large, reset with current weighted average
        static constexpr int64_t VWAP_RESET_THRESHOLD = INT64_MAX / 2;
        if (vwap_denominator_ > VWAP_RESET_THRESHOLD) {
            // Reset VWAP accumulators while preserving the current VWAP value
            double current_vwap = vwap_.load(std::memory_order_relaxed);
            vwap_numerator_ = current_vwap * 1000000; // Scale to reasonable precision
            vwap_denominator_ = 1000000;
        }
        
        vwap_numerator_ += trade_value;
        vwap_denominator_ += quantity;
        
        if (vwap_denominator_ > 0) {
            double new_vwap = vwap_numerator_ / vwap_denominator_;
            vwap_.store(new_vwap, std::memory_order_relaxed);
        }
        
        // Also update total value traded for other stats
        total_value_traded_.fetch_add(trade_value, std::memory_order_relaxed);
    }
}

void Stock::updateMarketData() {
    MarketDataMessage* msg = market_data_message_pool_.allocate();
    msg->symbol = symbol_;
    msg->last_price = last_price_.load(std::memory_order_relaxed);
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
    
    while (!market_data_queue_.enqueue(msg)) {
        if (!running_.load(std::memory_order_acquire)) {
            market_data_message_pool_.deallocate(msg);
            return;
        }
        std::this_thread::yield();
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

Price Stock::getChangePointsFixed() const {
    Price open = open_price_.load(std::memory_order_relaxed);
    Price current = last_price_.load(std::memory_order_relaxed);
    return current - open;
}

std::vector<PriceLevel> Stock::getTopBids(int count) const {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> cache_lock(snapshot_mutex_);
        int64_t last_snapshot = last_bids_snapshot_time_ms_.load(std::memory_order_relaxed);
        if (now - last_snapshot < SNAPSHOT_CACHE_MS && !cached_bids_.empty()) {
            int items = (std::min)(count, static_cast<int>(cached_bids_.size()));
            return std::vector<PriceLevel>(cached_bids_.begin(), cached_bids_.begin() + items);
        }
    }

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

    {
        std::lock_guard<std::mutex> cache_lock(snapshot_mutex_);
        cached_bids_ = bids;
        last_bids_snapshot_time_ms_.store(now, std::memory_order_relaxed);
        int items = (std::min)(count, static_cast<int>(cached_bids_.size()));
        return std::vector<PriceLevel>(cached_bids_.begin(), cached_bids_.begin() + items);
    }
}

std::vector<PriceLevel> Stock::getTopAsks(int count) const {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> cache_lock(snapshot_mutex_);
        int64_t last_snapshot = last_asks_snapshot_time_ms_.load(std::memory_order_relaxed);
        if (now - last_snapshot < SNAPSHOT_CACHE_MS && !cached_asks_.empty()) {
            int items = (std::min)(count, static_cast<int>(cached_asks_.size()));
            return std::vector<PriceLevel>(cached_asks_.begin(), cached_asks_.begin() + items);
        }
    }

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

    {
        std::lock_guard<std::mutex> cache_lock(snapshot_mutex_);
        cached_asks_ = asks;
        last_asks_snapshot_time_ms_.store(now, std::memory_order_relaxed);
        int items = (std::min)(count, static_cast<int>(cached_asks_.size()));
        return std::vector<PriceLevel>(cached_asks_.begin(), cached_asks_.begin() + items);
    }
}

Price Stock::getVWAPFixed() const {
    const double vwap_value = vwap_.load(std::memory_order_relaxed);
    return static_cast<Price>(std::llround(vwap_value * 100.0));
}

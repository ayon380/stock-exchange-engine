#define NOMINMAX
#include "StockExchange.h"
#include "CPUAffinity.h"
#include "../common/EngineLogging.h"
#include "../api/AuthenticationManager.h"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <random>
#include <future>
#include <utility>

StockExchange::StockExchange(const std::string& db_connection_string) 
    : running_(false), market_index_base_value_(1000.0) {  // Start index at 1000 like S&P 500
    if (!db_connection_string.empty()) {
        db_manager_ = std::make_unique<DatabaseManager>(db_connection_string);
    }
    
    // Initialize market index
    current_market_index_.index_name = "TECH500";
    current_market_index_.index_value = market_index_base_value_;
    current_market_index_.day_open = market_index_base_value_;
    current_market_index_.day_high = market_index_base_value_;
    current_market_index_.day_low = market_index_base_value_;
}

StockExchange::~StockExchange() {
    stop();
}

bool StockExchange::initialize() {
    ENGINE_LOG_DEV(std::cout << "Initializing Stock Exchange with " << STOCK_SYMBOLS.size() << " stocks..." << std::endl;);
    
    // Get available CPU cores for optimal affinity assignment
    auto available_cores = CPUAffinity::getAvailableCores();
    
    // CRITICAL FIX: Handle single-core or constrained environments
    if (available_cores.empty()) {
        // Fallback to a single default core (0) if no cores are available
        std::cerr << "Warning: No available cores detected, using core 0 for all threads" << std::endl;
        available_cores.push_back(0);
    }
    
    ENGINE_LOG_DEV(std::cout << "Available CPU cores: " << available_cores.size() << std::endl;);
    
    // Initialize database connection
    if (db_manager_) {
        if (!db_manager_->connect()) {
            std::cerr << "Warning: Failed to connect to database, continuing with in-memory only" << std::endl;
        } else {
            loadFromDatabase();
        }
    }
    
    // Initialize stocks with different starting prices and CPU affinity
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> price_dist(50.0, 200.0);
    
    for (size_t i = 0; i < STOCK_SYMBOLS.size(); ++i) {
        const auto& symbol = STOCK_SYMBOLS[i];
        double initial_price = price_dist(gen);
        
        // If we have database data, use that instead
        if (db_manager_ && db_manager_->isConnected()) {
            auto stock_data = db_manager_->getLatestStockData(symbol);
            if (stock_data.timestamp_ms > 0) {
                initial_price = stock_data.lastPriceToDouble();
            }
        }
        
        // Assign CPU cores in round-robin fashion
        // Each stock needs 3 cores: matching, market data, trade publisher
        int base_core = (i * 3) % available_cores.size();
        int matching_core = available_cores[base_core];
        int md_core = available_cores[(base_core + 1) % available_cores.size()];
        int trade_core = available_cores[(base_core + 2) % available_cores.size()];
        
        stocks_[symbol] = std::make_unique<Stock>(symbol, initial_price, 
                                                  matching_core, md_core, trade_core);
            stocks_[symbol]->setTradeCallback([this](const Trade& trade) {
                dispatchTrade(trade);
            });
        if (reserve_callback_) {
            stocks_[symbol]->setReservationHandlers(reserve_callback_, release_callback_);
        }
        
    ENGINE_LOG_DEV(std::cout << "Initialized " << symbol << " at $" << initial_price
                 << " (cores: " << matching_core << "," << md_core
                 << "," << trade_core << ")" << std::endl;);
    }
    
    return true;
}

void StockExchange::start() {
    if (running_.load()) {
    ENGINE_LOG_DEV(std::cout << "Stock Exchange is already running" << std::endl;);
        return;
    }
    
    running_.store(true);
    
    // Start all individual stock threads
    for (auto& [symbol, stock] : stocks_) {
        stock->start();
    }
    
    // Start index calculation thread
    index_thread_ = std::thread(&StockExchange::indexWorker, this);
    
    // Start database sync thread
    if (db_manager_) {
        db_sync_thread_ = std::thread(&StockExchange::databaseSyncWorker, this);
        db_manager_->startBackgroundSync();
    }
    
    ENGINE_LOG_DEV(std::cout << "Stock Exchange started with " << stocks_.size()
                             << " stocks, 1 index thread, and 1 database sync thread" << std::endl;);
}

void StockExchange::stop() {
    if (!running_.load()) {
        return;
    }
    
    ENGINE_LOG_DEV(std::cout << "Stopping StockExchange..." << std::endl;);
    running_.store(false);
    
    // Wake up the database sync thread
    db_sync_cv_.notify_all();
    
    // Stop all stock threads with progress indicator and timeout protection
    ENGINE_LOG_DEV(std::cout << "Stopping " << stocks_.size() << " stock threads..." << std::endl;);
    size_t stopped = 0;
    
    // First, prepare all stocks for shutdown (drain queues)
    for (auto& [symbol, stock] : stocks_) {
        ENGINE_LOG_DEV(std::cout << "  Preparing " << symbol << " for shutdown..." << std::flush;);
        stock->prepareForShutdown();
        ENGINE_LOG_DEV(std::cout << " prepared" << std::endl;);
    }
    
    // Give worker threads a moment to see running_=false and exit their loops
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Now stop/join all stocks
    for (auto& [symbol, stock] : stocks_) {
        ENGINE_LOG_DEV(std::cout << "  Stopping " << symbol << "..." << std::flush;);
        stock->stop();
        ENGINE_LOG_DEV(std::cout << " ✓" << std::endl;);
        stopped++;
    }
    std::cout << "✓ All " << stopped << " stock threads stopped" << std::endl;
    
    // Stop index thread with timeout
    std::cout << "✓ Stopping index thread..." << std::flush;
    if (index_thread_.joinable()) {
        auto future = std::async(std::launch::async, [this]() {
            index_thread_.join();
        });
        if (future.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout) {
            std::cerr << "Warning: index thread timeout, detaching" << std::endl;
            index_thread_.detach();
        }
    }
    std::cout << " done" << std::endl;
    
    // Stop database sync with timeout
    std::cout << "✓ Stopping database sync..." << std::flush;
    if (db_manager_) {
        db_manager_->stopBackgroundSync();
    }
    
    if (db_sync_thread_.joinable()) {
        auto future = std::async(std::launch::async, [this]() {
            db_sync_thread_.join();
        });
        if (future.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout) {
            std::cerr << "Warning: db_sync thread timeout, detaching" << std::endl;
            db_sync_thread_.detach();
        }
    }
    std::cout << " done" << std::endl;
    
    ENGINE_LOG_DEV(std::cout << "StockExchange stopped" << std::endl;);
    if (db_manager_ && db_manager_->isConnected()) {
        saveToDatabase();
    }
    
    ENGINE_LOG_DEV(std::cout << "Stock Exchange stopped" << std::endl;);
}

std::string StockExchange::submitOrder(const std::string& symbol, const Order& order) {
    if (!running_.load(std::memory_order_acquire)) {
        return "rejected: exchange not running";
    }

    auto it = stocks_.find(symbol);
    if (it == stocks_.end()) {
        return "Symbol not found";
    }
    
    std::string result = it->second->submitOrder(order);
    
    // Broadcast market data update after order submission
    if (result == "accepted") {
        broadcastMarketData(symbol);
    }
    
    return result;
}

Order StockExchange::getOrderStatus(const std::string& symbol, const std::string& order_id) {
    auto it = stocks_.find(symbol);
    if (it == stocks_.end()) {
        return Order{}; // Empty order
    }
    
    return it->second->getOrderStatus(order_id);
}

MarketDataUpdate StockExchange::getMarketData(const std::string& symbol) {
    auto it = stocks_.find(symbol);
    if (it == stocks_.end()) {
        return MarketDataUpdate{};
    }
    
    Stock* stock = it->second.get();
    MarketDataUpdate update;
    update.symbol = symbol;
    update.last_price = stock->getLastPriceFixed();
    update.last_qty = 0; // This would be from the last trade
    update.top_bids = stock->getTopBids(5);
    update.top_asks = stock->getTopAsks(5);
    update.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return update;
}

std::vector<IndexEntry> StockExchange::getTopIndex(const std::string& criterion, int top_n) {
    std::lock_guard<std::mutex> lock(index_mutex_);
    
    std::vector<IndexEntry> sorted_index = current_index_;
    
    if (criterion == "volume") {
        std::sort(sorted_index.begin(), sorted_index.end(),
                 [](const IndexEntry& a, const IndexEntry& b) {
                     return a.volume > b.volume;
                 });
    } else if (criterion == "change") {
        std::sort(sorted_index.begin(), sorted_index.end(),
                 [](const IndexEntry& a, const IndexEntry& b) {
                     return a.change_pct > b.change_pct;
                 });
    }
    
    if (sorted_index.size() > static_cast<size_t>(top_n)) {
        sorted_index.resize(top_n);
    }
    
    return sorted_index;
}

void StockExchange::subscribeToMarketData(const std::string& symbol, MarketDataCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    market_data_subscribers_[symbol].push_back(callback);
}

void StockExchange::subscribeToIndex(IndexUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    index_subscribers_.push_back(callback);
}

void StockExchange::subscribeToMarketIndex(MarketIndexCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    market_index_subscribers_.push_back(callback);
}

void StockExchange::subscribeToAllStocks(AllStocksCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    all_stocks_subscribers_.push_back(callback);
}

void StockExchange::unsubscribeFromMarketData(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    market_data_subscribers_[symbol].clear();
}

void StockExchange::unsubscribeFromIndex() {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    index_subscribers_.clear();
}

void StockExchange::unsubscribeFromMarketIndex() {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    market_index_subscribers_.clear();
}

void StockExchange::unsubscribeFromAllStocks() {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    all_stocks_subscribers_.clear();
}

void StockExchange::indexWorker() {
    while (running_.load()) {
        calculateIndex();
        calculateMarketIndex();
        broadcastIndex();
        broadcastMarketIndex();
        broadcastAllStocks();
        
        // Sleep in small increments to be responsive to shutdown
        for (int i = 0; i < 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void StockExchange::databaseSyncWorker() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(db_sync_mutex_);
            db_sync_cv_.wait_for(lock, std::chrono::seconds(30), [this]() { return !running_.load(); });
        }
        
        if (!running_.load()) break;
        
        saveToDatabase();
    }
}

void StockExchange::calculateIndex() {
    std::lock_guard<std::mutex> lock(index_mutex_);
    current_index_.clear();
    
    for (const auto& [symbol, stock] : stocks_) {
        IndexEntry entry(
            symbol,
            stock->getLastPriceFixed(),
            stock->getChangePercent(),
            stock->getVolume()
        );
        current_index_.push_back(entry);
    }
}

void StockExchange::calculateMarketIndex() {
    std::lock_guard<std::mutex> lock(market_index_mutex_);
    
    // Calculate index based on top 5 performing stocks (by market cap simulation)
    std::vector<std::pair<std::string, double>> stock_values;
    
    for (const auto& [symbol, stock] : stocks_) {
    double price_dollars = stock->getLastPrice() ;
    double market_cap = price_dollars * stock->getVolume(); // Simplified market cap
        stock_values.emplace_back(symbol, market_cap);
    }
    
    // Sort by market cap (descending)
    std::sort(stock_values.begin(), stock_values.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // Take all stocks for index calculation (instead of top 5)
    int constituents_count = static_cast<int>(stock_values.size());
    current_market_index_.constituents.clear();
    
    double total_weight = 0.0;
    double weighted_price_sum = 0.0;
    
    for (int i = 0; i < constituents_count; ++i) {
        const std::string& symbol = stock_values[i].first;
        auto& stock = stocks_[symbol];
        
        // Use equal weights for all stocks
        double weight = 1.0 / constituents_count;
        total_weight += weight;
        
    Price price_fixed = stock->getLastPriceFixed();
    double price = static_cast<double>(price_fixed) / 100.0;
        double change_pct = stock->getChangePercent();
        
        weighted_price_sum += price * weight;
        
        // Calculate contribution to index
        double contribution = (price * weight / market_index_base_value_) * 100;
        
        current_market_index_.constituents.emplace_back(
            symbol, price_fixed, weight, contribution, change_pct
        );
    }
    
    // Calculate new index value
    if (total_weight > 0) {
        double new_index_value = (weighted_price_sum / total_weight) * (market_index_base_value_ / 100.0);
        
        // Update daily stats
        if (current_market_index_.index_value == 0) {
            current_market_index_.day_open = new_index_value;
            current_market_index_.day_high = new_index_value;
            current_market_index_.day_low = new_index_value;
        } else {
            current_market_index_.day_high = (std::max)(current_market_index_.day_high, new_index_value);
            current_market_index_.day_low = (std::min)(current_market_index_.day_low, new_index_value);
        }
        
        // Calculate changes
        current_market_index_.change_points = new_index_value - current_market_index_.day_open;
        current_market_index_.change_percent = (current_market_index_.change_points / current_market_index_.day_open) * 100.0;
        
        current_market_index_.index_value = new_index_value;
    }
    
    current_market_index_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

MarketIndex StockExchange::getMarketIndex(const std::string& index_name) {
    std::lock_guard<std::mutex> lock(market_index_mutex_);
    return current_market_index_;
}

std::vector<StockSnapshot> StockExchange::getAllStocksSnapshot(bool include_order_book) {
    std::vector<StockSnapshot> snapshots;
    
    for (const auto& [symbol, stock] : stocks_) {
        StockSnapshot snapshot;
        snapshot.symbol = symbol;
    snapshot.last_price = stock->getLastPriceFixed();
    snapshot.change_points = stock->getChangePointsFixed();
        snapshot.change_percent = stock->getChangePercent();
    snapshot.day_high = stock->getDayHighFixed();
    snapshot.day_low = stock->getDayLowFixed();
    snapshot.day_open = stock->getDayOpenFixed();
        snapshot.volume = stock->getVolume();
    snapshot.vwap = stock->getVWAPFixed();
        
        if (include_order_book) {
            snapshot.top_bids = stock->getTopBids(3);
            snapshot.top_asks = stock->getTopAsks(3);
        }
        
        snapshots.push_back(snapshot);
    }
    
    return snapshots;
}

void StockExchange::broadcastMarketData(const std::string& symbol) {
    // CRITICAL FIX: Snapshot callbacks under lock, then invoke without holding lock
    // to prevent deadlock if callback tries to unsubscribe or if callback is slow
    std::vector<MarketDataCallback> callbacks_snapshot;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        auto it = market_data_subscribers_.find(symbol);
        if (it != market_data_subscribers_.end()) {
            callbacks_snapshot = it->second;
        }
    }
    
    // Invoke callbacks without holding the lock
    if (!callbacks_snapshot.empty()) {
        MarketDataUpdate update = getMarketData(symbol);
        for (const auto& callback : callbacks_snapshot) {
            callback(update);
        }
    }
}

void StockExchange::broadcastIndex() {
    // CRITICAL FIX: Snapshot callbacks under lock, then invoke without holding lock
    std::vector<IndexUpdateCallback> callbacks_snapshot;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        callbacks_snapshot = index_subscribers_;
    }
    
    // Invoke callbacks without holding the lock
    if (!callbacks_snapshot.empty()) {
        std::vector<IndexEntry> top_index = getTopIndex("volume", 5);
        for (const auto& callback : callbacks_snapshot) {
            callback(top_index);
        }
    }
}

void StockExchange::broadcastMarketIndex() {
    // CRITICAL FIX: Snapshot callbacks under lock, then invoke without holding lock
    std::vector<MarketIndexCallback> callbacks_snapshot;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        callbacks_snapshot = market_index_subscribers_;
    }
    
    // Invoke callbacks without holding the lock
    if (!callbacks_snapshot.empty()) {
        MarketIndex index_copy;
        {
            std::lock_guard<std::mutex> index_lock(market_index_mutex_);
            index_copy = current_market_index_;
        }
        
        for (const auto& callback : callbacks_snapshot) {
            callback(index_copy);
        }
    }
}

void StockExchange::broadcastAllStocks() {
    // CRITICAL FIX: Snapshot callbacks under lock, then invoke without holding lock
    std::vector<AllStocksCallback> callbacks_snapshot;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        callbacks_snapshot = all_stocks_subscribers_;
    }
    
    // Invoke callbacks without holding the lock
    if (!callbacks_snapshot.empty()) {
        std::vector<StockSnapshot> snapshots = getAllStocksSnapshot(true);
        
        for (const auto& callback : callbacks_snapshot) {
            callback(snapshots);
        }
    }
}

void StockExchange::dispatchTrade(const Trade& trade) {
    std::vector<TradeCallback> callbacks_snapshot;
    {
        std::lock_guard<std::mutex> lock(trade_subscribers_mutex_);
        callbacks_snapshot = trade_subscribers_;
    }

    for (const auto& callback : callbacks_snapshot) {
        callback(trade);
    }
}

void StockExchange::loadFromDatabase() {
    if (!db_manager_ || !db_manager_->isConnected()) {
        return;
    }
    
    ENGINE_LOG_DEV(std::cout << "Loading stock data from database..." << std::endl;);
    auto stock_data = db_manager_->loadStockData();
    
    // This data will be used during stock initialization
    // The actual loading happens in the initialize() method
}

void StockExchange::saveToDatabase() {
    if (!db_manager_ || !db_manager_->isConnected()) {
        return;
    }
    
    std::vector<StockData> data_batch;
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    for (const auto& [symbol, stock] : stocks_) {
        data_batch.emplace_back(
            symbol,
            stock->getLastPriceFixed(),
            stock->getDayOpenFixed(),
            stock->getVolume(),
            timestamp
        );
    }
    
    db_manager_->saveStockDataBatch(data_batch);
}

bool StockExchange::isHealthy() const {
    // Check if all stocks are running and database is connected (if configured)
    if (!running_.load()) {
        return false;
    }
    
    if (db_manager_ && !db_manager_->isConnected()) {
        return false;
    }
    
    return true;
}

void StockExchange::registerTradeObserver(TradeCallback callback) {
    std::lock_guard<std::mutex> lock(trade_subscribers_mutex_);
    trade_subscribers_.push_back(std::move(callback));
}

void StockExchange::setAuthenticationManager(AuthenticationManager* manager) {
    auth_manager_ = manager;

    if (!auth_manager_) {
        reserve_callback_ = nullptr;
        release_callback_ = nullptr;
        trade_observer_registered_ = false;
        return;
    }

    reserve_callback_ = [this](const Order& order, Price effective_price, std::string& reason) {
        if (!auth_manager_) {
            return true;
        }
        return auth_manager_->reserveForOrder(order, effective_price, reason);
    };

    release_callback_ = [this](const Order& order, const std::string& reason) {
        if (auth_manager_) {
            auth_manager_->releaseForOrder(order, reason);
        }
    };

    for (auto& [symbol, stock] : stocks_) {
        stock->setReservationHandlers(reserve_callback_, release_callback_);
    }

    if (!trade_observer_registered_) {
        registerTradeObserver([this](const Trade& trade) {
            if (auth_manager_) {
                auth_manager_->applyTrade(trade);
            }
        });
        trade_observer_registered_ = true;
    }
}

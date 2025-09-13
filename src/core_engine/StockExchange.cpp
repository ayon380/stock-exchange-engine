#define NOMINMAX
#include "StockExchange.h"
#include "CPUAffinity.h"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <random>

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
    std::cout << "Initializing Stock Exchange with " << STOCK_SYMBOLS.size() << " stocks..." << std::endl;
    
    // Get available CPU cores for optimal affinity assignment
    auto available_cores = CPUAffinity::getAvailableCores();
    std::cout << "Available CPU cores: " << available_cores.size() << std::endl;
    
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
                initial_price = stock_data.last_price;
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
        
        std::cout << "Initialized " << symbol << " at $" << initial_price 
                  << " (cores: " << matching_core << "," << md_core 
                  << "," << trade_core << ")" << std::endl;
    }
    
    return true;
}

void StockExchange::start() {
    if (running_.load()) {
        std::cout << "Stock Exchange is already running" << std::endl;
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
    
    std::cout << "Stock Exchange started with " << stocks_.size() 
              << " stocks, 1 index thread, and 1 database sync thread" << std::endl;
}

void StockExchange::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    // Stop all stock threads
    for (auto& [symbol, stock] : stocks_) {
        stock->stop();
    }
    
    // Stop index thread
    if (index_thread_.joinable()) {
        index_thread_.join();
    }
    
    // Stop database sync
    if (db_manager_) {
        db_manager_->stopBackgroundSync();
    }
    
    if (db_sync_thread_.joinable()) {
        db_sync_thread_.join();
    }
    
    // Final database save
    if (db_manager_ && db_manager_->isConnected()) {
        saveToDatabase();
    }
    
    std::cout << "Stock Exchange stopped" << std::endl;
}

std::string StockExchange::submitOrder(const std::string& symbol, const Order& order) {
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
    update.last_price = stock->getLastPrice();
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
        std::this_thread::sleep_for(std::chrono::seconds(1)); // Update every second
    }
}

void StockExchange::databaseSyncWorker() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30)); // Sync every 30 seconds
        
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
            stock->getLastPrice(),
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
        double market_cap = stock->getLastPrice() * stock->getVolume(); // Simplified market cap
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
        
        double price = stock->getLastPrice();
        double change_pct = stock->getChangePercent();
        
        weighted_price_sum += price * weight;
        
        // Calculate contribution to index
        double contribution = (price * weight / market_index_base_value_) * 100;
        
        current_market_index_.constituents.emplace_back(
            symbol, price, weight, contribution, change_pct
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
        snapshot.last_price = stock->getLastPrice();
        snapshot.change_points = stock->getChangePoints();
        snapshot.change_percent = stock->getChangePercent();
        snapshot.day_high = stock->getDayHigh();
        snapshot.day_low = stock->getDayLow();
        snapshot.day_open = stock->getDayOpen();
        snapshot.volume = stock->getVolume();
        snapshot.vwap = stock->getVWAP();
        
        if (include_order_book) {
            snapshot.top_bids = stock->getTopBids(3);
            snapshot.top_asks = stock->getTopAsks(3);
        }
        
        snapshots.push_back(snapshot);
    }
    
    return snapshots;
}

void StockExchange::broadcastMarketData(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    
    auto it = market_data_subscribers_.find(symbol);
    if (it != market_data_subscribers_.end()) {
        MarketDataUpdate update = getMarketData(symbol);
        for (const auto& callback : it->second) {
            callback(update);
        }
    }
}

void StockExchange::broadcastIndex() {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    
    if (!index_subscribers_.empty()) {
        std::vector<IndexEntry> top_index = getTopIndex("volume", 5);
        for (const auto& callback : index_subscribers_) {
            callback(top_index);
        }
    }
}

void StockExchange::broadcastMarketIndex() {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    
    if (!market_index_subscribers_.empty()) {
        MarketIndex index_copy;
        {
            std::lock_guard<std::mutex> index_lock(market_index_mutex_);
            index_copy = current_market_index_;
        }
        
        for (const auto& callback : market_index_subscribers_) {
            callback(index_copy);
        }
    }
}

void StockExchange::broadcastAllStocks() {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    
    if (!all_stocks_subscribers_.empty()) {
        std::vector<StockSnapshot> snapshots = getAllStocksSnapshot(true);
        
        for (const auto& callback : all_stocks_subscribers_) {
            callback(snapshots);
        }
    }
}

void StockExchange::loadFromDatabase() {
    if (!db_manager_ || !db_manager_->isConnected()) {
        return;
    }
    
    std::cout << "Loading stock data from database..." << std::endl;
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
            stock->getLastPrice(),
            stock->getLastPrice(), // For now, using last price as open price
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

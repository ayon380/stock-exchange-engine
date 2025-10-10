#include "DatabaseManager.h"
#include <iostream>
#include <chrono>

DatabaseManager::DatabaseManager(const std::string& connection_string, 
                                std::chrono::seconds sync_interval)
    : connection_string_(connection_string), sync_interval_(sync_interval), running_(false) {
}

DatabaseManager::~DatabaseManager() {
    stopBackgroundSync();
    disconnect();
}

bool DatabaseManager::connect() {
    try {
        conn_ = std::make_unique<pqxx::connection>(connection_string_);
        if (!conn_->is_open()) {
            std::cerr << "Failed to connect to PostgreSQL database" << std::endl;
            return false;
        }
        
        std::cout << "Connected to PostgreSQL database: " << conn_->dbname() << std::endl;
        initializeTables();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Database connection error: " << e.what() << std::endl;
        return false;
    }
}

void DatabaseManager::disconnect() {
    if (conn_ && conn_->is_open()) {
        conn_->close();
        std::cout << "Database connection closed" << std::endl;
    }
}

void DatabaseManager::initializeTables() {
    try {
        pqxx::work txn(*conn_);
        
        // Create stocks table
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS stocks (
                id SERIAL PRIMARY KEY,
                symbol VARCHAR(10) NOT NULL,
                last_price DECIMAL(15,4) NOT NULL,
                open_price DECIMAL(15,4) NOT NULL,
                volume BIGINT NOT NULL DEFAULT 0,
                timestamp_ms BIGINT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                UNIQUE(symbol, timestamp_ms)
            )
        )");
        
        // Create index on symbol and timestamp
        txn.exec(R"(
            CREATE INDEX IF NOT EXISTS idx_stocks_symbol_timestamp 
            ON stocks(symbol, timestamp_ms DESC)
        )");
        
        // Create orders table for audit trail
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS orders (
                id SERIAL PRIMARY KEY,
                order_id VARCHAR(50) UNIQUE NOT NULL,
                user_id VARCHAR(50) NOT NULL,
                symbol VARCHAR(10) NOT NULL,
                side INTEGER NOT NULL,
                order_type INTEGER NOT NULL,
                quantity BIGINT NOT NULL,
                price DECIMAL(15,4),
                status VARCHAR(20) DEFAULT 'open',
                timestamp_ms BIGINT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");
        
        // Create trades table
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS trades (
                id SERIAL PRIMARY KEY,
                buy_order_id VARCHAR(50) NOT NULL,
                sell_order_id VARCHAR(50) NOT NULL,
                symbol VARCHAR(10) NOT NULL,
                price DECIMAL(15,4) NOT NULL,
                quantity BIGINT NOT NULL,
                timestamp_ms BIGINT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");
        
        // Create user_accounts table
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS user_accounts (
                user_id VARCHAR(50) PRIMARY KEY,
                cash DECIMAL(15,4) NOT NULL DEFAULT 0,
                goog_position BIGINT NOT NULL DEFAULT 0,
                aapl_position BIGINT NOT NULL DEFAULT 0,
                tsla_position BIGINT NOT NULL DEFAULT 0,
                msft_position BIGINT NOT NULL DEFAULT 0,
                amzn_position BIGINT NOT NULL DEFAULT 0,
                buying_power DECIMAL(15,4) NOT NULL DEFAULT 0,
                day_trading_buying_power DECIMAL(15,4) NOT NULL DEFAULT 0,
                day_trades_count INTEGER NOT NULL DEFAULT 0,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");
        
        txn.commit();
        std::cout << "Database tables initialized successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error initializing database tables: " << e.what() << std::endl;
    }
}

void DatabaseManager::startBackgroundSync() {
    running_.store(true);
    sync_thread_ = std::thread(&DatabaseManager::syncWorker, this);
    std::cout << "Database background sync started with interval: " 
              << sync_interval_.count() << " seconds" << std::endl;
}

void DatabaseManager::stopBackgroundSync() {
    running_.store(false);
    sync_cv_.notify_all();
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
    std::cout << "Database background sync stopped" << std::endl;
}

void DatabaseManager::syncWorker() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(sync_mutex_);
            sync_cv_.wait_for(lock, sync_interval_, [this]() { return !running_.load(); });
        }
        
        if (!running_.load()) break;
        
        // This method will be called by the StockExchange to sync current data
        // The actual syncing logic is handled by the StockExchange class
        std::cout << "Database sync cycle completed" << std::endl;
    }
}

bool DatabaseManager::saveStockData(const StockData& data) {
    try {
        pqxx::work txn(*conn_);
        
        txn.exec_params(R"(
            INSERT INTO stocks (symbol, last_price, open_price, volume, timestamp_ms)
            VALUES ($1, $2, $3, $4, $5)
            ON CONFLICT (symbol, timestamp_ms) DO UPDATE SET
                last_price = EXCLUDED.last_price,
                open_price = EXCLUDED.open_price,
                volume = EXCLUDED.volume
        )", data.symbol, data.last_price, data.open_price, data.volume, data.timestamp_ms);
        
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving stock data: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::saveStockDataBatch(const std::vector<StockData>& data_batch) {
    try {
        pqxx::work txn(*conn_);
        
        for (const auto& data : data_batch) {
            txn.exec_params(R"(
                INSERT INTO stocks (symbol, last_price, open_price, volume, timestamp_ms)
                VALUES ($1, $2, $3, $4, $5)
                ON CONFLICT (symbol, timestamp_ms) DO UPDATE SET
                    last_price = EXCLUDED.last_price,
                    open_price = EXCLUDED.open_price,
                    volume = EXCLUDED.volume
            )", data.symbol, data.last_price, data.open_price, data.volume, data.timestamp_ms);
        }
        
        txn.commit();
        std::cout << "Saved " << data_batch.size() << " stock data records to database" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving stock data batch: " << e.what() << std::endl;
        return false;
    }
}

std::vector<StockData> DatabaseManager::loadStockData() {
    std::vector<StockData> result;
    
    try {
        pqxx::work txn(*conn_);
        
        auto res = txn.exec(R"(
            SELECT DISTINCT ON (symbol) 
                symbol, last_price, open_price, volume, timestamp_ms
            FROM stocks 
            ORDER BY symbol, timestamp_ms DESC
        )");
        
        for (const auto& row : res) {
            result.emplace_back(
                row[0].as<std::string>(),
                row[1].as<double>(),
                row[2].as<double>(),
                row[3].as<int64_t>(),
                row[4].as<int64_t>()
            );
        }
        
        txn.commit();
        std::cout << "Loaded " << result.size() << " stock data records from database" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading stock data: " << e.what() << std::endl;
    }
    
    return result;
}

StockData DatabaseManager::getLatestStockData(const std::string& symbol) {
    try {
        pqxx::work txn(*conn_);
        
        auto res = txn.exec_params(R"(
            SELECT symbol, last_price, open_price, volume, timestamp_ms
            FROM stocks 
            WHERE symbol = $1 
            ORDER BY timestamp_ms DESC 
            LIMIT 1
        )", symbol);
        
        if (!res.empty()) {
            const auto& row = res[0];
            return StockData(
                row[0].as<std::string>(),
                row[1].as<double>(),
                row[2].as<double>(),
                row[3].as<int64_t>(),
                row[4].as<int64_t>()
            );
        }
        
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "Error getting latest stock data: " << e.what() << std::endl;
    }
    
    // Return default data if not found
    return StockData(symbol, 100.0, 100.0, 0, 0);
}

bool DatabaseManager::saveOrder(const std::string& order_data) {
    // Implementation for saving order audit trail
    // This can be extended based on specific requirements
    return true;
}

bool DatabaseManager::saveTrade(const std::string& trade_data) {
    // Implementation for saving trade data
    // This can be extended based on specific requirements
    return true;
}

bool DatabaseManager::isConnected() const {
    return conn_ && conn_->is_open();
}

bool DatabaseManager::loadUserAccount(const std::string& user_id, UserAccount& account) {
    if (!conn_ || !conn_->is_open()) {
        std::cerr << "Database not connected" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);
        
        std::string query = "SELECT user_id, cash, goog_position, aapl_position, tsla_position, "
                           "msft_position, amzn_position, buying_power, day_trading_buying_power, "
                           "day_trades_count FROM user_accounts WHERE user_id = $1";
        
        auto result = txn.exec_params(query, user_id);
        
        if (result.empty()) {
            return false; // User not found
        }
        
        auto row = result[0];
        account.user_id = row["user_id"].as<std::string>();
        account.cash = row["cash"].as<double>();
        account.goog_position = row["goog_position"].as<long>();
        account.aapl_position = row["aapl_position"].as<long>();
        account.tsla_position = row["tsla_position"].as<long>();
        account.msft_position = row["msft_position"].as<long>();
        account.amzn_position = row["amzn_position"].as<long>();
        account.buying_power = row["buying_power"].as<double>();
        account.day_trading_buying_power = row["day_trading_buying_power"].as<double>();
        account.day_trades_count = row["day_trades_count"].as<int>();
        
        txn.commit();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading user account: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::saveUserAccount(const UserAccount& account) {
    if (!conn_ || !conn_->is_open()) {
        std::cerr << "Database not connected" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);
        
        std::string query = "UPDATE user_accounts SET cash = $2, goog_position = $3, "
                           "aapl_position = $4, tsla_position = $5, msft_position = $6, "
                           "amzn_position = $7, buying_power = $8, day_trading_buying_power = $9, "
                           "day_trades_count = $10 WHERE user_id = $1";
        
        auto result = txn.exec_params(query, account.user_id, account.cash, 
                                     account.goog_position, account.aapl_position,
                                     account.tsla_position, account.msft_position,
                                     account.amzn_position, account.buying_power,
                                     account.day_trading_buying_power, account.day_trades_count);
        
        txn.commit();
        return result.affected_rows() > 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving user account: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::createUserAccount(const std::string& user_id, double initial_cash) {
    if (!conn_ || !conn_->is_open()) {
        std::cerr << "Database not connected" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);
        
        std::string query = "INSERT INTO user_accounts (user_id, cash, goog_position, aapl_position, "
                           "tsla_position, msft_position, amzn_position, buying_power, "
                           "day_trading_buying_power, day_trades_count) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)";
        
        txn.exec_params(query, user_id, initial_cash, 0, 0, 0, 0, 0, 
                       initial_cash, initial_cash, 0);
        
        txn.commit();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error creating user account: " << e.what() << std::endl;
        return false;
    }
}

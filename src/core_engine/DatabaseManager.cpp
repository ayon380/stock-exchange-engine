#include "DatabaseManager.h"
#include "../common/EngineLogging.h"
#include <iostream>
#include <chrono>

DatabaseManager::DatabaseManager(const std::string& connection_string, 
                                std::chrono::seconds sync_interval,
                                size_t pool_size)
    : connection_string_(connection_string), sync_interval_(sync_interval), 
      pool_size_(pool_size), running_(false) {
}

DatabaseManager::~DatabaseManager() {
    stopBackgroundSync();
    disconnect();
}

bool DatabaseManager::connect() {
    try {
        // Initialize connection pool
        connection_pool_ = std::make_unique<ConnectionPool>(connection_string_, pool_size_);
        if (!connection_pool_->initialize()) {
            std::cerr << "Failed to initialize connection pool" << std::endl;
            // CRITICAL FIX: Reset connection_pool_ to nullptr on failure
            // Otherwise isConnected() returns true and saveToDatabase() will hang
            connection_pool_.reset();
            return false;
        }
        
        ENGINE_LOG_DEV(std::cout << "Connected to PostgreSQL database with pool size: " << pool_size_ << std::endl;);
        initializeTables();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Database connection error: " << e.what() << std::endl;
        // CRITICAL FIX: Reset connection_pool_ to nullptr on exception
        connection_pool_.reset();
        return false;
    }
}

void DatabaseManager::disconnect() {
    connection_pool_.reset();
    ENGINE_LOG_DEV(std::cout << "Database connection pool closed" << std::endl;);
}

void DatabaseManager::initializeTables() {
    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
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
                cash BIGINT NOT NULL DEFAULT 0,
                aapl_qty BIGINT NOT NULL DEFAULT 0,
                googl_qty BIGINT NOT NULL DEFAULT 0,
                msft_qty BIGINT NOT NULL DEFAULT 0,
                amzn_qty BIGINT NOT NULL DEFAULT 0,
                tsla_qty BIGINT NOT NULL DEFAULT 0,
                buying_power BIGINT NOT NULL DEFAULT 0,
                day_trading_buying_power BIGINT NOT NULL DEFAULT 0,
                total_trades BIGINT NOT NULL DEFAULT 0,
                realized_pnl BIGINT NOT NULL DEFAULT 0,
                is_active BOOLEAN NOT NULL DEFAULT true,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");
        
    txn.commit();
    ENGINE_LOG_DEV(std::cout << "Database tables initialized successfully" << std::endl;);
    } catch (const std::exception& e) {
        std::cerr << "Error initializing database tables: " << e.what() << std::endl;
    }
}

void DatabaseManager::startBackgroundSync() {
    running_.store(true);
    sync_thread_ = std::thread(&DatabaseManager::syncWorker, this);
    ENGINE_LOG_DEV(std::cout << "Database background sync started with interval: "
                             << sync_interval_.count() << " seconds" << std::endl;);
}

void DatabaseManager::stopBackgroundSync() {
    running_.store(false);
    sync_cv_.notify_all();
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
    ENGINE_LOG_DEV(std::cout << "Database background sync stopped" << std::endl;);
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
    ENGINE_LOG_DEV(std::cout << "Database sync cycle completed" << std::endl;);
    }
}

bool DatabaseManager::saveStockData(const StockData& data) {
    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
        txn.exec_params(R"(
            INSERT INTO stocks (symbol, last_price, open_price, volume, timestamp_ms)
            VALUES ($1, $2, $3, $4, $5)
            ON CONFLICT (symbol, timestamp_ms) DO UPDATE SET
                last_price = EXCLUDED.last_price,
                open_price = EXCLUDED.open_price,
                volume = EXCLUDED.volume
        )", data.symbol, data.lastPriceToDouble(), data.openPriceToDouble(), data.volume, data.timestamp_ms);
        
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving stock data: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::saveStockDataBatch(const std::vector<StockData>& data_batch) {
    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
        for (const auto& data : data_batch) {
            txn.exec_params(R"(
                INSERT INTO stocks (symbol, last_price, open_price, volume, timestamp_ms)
                VALUES ($1, $2, $3, $4, $5)
                ON CONFLICT (symbol, timestamp_ms) DO UPDATE SET
                    last_price = EXCLUDED.last_price,
                    open_price = EXCLUDED.open_price,
                    volume = EXCLUDED.volume
            )", data.symbol, data.lastPriceToDouble(), data.openPriceToDouble(), data.volume, data.timestamp_ms);
        }
        
    txn.commit();
    ENGINE_LOG_DEV(std::cout << "Saved " << data_batch.size() << " stock data records to database" << std::endl;);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving stock data batch: " << e.what() << std::endl;
        return false;
    }
}

std::vector<StockData> DatabaseManager::loadStockData() {
    std::vector<StockData> result;
    
    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
        auto res = txn.exec(R"(
            SELECT DISTINCT ON (symbol) 
                symbol, last_price, open_price, volume, timestamp_ms
            FROM stocks 
            ORDER BY symbol, timestamp_ms DESC
        )");
        
        for (const auto& row : res) {
            result.emplace_back(
                row[0].as<std::string>(),
                StockData::fromDouble(row[1].as<double>()),
                StockData::fromDouble(row[2].as<double>()),
                row[3].as<int64_t>(),
                row[4].as<int64_t>()
            );
        }
        
    txn.commit();
    ENGINE_LOG_DEV(std::cout << "Loaded " << result.size() << " stock data records from database" << std::endl;);
    } catch (const std::exception& e) {
        std::cerr << "Error loading stock data: " << e.what() << std::endl;
    }
    
    return result;
}

StockData DatabaseManager::getLatestStockData(const std::string& symbol) {
    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
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
                StockData::fromDouble(row[1].as<double>()),
                StockData::fromDouble(row[2].as<double>()),
                row[3].as<int64_t>(),
                row[4].as<int64_t>()
            );
        }
        
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "Error getting latest stock data: " << e.what() << std::endl;
    }
    
    // Return default data if not found
    return StockData(symbol, StockData::fromDouble(100.0), StockData::fromDouble(100.0), 0, 0);
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
    // CRITICAL FIX: Check if pool exists and is initialized, not if connections are available
    // During normal load, all connections may be checked out temporarily, which doesn't mean
    // the database is disconnected. Only return false if the pool doesn't exist at all.
    return connection_pool_ != nullptr;
}

bool DatabaseManager::loadUserAccount(const std::string& user_id, UserAccount& account) {
    if (!connection_pool_) {
        std::cerr << "Database not connected" << std::endl;
        return false;
    }

    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
        std::string query = "SELECT user_id, cash, aapl_qty, googl_qty, msft_qty, "
                           "amzn_qty, tsla_qty, buying_power, day_trading_buying_power, "
                           "total_trades, realized_pnl, is_active FROM user_accounts WHERE user_id = $1";
        
        auto result = txn.exec_params(query, user_id);
        
        if (result.empty()) {
            return false; // User not found
        }
        
        auto row = result[0];
    account.user_id = row["user_id"].as<std::string>();
    account.cash = row["cash"].as<long long>();
        account.aapl_qty = row["aapl_qty"].as<long>();
        account.googl_qty = row["googl_qty"].as<long>();
        account.msft_qty = row["msft_qty"].as<long>();
        account.amzn_qty = row["amzn_qty"].as<long>();
        account.tsla_qty = row["tsla_qty"].as<long>();
    account.buying_power = row["buying_power"].as<long long>();
    account.day_trading_buying_power = row["day_trading_buying_power"].as<long long>();
        account.total_trades = row["total_trades"].as<int64_t>();
    account.realized_pnl = row["realized_pnl"].as<int64_t>();
        account.is_active = row["is_active"].as<bool>();
        
        txn.commit();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading user account: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::saveUserAccount(const UserAccount& account) {
    if (!connection_pool_) {
        std::cerr << "Database not connected" << std::endl;
        return false;
    }

    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
        std::string query = "UPDATE user_accounts SET cash = $2, aapl_qty = $3, "
                           "googl_qty = $4, msft_qty = $5, amzn_qty = $6, "
                           "tsla_qty = $7, buying_power = $8, day_trading_buying_power = $9, "
                           "total_trades = $10, realized_pnl = $11, is_active = $12 WHERE user_id = $1";
        
        auto result = txn.exec_params(query, account.user_id, account.cash, 
                                     account.aapl_qty, account.googl_qty,
                                     account.msft_qty, account.amzn_qty,
                                     account.tsla_qty, account.buying_power,
                                     account.day_trading_buying_power, account.total_trades,
                                     account.realized_pnl, account.is_active);
        
        txn.commit();
        return result.affected_rows() > 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving user account: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::createUserAccount(const std::string& user_id, CashAmount initial_cash) {
    if (!connection_pool_) {
        std::cerr << "Database not connected" << std::endl;
        return false;
    }

    try {
        ScopedConnection conn(*connection_pool_);
        pqxx::work txn(conn.get());
        
        std::string query = "INSERT INTO user_accounts (user_id, cash, aapl_qty, googl_qty, "
                           "msft_qty, amzn_qty, tsla_qty, buying_power, "
                           "day_trading_buying_power, total_trades, realized_pnl, is_active) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)";
        
        txn.exec_params(query, user_id, initial_cash, 0, 0, 0, 0, 0, 
                       initial_cash, initial_cash, 0, 0, 1);
        
        txn.commit();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error creating user account: " << e.what() << std::endl;
        return false;
    }
}

DatabaseManager::UserAccount DatabaseManager::getUserAccount(const std::string& user_id) {
    UserAccount account;
    if (!loadUserAccount(user_id, account)) {
        return UserAccount(); // Return empty account
    }
    return account;
}

bool DatabaseManager::updateUserAccount(const UserAccount& account) {
    return saveUserAccount(account);
}

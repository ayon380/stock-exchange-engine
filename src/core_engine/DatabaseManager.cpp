/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#include "DatabaseManager.h"
#include "../common/EngineLogging.h"
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>

DatabaseManager::DatabaseManager(const std::string &connection_string,
                                 std::chrono::seconds sync_interval,
                                 size_t pool_size)
    : connection_string_(connection_string), sync_interval_(sync_interval),
      pool_size_(pool_size), running_(false) {}

DatabaseManager::~DatabaseManager() {
  stopBackgroundSync();
  disconnect();
}

bool DatabaseManager::connect() {
  try {
    // Initialize connection pool
    connection_pool_ =
        std::make_unique<ConnectionPool>(connection_string_, pool_size_);
    if (!connection_pool_->initialize()) {
      std::cerr << "Failed to initialize connection pool" << std::endl;
      // CRITICAL FIX: Reset connection_pool_ to nullptr on failure
      // Otherwise isConnected() returns true and saveToDatabase() will hang
      connection_pool_.reset();
      return false;
    }

    ENGINE_LOG_DEV(std::cout
                       << "Connected to PostgreSQL database with pool size: "
                       << pool_size_ << std::endl;);
    initializeTables();
    return true;
  } catch (const std::exception &e) {
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
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");

    txn.exec(R"(
            ALTER TABLE orders
            ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )");

    txn.exec(R"(
            CREATE INDEX IF NOT EXISTS idx_orders_timestamp
            ON orders (timestamp_ms DESC)
        )");

    // Create trades table
    txn.exec(R"(
            CREATE TABLE IF NOT EXISTS trades (
                id SERIAL PRIMARY KEY,
                trade_id VARCHAR(100) NOT NULL,
                buy_order_id VARCHAR(50) NOT NULL,
                sell_order_id VARCHAR(50) NOT NULL,
                symbol VARCHAR(10) NOT NULL,
                price DECIMAL(15,4) NOT NULL,
                quantity BIGINT NOT NULL,
                buyer_id VARCHAR(50) NOT NULL,
                seller_id VARCHAR(50) NOT NULL,
                timestamp_ms BIGINT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");

    txn.exec(R"(
            ALTER TABLE trades
            ADD COLUMN IF NOT EXISTS trade_id VARCHAR(100)
        )");

    txn.exec(R"(
            ALTER TABLE trades
            ADD COLUMN IF NOT EXISTS buyer_id VARCHAR(50)
        )");

    txn.exec(R"(
            ALTER TABLE trades
            ADD COLUMN IF NOT EXISTS seller_id VARCHAR(50)
        )");

    txn.exec(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_trades_trade_id
            ON trades (trade_id)
        )");

    // Master stock metadata table (admin CLI support)
    txn.exec(R"(
            CREATE TABLE IF NOT EXISTS stocks_master (
                symbol VARCHAR(10) PRIMARY KEY,
                company_name VARCHAR(120) NOT NULL,
                sector VARCHAR(80) NOT NULL,
                market_cap BIGINT,
                initial_price DECIMAL(15,4) NOT NULL,
                is_active BOOLEAN NOT NULL DEFAULT TRUE,
                listing_date DATE DEFAULT CURRENT_DATE,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");

    txn.exec(R"(
            ALTER TABLE stocks_master
            ADD COLUMN IF NOT EXISTS market_cap BIGINT
        )");

    txn.exec(R"(
            ALTER TABLE stocks_master
            ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )");

    // Security event log (Regulation S-P)
    txn.exec(R"(
            CREATE TABLE IF NOT EXISTS security_events (
                id SERIAL PRIMARY KEY,
                event_type VARCHAR(64) NOT NULL,
                user_id VARCHAR(50),
                ip_address INET,
                connection_id VARCHAR(64),
                event_data JSONB,
                severity VARCHAR(16),
                timestamp_ms BIGINT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");

    txn.exec(R"(
            CREATE INDEX IF NOT EXISTS idx_security_events_timestamp
            ON security_events (timestamp_ms DESC)
        )");

    // Circuit breaker events (Rule 80B)
    txn.exec(R"(
            CREATE TABLE IF NOT EXISTS circuit_breaker_events (
                id SERIAL PRIMARY KEY,
                symbol VARCHAR(10) NOT NULL,
                trigger_level INTEGER NOT NULL,
                trigger_price DECIMAL(15,4) NOT NULL,
                reference_price DECIMAL(15,4) NOT NULL,
                halt_duration_minutes INTEGER NOT NULL,
                triggered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )");

    txn.exec(R"(
            ALTER TABLE circuit_breaker_events
            ADD COLUMN IF NOT EXISTS triggered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )");

    txn.exec(R"(
            CREATE INDEX IF NOT EXISTS idx_circuit_breaker_symbol
            ON circuit_breaker_events (symbol, triggered_at DESC)
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
    ENGINE_LOG_DEV(std::cout << "Database tables initialized successfully"
                             << std::endl;);
  } catch (const std::exception &e) {
    std::cerr << "Error initializing database tables: " << e.what()
              << std::endl;
  }
}

void DatabaseManager::startBackgroundSync() {
  running_.store(true);
  sync_thread_ = std::thread(&DatabaseManager::syncWorker, this);

  // Start persistence worker
  persistence_running_.store(true);
  persistence_thread_ = std::thread(&DatabaseManager::persistenceWorker, this);

  ENGINE_LOG_DEV(std::cout << "Database background sync and persistence started"
                           << std::endl;);
}

void DatabaseManager::stopBackgroundSync() {
  running_.store(false);
  persistence_running_.store(false);

  sync_cv_.notify_all();

  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }

  if (persistence_thread_.joinable()) {
    persistence_thread_.join();
  }

  ENGINE_LOG_DEV(std::cout << "Database background sync and persistence stopped"
                           << std::endl;);
}

void DatabaseManager::syncWorker() {
  while (running_.load()) {
    {
      std::unique_lock<std::mutex> lock(sync_mutex_);
      sync_cv_.wait_for(lock, sync_interval_,
                        [this]() { return !running_.load(); });
    }

    if (!running_.load())
      break;

    // This method will be called by the StockExchange to sync current data
    // The actual syncing logic is handled by the StockExchange class
    ENGINE_LOG_DEV(std::cout << "Database sync cycle completed" << std::endl;);
  }
}

bool DatabaseManager::saveStockData(const StockData &data) {
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
        )",
                    data.symbol, data.lastPriceToDouble(),
                    data.openPriceToDouble(), data.volume, data.timestamp_ms);

    txn.commit();
    return true;
  } catch (const std::exception &e) {
    std::cerr << "Error saving stock data: " << e.what() << std::endl;
    return false;
  }
}

bool DatabaseManager::saveStockDataBatch(
    const std::vector<StockData> &data_batch) {
  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    for (const auto &data : data_batch) {
      txn.exec_params(R"(
                INSERT INTO stocks (symbol, last_price, open_price, volume, timestamp_ms)
                VALUES ($1, $2, $3, $4, $5)
                ON CONFLICT (symbol, timestamp_ms) DO UPDATE SET
                    last_price = EXCLUDED.last_price,
                    open_price = EXCLUDED.open_price,
                    volume = EXCLUDED.volume
            )",
                      data.symbol, data.lastPriceToDouble(),
                      data.openPriceToDouble(), data.volume, data.timestamp_ms);
    }

    txn.commit();
    ENGINE_LOG_DEV(std::cout << "Saved " << data_batch.size()
                             << " stock data records to database"
                             << std::endl;);
    return true;
  } catch (const std::exception &e) {
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

    for (const auto &row : res) {
      result.emplace_back(row[0].as<std::string>(),
                          StockData::fromDouble(row[1].as<double>()),
                          StockData::fromDouble(row[2].as<double>()),
                          row[3].as<int64_t>(), row[4].as<int64_t>());
    }

    txn.commit();
    ENGINE_LOG_DEV(std::cout << "Loaded " << result.size()
                             << " stock data records from database"
                             << std::endl;);
  } catch (const std::exception &e) {
    std::cerr << "Error loading stock data: " << e.what() << std::endl;
  }

  return result;
}

StockData DatabaseManager::getLatestStockData(const std::string &symbol) {
  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    auto res = txn.exec_params(R"(
            SELECT symbol, last_price, open_price, volume, timestamp_ms
            FROM stocks 
            WHERE symbol = $1 
            ORDER BY timestamp_ms DESC 
            LIMIT 1
        )",
                               symbol);

    if (!res.empty()) {
      const auto &row = res[0];
      return StockData(row[0].as<std::string>(),
                       StockData::fromDouble(row[1].as<double>()),
                       StockData::fromDouble(row[2].as<double>()),
                       row[3].as<int64_t>(), row[4].as<int64_t>());
    }

    txn.commit();
  } catch (const std::exception &e) {
    std::cerr << "Error getting latest stock data: " << e.what() << std::endl;
  }

  // Return default data if not found
  return StockData(symbol, StockData::fromDouble(100.0),
                   StockData::fromDouble(100.0), 0, 0);
}

bool DatabaseManager::saveOrder(const std::string &order_data) {
  // Parse JSON order data and save to database
  // For now, accepting JSON string format
  // Format expected: {"order_id":"...", "user_id":"...", "symbol":"...", ...}
  // This is a simplified implementation - enhance as needed
  ENGINE_LOG_DEV(std::cout << "[DB] Saving order (stub): "
                           << order_data.substr(0, 50) << "..." << std::endl;);
  return true;
}

bool DatabaseManager::saveTrade(const std::string &trade_data) {
  // Parse JSON trade data and save to database
  // This is a simplified implementation - enhance as needed
  ENGINE_LOG_DEV(std::cout << "[DB] Saving trade (stub): "
                           << trade_data.substr(0, 50) << "..." << std::endl;);
  return true;
}

// SEC Compliance: Order Persistence (17a-3) - ASYNC IMPLEMENTATION
bool DatabaseManager::persistOrder(const std::string &order_id,
                                   const std::string &user_id,
                                   const std::string &symbol, int side,
                                   int type, int64_t quantity, Price price,
                                   const std::string &status,
                                   int64_t timestamp_ms) {
  if (!connection_pool_)
    return false;

  // Allocate event from pool
  auto entry = persistence_pool_.allocate();
  if (!entry)
    return false; // Should fallback to new, but if that fails we are OOM

  entry->type = PersistenceType::ORDER;
  std::strncpy(entry->order_id, order_id.c_str(), sizeof(entry->order_id) - 1);
  entry->order_id[sizeof(entry->order_id) - 1] =
      '\0'; // Ensure null termination
  std::strncpy(entry->user_id, user_id.c_str(), sizeof(entry->user_id) - 1);
  entry->user_id[sizeof(entry->user_id) - 1] = '\0'; // Ensure null termination
  std::strncpy(entry->symbol, symbol.c_str(), sizeof(entry->symbol) - 1);
  entry->symbol[sizeof(entry->symbol) - 1] = '\0'; // Ensure null termination
  entry->side = side;
  entry->order_type = type;
  entry->quantity = quantity;
  entry->price = price;
  std::strncpy(entry->status, status.c_str(), sizeof(entry->status) - 1);
  entry->status[sizeof(entry->status) - 1] = '\0'; // Ensure null termination
  entry->timestamp_ms = timestamp_ms;

  // Enqueue for background processing
  if (!persistence_queue_.enqueue(entry)) {
    // Fallback NO LONGER BLOCKS
    // If queue is full, we must drop the event rather than blocking the
    // matching engine In a real production system, this would go to a separate
    // high-speed "spillover" log file or ring buffer on another thread.
    std::cerr << "[CRITICAL] Persistence queue full - DROPPING order "
              << order_id << " to prevent engine stall" << std::endl;

    // We still return true to the caller so they don't think the order failed
    // validation. The order is in memory and processed, just not persisted to
    // DB yet.
    persistence_pool_.deallocate(entry);
  }

  return true;
}

// SEC Compliance: Trade Persistence (17a-3) - ASYNC IMPLEMENTATION
bool DatabaseManager::persistTrade(const std::string &buy_order_id,
                                   const std::string &sell_order_id,
                                   const std::string &symbol, Price price,
                                   int64_t quantity,
                                   const std::string &buyer_id,
                                   const std::string &seller_id,
                                   int64_t timestamp_ms) {
  if (!connection_pool_)
    return false;

  auto entry = persistence_pool_.allocate();
  if (!entry)
    return false;

  entry->type = PersistenceType::TRADE;
  std::strncpy(entry->buy_order_id, buy_order_id.c_str(),
               sizeof(entry->buy_order_id) - 1);
  entry->buy_order_id[sizeof(entry->buy_order_id) - 1] =
      '\0'; // Ensure null termination
  std::strncpy(entry->sell_order_id, sell_order_id.c_str(),
               sizeof(entry->sell_order_id) - 1);
  entry->sell_order_id[sizeof(entry->sell_order_id) - 1] =
      '\0'; // Ensure null termination
  std::strncpy(entry->symbol, symbol.c_str(), sizeof(entry->symbol) - 1);
  entry->symbol[sizeof(entry->symbol) - 1] = '\0'; // Ensure null termination
  entry->price = price;
  entry->quantity = quantity;
  std::strncpy(entry->buyer_id, buyer_id.c_str(), sizeof(entry->buyer_id) - 1);
  entry->buyer_id[sizeof(entry->buyer_id) - 1] =
      '\0'; // Ensure null termination
  std::strncpy(entry->seller_id, seller_id.c_str(),
               sizeof(entry->seller_id) - 1);
  entry->seller_id[sizeof(entry->seller_id) - 1] =
      '\0'; // Ensure null termination
  entry->timestamp_ms = timestamp_ms;

  if (!persistence_queue_.enqueue(entry)) {
    // Fallback NO LONGER BLOCKS
    std::cerr << "[CRITICAL] Persistence queue full - DROPPING trade " << symbol
              << " to prevent engine stall" << std::endl;

    persistence_pool_.deallocate(entry);
  }

  return true;
}

void DatabaseManager::persistenceWorker() {
  const int BATCH_SIZE = 100;
  std::vector<PersistenceEntry *> batch;
  batch.reserve(BATCH_SIZE);

  while (persistence_running_.load()) {
    // Drain queue up to batch size
    for (int i = 0; i < BATCH_SIZE; ++i) {
      auto entry = persistence_queue_.dequeue();
      if (entry) {
        batch.push_back(entry);
      } else {
        break; // Queue empty (or skipped slot)
      }
    }

    if (batch.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    try {
      ScopedConnection conn(*connection_pool_);
      pqxx::work txn(conn.get());

      for (auto *entry : batch) {
        if (entry->type == PersistenceType::ORDER) {
          double price_dollars = static_cast<double>(entry->price) / 100.0;
          txn.exec_params(R"(
                        INSERT INTO orders (order_id, user_id, symbol, side, order_type, quantity, price, status, timestamp_ms)
                        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
                        ON CONFLICT (order_id) DO UPDATE 
                        SET user_id = EXCLUDED.user_id,
                            symbol = EXCLUDED.symbol,
                            side = EXCLUDED.side,
                            order_type = EXCLUDED.order_type,
                            quantity = EXCLUDED.quantity,
                            price = EXCLUDED.price,
                            status = EXCLUDED.status,
                            timestamp_ms = EXCLUDED.timestamp_ms,
                            updated_at = CURRENT_TIMESTAMP
                    )",
                          entry->order_id, entry->user_id, entry->symbol,
                          entry->side, entry->order_type, entry->quantity,
                          price_dollars, entry->status, entry->timestamp_ms);
        } else if (entry->type == PersistenceType::TRADE) {
          double price_dollars = static_cast<double>(entry->price) / 100.0;

          // Re-generate ID here or store it in entry?
          // Storing in entry would be better but let's generate here for now to
          // save space Note: Sequence might be slightly out of order but that's
          // fine for ID uniqueness
          const uint64_t sequence =
              trade_id_sequence_.fetch_add(1, std::memory_order_relaxed);
          // Use simple hash of ID strings for consistency
          size_t h1 = 0, h2 = 0;
          for (const char *p = entry->buy_order_id; *p; ++p)
            h1 = h1 * 31 + *p;
          for (const char *p = entry->sell_order_id; *p; ++p)
            h2 = h2 * 31 + *p;

          std::ostringstream id_builder;
          id_builder << "TRD_" << entry->timestamp_ms << "_" << std::hex
                     << std::uppercase << (h1 ^ h2) << "_" << sequence;
          const std::string trade_id = id_builder.str();

          txn.exec_params(R"(
                        INSERT INTO trades (trade_id, buy_order_id, sell_order_id, symbol, price, quantity, 
                                           buyer_id, seller_id, timestamp_ms)
                        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
                        ON CONFLICT (trade_id) DO NOTHING
                    )",
                          trade_id, entry->buy_order_id, entry->sell_order_id,
                          entry->symbol, price_dollars, entry->quantity,
                          entry->buyer_id, entry->seller_id,
                          entry->timestamp_ms);
        }
      }

      txn.commit();
    } catch (const std::exception &e) {
      std::cerr << "Persistence worker error: " << e.what() << std::endl;
      // In a real system, we might retry or DLQ these events
    }

    // Return to pool
    for (auto *entry : batch) {
      persistence_pool_.deallocate(entry);
    }
    batch.clear();
  }
}

// Security Event Logging (SEC Regulation S-P)
bool DatabaseManager::logSecurityEvent(const std::string &event_type,
                                       const std::string &user_id,
                                       const std::string &ip_address,
                                       const std::string &connection_id,
                                       const std::string &event_data,
                                       const std::string &severity,
                                       int64_t timestamp_ms) {
  if (!connection_pool_)
    return false;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query = R"(
            INSERT INTO security_events (event_type, user_id, ip_address, connection_id, 
                                        event_data, severity, timestamp_ms)
            VALUES ($1, $2, $3::inet, $4, $5::jsonb, $6, $7)
        )";

    txn.exec_params(query, event_type, user_id, ip_address, connection_id,
                    event_data, severity, timestamp_ms);
    txn.commit();

    ENGINE_LOG_DEV(std::cout << "[SECURITY] Event logged: " << event_type
                             << " user=" << user_id << " severity=" << severity
                             << std::endl;);
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to log security event: " << e.what()
              << std::endl;
    return false;
  }
}

// Circuit Breaker Event Logging (SEC Rule 80B)
bool DatabaseManager::logCircuitBreakerEvent(const std::string &symbol,
                                             int level, Price trigger_price,
                                             Price reference_price,
                                             int halt_duration_minutes) {
  if (!connection_pool_)
    return false;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    double trigger_dollars = static_cast<double>(trigger_price) / 100.0;
    double reference_dollars = static_cast<double>(reference_price) / 100.0;

    std::string query = R"(
            INSERT INTO circuit_breaker_events (symbol, trigger_level, trigger_price, 
                                               reference_price, halt_duration_minutes)
            VALUES ($1, $2, $3, $4, $5)
        )";

    txn.exec_params(query, symbol, level, trigger_dollars, reference_dollars,
                    halt_duration_minutes);
    txn.commit();

    std::cout << "[CIRCUIT BREAKER] Level " << level << " triggered for "
              << symbol << " at $" << trigger_dollars << " (ref: $"
              << reference_dollars << ")" << std::endl;
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to log circuit breaker event: " << e.what()
              << std::endl;
    return false;
  }
}

// Stock Management: Add New Stock (Admin Operation)
bool DatabaseManager::addStock(const std::string &symbol,
                               const std::string &company_name,
                               const std::string &sector,
                               double initial_price) {
  if (!connection_pool_)
    return false;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query = R"(
            INSERT INTO stocks_master (symbol, company_name, sector, initial_price, is_active)
            VALUES ($1, $2, $3, $4, TRUE)
            ON CONFLICT (symbol) DO UPDATE
            SET company_name = EXCLUDED.company_name,
                sector = EXCLUDED.sector,
                initial_price = EXCLUDED.initial_price,
                is_active = TRUE,
                updated_at = CURRENT_TIMESTAMP
        )";

    txn.exec_params(query, symbol, company_name, sector, initial_price);
    txn.commit();

    std::cout << "[ADMIN] Stock added: " << symbol << " (" << company_name
              << ") @ $" << initial_price << std::endl;
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to add stock " << symbol << ": " << e.what()
              << std::endl;
    return false;
  }
}

// Stock Management: Remove Stock (Admin Operation)
bool DatabaseManager::removeStock(const std::string &symbol) {
  if (!connection_pool_)
    return false;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query = R"(
            UPDATE stocks_master 
            SET is_active = FALSE, updated_at = CURRENT_TIMESTAMP
            WHERE symbol = $1
        )";

    txn.exec_params(query, symbol);
    txn.commit();

    std::cout << "[ADMIN] Stock deactivated: " << symbol << std::endl;
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to remove stock " << symbol << ": "
              << e.what() << std::endl;
    return false;
  }
}

// Stock Management: Update Stock Info
bool DatabaseManager::updateStock(const std::string &symbol,
                                  const std::string &company_name,
                                  const std::string &sector) {
  if (!connection_pool_)
    return false;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query = R"(
            UPDATE stocks_master 
            SET company_name = $2, sector = $3, updated_at = CURRENT_TIMESTAMP
            WHERE symbol = $1
        )";

    txn.exec_params(query, symbol, company_name, sector);
    txn.commit();

    std::cout << "[ADMIN] Stock updated: " << symbol << std::endl;
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to update stock " << symbol << ": "
              << e.what() << std::endl;
    return false;
  }
}

// Get All Stocks
std::vector<DatabaseManager::StockInfo> DatabaseManager::getAllStocks() {
  std::vector<StockInfo> result;

  if (!connection_pool_)
    return result;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    auto res = txn.exec(R"(
            SELECT symbol, company_name, sector, COALESCE(market_cap, 0), initial_price, 
                   is_active, listing_date::text
            FROM stocks_master
            ORDER BY symbol
        )");

    for (const auto &row : res) {
      StockInfo info;
      info.symbol = row[0].as<std::string>();
      info.company_name = row[1].as<std::string>();
      info.sector = row[2].as<std::string>();
      info.market_cap = row[3].as<int64_t>();
      info.initial_price = row[4].as<double>();
      info.is_active = row[5].as<bool>();
      info.listing_date = row[6].as<std::string>();
      result.push_back(info);
    }

    txn.commit();
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to get all stocks: " << e.what()
              << std::endl;
  }

  return result;
}

// Get Stock Info
DatabaseManager::StockInfo
DatabaseManager::getStockInfo(const std::string &symbol) {
  StockInfo info;

  if (!connection_pool_)
    return info;

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    auto res = txn.exec_params(R"(
            SELECT symbol, company_name, sector, COALESCE(market_cap, 0), initial_price, 
                   is_active, listing_date::text
            FROM stocks_master
            WHERE symbol = $1
        )",
                               symbol);

    if (!res.empty()) {
      const auto &row = res[0];
      info.symbol = row[0].as<std::string>();
      info.company_name = row[1].as<std::string>();
      info.sector = row[2].as<std::string>();
      info.market_cap = row[3].as<int64_t>();
      info.initial_price = row[4].as<double>();
      info.is_active = row[5].as<bool>();
      info.listing_date = row[6].as<std::string>();
    }

    txn.commit();
  } catch (const std::exception &e) {
    std::cerr << "[DB ERROR] Failed to get stock info for " << symbol << ": "
              << e.what() << std::endl;
  }

  return info;
}

bool DatabaseManager::isConnected() const {
  // CRITICAL FIX: Check if pool exists and is initialized, not if connections
  // are available During normal load, all connections may be checked out
  // temporarily, which doesn't mean the database is disconnected. Only return
  // false if the pool doesn't exist at all.
  return connection_pool_ != nullptr;
}

bool DatabaseManager::loadUserAccount(const std::string &user_id,
                                      UserAccount &account) {
  if (!connection_pool_) {
    std::cerr << "Database not connected" << std::endl;
    return false;
  }

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query =
        "SELECT user_id, cash, aapl_qty, googl_qty, msft_qty, "
        "amzn_qty, tsla_qty, buying_power, day_trading_buying_power, "
        "total_trades, realized_pnl, is_active FROM user_accounts WHERE "
        "user_id = $1";

    auto result = txn.exec_params(query, user_id);

    if (result.empty()) {
      return false; // User not found
    }

    auto row = result[0];
    account.user_id = row["user_id"].as<std::string>();
    account.cash = row["cash"].as<long long>();
    account.aapl_qty = row["aapl_qty"].as<int64_t>();
    account.googl_qty = row["googl_qty"].as<int64_t>();
    account.msft_qty = row["msft_qty"].as<int64_t>();
    account.amzn_qty = row["amzn_qty"].as<int64_t>();
    account.tsla_qty = row["tsla_qty"].as<int64_t>();
    account.buying_power = row["buying_power"].as<long long>();
    account.day_trading_buying_power =
        row["day_trading_buying_power"].as<long long>();
    account.total_trades = row["total_trades"].as<int64_t>();
    account.realized_pnl = row["realized_pnl"].as<int64_t>();
    account.is_active = row["is_active"].as<bool>();

    txn.commit();
    return true;

  } catch (const std::exception &e) {
    std::cerr << "Error loading user account: " << e.what() << std::endl;
    return false;
  }
}

bool DatabaseManager::saveUserAccount(const UserAccount &account) {
  if (!connection_pool_) {
    std::cerr << "Database not connected" << std::endl;
    return false;
  }

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query =
        "UPDATE user_accounts SET cash = $2, aapl_qty = $3, "
        "googl_qty = $4, msft_qty = $5, amzn_qty = $6, "
        "tsla_qty = $7, buying_power = $8, day_trading_buying_power = $9, "
        "total_trades = $10, realized_pnl = $11, is_active = $12 WHERE user_id "
        "= $1";

    auto result = txn.exec_params(
        query, account.user_id, account.cash, account.aapl_qty,
        account.googl_qty, account.msft_qty, account.amzn_qty, account.tsla_qty,
        account.buying_power, account.day_trading_buying_power,
        account.total_trades, account.realized_pnl, account.is_active);

    txn.commit();
    return result.affected_rows() > 0;

  } catch (const std::exception &e) {
    std::cerr << "Error saving user account: " << e.what() << std::endl;
    return false;
  }
}

bool DatabaseManager::createUserAccount(const std::string &user_id,
                                        CashAmount initial_cash) {
  if (!connection_pool_) {
    std::cerr << "Database not connected" << std::endl;
    return false;
  }

  try {
    ScopedConnection conn(*connection_pool_);
    pqxx::work txn(conn.get());

    std::string query =
        "INSERT INTO user_accounts (user_id, cash, aapl_qty, googl_qty, "
        "msft_qty, amzn_qty, tsla_qty, buying_power, "
        "day_trading_buying_power, total_trades, realized_pnl, is_active) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)";

    txn.exec_params(query, user_id, initial_cash, 0, 0, 0, 0, 0, initial_cash,
                    initial_cash, 0, 0, 1);

    txn.commit();
    return true;

  } catch (const std::exception &e) {
    std::cerr << "Error creating user account: " << e.what() << std::endl;
    return false;
  }
}

DatabaseManager::UserAccount
DatabaseManager::getUserAccount(const std::string &user_id) {
  UserAccount account;
  if (!loadUserAccount(user_id, account)) {
    return UserAccount(); // Return empty account
  }
  return account;
}

bool DatabaseManager::updateUserAccount(const UserAccount &account) {
  return saveUserAccount(account);
}

#include "AuthenticationManager.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <utility>
#include <limits>
#include "../core_engine/Stock.h"

AuthenticationManager::AuthenticationManager(const std::string& redis_host, int redis_port, DatabaseManager* db_manager)
    : db_manager_(db_manager) {
    try {
        // Create Redis connection
        redis_client_ = std::make_unique<sw::redis::Redis>("tcp://" + redis_host + ":" + std::to_string(redis_port));
    } catch (const std::exception& e) {
        std::cerr << "Failed to create Redis client: " << e.what() << std::endl;
        redis_client_ = nullptr;
    }
}

AuthenticationManager::~AuthenticationManager() = default;

bool AuthenticationManager::initialize() {
    if (!redis_client_) {
        std::cerr << "Redis client not initialized" << std::endl;
        return false;
    }
    
    try {
        // Test Redis connection
        auto result = redis_client_->ping();
        std::cout << "Redis connection established: " << result << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to Redis: " << e.what() << std::endl;
        return false;
    }
}

AuthResult AuthenticationManager::authenticateConnection(ConnectionId conn_id, const std::string& jwt_token) {
    // Check if connection is already authenticated
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(conn_id);
        if (it != sessions_.end() && it->second.isAuthenticated) {
            return AuthResult::ALREADY_AUTHENTICATED;
        }
    }
    
    // Validate JWT token with Redis
    UserId user_id;
    if (!validateJWTWithRedis(jwt_token, user_id)) {
        return AuthResult::INVALID_TOKEN;
    }
    
    // Ensure account is loaded in memory
    if (!getAccount(user_id)) {
        if (!loadAccountFromDatabase(user_id)) {
            std::cerr << "Failed to load account for user: " << user_id << std::endl;
            return AuthResult::USER_NOT_FOUND;
        }
    }
    
    // Create/update session
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[conn_id] = Session(user_id);
    }
    
    std::cout << "User " << user_id << " authenticated successfully on connection " << conn_id << std::endl;
    return AuthResult::SUCCESS;
}

bool AuthenticationManager::isAuthenticated(ConnectionId conn_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(conn_id);
    return it != sessions_.end() && it->second.isAuthenticated;
}

UserId AuthenticationManager::getUserId(ConnectionId conn_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(conn_id);
    if (it != sessions_.end() && it->second.isAuthenticated) {
        return it->second.userId;
    }
    return "";
}

Account* AuthenticationManager::getAccount(const UserId& user_id) {
    std::shared_lock<std::shared_mutex> lock(accounts_mutex_);
    auto it = accounts_.find(user_id);
    if (it != accounts_.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool AuthenticationManager::loadAccountFromDatabase(const UserId& user_id) {
    if (!db_manager_) {
        std::cerr << "Database manager not available" << std::endl;
        return false;
    }
    
    try {
        // Try to load existing account
        DatabaseManager::UserAccount db_account;
        if (db_manager_->loadUserAccount(user_id, db_account)) {
            // Create in-memory account from database data
            auto account = std::make_unique<Account>();
            account->cash.store(db_account.cash);
            account->aapl_qty.store(db_account.aapl_qty);
            account->googl_qty.store(db_account.googl_qty);
            account->msft_qty.store(db_account.msft_qty);
            account->amzn_qty.store(db_account.amzn_qty);
            account->tsla_qty.store(db_account.tsla_qty);
            account->buying_power.store(db_account.buying_power);
            account->day_trading_buying_power.store(db_account.day_trading_buying_power);
            account->total_trades.store(db_account.total_trades);
            account->realized_pnl.store(db_account.realized_pnl);
            account->is_active.store(db_account.is_active);
            
            // Store in accounts map
            {
                std::unique_lock<std::shared_mutex> lock(accounts_mutex_);
                accounts_[user_id] = std::move(account);
            }
            
            std::cout << "Loaded existing account for user: " << user_id << std::endl;
            return true;
        } else {
            // Create new account
            CashAmount initial_cash = 10000000; // $100,000.00 in fixed-point (100,000 * 100)
            if (!db_manager_->createUserAccount(user_id, initial_cash)) {
                std::cerr << "Failed to create new user account in database" << std::endl;
                return false;
            }
            
            // Create in-memory account
            auto account = std::make_unique<Account>(initial_cash);
            
            std::unique_lock<std::shared_mutex> lock(accounts_mutex_);
            accounts_[user_id] = std::move(account);
            
            std::cout << "Created new account for user: " << user_id << std::endl;
            return true;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Database error loading account for user " << user_id << ": " << e.what() << std::endl;
        return false;
    }
}

void AuthenticationManager::removeSession(ConnectionId conn_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(conn_id);
    if (it != sessions_.end()) {
        std::cout << "Removing session for user: " << it->second.userId << " on connection " << conn_id << std::endl;
        sessions_.erase(it);
    }
}

void AuthenticationManager::updateLastActivity(ConnectionId conn_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(conn_id);
    if (it != sessions_.end()) {
        it->second.lastActivity = std::chrono::steady_clock::now();
    }
}

void AuthenticationManager::cleanupInactiveSessions(std::chrono::minutes timeout) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if (now - it->second.lastActivity > timeout) {
            std::cout << "Cleaning up inactive session for user: " << it->second.userId << std::endl;
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

bool AuthenticationManager::checkBuyingPower(const UserId& user_id, CashAmount required_cash) {
    auto account = getAccount(user_id);
    if (!account) {
        return false;
    }
    CashAmount available_cash = account->cash.load() - account->reserved_cash.load();
    return available_cash >= required_cash;
}

void AuthenticationManager::applyTrade(const Trade& trade) {
    const CashAmount trade_value = trade.price * trade.quantity;

    // CRITICAL FIX: Process buyer and seller in a consistent order to prevent deadlock
    // Always lock in the same order: buyer first, then seller (or use std::scoped_lock)
    if (!trade.buy_user_id.empty()) {
        if (auto buyer = getAccount(trade.buy_user_id)) {
            // CRITICAL FIX: Acquire locks in consistent order to prevent deadlock
            // Lock order: account_mutex FIRST, then reservations_mutex
            std::scoped_lock<std::mutex, std::mutex> lock(buyer->account_mutex, reservations_mutex_);
            
            // Consume reservation inline to avoid nested locking
            auto it = order_reservations_.find(trade.buy_order_id);
            if (it != order_reservations_.end()) {
                Reservation& reservation = it->second;
                if (reservation.reserved_cash > 0) {
                    CashAmount consumed = static_cast<CashAmount>(trade.price) * trade.quantity;
                    CashAmount reduction = std::min<CashAmount>(reservation.reserved_cash, consumed);
                    CashAmount current_reserved = buyer->reserved_cash.load();
                    buyer->reserved_cash.store(current_reserved >= reduction ? current_reserved - reduction : 0, std::memory_order_relaxed);
                    reservation.reserved_cash -= reduction;
                    reservation.reserved_quantity = reservation.reserved_quantity > trade.quantity ? reservation.reserved_quantity - trade.quantity : 0;
                }
                if (reservation.reserved_cash == 0 && reservation.reserved_quantity == 0) {
                    order_reservations_.erase(it);
                }
            }
            
            // Apply trade to account
            if (auto* position = locatePosition(buyer, trade.symbol)) {
                position->fetch_add(static_cast<int64_t>(trade.quantity), std::memory_order_relaxed);
            }
            buyer->cash.fetch_sub(trade_value, std::memory_order_relaxed);
            buyer->buying_power.store(buyer->cash.load() - buyer->reserved_cash.load(), std::memory_order_relaxed);
            buyer->day_trading_buying_power.store(buyer->buying_power.load(), std::memory_order_relaxed);
            buyer->total_trades.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!trade.sell_user_id.empty()) {
        if (auto seller = getAccount(trade.sell_user_id)) {
            // CRITICAL FIX: Acquire locks in consistent order to prevent deadlock
            // Lock order: account_mutex FIRST, then reservations_mutex
            std::scoped_lock<std::mutex, std::mutex> lock(seller->account_mutex, reservations_mutex_);
            
            // Consume reservation inline to avoid nested locking
            auto it = order_reservations_.find(trade.sell_order_id);
            if (it != order_reservations_.end()) {
                Reservation& reservation = it->second;
                if (reservation.reserved_quantity > 0) {
                    auto* reserved_position = locateReservedPosition(seller, reservation.symbol);
                    if (reserved_position) {
                        int64_t reduction = std::min<int64_t>(reservation.reserved_quantity, trade.quantity);
                        int64_t current_reserved = reserved_position->load();
                        reserved_position->store(current_reserved >= reduction ? current_reserved - reduction : 0, std::memory_order_relaxed);
                        reservation.reserved_quantity -= reduction;
                    }
                }
                if (reservation.reserved_cash == 0 && reservation.reserved_quantity == 0) {
                    order_reservations_.erase(it);
                }
            }
            
            // Apply trade to account
            if (auto* position = locatePosition(seller, trade.symbol)) {
                position->fetch_sub(static_cast<int64_t>(trade.quantity), std::memory_order_relaxed);
            }
            seller->cash.fetch_add(trade_value, std::memory_order_relaxed);
            seller->buying_power.store(seller->cash.load() - seller->reserved_cash.load(), std::memory_order_relaxed);
            seller->day_trading_buying_power.store(seller->buying_power.load(), std::memory_order_relaxed);
            seller->total_trades.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool AuthenticationManager::reserveForOrder(const Order& order, Price effective_price, std::string& reason) {
    if (order.order_id.empty()) {
        reason = "rejected: missing order_id";
        return false;
    }

    auto account = getAccount(order.user_id);
    if (!account) {
        reason = "rejected: account not found";
        return false;
    }

    std::scoped_lock<std::mutex, std::mutex> lock(account->account_mutex, reservations_mutex_);

    if (order_reservations_.find(order.order_id) != order_reservations_.end()) {
        reason = "rejected: duplicate order_id";
        return false;
    }

    if (order.side == 0) {
        if (effective_price <= 0) {
            effective_price = Order::fromDouble(0.01);
        }
        if (order.quantity > std::numeric_limits<int64_t>::max() / effective_price) {
            reason = "rejected: order value too large";
            return false;
        }
        CashAmount required_cash = static_cast<CashAmount>(effective_price) * order.quantity;
        CashAmount available_cash = account->cash.load() - account->reserved_cash.load();
        if (available_cash < required_cash) {
            reason = "rejected: insufficient buying power";
            return false;
        }
        account->reserved_cash.fetch_add(required_cash, std::memory_order_relaxed);
        account->buying_power.store(account->cash.load() - account->reserved_cash.load(), std::memory_order_relaxed);
        account->day_trading_buying_power.store(account->buying_power.load(), std::memory_order_relaxed);
        Reservation reservation;
        reservation.reserved_cash = required_cash;
        reservation.reserved_quantity = order.quantity;
        reservation.symbol = order.symbol;
        reservation.side = order.side;
        reservation.price = effective_price;
        order_reservations_.emplace(order.order_id, std::move(reservation));
        return true;
    }

    auto* position = locatePosition(account, order.symbol);
    auto* reserved_position = locateReservedPosition(account, order.symbol);
    if (!position || !reserved_position) {
        reason = "rejected: unknown symbol";
        return false;
    }

    int64_t available_shares = position->load() - reserved_position->load();
    if (available_shares < order.quantity) {
        reason = "rejected: insufficient shares";
        return false;
    }

    reserved_position->fetch_add(static_cast<int64_t>(order.quantity), std::memory_order_relaxed);
    Reservation reservation;
    reservation.reserved_quantity = order.quantity;
    reservation.symbol = order.symbol;
    reservation.side = order.side;
    reservation.price = effective_price;
    order_reservations_.emplace(order.order_id, std::move(reservation));
    return true;
}

void AuthenticationManager::releaseForOrder(const Order& order, const std::string& /*reason*/) {
    auto account = getAccount(order.user_id);
    if (!account) {
        return;
    }

    std::scoped_lock<std::mutex, std::mutex> lock(account->account_mutex, reservations_mutex_);
    auto it = order_reservations_.find(order.order_id);
    if (it == order_reservations_.end()) {
        return;
    }

    const Reservation& reservation = it->second;
    if (reservation.side == 0 && reservation.reserved_cash > 0) {
        CashAmount reserved_cash = reservation.reserved_cash;
        CashAmount current_reserved = account->reserved_cash.load();
        account->reserved_cash.store(current_reserved >= reserved_cash ? current_reserved - reserved_cash : 0, std::memory_order_relaxed);
        account->buying_power.store(account->cash.load() - account->reserved_cash.load(), std::memory_order_relaxed);
        account->day_trading_buying_power.store(account->buying_power.load(), std::memory_order_relaxed);
    } else if (reservation.side == 1 && reservation.reserved_quantity > 0) {
        if (auto* reserved_position = locateReservedPosition(account, reservation.symbol)) {
            int64_t current_reserved = reserved_position->load();
            int64_t release_qty = std::min<int64_t>(reservation.reserved_quantity, current_reserved);
            reserved_position->store(current_reserved - release_qty, std::memory_order_relaxed);
        }
    }

    order_reservations_.erase(it);
}

std::atomic<int64_t>* AuthenticationManager::locatePosition(Account* account, const std::string& symbol) {
    if (!account) {
        return nullptr;
    }
    if (symbol == "GOOGL" || symbol == "GOOG") return &account->googl_qty;
    if (symbol == "AAPL") return &account->aapl_qty;
    if (symbol == "TSLA") return &account->tsla_qty;
    if (symbol == "MSFT") return &account->msft_qty;
    if (symbol == "AMZN") return &account->amzn_qty;
    return nullptr;
}

std::atomic<int64_t>* AuthenticationManager::locateReservedPosition(Account* account, const std::string& symbol) {
    if (!account) {
        return nullptr;
    }
    if (symbol == "GOOGL" || symbol == "GOOG") return &account->reserved_googl;
    if (symbol == "AAPL") return &account->reserved_aapl;
    if (symbol == "TSLA") return &account->reserved_tsla;
    if (symbol == "MSFT") return &account->reserved_msft;
    if (symbol == "AMZN") return &account->reserved_amzn;
    return nullptr;
}

void AuthenticationManager::consumeReservationForTrade(const std::string& order_id,
                                                       const UserId& user_id,
                                                       Price execution_price,
                                                       int64_t quantity,
                                                       int side) {
    if (order_id.empty()) {
        return;
    }

    auto account = getAccount(user_id);
    if (!account) {
        return;
    }

    std::scoped_lock<std::mutex, std::mutex> lock(account->account_mutex, reservations_mutex_);
    auto it = order_reservations_.find(order_id);
    if (it == order_reservations_.end()) {
        return;
    }

    Reservation& reservation = it->second;
    if (side == 0 && reservation.reserved_cash > 0) {
        CashAmount consumed = static_cast<CashAmount>(execution_price) * quantity;
        CashAmount reduction = std::min<CashAmount>(reservation.reserved_cash, consumed);
        CashAmount current_reserved = account->reserved_cash.load();
        account->reserved_cash.store(current_reserved >= reduction ? current_reserved - reduction : 0, std::memory_order_relaxed);
        reservation.reserved_cash -= reduction;
        reservation.reserved_quantity = reservation.reserved_quantity > quantity ? reservation.reserved_quantity - quantity : 0;
        account->buying_power.store(account->cash.load() - account->reserved_cash.load(), std::memory_order_relaxed);
        account->day_trading_buying_power.store(account->buying_power.load(), std::memory_order_relaxed);
    } else if (side == 1 && reservation.reserved_quantity > 0) {
        auto* reserved_position = locateReservedPosition(account, reservation.symbol);
        if (reserved_position) {
            int64_t reduction = std::min<int64_t>(reservation.reserved_quantity, quantity);
            int64_t current_reserved = reserved_position->load();
            reserved_position->store(current_reserved >= reduction ? current_reserved - reduction : 0, std::memory_order_relaxed);
            reservation.reserved_quantity -= reduction;
        }
    }

    if (reservation.reserved_cash == 0 && reservation.reserved_quantity == 0) {
        order_reservations_.erase(it);
    }
}

size_t AuthenticationManager::getActiveSessionCount() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

size_t AuthenticationManager::getLoadedAccountCount() {
    std::shared_lock<std::shared_mutex> lock(accounts_mutex_);
    return accounts_.size();
}

void AuthenticationManager::syncAllAccountsToDatabase() {
    if (!db_manager_) {
        return;
    }
    
    std::shared_lock<std::shared_mutex> lock(accounts_mutex_);
    
    int synced_count = 0;
    int failed_count = 0;
    
    for (const auto& [user_id, account] : accounts_) {
        // Convert in-memory Account to DatabaseManager::UserAccount
        DatabaseManager::UserAccount db_account;
        db_account.user_id = user_id;
        db_account.cash = account->cash.load();
        db_account.aapl_qty = account->aapl_qty.load();
        db_account.googl_qty = account->googl_qty.load();
        db_account.msft_qty = account->msft_qty.load();
        db_account.amzn_qty = account->amzn_qty.load();
        db_account.tsla_qty = account->tsla_qty.load();
        db_account.buying_power = account->buying_power.load();
        db_account.day_trading_buying_power = account->day_trading_buying_power.load();
        db_account.total_trades = account->total_trades.load();
        db_account.realized_pnl = account->realized_pnl.load();
        db_account.is_active = account->is_active.load();
        
        // Save to database
        if (db_manager_->updateUserAccount(db_account)) {
            synced_count++;
        } else {
            failed_count++;
            std::cerr << "Failed to sync account to DB: " << user_id << std::endl;
        }
    }
    
    if (synced_count > 0) {
        std::cout << "✅ Synced " << synced_count << " accounts to database";
        if (failed_count > 0) {
            std::cout << " (" << failed_count << " failed)";
        }
        std::cout << std::endl;
    }
}

void AuthenticationManager::clearCachedAccounts() {
    {
        std::unique_lock<std::shared_mutex> lock(accounts_mutex_);
        accounts_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(reservations_mutex_);
        order_reservations_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }

    std::cout << "Authentication cache cleared; accounts will be reloaded from database on next use" << std::endl;
}

bool AuthenticationManager::validateJWTWithRedis(const std::string& jwt_token, UserId& user_id) {
    if (!redis_client_) {
        std::cerr << "Redis client not available" << std::endl;
        return false;
    }
    
    try {
        // Construct Redis key
        std::string redis_key = TRADING_TOKEN_PREFIX + jwt_token;
        
        // Query Redis for the token
        auto result = redis_client_->get(redis_key);
        if (!result) {
            std::cerr << "Token not found in Redis: " << jwt_token.substr(0, 20) << "..." << std::endl;
            return false;
        }
        
        user_id = *result;
        std::cout << "Token validated for user: " << user_id << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Redis error validating token: " << e.what() << std::endl;
        return false;
    }
}

bool AuthenticationManager::createAccountEntry(const UserId& user_id, const Account& account_data) {
    auto new_account = std::make_unique<Account>(account_data);
    
    std::unique_lock<std::shared_mutex> lock(accounts_mutex_);
    accounts_[user_id] = std::move(new_account);
    return true;
}

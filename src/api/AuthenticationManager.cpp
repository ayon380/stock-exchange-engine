#include "AuthenticationManager.h"
#include <iostream>
#include <chrono>
#include <algorithm>

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
    
    return account->buying_power.load() >= required_cash;
}

bool AuthenticationManager::updatePosition(const UserId& user_id, const std::string& symbol, long quantity_change, Price price) {
    auto account = getAccount(user_id);
    if (!account) {
        return false;
    }
    
    // Update position based on symbol
    std::atomic<long>* position = nullptr;
    if (symbol == "GOOGL" || symbol == "GOOG") position = &account->googl_qty;
    else if (symbol == "AAPL") position = &account->aapl_qty;
    else if (symbol == "TSLA") position = &account->tsla_qty;
    else if (symbol == "MSFT") position = &account->msft_qty;
    else if (symbol == "AMZN") position = &account->amzn_qty;
    
    if (position) {
        long old_position = position->load();
        position->store(old_position + quantity_change);
    } else {
        std::cerr << "Unknown symbol: " << symbol << std::endl;
        return false;
    }
    
    // Update cash (subtract for buys, add for sells) - integer arithmetic
    CashAmount cash_change = -quantity_change * price;
    CashAmount old_cash = account->cash.load();
    account->cash.store(old_cash + cash_change);
    
    // Update buying power (simplified calculation)
    account->buying_power.store(account->cash.load());
    
    return true;
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

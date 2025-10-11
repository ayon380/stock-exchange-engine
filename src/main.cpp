#include "api/GRPCServer.h"
#include "api/TCPServer.h"
#include "api/SharedMemoryQueue.h"
#include "api/AuthenticationManager.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

std::atomic<bool> shutdown_requested(false);
std::atomic<bool> mode_switch_requested(false);

// Non-blocking keyboard input detection
int kbhit() {
    struct termios oldt, newt;
    int ch;
    int oldf;
    
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    
    ch = getchar();
    
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    
    if(ch != EOF) {
        ungetc(ch, stdin);
        return 1;
    }
    
    return 0;
}

// Admin mode functions
void displayAdminMenu() {
    std::cout << "\n╔══════════════════════════════════════╗" << std::endl;
    std::cout << "║     ADMIN MODE - Account Management  ║" << std::endl;
    std::cout << "╠══════════════════════════════════════╣" << std::endl;
    std::cout << "║  1. View Account Balance             ║" << std::endl;
    std::cout << "║  2. Deposit Funds                    ║" << std::endl;
    std::cout << "║  3. Withdraw Funds                   ║" << std::endl;
    std::cout << "║  4. View All Accounts                ║" << std::endl;
    std::cout << "║  5. Create New Account               ║" << std::endl;
    std::cout << "║  E. Return to Trading Mode           ║" << std::endl;
    std::cout << "║  Q. Quit Application                 ║" << std::endl;
    std::cout << "╚══════════════════════════════════════╝" << std::endl;
    std::cout << "Enter choice: ";
}

void viewAccountBalance(DatabaseManager* db) {
    std::string user_id;
    std::cout << "\nEnter User ID: ";
    std::cin >> user_id;
    
    auto account = db->getUserAccount(user_id);
    if (account.user_id.empty()) {
        std::cout << "❌ Account not found!" << std::endl;
        return;
    }
    
    std::cout << "\n╔═══════════════ Account Info ═══════════════╗" << std::endl;
    std::cout << "  User ID: " << account.user_id << std::endl;
    std::cout << "  Cash: $" << std::fixed << std::setprecision(2) << account.cashToDouble() << std::endl;
    std::cout << "  AAPL: " << account.aapl_qty << std::endl;
    std::cout << "  GOOGL: " << account.googl_qty << std::endl;
    std::cout << "  MSFT: " << account.msft_qty << std::endl;
    std::cout << "  AMZN: " << account.amzn_qty << std::endl;
    std::cout << "  TSLA: " << account.tsla_qty << std::endl;
    std::cout << "  Total Trades: " << account.total_trades << std::endl;
    std::cout << "  P&L: $" << account.realized_pnl / 100.0 << std::endl;
    std::cout << "  Status: " << (account.is_active ? "Active" : "Suspended") << std::endl;
    std::cout << "╚════════════════════════════════════════════╝" << std::endl;
}

void depositFunds(DatabaseManager* db) {
    std::string user_id;
    double amount;
    
    std::cout << "\nEnter User ID: ";
    std::cin >> user_id;
    
    auto account = db->getUserAccount(user_id);
    if (account.user_id.empty()) {
        std::cout << "❌ Account not found!" << std::endl;
        return;
    }
    
    std::cout << "Current Balance: $" << std::fixed << std::setprecision(2) << account.cashToDouble() << std::endl;
    std::cout << "Enter deposit amount: $";
    std::cin >> amount;
    
    if (amount <= 0) {
        std::cout << "❌ Invalid amount!" << std::endl;
        return;
    }
    
    CashAmount new_cash = account.cash + DatabaseManager::UserAccount::fromDouble(amount);
    account.cash = new_cash;
    
    if (db->updateUserAccount(account)) {
        std::cout << "✅ Deposit successful!" << std::endl;
        std::cout << "New Balance: $" << std::fixed << std::setprecision(2) << (new_cash / 100.0) << std::endl;
    } else {
        std::cout << "❌ Deposit failed!" << std::endl;
    }
}

void withdrawFunds(DatabaseManager* db) {
    std::string user_id;
    double amount;
    
    std::cout << "\nEnter User ID: ";
    std::cin >> user_id;
    
    auto account = db->getUserAccount(user_id);
    if (account.user_id.empty()) {
        std::cout << "❌ Account not found!" << std::endl;
        return;
    }
    
    std::cout << "Current Balance: $" << std::fixed << std::setprecision(2) << account.cashToDouble() << std::endl;
    std::cout << "Enter withdrawal amount: $";
    std::cin >> amount;
    
    if (amount <= 0) {
        std::cout << "❌ Invalid amount!" << std::endl;
        return;
    }
    
    CashAmount withdrawal = DatabaseManager::UserAccount::fromDouble(amount);
    if (withdrawal > account.cash) {
        std::cout << "❌ Insufficient funds!" << std::endl;
        return;
    }
    
    account.cash -= withdrawal;
    
    if (db->updateUserAccount(account)) {
        std::cout << "✅ Withdrawal successful!" << std::endl;
        std::cout << "New Balance: $" << std::fixed << std::setprecision(2) << (account.cash / 100.0) << std::endl;
    } else {
        std::cout << "❌ Withdrawal failed!" << std::endl;
    }
}

void viewAllAccounts(DatabaseManager* db) {
    std::cout << "\n╔═══════════════ All Accounts ═══════════════╗" << std::endl;
    // Note: You'll need to add a getAllAccounts() method to DatabaseManager
    std::cout << "Feature coming soon - getAllAccounts() needs to be implemented" << std::endl;
    std::cout << "╚════════════════════════════════════════════╝" << std::endl;
}

void createNewAccount(DatabaseManager* db) {
    std::string user_id;
    double initial_cash;
    
    std::cout << "\nEnter new User ID: ";
    std::cin >> user_id;
    
    std::cout << "Enter initial cash amount: $";
    std::cin >> initial_cash;
    
    if (initial_cash < 0) {
        std::cout << "❌ Invalid amount!" << std::endl;
        return;
    }
    
    CashAmount cash = DatabaseManager::UserAccount::fromDouble(initial_cash);
    
    if (db->createUserAccount(user_id, cash)) {
        std::cout << "✅ Account created successfully!" << std::endl;
        std::cout << "User ID: " << user_id << std::endl;
        std::cout << "Initial Balance: $" << std::fixed << std::setprecision(2) << initial_cash << std::endl;
    } else {
        std::cout << "❌ Account creation failed! (User ID may already exist)" << std::endl;
    }
}

void runAdminMode(DatabaseManager* db) {
    std::cout << "\n🔒 Entering ADMIN MODE - Exchange is CLOSED" << std::endl;
    std::cout << "All trading is suspended. Account management enabled." << std::endl;
    
    bool exit_admin = false;
    while (!exit_admin && !shutdown_requested.load()) {
        displayAdminMenu();
        
        char choice;
        std::cin >> choice;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        switch(choice) {
            case '1':
                viewAccountBalance(db);
                break;
            case '2':
                depositFunds(db);
                break;
            case '3':
                withdrawFunds(db);
                break;
            case '4':
                viewAllAccounts(db);
                break;
            case '5':
                createNewAccount(db);
                break;
            case 'E':
            case 'e':
                std::cout << "\n🔓 Exiting Admin Mode - Returning to Trading Mode..." << std::endl;
                exit_admin = true;
                break;
            case 'Q':
            case 'q':
                std::cout << "\n👋 Shutting down application..." << std::endl;
                shutdown_requested.store(true);
                exit_admin = true;
                break;
            default:
                std::cout << "❌ Invalid choice!" << std::endl;
                break;
        }
        
        if (!exit_admin && !shutdown_requested.load()) {
            std::cout << "\nPress Enter to continue...";
            std::cin.get();
        }
    }
}

#ifdef _WIN32
#include <windows.h>
BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT || dwCtrlType == CTRL_CLOSE_EVENT) {
        shutdown_requested.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
#include <signal.h>
void signalHandler(int signal) {
    shutdown_requested.store(true);
}
#endif

int main() {
    // Set up signal handlers for graceful shutdown
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(consoleCtrlHandler, TRUE)) {
        std::cerr << "Failed to set console control handler" << std::endl;
        return -1;
    }
#else
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        std::cerr << "Failed to set SIGINT handler" << std::endl;
        return -1;
    }
    
    if (sigaction(SIGTERM, &sa, nullptr) == -1) {
        std::cerr << "Failed to set SIGTERM handler" << std::endl;
        return -1;
    }
#endif
    
    std::string server_address("0.0.0.0:50051");
    
    // Database connection string (modify as needed)
    std::string db_connection = "host=localhost port=5432 dbname=stockexchange user=myuser password=mypassword";
    
    // Initialize gRPC server with database connection
    GRPCServer service(db_connection);
    
    if (!service.initialize()) {
        std::cerr << "Failed to initialize gRPC service" << std::endl;
        return -1;
    }
    
    // Start the stock exchange engine
    service.start();
    
    // Initialize Authentication Manager
    std::string redis_host = "localhost";
    int redis_port = 6379;
    auto auth_manager = std::make_unique<AuthenticationManager>(redis_host, redis_port, service.getExchange()->getDatabaseManager());
    
    if (!auth_manager->initialize()) {
        std::cerr << "Failed to initialize Authentication Manager" << std::endl;
        return -1;
    }
    
    std::cout << "Authentication Manager initialized successfully" << std::endl;
    
    // Initialize TCP server for high-performance order submission
    std::string tcp_address("0.0.0.0");
    uint16_t tcp_port = 50052; // Different port from gRPC
    TCPServer tcp_server(tcp_address, tcp_port, service.getExchange(), auth_manager.get());
    
    // Initialize shared memory server for ultra-low latency local clients
    std::string shm_name("stock_exchange_orders");
    SharedMemoryOrderServer shm_server(shm_name, service.getExchange());
    
    // Set up gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    
    // Set max message sizes for streaming
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024); // 4MB
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);    // 4MB
    
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "\n=== Stock Exchange Server ===" << std::endl;
    std::cout << "gRPC Server listening on " << server_address << std::endl;
    std::cout << "TCP Order Server listening on " << tcp_address << ":" << tcp_port << std::endl;
    std::cout << "Shared Memory Server available at '" << shm_name << "'" << std::endl;
    std::cout << "Features:" << std::endl;
    std::cout << "  • High-performance TCP binary protocol for order submission" << std::endl;
    std::cout << "  • Ultra-low latency shared memory for local clients" << std::endl;
    std::cout << "  • gRPC streaming for market data, UI, and demo purposes" << std::endl;
    auto symbols = service.getExchange()->getSymbols();
    std::ostringstream symbol_stream;
    for (size_t i = 0; i < symbols.size(); ++i) {
        symbol_stream << symbols[i];
        if (i + 1 < symbols.size()) {
            symbol_stream << ", ";
        }
    }
    std::cout << "  • " << symbols.size() << " Stock symbols (" << symbol_stream.str() << ")" << std::endl;
    std::cout << "  • Individual threads per stock" << std::endl;
    std::cout << "  • Real-time streaming market data" << std::endl;
    std::cout << "  • Market Index (TECH500) - like S&P 500/Sensex" << std::endl;
    std::cout << "  • Live streaming of all stock prices" << std::endl;
    std::cout << "  • Top 5 index streaming" << std::endl;
    std::cout << "  • PostgreSQL integration (30sec sync)" << std::endl;
    std::cout << "  • Order matching engine" << std::endl;
    std::cout << "\n📊 TRADING MODE - Exchange is OPEN" << std::endl;
    std::cout << "Press 'E' to enter ADMIN MODE (close exchange for account management)" << std::endl;
    std::cout << "Press 'E' twice to EXIT application" << std::endl;
    std::cout << "=============================" << std::endl;
    
    // Start TCP server
    tcp_server.start();
    
    // Start shared memory server
    if (!shm_server.start()) {
        std::cerr << "Warning: Failed to start shared memory server" << std::endl;
    }
    
    // Start account sync thread (syncs every 30 seconds)
    std::atomic<bool> sync_running(true);
    std::thread account_sync_thread([&auth_manager, &sync_running]() {
        while (sync_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (sync_running.load()) {
                auth_manager->syncAllAccountsToDatabase();
            }
        }
    });
    
    std::cout << "💾 Account balance sync: Every 30 seconds to database" << std::endl;
    
    // Main loop - handle mode switching
    bool in_trading_mode = true;
    bool e_pressed_once = false;
    auto last_e_press = std::chrono::steady_clock::now();
    
    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Check for keyboard input
        if (kbhit()) {
            char ch = getchar();
            if (ch == 'E' || ch == 'e') {
                auto now = std::chrono::steady_clock::now();
                auto time_since_last_press = std::chrono::duration_cast<std::chrono::seconds>(now - last_e_press).count();
                
                if (e_pressed_once && time_since_last_press < 3) {
                    // Second 'E' press within 3 seconds - EXIT
                    std::cout << "\n👋 Double E detected - Shutting down application..." << std::endl;
                    shutdown_requested.store(true);
                    break;
                } else {
                    // First 'E' press or too long since last press
                    e_pressed_once = true;
                    last_e_press = now;
                    
                    if (in_trading_mode) {
                        // Switch to ADMIN mode
                        std::cout << "\n🔒 Stopping exchange for ADMIN MODE..." << std::endl;
                        
                        // Stop trading servers
                        tcp_server.stop();
                        shm_server.stop();
                        service.stop();
                        
                        std::cout << "✅ Exchange stopped. All trading suspended." << std::endl;
                        
                        in_trading_mode = false;
                        
                        // Enter admin mode
                        runAdminMode(service.getExchange()->getDatabaseManager());
                        
                        if (!shutdown_requested.load()) {
                            // Returning from admin mode - restart trading
                            std::cout << "\n🔓 Restarting exchange for TRADING MODE..." << std::endl;
                            service.start();
                            tcp_server.start();
                            if (!shm_server.start()) {
                                std::cerr << "Warning: Failed to restart shared memory server" << std::endl;
                            }
                            std::cout << "✅ Exchange restarted. Trading resumed." << std::endl;
                            std::cout << "\n📊 TRADING MODE - Exchange is OPEN" << std::endl;
                            std::cout << "Press 'E' to enter ADMIN MODE" << std::endl;
                            std::cout << "Press 'E' twice to EXIT application" << std::endl;
                            in_trading_mode = true;
                        }
                        
                        e_pressed_once = false; // Reset after mode switch
                    }
                }
            }
        }
        
        // Reset E press flag after 3 seconds
        auto now = std::chrono::steady_clock::now();
        auto time_since_last_press = std::chrono::duration_cast<std::chrono::seconds>(now - last_e_press).count();
        if (e_pressed_once && time_since_last_press >= 3) {
            e_pressed_once = false;
        }
    }
    
    std::cout << "\nShutdown signal received. Gracefully shutting down..." << std::endl;
    
    // Stop account sync thread
    std::cout << "Stopping account sync thread..." << std::endl;
    sync_running.store(false);
    if (account_sync_thread.joinable()) {
        account_sync_thread.join();
    }
    std::cout << "Account sync thread stopped" << std::endl;
    
    // Final sync before shutdown
    std::cout << "Performing final account sync to database..." << std::endl;
    auth_manager->syncAllAccountsToDatabase();
    std::cout << "Final sync complete" << std::endl;
    
    // Clean shutdown
    if (in_trading_mode) {
        std::cout << "Stopping TCP server..." << std::endl;
        tcp_server.stop();
        std::cout << "TCP server stopped" << std::endl;
        
        shm_server.stop();
        std::cout << "Shared memory server stopped" << std::endl;
        
        service.stop();
        std::cout << "gRPC service stopped" << std::endl;
    }
    
    // Then shutdown the gRPC server
    server->Shutdown();
    
    std::cout << "Stock Exchange Server shut down successfully" << std::endl;
    return 0;
}

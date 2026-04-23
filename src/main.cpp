/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

// Standard C++ includes for threading, synchronization, and utilities
#include <algorithm>  // For std::min, std::max - used in admin functions for buying power calculations
#include <atomic>     // For thread-safe variables like shutdown flags - atomic operations avoid race conditions without locks
#include <cerrno>     // For errno - used in kbhit for error checking on terminal operations
#include <chrono>     // For time handling in telemetry and delays - high-resolution timing for performance metrics
#include <condition_variable>  // For sync_cv - allows threads to wait efficiently with timeouts, better than busy-waiting
#include <csignal>    // For signal handling - cross-platform signal registration for graceful shutdown
#include <cstdio>     // For perror - error reporting in kbhit
#include <cstdlib>    // For getenv, setenv - environment variable management
#include <fcntl.h>    // For fcntl - file descriptor control in kbhit for non-blocking I/O
#include <fstream>    // For file I/O in loadEnvFile and readFileContents
#include <grpcpp/grpcpp.h>  // gRPC library - for building the secure streaming server, chosen for high-performance RPC over REST
#include <iomanip>    // For std::setprecision - formatting output in telemetry and admin displays
#include <iostream>   // Standard I/O - console output for logs and user interaction
#include <limits>     // For std::numeric_limits - input validation in admin functions
#include <memory>     // For std::unique_ptr, std::make_unique - RAII for resource management, prevents leaks
#include <mutex>      // For std::mutex - protects shared state in sync thread
#include <sstream>    // For string streams in formatting functions
#include <stdexcept>  // For std::runtime_error - exception handling in file reads
#include <string>     // Standard string handling
#include <termios.h>  // For terminal control in kbhit - Unix-specific TTY manipulation for non-blocking input
#include <thread>     // For std::thread - concurrent execution of servers, sync, and telemetry
#include <unistd.h>   // For isatty, getchar, etc. - Unix system calls for TTY detection and I/O

// Custom project headers for modular components
#include "api/AuthenticationManager.h"  // Handles user auth with Redis - centralized auth logic
#include "api/GRPCServer.h"             // gRPC service implementation - streaming market data
#include "api/SharedMemoryQueue.h"      // Shared memory for ultra-low latency - bypasses network stack
#include "api/TCPServer.h"              // TCP server for binary orders - high-throughput protocol
#include "common/EngineConfig.h"        // Configuration management - dev mode flags
#include "common/EngineLogging.h"       // Logging macros - conditional logging based on mode
#include "common/EngineTelemetry.h"     // Performance metrics - real-time stats collection


// Global atomic variables for thread-safe state management across the application
// std::atomic ensures operations are lock-free and visible to all threads without data races
std::atomic<bool> shutdown_requested(false);  // Signals all threads to stop gracefully - atomic prevents race conditions on shutdown
std::atomic<bool> mode_switch_requested(false);  // For switching modes (trading/admin) - used in keyboard handling
std::atomic<bool> telemetry_display_paused(false);  // Pauses telemetry output during admin mode - atomic for thread safety

namespace {  // Anonymous namespace to limit scope of utility functions to this file only

// Checks if stdin is connected to a terminal (TTY) for interactive input
// isatty() is a POSIX function - returns 1 if TTY, 0 otherwise
// Cached in static variable to avoid repeated syscalls
bool stdin_is_tty() {
  static const bool is_tty = (isatty(STDIN_FILENO) == 1);
  return is_tty;
}

// Retrieves environment variable or returns fallback if not set or empty
// getenv() is C standard - safer than direct access, handles null/empty
std::string getEnvOrDefault(const char *name, const std::string &fallback) {
  if (!name) {  // Null check for safety
    return fallback;
  }
  const char *value = std::getenv(name);  // C function for env vars
  if (value && *value != '\0') {  // Check if set and non-empty
    return value;
  }
  return fallback;
}

// Reads entire file into string using binary mode for TLS certs
// std::ifstream with binary flag preserves exact bytes
// Throws runtime_error on failure - exception handling for config errors
std::string readFileContents(const std::string &path) {
  std::ifstream file(path, std::ios::binary);  // Binary mode for cert files
  if (!file) {
    throw std::runtime_error("Failed to open file: " + path);
  }

  std::ostringstream buffer;  // String stream for efficient concatenation
  buffer << file.rdbuf();  // Read entire file at once
  return buffer.str();
}

// Loads .env file and sets environment variables
// Parses key=value format, handles comments (#), trims whitespace/quotes
// setenv() is POSIX - sets env vars without overriding existing ones (0 flag)
// Used for configuration instead of hardcoded values for flexibility
void loadEnvFile(const std::string &path) {
  std::ifstream file(path);
  if (!file)
    return;  // Silent failure if .env not found

  std::string line;
  while (std::getline(file, line)) {  // Read line by line
    // Trim leading whitespace
    size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
      continue; // Empty line

    if (line[first] == '#')
      continue; // Comment

    // Find delimiter
    size_t delimiterPos = line.find('=', first);
    if (delimiterPos == std::string::npos)
      continue;

    std::string key = line.substr(first, delimiterPos - first);
    std::string value = line.substr(delimiterPos + 1);

    // Trim trailing whitespace from key
    size_t lastKey = key.find_last_not_of(" \t");
    if (lastKey != std::string::npos) {
      key = key.substr(0, lastKey + 1);
    }

    // Trim quotes from value if present
    size_t firstVal = value.find_first_not_of(" \t");
    if (firstVal == std::string::npos) {
      value = "";
    } else {
      value = value.substr(firstVal);
      size_t lastVal = value.find_last_not_of(" \t\r\n");
      if (lastVal != std::string::npos) {
        value = value.substr(0, lastVal + 1);
      }

      // Remove surrounding quotes if present
      if (value.size() >= 2 &&
          ((value.front() == '"' && value.back() == '"') ||
           (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
      }
    }

    // Only set if not already set (environment takes precedence)
    setenv(key.c_str(), value.c_str(), 0);  // 0 = don't overwrite existing
  }
}

} // namespace

// Non-blocking keyboard input detection
// kbhit() is not standard C++ - this is a custom implementation using POSIX termios
// Why not simple getchar()? getchar() blocks until input, halting the main loop
// This allows checking for key presses without stopping execution, enabling interactive control
int kbhit() {
  if (!stdin_is_tty()) {  // Only works on TTY - return 0 for pipes/files
    return 0;
  }

  struct termios oldt;  // Save original terminal settings
  if (tcgetattr(STDIN_FILENO, &oldt) == -1) {  // Get current TTY attributes
    if (errno != ENOTTY) {  // ENOTTY = not a TTY, ignore; other errors print
      std::perror("tcgetattr");
    }
    return 0;
  }

  struct termios newt = oldt;  // Copy settings
  newt.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode (line buffering) and echo
  if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == -1) {  // Apply immediately
    std::perror("tcsetattr");
    return 0;
  }

  int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);  // Get file descriptor flags
  if (oldf == -1) {
    std::perror("fcntl(F_GETFL)");
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore on error
    return 0;
  }

  if (fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK) == -1) {  // Set non-blocking
    std::perror("fcntl(F_SETFL)");
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
  }

  int ch = getchar();  // Now non-blocking - returns EOF if no input

  // Always attempt to restore state - critical for terminal integrity
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  if (ch != EOF) {
    ungetc(ch, stdin);  // Push back to stdin so it can be read normally later
    return 1;  // Key available
  }

  return 0;  // No key pressed
}

namespace {  // Second anonymous namespace for display utilities

// Formats double with fixed precision for consistent output
// std::fixed and setprecision ensure decimal places, better than printf for C++
std::string formatDouble(double value, int precision) {
  std::ostringstream oss;  // String stream for safe formatting
  oss << std::fixed << std::setprecision(precision) << value;
  return oss.str();
}

// Formats latency in human-readable units (us/ms)
// Converts microseconds to appropriate unit, handles n/a for invalid values
std::string formatLatency(double microseconds) {
  if (microseconds <= 0.0) {
    return "n/a";  // Invalid latency
  }

  if (microseconds >= 1000.0) {  // Convert to milliseconds
    const double ms = microseconds / 1000.0;
    const int precision = ms >= 10.0 ? 1 : 2;  // Adaptive precision
    return formatDouble(ms, precision) + " ms";
  }

  const int precision = microseconds >= 100.0 ? 1 : 2;
  return formatDouble(microseconds, precision) + " us";  // Microseconds
}

// Renders ASCII art banner with colors using ANSI escape codes
// std::this_thread::sleep_for adds animation delay - threads allow non-blocking UI
void renderAurexBanner() {
  static constexpr const char *banner[] = {  // Static constexpr for compile-time array
    "      █████╗ ██╗   ██╗██████╗ ███████╗██╗  ██╗",
    "     ██╔══██╗██║   ██║██╔══██╗██╔════╝╚██╗██╔╝",
    "     ███████║██║   ██║██████╔╝█████╗   ╚███╔╝ ",
    "     ██╔══██║██║   ██║██╔══██╗██╔══╝   ██╔██╗ ",
    "     ██║  ██║╚██████╔╝██║  ██║███████╗██╔╝ ██╗",
    "     ╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝"};

  static constexpr int colors[] = {198, 199, 200, 201, 207, 213};  // ANSI color codes

  for (size_t i = 0; i < sizeof(banner) / sizeof(banner[0]); ++i) {
    const int color = colors[i % (sizeof(colors) / sizeof(colors[0]))];
    std::cout << "\033[1;38;5;" << color << "m" << banner[i] << "\033[0m"
              << std::endl;  // ANSI escape for bold colored text
    std::this_thread::sleep_for(std::chrono::milliseconds(35));  // Animation delay
  }

  const std::string name = "AUREX";
  std::cout << "      ";
  for (size_t i = 0; i < name.size(); ++i) {
    const int color = 198 + static_cast<int>(i) * 3;  // Gradient colors
    std::cout << "\033[1;38;5;" << color << "m" << name[i] << "\033[0m";
    if (i + 1 < name.size()) {
      std::cout << ' ';
    }
  }
  std::cout << std::endl;
  std::cout << "      \033[1;38;5;213mMatching Engine\033[0m" << std::endl;  // Colored subtitle
  std::cout << std::endl;
}

// Prints minimal telemetry info in production mode
// Shows endpoints for monitoring without verbose startup logs
void printMinimalInstructions(const std::string &server_address,
                              const std::string &tcp_address, uint16_t tcp_port,
                              const std::string &shm_name) {
  std::cout << "Minimal telemetry mode active" << std::endl;
  std::cout << "Endpoints: gRPC=" << server_address << " | TCP=" << tcp_address
            << ':' << tcp_port << " | SHM='" << shm_name << "'" << std::endl;
  std::cout << "Press 'E' for admin tasks. Launch with -dev for verbose logs."
            << std::endl;
  std::cout << std::endl;
}

} // namespace

// Admin mode functions - console-based interface for account/stock management
// These run when trading is suspended, allowing safe DB operations

// Displays menu with ASCII box drawing - better UX than plain text
void displayAdminMenu() {
  std::cout << "\n╔══════════════════════════════════════╗" << std::endl;
  std::cout << "║     ADMIN MODE - Account Management  ║" << std::endl;
  std::cout << "╠══════════════════════════════════════╣" << std::endl;
  std::cout << "║  1. View Account Balance             ║" << std::endl;
  std::cout << "║  2. Deposit Funds                    ║" << std::endl;
  std::cout << "║  3. Withdraw Funds                   ║" << std::endl;
  std::cout << "║  4. View All Accounts                ║" << std::endl;
  std::cout << "║  5. Create New Account               ║" << std::endl;
  std::cout << "║  6. List Stocks                      ║" << std::endl;
  std::cout << "║  7. Add New Stock                    ║" << std::endl;
  std::cout << "║  8. Update Stock Info                ║" << std::endl;
  std::cout << "║  9. Deactivate Stock                 ║" << std::endl;
  std::cout << "║  E. Return to Trading Mode           ║" << std::endl;
  std::cout << "║  Q. Quit Application                 ║" << std::endl;
  std::cout << "╚══════════════════════════════════════╝" << std::endl;
  std::cout << "Enter choice: ";
}

// Views account details using DatabaseManager
// Queries DB for user account, displays formatted info
// Why not simple cout? Formatted output with fixed precision for financial data
void viewAccountBalance(DatabaseManager *db) {
  std::string user_id;
  std::cout << "\nEnter User ID: ";
  std::cin >> user_id;  // Simple input - admin mode is interactive

  auto account = db->getUserAccount(user_id);  // DB query
  if (account.user_id.empty()) {  // Check if account exists
    std::cout << "❌ Account not found!" << std::endl;
    return;
  }

  std::cout << "\n╔═══════════════ Account Info ═══════════════╗" << std::endl;
  std::cout << "  User ID: " << account.user_id << std::endl;
  std::cout << "  Cash: $" << std::fixed << std::setprecision(2)  // Fixed 2 decimals for money
            << account.cashToDouble() << std::endl;  // Convert internal cash type to double
  std::cout << "  AAPL: " << account.aapl_qty << std::endl;  // Stock quantities
  std::cout << "  GOOGL: " << account.googl_qty << std::endl;
  std::cout << "  MSFT: " << account.msft_qty << std::endl;
  std::cout << "  AMZN: " << account.amzn_qty << std::endl;
  std::cout << "  TSLA: " << account.tsla_qty << std::endl;
  std::cout << "  Total Trades: " << account.total_trades << std::endl;
  std::cout << "  P&L: $" << account.realized_pnl / 100.0 << std::endl;  // P&L stored as cents
  std::cout << "  Status: " << (account.is_active ? "Active" : "Suspended")
            << std::endl;
  std::cout << "╚════════════════════════════════════════════╝" << std::endl;
}

// Deposits funds to account - updates cash and buying power
// Uses DatabaseManager::UserAccount types for precise financial math
// Why not simple double? Prevents floating-point precision errors in money calculations
void depositFunds(DatabaseManager *db) {
  std::string user_id;
  double amount;

  std::cout << "\nEnter User ID: ";
  std::cin >> user_id;

  auto account = db->getUserAccount(user_id);
  if (account.user_id.empty()) {
    std::cout << "❌ Account not found!" << std::endl;
    return;
  }

  std::cout << "Current Balance: $" << std::fixed << std::setprecision(2)
            << account.cashToDouble() << std::endl;
  std::cout << "Enter deposit amount: $";
  std::cin >> amount;

  if (amount <= 0) {  // Validation - no negative deposits
    std::cout << "❌ Invalid amount!" << std::endl;
    return;
  }

  CashAmount new_cash =  // Custom CashAmount type for exact cents
      account.cash + DatabaseManager::UserAccount::fromDouble(amount);  // Convert double to internal type
  account.cash = new_cash;
  account.buying_power = new_cash;  // Update buying power
  account.day_trading_buying_power = new_cash;  // Pattern day trading rules

  if (db->updateUserAccount(account)) {  // DB update
    std::cout << "✅ Deposit successful!" << std::endl;
    std::cout << "New Balance: $" << std::fixed << std::setprecision(2)
              << account.cashToDouble() << std::endl;
  } else {
    std::cout << "❌ Deposit failed!" << std::endl;
  }
}

// Withdraws funds - similar to deposit but checks balance
// std::min used for buying power to prevent negative values
void withdrawFunds(DatabaseManager *db) {
  std::string user_id;
  double amount;

  std::cout << "\nEnter User ID: ";
  std::cin >> user_id;

  auto account = db->getUserAccount(user_id);
  if (account.user_id.empty()) {
    std::cout << "❌ Account not found!" << std::endl;
    return;
  }

  std::cout << "Current Balance: $" << std::fixed << std::setprecision(2)
            << account.cashToDouble() << std::endl;
  std::cout << "Enter withdrawal amount: $";
  std::cin >> amount;

  if (amount <= 0) {
    std::cout << "❌ Invalid amount!" << std::endl;
    return;
  }

  CashAmount withdrawal = DatabaseManager::UserAccount::fromDouble(amount);
  if (withdrawal > account.cash) {  // Insufficient funds check
    std::cout << "❌ Insufficient funds!" << std::endl;
    return;
  }

  account.cash -= withdrawal;
  account.buying_power = std::min(account.buying_power, account.cash);  // Clamp to cash
  account.day_trading_buying_power =
      std::min(account.day_trading_buying_power, account.cash);

  if (db->updateUserAccount(account)) {
    std::cout << "✅ Withdrawal successful!" << std::endl;
    std::cout << "New Balance: $" << std::fixed << std::setprecision(2)
              << account.cashToDouble() << std::endl;
  } else {
    std::cout << "❌ Withdrawal failed!" << std::endl;
  }
}

// Placeholder for viewing all accounts - not implemented yet
void viewAllAccounts(DatabaseManager *db) {
  std::cout << "\n╔═══════════════ All Accounts ═══════════════╗" << std::endl;
  // Note: You'll need to add a getAllAccounts() method to DatabaseManager
  std::cout << "Feature coming soon - getAllAccounts() needs to be implemented"
            << std::endl;
  std::cout << "╚════════════════════════════════════════════╝" << std::endl;
}

// Creates new account with initial cash
// Validates input, uses custom CashAmount for precision
void createNewAccount(DatabaseManager *db) {
  std::string user_id;
  double initial_cash;

  std::cout << "\nEnter new User ID: ";
  std::cin >> user_id;

  std::cout << "Enter initial cash amount: $";
  std::cin >> initial_cash;

  if (initial_cash < 0) {  // No negative initial balance
    std::cout << "❌ Invalid amount!" << std::endl;
    return;
  }

  CashAmount cash = DatabaseManager::UserAccount::fromDouble(initial_cash);

  if (db->createUserAccount(user_id, cash)) {
    std::cout << "✅ Account created successfully!" << std::endl;
    std::cout << "User ID: " << user_id << std::endl;
    std::cout << "Initial Balance: $" << std::fixed << std::setprecision(2)
              << initial_cash << std::endl;
  } else {
    std::cout << "❌ Account creation failed! (User ID may already exist)"
              << std::endl;
  }
}

// Lists all stocks from database
// Iterates through vector, shows active/inactive status
void listAllStocks(DatabaseManager *db) {
  auto stocks = db->getAllStocks();  // Get all stock records

  std::cout << "\n╔══════════════════════════════════════════════╗"
            << std::endl;
  std::cout << "  Registered Stocks" << std::endl;
  std::cout << "╠══════════════════════════════════════════════╣" << std::endl;

  if (stocks.empty()) {
    std::cout << "  No stocks found in master list." << std::endl;
  } else {
    for (const auto &stock : stocks) {  // Range-based for loop - modern C++
      std::cout << "  Symbol:    " << stock.symbol;
      if (!stock.is_active) {
        std::cout << " (INACTIVE)";  // Show deactivated stocks
      }
      std::cout << "\n  Company:   " << stock.company_name
                << "\n  Sector:    " << stock.sector << "\n  Price:     $"
                << std::fixed << std::setprecision(2) << stock.initial_price
                << "\n  Market Cap:" << stock.market_cap
                << "\n  Listed:    " << stock.listing_date << "\n"
                << std::endl;
    }
  }

  std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
}

// Adds new stock to database
// Uses getline for string input after cin - handles spaces in names
void addNewStock(DatabaseManager *db) {
  std::string symbol;
  std::string company_name;
  std::string sector;
  double initial_price = 0.0;

  std::cout << "\nEnter stock symbol (e.g., AAPL): ";
  std::getline(std::cin, symbol);  // getline for full line input

  if (symbol.empty()) {
    std::cout << "❌ Symbol cannot be empty." << std::endl;
    return;
  }

  std::cout << "Enter company name: ";
  std::getline(std::cin, company_name);

  std::cout << "Enter sector: ";
  std::getline(std::cin, sector);

  std::cout << "Enter initial price: $";
  if (!(std::cin >> initial_price)) {  // Check cin success
    std::cin.clear();  // Clear error state
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');  // Ignore bad input
    std::cout << "❌ Invalid price input." << std::endl;
    return;
  }
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');  // Consume newline

  if (initial_price <= 0.0) {
    std::cout << "❌ Price must be greater than zero." << std::endl;
    return;
  }

  if (db->addStock(symbol, company_name, sector, initial_price)) {
    std::cout << "✅ Stock " << symbol << " registered successfully."
              << std::endl;
  } else {
    std::cout << "❌ Failed to register stock." << std::endl;
  }
}

// Updates stock info - similar to add but for existing stock
void updateStockInfo(DatabaseManager *db) {
  std::string symbol;
  std::string company_name;
  std::string sector;

  std::cout << "\nEnter stock symbol to update: ";
  std::getline(std::cin, symbol);

  if (symbol.empty()) {
    std::cout << "❌ Symbol cannot be empty." << std::endl;
    return;
  }

  std::cout << "Enter new company name: ";
  std::getline(std::cin, company_name);

  std::cout << "Enter new sector: ";
  std::getline(std::cin, sector);

  if (db->updateStock(symbol, company_name, sector)) {
    std::cout << "✅ Stock " << symbol << " updated." << std::endl;
  } else {
    std::cout << "❌ Failed to update stock " << symbol << '.' << std::endl;
  }
}

// Deactivates stock - sets inactive flag
void deactivateStock(DatabaseManager *db) {
  std::string symbol;

  std::cout << "\nEnter stock symbol to deactivate: ";
  std::getline(std::cin, symbol);

  if (symbol.empty()) {
    std::cout << "❌ Symbol cannot be empty." << std::endl;
    return;
  }

  if (db->removeStock(symbol)) {  // Note: removeStock likely sets inactive
    std::cout << "✅ Stock " << symbol << " set to inactive." << std::endl;
  } else {
    std::cout << "❌ Failed to deactivate stock " << symbol << '.' << std::endl;
  }
}

// Main admin mode loop - handles menu and switches to trading mode
// Uses switch-case for menu options - cleaner than if-else chain
// Atomic checks for shutdown to allow interruption
void runAdminMode(DatabaseManager *db) {
  std::cout << "\n🔒 Entering ADMIN MODE - Exchange is CLOSED" << std::endl;
  std::cout << "All trading is suspended. Account management enabled."
            << std::endl;

  bool exit_admin = false;
  while (!exit_admin && !shutdown_requested.load()) {  // Loop until exit or shutdown
    displayAdminMenu();

    char choice;
    std::cin >> choice;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');  // Consume newline

    switch (choice) {  // Switch on choice
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
    case '6':
      listAllStocks(db);
      break;
    case '7':
      addNewStock(db);
      break;
    case '8':
      updateStockInfo(db);
      break;
    case '9':
      deactivateStock(db);
      break;
    case 'E':
    case 'e':
      std::cout << "\n🔓 Exiting Admin Mode - Returning to Trading Mode..."
                << std::endl;
      exit_admin = true;
      break;
    case 'Q':
    case 'q':
      std::cout << "\n👋 Shutting down application..." << std::endl;
      shutdown_requested.store(true);  // Signal shutdown
      exit_admin = true;
      break;
    default:
      std::cout << "❌ Invalid choice!" << std::endl;
      break;
    }

    if (!exit_admin && !shutdown_requested.load()) {
      std::cout << "\nPress Enter to continue...";
      std::cin.get();  // Wait for enter
    }
  }
}

#ifdef _WIN32  // Windows-specific signal handling
#include <windows.h>  // Windows API for console control
BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType) {  // Callback for console events
  if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT ||
      dwCtrlType == CTRL_CLOSE_EVENT) {  // Handle common shutdown signals
    shutdown_requested.store(true);  // Set atomic flag
    return TRUE;  // Handled
  }
  return FALSE;  // Not handled
}
#else  // Unix signal handling
#include <signal.h>  // POSIX signals
void signalHandler(int signal) {  // Signal callback
  shutdown_requested.store(true);  // Set flag on SIGINT/SIGTERM
}
#endif

// Main entry point - orchestrates the entire stock exchange application
// Why int main(int argc, char *argv[])? Standard C++ signature for command-line args
int main(int argc, char *argv[]) {
  bool dev_mode = false;  // Flag for developer mode - affects logging and UI
  for (int i = 1; i < argc; ++i) {  // Parse command-line args
    std::string arg(argv[i]);
    if (arg == "-dev" || arg == "--dev") {  // Check for dev flag
      dev_mode = true;
    }
  }
  engine_config::setDevMode(dev_mode);  // Set global config

#ifdef _WIN32  // Windows signal setup
  if (!SetConsoleCtrlHandler(consoleCtrlHandler, TRUE)) {  // Register handler
    std::cerr << "Failed to set console control handler" << std::endl;
    return -1;  // Exit on failure
  }
#else  // Unix signal setup
  struct sigaction sa;  // Signal action struct
  sa.sa_handler = signalHandler;  // Set handler function
  sigemptyset(&sa.sa_mask);  // No blocked signals
  sa.sa_flags = 0;  // Default flags

  if (sigaction(SIGINT, &sa, nullptr) == -1) {  // Handle Ctrl+C
    std::cerr << "Failed to set SIGINT handler" << std::endl;
    return -1;
  }

  if (sigaction(SIGTERM, &sa, nullptr) == -1) {  // Handle termination
    std::cerr << "Failed to set SIGTERM handler" << std::endl;
    return -1;
  }
#endif

  // Load environment files for configuration
  loadEnvFile(".env");  // Current directory
  loadEnvFile("../.env");  // Parent directory

  // Get required database connection string from env
  std::string db_connection = getEnvOrDefault("AUREX_DB_DSN", "");
  if (db_connection.empty()) {  // Mandatory config
    std::cerr << "Missing database connection string. Set AUREX_DB_DSN to a "
                 "valid PostgreSQL DSN."
              << std::endl;
    return -1;
  }

  // Initialize gRPC service with DB connection
  GRPCServer service(db_connection);

  if (!service.initialize()) {  // Check init success
    std::cerr << "Failed to initialize gRPC service" << std::endl;
    return -1;
  }

  service.start();  // Start the exchange engine

  // Initialize authentication manager with Redis for session caching
  std::string redis_host = getEnvOrDefault("AUREX_REDIS_HOST", "localhost");
  int redis_port = 6379;  // Default Redis port
  if (const std::string redis_port_env =
          getEnvOrDefault("AUREX_REDIS_PORT", "");
      !redis_port_env.empty()) {
    try {
      redis_port = std::stoi(redis_port_env);  // Convert string to int
    } catch (const std::exception &) {
      std::cerr << "Invalid AUREX_REDIS_PORT value: '" << redis_port_env << "'"
                << std::endl;
      return -1;
    }
  }
  // std::make_unique for RAII - automatically manages lifetime
  auto auth_manager = std::make_unique<AuthenticationManager>(
      redis_host, redis_port, service.getExchange()->getDatabaseManager());

  if (!auth_manager->initialize()) {
    std::cerr << "Failed to initialize Authentication Manager" << std::endl;
    return -1;
  }

  ENGINE_LOG_DEV(std::cout << "Authentication Manager initialized successfully"
                           << std::endl;);  // Conditional logging macro

  service.getExchange()->setAuthenticationManager(auth_manager.get());  // Link auth to exchange

  // Initialize TCP server for binary order submission
  std::string tcp_address = getEnvOrDefault("AUREX_TCP_ADDRESS", "0.0.0.0");
  uint16_t tcp_port = 50052;  // Default TCP port
  if (const std::string tcp_port_env = getEnvOrDefault("AUREX_TCP_PORT", "");
      !tcp_port_env.empty()) {
    try {
      int parsed_port = std::stoi(tcp_port_env);
      if (parsed_port <= 0 || parsed_port > 65535) {  // Validate port range
        throw std::out_of_range("port range");
      }
      tcp_port = static_cast<uint16_t>(parsed_port);
    } catch (const std::exception &) {
      std::cerr << "Invalid AUREX_TCP_PORT value: '" << tcp_port_env << "'"
                << std::endl;
      return -1;
    }
  }
  TCPServer tcp_server(tcp_address, tcp_port, service.getExchange(),
                       auth_manager.get());  // Pass exchange and auth

  // Initialize shared memory server for ultra-low latency
  std::string shm_name =
      getEnvOrDefault("AUREX_SHM_NAME", "stock_exchange_orders");
  SharedMemoryOrderServer shm_server(shm_name, service.getExchange(),
                                     auth_manager.get());

  // Load TLS certificates for secure gRPC
  const std::string grpc_cert_path =
      getEnvOrDefault("AUREX_GRPC_CERT_PATH", "server.crt");
  const std::string grpc_key_path =
      getEnvOrDefault("AUREX_GRPC_KEY_PATH", "server.key");

  std::string grpc_certificate;
  std::string grpc_private_key;
  try {
    grpc_certificate = readFileContents(grpc_cert_path);  // Read cert file
    grpc_private_key = readFileContents(grpc_key_path);  // Read key file
  } catch (const std::exception &ex) {
    std::cerr << "Failed to load gRPC TLS materials: " << ex.what()
              << std::endl;
    return -1;
  }

  // Configure gRPC SSL credentials
  grpc::SslServerCredentialsOptions ssl_options;
  ssl_options.pem_key_cert_pairs.push_back(  // Add cert-key pair
      {grpc_private_key, grpc_certificate});
  auto grpc_credentials = grpc::SslServerCredentials(ssl_options);  // Create SSL creds

  // Build gRPC server
  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc_credentials);  // Bind to address with SSL
  builder.RegisterService(&service);  // Register our service
  builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);  // 4MB max receive - for large messages
  builder.SetMaxSendMessageSize(4 * 1024 * 1024);  // 4MB max send

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());  // Build and start server
  if (!server) {
    std::cerr << "Failed to start gRPC server" << std::endl;
    return -1;
  }

  auto symbols = service.getExchange()->getSymbols();  // Get list of traded symbols

  if (dev_mode) {  // Verbose startup info in dev mode
    std::cout << "\n=== Stock Exchange Server ===" << std::endl;
    std::cout << "Developer mode enabled (-dev). Verbose logging active."
              << std::endl;
    std::cout << "gRPC Server listening on " << server_address << std::endl;
    std::cout << "gRPC TLS certificate: " << grpc_cert_path << std::endl;
    std::cout << "TCP Order Server listening on " << tcp_address << ":"
              << tcp_port << std::endl;
    std::cout << "Shared Memory Server available at '" << shm_name << "'"
              << std::endl;
    std::cout << "Features:" << std::endl;
    std::cout << "  • High-performance TCP binary protocol for order submission"
              << std::endl;
    std::cout << "  • Ultra-low latency shared memory for local clients"
              << std::endl;
    std::cout << "  • gRPC streaming for market data, UI, and demo purposes"
              << std::endl;
    std::ostringstream symbol_stream;  // Build symbol list string
    for (size_t i = 0; i < symbols.size(); ++i) {
      symbol_stream << symbols[i];
      if (i + 1 < symbols.size()) {
        symbol_stream << ", ";
      }
    }
    std::cout << "  • " << symbols.size() << " Stock symbols ("
              << symbol_stream.str() << ")" << std::endl;
    std::cout << "  • Individual threads per stock" << std::endl;
    std::cout << "  • Real-time streaming market data" << std::endl;
    std::cout << "  • Market Index (TECH500) - like S&P 500/Sensex"
              << std::endl;
    std::cout << "  • Live streaming of all stock prices" << std::endl;
    std::cout << "  • Top 5 index streaming" << std::endl;
    std::cout << "  • PostgreSQL integration (30sec sync)" << std::endl;
    std::cout << "  • Order matching engine" << std::endl;
  }

  tcp_server.start();  // Start TCP server

  if (!shm_server.start()) {  // Start shared memory server
    std::cerr << "Warning: Failed to start shared memory server" << std::endl;
  }

  // Account sync thread - periodically syncs Redis accounts to PostgreSQL
  // std::atomic<bool> for running flag - thread-safe control
  // std::condition_variable for efficient waiting with timeout
  std::atomic<bool> sync_running(true);
  std::mutex sync_mutex;  // Protects condition variable
  std::condition_variable sync_cv;  // For timed waits
  std::thread account_sync_thread([&auth_manager, &sync_running, &sync_mutex,
                                   &sync_cv]() {  // Lambda for thread function
    std::unique_lock<std::mutex> lock(sync_mutex);  // Lock for condition variable
    while (sync_running.load(std::memory_order_relaxed)) {  // Check running flag
      const bool exit_requested =
          sync_cv.wait_for(lock, std::chrono::seconds(30), [&sync_running]() {  // Wait 30s or until notified
            return !sync_running.load(std::memory_order_relaxed);
          });

      if (exit_requested) {  // Shutdown requested
        break;
      }

      lock.unlock();  // Unlock during work
      auth_manager->syncAllAccountsToDatabase();  // Sync accounts
      lock.lock();  // Re-lock for next iteration
    }
  });

  ENGINE_LOG_DEV(  // Conditional logging
      std::cout << "💾 Account balance sync: Every 30 seconds to database"
                << std::endl;);

  // Telemetry thread - displays real-time performance stats
  std::thread telemetry_thread;
  bool telemetry_thread_started = false;

  if (!dev_mode) {  // Only in production mode
    telemetry_display_paused.store(true, std::memory_order_relaxed);  // Start paused

    telemetry_thread = std::thread([]() {  // Lambda for telemetry display
      auto &telemetry = EngineTelemetry::instance();  // Singleton instance
      telemetry.snapshot();  // Initial snapshot
      size_t previous_length = 0;  // For clearing previous output

      while (!shutdown_requested.load(std::memory_order_relaxed)) {
        if (telemetry_display_paused.load(std::memory_order_relaxed)) {  // Paused during admin
          if (previous_length != 0) {
            std::cout << "\r" << std::string(previous_length, ' ') << "\r"  // Clear line
                      << std::flush;
            previous_length = 0;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(150));  // Sleep when paused
          continue;
        }

        auto snapshot = telemetry.snapshot();  // Get current stats
        std::string line = "Orders " + std::to_string(snapshot.totalOrders);  // Build status line
        const double ops = snapshot.ordersPerSecond;
        if (ops < 0.05) {
          line += " (0/s)";
        } else {
          const int precision = ops >= 10.0 ? 0 : 1;
          line += " (" + formatDouble(ops, precision) + "/s)";
        }
        line += " | Avg Lat " + formatLatency(snapshot.averageLatencyUs);
        line += " | CPU " +
                formatDouble(std::max(0.0, snapshot.cpuPercent), 1) + "%";  // Clamp CPU
        line += " | Mem " + formatDouble(snapshot.memoryMb, 1) + " MB";

        const size_t line_length = line.size();
        std::cout << "\r" << line;  // Overwrite line
        if (line_length < previous_length) {
          std::cout << std::string(previous_length - line_length, ' ');  // Pad if shorter
        }
        std::cout << std::flush;
        previous_length = line_length;

        for (int i = 0; i < 10; ++i) {  // Update every 100ms for 1 second
          if (shutdown_requested.load(std::memory_order_relaxed) ||
              telemetry_display_paused.load(std::memory_order_relaxed)) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }

      std::cout << "\r" << std::string(previous_length, ' ') << "\r"  // Final clear
                << std::flush;
    });
    telemetry_thread_started = true;
  }

    renderAurexBanner();
    printMinimalInstructions(server_address, tcp_address, tcp_port, shm_name);
    telemetry_display_paused.store(false, std::memory_order_relaxed);
  if (!dev_mode) {  // Production mode UI
    renderAurexBanner();  // Animated banner
    printMinimalInstructions(server_address, tcp_address, tcp_port, shm_name);  // Endpoint info
    telemetry_display_paused.store(false, std::memory_order_relaxed);  // Start telemetry
  } else {  // Dev mode UI
    std::cout << "\n📊 TRADING MODE - Exchange is OPEN" << std::endl;
    std::cout << "Press 'E' to enter ADMIN MODE (close exchange for account "
                 "management)"
              << std::endl;
    std::cout << "Press 'E' twice to EXIT application" << std::endl;
    std::cout << "=============================" << std::endl;
  }

  // Main event loop for keyboard input and mode switching
  bool in_trading_mode = true;  // Current mode flag
  bool e_pressed_once = false;  // For double-E detection
  auto last_e_press = std::chrono::steady_clock::now();  // Timestamp for double press

  while (!shutdown_requested.load(std::memory_order_relaxed)) {  // Main loop
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Poll interval

    if (kbhit()) {  // Check for key press
      char ch = getchar();  // Get the key
      if (ch == 'E' || ch == 'e') {
        auto now = std::chrono::steady_clock::now();
        auto time_since_last_press =
            std::chrono::duration_cast<std::chrono::seconds>(now - last_e_press)
                .count();

        if (e_pressed_once && time_since_last_press < 3) {  // Double E within 3s
          telemetry_display_paused.store(true, std::memory_order_relaxed);
          std::cout << std::endl;
          std::cout << "\n👋 Double E detected - Shutting down application..."
                    << std::endl;
          shutdown_requested.store(true, std::memory_order_relaxed);
          break;
        } else {
          e_pressed_once = true;
          last_e_press = now;

          if (in_trading_mode) {  // Enter admin mode
            telemetry_display_paused.store(true, std::memory_order_relaxed);
            std::cout << std::endl;
            std::cout << "\n🔒 Stopping exchange for ADMIN MODE..."
                      << std::endl;

            std::cout << "  Stopping TCP server..." << std::flush;
            tcp_server.stop();
            std::cout << " ✓" << std::endl;

            std::cout << "  Stopping shared memory server..." << std::flush;
            shm_server.stop();
            std::cout << " ✓" << std::endl;

            std::cout
                << "  Stopping exchange engine (5 stocks, index, db sync)..."
                << std::flush;
            service.stop();  // Stop trading engine
            std::cout << " ✓" << std::endl;

            std::cout << "✅ Exchange stopped. All trading suspended."
                      << std::endl;

            in_trading_mode = false;

            runAdminMode(service.getExchange()->getDatabaseManager());  // Run admin menu

            if (!shutdown_requested.load(std::memory_order_relaxed)) {
              std::cout << "\n🔓 Restarting exchange for TRADING MODE..."
                        << std::endl;
              auth_manager->clearCachedAccounts();  // Clear Redis cache
              service.start();  // Restart engine
              tcp_server.start();
              if (!shm_server.start()) {
                std::cerr << "Warning: Failed to restart shared memory server"
                          << std::endl;
              }
              std::cout << "✅ Exchange restarted. Trading resumed."
                        << std::endl;
              if (dev_mode) {
                std::cout << "\n📊 TRADING MODE - Exchange is OPEN"
                          << std::endl;
                std::cout << "Press 'E' to enter ADMIN MODE" << std::endl;
                std::cout << "Press 'E' twice to EXIT application" << std::endl;
              } else {
                std::cout << "\nTrading mode resumed." << std::endl;
                telemetry_display_paused.store(false,
                                               std::memory_order_relaxed);
              }
              in_trading_mode = true;
            }

            e_pressed_once = false;

            if (in_trading_mode && !dev_mode) {
              telemetry_display_paused.store(false, std::memory_order_relaxed);
            }
          }
        }
      }
    }

    // Reset double-press flag after 3 seconds
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_press =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_e_press)
            .count();
    if (e_pressed_once && time_since_last_press >= 3) {
      e_pressed_once = false;
    }
  }

  // Graceful shutdown sequence - ensures all resources are cleaned up properly
  telemetry_display_paused.store(true, std::memory_order_relaxed);  // Stop telemetry display
  std::cout << "\nShutdown signal received. Gracefully shutting down..."
            << std::endl;

  // Stop account sync thread - uses atomic and condition variable for clean shutdown
  std::cout << "Stopping account sync thread..." << std::endl;
  sync_running.store(false, std::memory_order_relaxed);  // Signal thread to exit loop
  sync_cv.notify_all();  // Wake up the waiting thread
  if (account_sync_thread.joinable()) {
    account_sync_thread.join();  // Wait for thread completion - prevents resource leaks
  }
  std::cout << "Account sync thread stopped" << std::endl;

  // Final database sync - ensures no data loss
  std::cout << "Performing final account sync to database..." << std::endl;
  auth_manager->syncAllAccountsToDatabase();  // Flush Redis cache to PostgreSQL
  std::cout << "Final sync complete" << std::endl;

  // Stop servers only if still in trading mode
  if (in_trading_mode) {
    std::cout << "Stopping TCP server..." << std::endl;
    tcp_server.stop();  // Graceful TCP shutdown
    std::cout << "TCP server stopped" << std::endl;

    shm_server.stop();  // Shared memory cleanup
    std::cout << "Shared memory server stopped" << std::endl;

    service.stop();  // Stop exchange engine
    std::cout << "gRPC service stopped" << std::endl;
  }

  server->Shutdown();  // Stop gRPC server - blocks until all RPCs complete

  std::cout << "Stock Exchange Server shut down successfully" << std::endl;

  // Ensure telemetry thread exits
  if (telemetry_thread_started && telemetry_thread.joinable()) {
    shutdown_requested.store(true, std::memory_order_relaxed);  // Extra signal if needed
    telemetry_thread.join();  // Wait for telemetry thread
  }

  return 0;  // Successful exit - indicates clean shutdown
}

/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#include "api/AuthenticationManager.h"
#include "api/GRPCServer.h"
#include "api/SharedMemoryQueue.h"
#include "api/TCPServer.h"
#include "common/EngineConfig.h"
#include "common/EngineLogging.h"
#include "common/EngineTelemetry.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

std::atomic<bool> shutdown_requested(false);
std::atomic<bool> mode_switch_requested(false);
std::atomic<bool> telemetry_display_paused(false);

namespace {

bool stdin_is_tty() {
  static const bool is_tty = (isatty(STDIN_FILENO) == 1);
  return is_tty;
}

std::string getEnvOrDefault(const char *name, const std::string &fallback) {
  if (!name) {
    return fallback;
  }
  const char *value = std::getenv(name);
  if (value && *value != '\0') {
    return value;
  }
  return fallback;
}

std::string readFileContents(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Failed to open file: " + path);
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void loadEnvFile(const std::string &path) {
  std::ifstream file(path);
  if (!file)
    return;

  std::string line;
  while (std::getline(file, line)) {
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
    setenv(key.c_str(), value.c_str(), 0);
  }
}

} // namespace

// Non-blocking keyboard input detection
int kbhit() {
  if (!stdin_is_tty()) {
    return 0;
  }

  struct termios oldt;
  if (tcgetattr(STDIN_FILENO, &oldt) == -1) {
    if (errno != ENOTTY) {
      std::perror("tcgetattr");
    }
    return 0;
  }

  struct termios newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == -1) {
    std::perror("tcsetattr");
    return 0;
  }

  int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (oldf == -1) {
    std::perror("fcntl(F_GETFL)");
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
  }

  if (fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK) == -1) {
    std::perror("fcntl(F_SETFL)");
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
  }

  int ch = getchar();

  // Always attempt to restore state
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  if (ch != EOF) {
    ungetc(ch, stdin);
    return 1;
  }

  return 0;
}

namespace {

std::string formatDouble(double value, int precision) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << value;
  return oss.str();
}

std::string formatLatency(double microseconds) {
  if (microseconds <= 0.0) {
    return "n/a";
  }

  if (microseconds >= 1000.0) {
    const double ms = microseconds / 1000.0;
    const int precision = ms >= 10.0 ? 1 : 2;
    return formatDouble(ms, precision) + " ms";
  }

  const int precision = microseconds >= 100.0 ? 1 : 2;
  return formatDouble(microseconds, precision) + " us";
}

void renderAurexBanner() {
  static constexpr const char *banner[] = {
    "      █████╗ ██╗   ██╗██████╗ ███████╗██╗  ██╗",
    "     ██╔══██╗██║   ██║██╔══██╗██╔════╝╚██╗██╔╝",
    "     ███████║██║   ██║██████╔╝█████╗   ╚███╔╝ ",
    "     ██╔══██║██║   ██║██╔══██╗██╔══╝   ██╔██╗ ",
    "     ██║  ██║╚██████╔╝██║  ██║███████╗██╔╝ ██╗",
    "     ╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝"};

  static constexpr int colors[] = {198, 199, 200, 201, 207, 213};

  for (size_t i = 0; i < sizeof(banner) / sizeof(banner[0]); ++i) {
    const int color = colors[i % (sizeof(colors) / sizeof(colors[0]))];
    std::cout << "\033[1;38;5;" << color << "m" << banner[i] << "\033[0m"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
  }

  const std::string name = "AUREX";
  std::cout << "      ";
  for (size_t i = 0; i < name.size(); ++i) {
    const int color = 198 + static_cast<int>(i) * 3;
    std::cout << "\033[1;38;5;" << color << "m" << name[i] << "\033[0m";
    if (i + 1 < name.size()) {
      std::cout << ' ';
    }
  }
  std::cout << std::endl;
  std::cout << "      \033[1;38;5;213mMatching Engine\033[0m" << std::endl;
  std::cout << std::endl;
}

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
  std::cout << "║  6. List Stocks                      ║" << std::endl;
  std::cout << "║  7. Add New Stock                    ║" << std::endl;
  std::cout << "║  8. Update Stock Info                ║" << std::endl;
  std::cout << "║  9. Deactivate Stock                 ║" << std::endl;
  std::cout << "║  E. Return to Trading Mode           ║" << std::endl;
  std::cout << "║  Q. Quit Application                 ║" << std::endl;
  std::cout << "╚══════════════════════════════════════╝" << std::endl;
  std::cout << "Enter choice: ";
}

void viewAccountBalance(DatabaseManager *db) {
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
  std::cout << "  Cash: $" << std::fixed << std::setprecision(2)
            << account.cashToDouble() << std::endl;
  std::cout << "  AAPL: " << account.aapl_qty << std::endl;
  std::cout << "  GOOGL: " << account.googl_qty << std::endl;
  std::cout << "  MSFT: " << account.msft_qty << std::endl;
  std::cout << "  AMZN: " << account.amzn_qty << std::endl;
  std::cout << "  TSLA: " << account.tsla_qty << std::endl;
  std::cout << "  Total Trades: " << account.total_trades << std::endl;
  std::cout << "  P&L: $" << account.realized_pnl / 100.0 << std::endl;
  std::cout << "  Status: " << (account.is_active ? "Active" : "Suspended")
            << std::endl;
  std::cout << "╚════════════════════════════════════════════╝" << std::endl;
}

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

  if (amount <= 0) {
    std::cout << "❌ Invalid amount!" << std::endl;
    return;
  }

  CashAmount new_cash =
      account.cash + DatabaseManager::UserAccount::fromDouble(amount);
  account.cash = new_cash;
  account.buying_power = new_cash;
  account.day_trading_buying_power = new_cash;

  if (db->updateUserAccount(account)) {
    std::cout << "✅ Deposit successful!" << std::endl;
    std::cout << "New Balance: $" << std::fixed << std::setprecision(2)
              << account.cashToDouble() << std::endl;
  } else {
    std::cout << "❌ Deposit failed!" << std::endl;
  }
}

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
  if (withdrawal > account.cash) {
    std::cout << "❌ Insufficient funds!" << std::endl;
    return;
  }

  account.cash -= withdrawal;
  account.buying_power = std::min(account.buying_power, account.cash);
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

void viewAllAccounts(DatabaseManager *db) {
  std::cout << "\n╔═══════════════ All Accounts ═══════════════╗" << std::endl;
  // Note: You'll need to add a getAllAccounts() method to DatabaseManager
  std::cout << "Feature coming soon - getAllAccounts() needs to be implemented"
            << std::endl;
  std::cout << "╚════════════════════════════════════════════╝" << std::endl;
}

void createNewAccount(DatabaseManager *db) {
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
    std::cout << "Initial Balance: $" << std::fixed << std::setprecision(2)
              << initial_cash << std::endl;
  } else {
    std::cout << "❌ Account creation failed! (User ID may already exist)"
              << std::endl;
  }
}

void listAllStocks(DatabaseManager *db) {
  auto stocks = db->getAllStocks();

  std::cout << "\n╔══════════════════════════════════════════════╗"
            << std::endl;
  std::cout << "  Registered Stocks" << std::endl;
  std::cout << "╠══════════════════════════════════════════════╣" << std::endl;

  if (stocks.empty()) {
    std::cout << "  No stocks found in master list." << std::endl;
  } else {
    for (const auto &stock : stocks) {
      std::cout << "  Symbol:    " << stock.symbol;
      if (!stock.is_active) {
        std::cout << " (INACTIVE)";
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

void addNewStock(DatabaseManager *db) {
  std::string symbol;
  std::string company_name;
  std::string sector;
  double initial_price = 0.0;

  std::cout << "\nEnter stock symbol (e.g., AAPL): ";
  std::getline(std::cin, symbol);

  if (symbol.empty()) {
    std::cout << "❌ Symbol cannot be empty." << std::endl;
    return;
  }

  std::cout << "Enter company name: ";
  std::getline(std::cin, company_name);

  std::cout << "Enter sector: ";
  std::getline(std::cin, sector);

  std::cout << "Enter initial price: $";
  if (!(std::cin >> initial_price)) {
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cout << "❌ Invalid price input." << std::endl;
    return;
  }
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

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

void deactivateStock(DatabaseManager *db) {
  std::string symbol;

  std::cout << "\nEnter stock symbol to deactivate: ";
  std::getline(std::cin, symbol);

  if (symbol.empty()) {
    std::cout << "❌ Symbol cannot be empty." << std::endl;
    return;
  }

  if (db->removeStock(symbol)) {
    std::cout << "✅ Stock " << symbol << " set to inactive." << std::endl;
  } else {
    std::cout << "❌ Failed to deactivate stock " << symbol << '.' << std::endl;
  }
}

void runAdminMode(DatabaseManager *db) {
  std::cout << "\n🔒 Entering ADMIN MODE - Exchange is CLOSED" << std::endl;
  std::cout << "All trading is suspended. Account management enabled."
            << std::endl;

  bool exit_admin = false;
  while (!exit_admin && !shutdown_requested.load()) {
    displayAdminMenu();

    char choice;
    std::cin >> choice;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    switch (choice) {
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
  if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT ||
      dwCtrlType == CTRL_CLOSE_EVENT) {
    shutdown_requested.store(true);
    return TRUE;
  }
  return FALSE;
}
#else
#include <signal.h>
void signalHandler(int signal) { shutdown_requested.store(true); }
#endif

int main(int argc, char *argv[]) {
  bool dev_mode = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "-dev" || arg == "--dev") {
      dev_mode = true;
    }
  }
  engine_config::setDevMode(dev_mode);

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

  // Try to load .env from current directory or parent directory
  loadEnvFile(".env");
  loadEnvFile("../.env");

  std::string server_address =
      getEnvOrDefault("AUREX_GRPC_ADDRESS", "0.0.0.0:50051");
  std::string db_connection = getEnvOrDefault("AUREX_DB_DSN", "");
  if (db_connection.empty()) {
    std::cerr << "Missing database connection string. Set AUREX_DB_DSN to a "
                 "valid PostgreSQL DSN."
              << std::endl;
    return -1;
  }

  GRPCServer service(db_connection);

  if (!service.initialize()) {
    std::cerr << "Failed to initialize gRPC service" << std::endl;
    return -1;
  }

  service.start();

  std::string redis_host = getEnvOrDefault("AUREX_REDIS_HOST", "localhost");
  int redis_port = 6379;
  if (const std::string redis_port_env =
          getEnvOrDefault("AUREX_REDIS_PORT", "");
      !redis_port_env.empty()) {
    try {
      redis_port = std::stoi(redis_port_env);
    } catch (const std::exception &) {
      std::cerr << "Invalid AUREX_REDIS_PORT value: '" << redis_port_env << "'"
                << std::endl;
      return -1;
    }
  }
  auto auth_manager = std::make_unique<AuthenticationManager>(
      redis_host, redis_port, service.getExchange()->getDatabaseManager());

  if (!auth_manager->initialize()) {
    std::cerr << "Failed to initialize Authentication Manager" << std::endl;
    return -1;
  }

  ENGINE_LOG_DEV(std::cout << "Authentication Manager initialized successfully"
                           << std::endl;);

  service.getExchange()->setAuthenticationManager(auth_manager.get());

  std::string tcp_address = getEnvOrDefault("AUREX_TCP_ADDRESS", "0.0.0.0");
  uint16_t tcp_port = 50052;
  if (const std::string tcp_port_env = getEnvOrDefault("AUREX_TCP_PORT", "");
      !tcp_port_env.empty()) {
    try {
      int parsed_port = std::stoi(tcp_port_env);
      if (parsed_port <= 0 || parsed_port > 65535) {
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
                       auth_manager.get());

  std::string shm_name =
      getEnvOrDefault("AUREX_SHM_NAME", "stock_exchange_orders");
  SharedMemoryOrderServer shm_server(shm_name, service.getExchange(),
                                     auth_manager.get());

  const std::string grpc_cert_path =
      getEnvOrDefault("AUREX_GRPC_CERT_PATH", "server.crt");
  const std::string grpc_key_path =
      getEnvOrDefault("AUREX_GRPC_KEY_PATH", "server.key");

  std::string grpc_certificate;
  std::string grpc_private_key;
  try {
    grpc_certificate = readFileContents(grpc_cert_path);
    grpc_private_key = readFileContents(grpc_key_path);
  } catch (const std::exception &ex) {
    std::cerr << "Failed to load gRPC TLS materials: " << ex.what()
              << std::endl;
    return -1;
  }

  grpc::SslServerCredentialsOptions ssl_options;
  ssl_options.pem_key_cert_pairs.push_back(
      {grpc_private_key, grpc_certificate});
  auto grpc_credentials = grpc::SslServerCredentials(ssl_options);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc_credentials);
  builder.RegisterService(&service);
  builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);
  builder.SetMaxSendMessageSize(4 * 1024 * 1024);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "Failed to start gRPC server" << std::endl;
    return -1;
  }

  auto symbols = service.getExchange()->getSymbols();

  if (dev_mode) {
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
    std::ostringstream symbol_stream;
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

  tcp_server.start();

  if (!shm_server.start()) {
    std::cerr << "Warning: Failed to start shared memory server" << std::endl;
  }

  std::atomic<bool> sync_running(true);
  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  std::thread account_sync_thread([&auth_manager, &sync_running, &sync_mutex,
                                   &sync_cv]() {
    std::unique_lock<std::mutex> lock(sync_mutex);
    while (sync_running.load(std::memory_order_relaxed)) {
      const bool exit_requested =
          sync_cv.wait_for(lock, std::chrono::seconds(30), [&sync_running]() {
            return !sync_running.load(std::memory_order_relaxed);
          });

      if (exit_requested) {
        break;
      }

      lock.unlock();
      auth_manager->syncAllAccountsToDatabase();
      lock.lock();
    }
  });

  ENGINE_LOG_DEV(
      std::cout << "💾 Account balance sync: Every 30 seconds to database"
                << std::endl;);

  std::thread telemetry_thread;
  bool telemetry_thread_started = false;

  if (!dev_mode) {
    telemetry_display_paused.store(true, std::memory_order_relaxed);

    telemetry_thread = std::thread([]() {
      auto &telemetry = EngineTelemetry::instance();
      telemetry.snapshot();
      size_t previous_length = 0;

      while (!shutdown_requested.load(std::memory_order_relaxed)) {
        if (telemetry_display_paused.load(std::memory_order_relaxed)) {
          if (previous_length != 0) {
            std::cout << "\r" << std::string(previous_length, ' ') << "\r"
                      << std::flush;
            previous_length = 0;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
          continue;
        }

        auto snapshot = telemetry.snapshot();
        std::string line = "Orders " + std::to_string(snapshot.totalOrders);
        const double ops = snapshot.ordersPerSecond;
        if (ops < 0.05) {
          line += " (0/s)";
        } else {
          const int precision = ops >= 10.0 ? 0 : 1;
          line += " (" + formatDouble(ops, precision) + "/s)";
        }
        line += " | Avg Lat " + formatLatency(snapshot.averageLatencyUs);
        line += " | CPU " +
                formatDouble(std::max(0.0, snapshot.cpuPercent), 1) + "%";
        line += " | Mem " + formatDouble(snapshot.memoryMb, 1) + " MB";

        const size_t line_length = line.size();
        std::cout << "\r" << line;
        if (line_length < previous_length) {
          std::cout << std::string(previous_length - line_length, ' ');
        }
        std::cout << std::flush;
        previous_length = line_length;

        for (int i = 0; i < 10; ++i) {
          if (shutdown_requested.load(std::memory_order_relaxed) ||
              telemetry_display_paused.load(std::memory_order_relaxed)) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }

      std::cout << "\r" << std::string(previous_length, ' ') << "\r"
                << std::flush;
    });
    telemetry_thread_started = true;

    renderAurexBanner();
    printMinimalInstructions(server_address, tcp_address, tcp_port, shm_name);
    telemetry_display_paused.store(false, std::memory_order_relaxed);
  } else {
    std::cout << "\n📊 TRADING MODE - Exchange is OPEN" << std::endl;
    std::cout << "Press 'E' to enter ADMIN MODE (close exchange for account "
                 "management)"
              << std::endl;
    std::cout << "Press 'E' twice to EXIT application" << std::endl;
    std::cout << "=============================" << std::endl;
  }

  bool in_trading_mode = true;
  bool e_pressed_once = false;
  auto last_e_press = std::chrono::steady_clock::now();

  while (!shutdown_requested.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (kbhit()) {
      char ch = getchar();
      if (ch == 'E' || ch == 'e') {
        auto now = std::chrono::steady_clock::now();
        auto time_since_last_press =
            std::chrono::duration_cast<std::chrono::seconds>(now - last_e_press)
                .count();

        if (e_pressed_once && time_since_last_press < 3) {
          telemetry_display_paused.store(true, std::memory_order_relaxed);
          std::cout << std::endl;
          std::cout << "\n👋 Double E detected - Shutting down application..."
                    << std::endl;
          shutdown_requested.store(true, std::memory_order_relaxed);
          break;
        } else {
          e_pressed_once = true;
          last_e_press = now;

          if (in_trading_mode) {
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
            service.stop();
            std::cout << " ✓" << std::endl;

            std::cout << "✅ Exchange stopped. All trading suspended."
                      << std::endl;

            in_trading_mode = false;

            runAdminMode(service.getExchange()->getDatabaseManager());

            if (!shutdown_requested.load(std::memory_order_relaxed)) {
              std::cout << "\n🔓 Restarting exchange for TRADING MODE..."
                        << std::endl;
              auth_manager->clearCachedAccounts();
              service.start();
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

    auto now = std::chrono::steady_clock::now();
    auto time_since_last_press =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_e_press)
            .count();
    if (e_pressed_once && time_since_last_press >= 3) {
      e_pressed_once = false;
    }
  }

  telemetry_display_paused.store(true, std::memory_order_relaxed);
  std::cout << "\nShutdown signal received. Gracefully shutting down..."
            << std::endl;

  std::cout << "Stopping account sync thread..." << std::endl;
  sync_running.store(false, std::memory_order_relaxed);
  sync_cv.notify_all();
  if (account_sync_thread.joinable()) {
    account_sync_thread.join();
  }
  std::cout << "Account sync thread stopped" << std::endl;

  std::cout << "Performing final account sync to database..." << std::endl;
  auth_manager->syncAllAccountsToDatabase();
  std::cout << "Final sync complete" << std::endl;

  if (in_trading_mode) {
    std::cout << "Stopping TCP server..." << std::endl;
    tcp_server.stop();
    std::cout << "TCP server stopped" << std::endl;

    shm_server.stop();
    std::cout << "Shared memory server stopped" << std::endl;

    service.stop();
    std::cout << "gRPC service stopped" << std::endl;
  }

  server->Shutdown();

  std::cout << "Stock Exchange Server shut down successfully" << std::endl;

  if (telemetry_thread_started && telemetry_thread.joinable()) {
    shutdown_requested.store(true, std::memory_order_relaxed);
    telemetry_thread.join();
  }

  return 0;
}

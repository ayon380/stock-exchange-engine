#include "api/GRPCServer.h"
#include "api/TCPServer.h"
#include "api/SharedMemoryQueue.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

std::atomic<bool> shutdown_requested(false);

void signalHandler(int signal) {
    std::cout << "\nShutdown signal received (" << signal << "). Gracefully shutting down..." << std::endl;
    shutdown_requested.store(true);
}

int main() {
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
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
    
    // Initialize TCP server for high-performance order submission
    std::string tcp_address("0.0.0.0");
    uint16_t tcp_port = 50052; // Different port from gRPC
    TCPServer tcp_server(tcp_address, tcp_port, service.getExchange());
    
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
    std::cout << "  • 8 Stock symbols (AAPL, GOOGL, MSFT, TSLA, AMZN, META, NVDA, NFLX)" << std::endl;
    std::cout << "  • Individual threads per stock" << std::endl;
    std::cout << "  • Real-time streaming market data" << std::endl;
    std::cout << "  • Market Index (TECH500) - like S&P 500/Sensex" << std::endl;
    std::cout << "  • Live streaming of all stock prices" << std::endl;
    std::cout << "  • Top 5 index streaming" << std::endl;
    std::cout << "  • PostgreSQL integration (30sec sync)" << std::endl;
    std::cout << "  • Order matching engine" << std::endl;
    std::cout << "Press Ctrl+C to shutdown gracefully" << std::endl;
    std::cout << "=============================" << std::endl;
    
    // Start TCP server
    tcp_server.start();
    
    // Start shared memory server
    if (!shm_server.start()) {
        std::cerr << "Warning: Failed to start shared memory server" << std::endl;
    }
    
    // Wait for shutdown signal instead of blocking indefinitely
    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "Initiating graceful shutdown..." << std::endl;
    
    // Stop TCP server first
    tcp_server.stop();
    
    // Stop shared memory server
    shm_server.stop();
    
    // Stop the service first
    service.stop();
    
    // Then shutdown the gRPC server
    server->Shutdown();
    
    std::cout << "Stock Exchange Server shut down successfully" << std::endl;
    return 0;
}

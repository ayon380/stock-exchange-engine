#include "api/GRPCServer.h"
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
    
    // Set up gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    
    // Set max message sizes for streaming
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024); // 4MB
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);    // 4MB
    
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "\n=== Stock Exchange Server ===" << std::endl;
    std::cout << "Server listening on " << server_address << std::endl;
    std::cout << "Features:" << std::endl;
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
    
    // Wait for shutdown signal instead of blocking indefinitely
    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "Initiating graceful shutdown..." << std::endl;
    
    // Stop the service first
    service.stop();
    
    // Then shutdown the gRPC server
    server->Shutdown();
    
    std::cout << "Stock Exchange Server shut down successfully" << std::endl;
    return 0;
}

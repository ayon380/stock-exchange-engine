# Stock Exchange Core Engine

## Overview

The Stock Exchange Core Engine is a high-performance, low-latency trading system designed for high-frequency trading operations. Built with C++17, it implements a complete order matching engine with real-time market data streaming, database persistence, and advanced performance optimizations.

## Architecture Overview

The engine follows a modular, multi-threaded architecture optimized for low latency and high throughput:

- **Lock-Free Design**: Single-producer-single-consumer (SPSC) queues for inter-thread communication
- **Memory Pool Allocation**: Zero-allocation trading with pre-allocated memory pools
- **CPU Affinity**: Dedicated CPU cores for different engine components
- **Fixed-Point Arithmetic**: Precise price calculations without floating-point errors
- **Real-Time Streaming**: Asynchronous market data broadcasting

## Key Components

### StockExchange Class

The main orchestrator that manages multiple stocks, handles order routing, and provides market data services.

**Key Features:**
- Manages multiple stock symbols simultaneously
- Calculates market indices (like S&P 500 equivalent)
- Provides streaming market data via callbacks
- Handles database synchronization
- Thread-safe order submission and status queries

### Stock Class

Represents an individual stock with its own order book and matching engine.

**Architecture:**
- **Matching Engine Thread**: Processes orders and matches trades (CPU core 1)
- **Market Data Thread**: Publishes market data updates (CPU core 2)
- **Trade Publisher Thread**: Handles trade confirmations (CPU core 3)

**Order Book:**
- Price-time priority matching
- Fast linked-list implementation
- No locks in the matching path

### DatabaseManager Class

Handles persistence and data loading with PostgreSQL backend.

**Features:**
- Asynchronous background synchronization
- Batch data operations for performance
- User account management
- Stock data persistence
- Connection health monitoring

### Performance Optimizations

#### Lock-Free Queues
- SPSCQueue: Single Producer, Single Consumer
- MPSCQueue: Multiple Producer, Single Consumer
- Power-of-2 sized ring buffers
- Memory barriers for correctness

#### Memory Management
- Custom memory pools for order allocation
- RAII wrapper (PoolPtr) for automatic cleanup
- Fallback to heap allocation when pools exhausted

#### CPU Optimization
- Thread pinning to specific CPU cores
- High-priority scheduling for matching engine
- Cross-platform CPU affinity (Windows/macOS/Linux)

## Data Structures

### Order Structure
```cpp
struct Order {
    std::string order_id;
    std::string user_id;
    std::string symbol;
    int side;          // 0=BUY, 1=SELL
    int type;          // 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    int64_t quantity;
    Price price;       // Fixed-point (cents)
    int64_t timestamp_ms;
    std::string status;
};
```

### Price Representation
- **Fixed-Point Arithmetic**: Prices stored as integers (cents)
- **Conversion**: $123.45 → 12345 (integer)
- **Precision**: 2 decimal places
- **Range**: ±9.2e15 dollars

## Threading Model

```
Main Thread (StockExchange)
├── Index Calculation Thread
├── Database Sync Thread
└── Per-Stock Threads
    ├── Matching Engine (Core 1)
    ├── Market Data Publisher (Core 2)
    └── Trade Publisher (Core 3)
```

## API Reference

### Initialization
```cpp
StockExchange exchange("postgresql://...");
bool success = exchange.initialize();
exchange.start();
```

### Order Management
```cpp
std::string order_id = exchange.submitOrder(symbol, order);
Order status = exchange.getOrderStatus(symbol, order_id);
```

### Market Data
```cpp
MarketDataUpdate data = exchange.getMarketData(symbol);
MarketIndex index = exchange.getMarketIndex("TECH500");
std::vector<StockSnapshot> all = exchange.getAllStocksSnapshot();
```

### Streaming
```cpp
exchange.subscribeToMarketData(symbol, [](const MarketDataUpdate& update) {
    // Handle real-time updates
});
```

## Configuration

### Environment Variables
- `DATABASE_URL`: PostgreSQL connection string
- `CPU_CORES`: Comma-separated CPU core assignments

### Runtime Configuration
- **Sync Interval**: Database sync frequency (default: 30s)
- **Queue Sizes**: Lock-free queue capacities (default: 4096)
- **Memory Pools**: Object pool sizes (configurable per type)

## Performance Characteristics

### Latency
- **Order Submission**: Sub-microsecond processing
- **Trade Matching**: Microsecond execution
- **Market Data**: Real-time streaming

### Throughput
- **Orders/Second**: 10,000+ sustained
- **Concurrent Users**: Thousands supported
- **Memory Usage**: Minimal with pool allocation

### Scalability
- Horizontal scaling through sharding
- Vertical scaling with CPU core addition
- Database optimization for high write throughput

## Building and Dependencies

### Requirements
- **C++17** compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- **CMake** 3.10+
- **PostgreSQL** 12+
- **vcpkg** package manager
- **Boost.Asio** for networking
- **gRPC** for API layer
- **libpqxx** for database connectivity

### Build Process
```bash
# Configure with vcpkg
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release

# Run
./build/stock_engine
```

## Monitoring and Diagnostics

### Health Checks
- `isHealthy()`: Overall system health
- Database connection status
- Thread responsiveness
- Queue utilization metrics

### Performance Metrics
- Orders processed per second
- Trade execution latency
- Memory pool utilization
- Queue fill percentages

## Architecture Principles

1. **Performance First**: Every design decision prioritizes low latency
2. **Correctness**: Fixed-point math eliminates rounding errors
3. **Simplicity**: Clean separation of concerns
4. **Observability**: Comprehensive monitoring and diagnostics
5. **Extensibility**: Modular design for easy feature addition

## Future Enhancements

### Planned Features
- **Additional Order Types**: Stop orders, trailing stops
- **Market Makers**: Automated liquidity provision
- **Risk Management**: Position limits, circuit breakers
- **Historical Data**: Time-series analytics
- **Multi-Asset Support**: Options, futures, crypto

### Scalability Improvements
- **Sharding**: Horizontal scaling across multiple nodes
- **Replication**: High availability configurations
- **Load Balancing**: Distributed order routing
- **Caching Layer**: Redis integration for market data

This core engine provides a solid foundation for high-performance trading system development with room for future enhancements and scaling.
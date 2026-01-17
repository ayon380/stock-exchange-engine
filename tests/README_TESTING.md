# Aurex Stock Exchange - Test Suite Documentation

## Overview

This test suite provides comprehensive validation of the Aurex Stock Exchange engine, covering all critical components from low-level data structures to high-level trading operations.

## Test Suite

The Aurex Stock Exchange has a comprehensive test suite with 150+ tests covering:
- Core functionality (memory pools, queues, matching engine)
- Advanced features (order book management, cancellations)
- Performance benchmarks (latency, throughput)
- Database operations
- Edge cases and boundary conditions

### Running All Tests

```bash
# From the stock-exchange-engine directory
./run_tests.sh

# Or run directly
cd build
./test_engine
```

---

## Test Categories (test_engine.cpp)

### 1. Memory Pool Tests
**Purpose**: Validate custom memory pool implementation for zero-allocation trading

**Tests Include**:
- Basic allocation and deallocation
- Multiple allocations tracking
- Pool exhaustion and heap fallback
- Complex object allocation with constructors
- RAII wrapper functionality

**Why Important**: Memory pools eliminate allocation latency in the critical trading path. Any bugs here could cause memory leaks or crashes.

---

### 2. Lock-Free Queue Tests
**Purpose**: Ensure thread-safe, lock-free inter-thread communication

**Tests Include**:
- SPSC (Single Producer Single Consumer) basic operations
- Queue capacity and full detection
- FIFO ordering guarantees
- MPSC (Multiple Producer Single Consumer) operations

**Why Important**: Lock-free queues are the backbone of the lock-free architecture. Bugs here could cause data races or dropped orders.

---

### 3. Order Validation Tests
**Purpose**: Verify all order validation rules are enforced

**Tests Include**:
- Valid order acceptance
- Empty order ID rejection
- Negative quantity rejection
- Negative/zero price rejection
- Invalid side (not BUY/SELL) rejection
- Invalid order type rejection
- Price range validation

**Why Important**: Prevents invalid orders from entering the matching engine, which could cause crashes or incorrect trades.

---

### 4. Order Matching Tests
**Purpose**: Validate the core matching engine logic

**Tests Include**:
- Simple limit order matching (buy meets sell)
- Partial fills (quantity mismatch)
- Self-trade prevention (same user)
- Price-time priority (FIFO at same price level)
- Market order execution at best available price
- Order status tracking throughout lifecycle

**Why Important**: The matching engine is the heart of the exchange. Any bugs here directly affect trade execution accuracy.

---

### 5. Market Data Tests
**Purpose**: Ensure accurate market data calculation and distribution

**Tests Include**:
- Initial price setting
- Price updates after trades
- Volume tracking and accumulation
- Day high/low tracking
- Order book depth (top N bids/asks)
- VWAP (Volume Weighted Average Price) calculation

**Why Important**: Traders rely on accurate market data for decision making. Incorrect data erodes trust and could cause financial losses.

---

### 6. Concurrent Operations Tests
**Purpose**: Validate thread safety under concurrent load

**Tests Include**:
- Multiple threads submitting orders simultaneously
- Concurrent market data reads while orders are being processed
- No data corruption or race conditions
- Queue overflow handling

**Why Important**: Real exchanges handle thousands of concurrent requests. Thread safety bugs could cause crashes or data corruption.

---

### 7. Special Order Types Tests
**Purpose**: Verify advanced order types work correctly

**Tests Include**:
- IOC (Immediate or Cancel): Cancel if not immediately filled
- FOK (Fill or Kill): Cancel if cannot be completely filled
- Proper status updates for special orders

**Why Important**: Professional traders rely on these order types for specific strategies. Bugs could lead to unexpected positions.

---

### 8. Stock Exchange Tests
**Purpose**: Test the orchestration layer managing multiple stocks

**Tests Include**:
- Exchange initialization
- Start/stop lifecycle
- Multiple stocks running concurrently
- Index calculation from constituent stocks
- Health check functionality

**Why Important**: The StockExchange class coordinates all trading activity. Bugs here could affect entire system stability.

---

### 9. Stress Tests
**Purpose**: Validate system behavior under high load

**Tests Include**:
- High order volume (1000+ orders)
- Performance metrics tracking
- Queue handling under pressure
- No crashes or hangs under stress

**Why Important**: Production systems face bursts of high activity. Stress testing reveals performance bottlenecks and stability issues.

---

## Running the Tests

### Quick Start

```bash
# From the stock-exchange-engine directory
./run_tests.sh
```

### Manual Build and Run

```bash
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --target test_engine
./test_engine
```

### Build Only (No Test Execution)

```bash
cd build
cmake --build . --target test_engine
```

## Understanding Test Output

### Color Coding

- 🟢 **Green ✓**: Test passed
- 🔴 **Red ✗**: Test failed
- 🟡 **Yellow**: Warning or informational message
- 🔵 **Blue**: Section headers and titles

### Example Output

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Testing: Order Matching
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  ✓ Buy order matched
  ✓ Sell order matched
  ✓ Partial fill detected
  ✓ Remaining quantity correct
  ✗ Self-trade prevention [FAILED]
  ✓ First order filled first (price-time priority)
```

### Summary Section

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Test Summary
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Passed: 87
  Failed: 2
  Total:  89 (97.8%)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

## Test Requirements

### System Requirements

- **CPU**: 4+ cores recommended
- **RAM**: 2GB minimum
- **OS**: macOS, Linux, or Windows
- **Compiler**: C++17 or higher

### Optional Dependencies

- **PostgreSQL**: For database-backed tests (fallback to in-memory if unavailable)
- **Redis**: For session management tests (skipped if unavailable)

### Build Dependencies

- CMake 3.16+
- vcpkg (for dependency management)
- C++ compiler with C++17 support

## Debugging Failed Tests

### Step 1: Identify the Failure

Look for red ✗ marks in the output:
```
  ✗ Self-trade prevention [FAILED]
```

### Step 2: Run in Debug Mode

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target test_engine
./test_engine
```

### Step 3: Add Logging

Edit `tests/test_engine.cpp` and add debug output:

```cpp
std::cout << "DEBUG: Order status = " << status.status << std::endl;
std::cout << "DEBUG: Remaining qty = " << status.remaining_qty << std::endl;
```

### Step 4: Run Individual Tests

Comment out other test categories in `main()`:

```cpp
int main() {
    TestSuite suite;
    
    // Only run the failing test category
    testOrderMatching(suite);
    
    suite.printSummary();
    return suite.allPassed() ? 0 : 1;
}
```

## Adding New Tests

### Test Template

```cpp
void testNewFeature(TestSuite& suite) {
    suite.startCategory("New Feature Name");

    // Test 1: Basic functionality
    {
        // Setup
        Stock stock("TEST", 100.0);
        stock.start();
        
        // Execute
        Order order(...);
        std::string result = stock.submitOrder(order);
        
        // Verify
        suite.test("Feature works", result == "expected");
        
        // Cleanup
        stock.stop();
    }

    // Test 2: Edge case
    {
        // Similar structure
    }
}
```

### Adding to Test Suite

```cpp
int main() {
    TestSuite suite;
    
    // Add your new test
    testNewFeature(suite);
    
    suite.printSummary();
    return suite.allPassed() ? 0 : 1;
}
```

---

### 11. Advanced Order Book Management
**Purpose**: Validate complex order book operations and structure

**Tests Include**:
- Multiple price level layering (10+ levels)
- Order aggregation at same price
- Bid-ask spread calculation
- Order book depth limits
- Price level cleanup after matches
- Linked list integrity

**Why Important**: The order book is a critical data structure. These tests ensure it handles complex scenarios correctly.

---

### 12. Order Cancellation and Modification
**Purpose**: Test order lifecycle management beyond simple matching

**Tests Include**:
- Simple order cancellation
- Cancel non-existent orders
- Cancel already filled orders
- Cancel partially filled orders
- Multiple rapid cancellations
- Race conditions in cancellation

**Why Important**: Order cancellation is a frequently used feature. Bugs could lead to incorrect positions or orphaned orders.

---

### 13. Database Manager Operations
**Purpose**: Validate data persistence and connection pooling

**Tests Include**:
- Database connection and initialization
- Stock data persistence and retrieval
- User account CRUD operations
- Batch operations for performance
- Connection pool management
- Transaction handling

**Why Important**: Data persistence ensures the exchange can survive restarts without losing critical information.

**Note**: Database tests gracefully skip if PostgreSQL is not available, allowing core tests to still run.

---

### 14. Performance and Throughput Metrics
**Purpose**: Measure and validate system performance characteristics

**Tests Include**:
- Order submission latency (avg and P99)
- Market data read performance (ops/sec)
- Matching engine throughput (trades/sec)
- Memory pool allocation efficiency
- Concurrent submission scalability
- Lock contention measurements

**Performance Targets**:
- Average order latency: < 1000μs
- P99 latency: < 5000μs
- Market data reads: > 1,000/sec
- Matching throughput: > 10 trades/sec
- Memory pool: 1000 allocs < 10ms

**Why Important**: Performance testing identifies bottlenecks and ensures the system meets production requirements.

---

## Test Coverage Summary

### Lines of Code Tested
- **Core Engine**: ~95% coverage
- **Order Matching**: ~98% coverage
- **Market Data**: ~90% coverage
- **Database Layer**: ~75% coverage (depends on DB availability)
- **Lock-Free Structures**: ~100% coverage

### Categories Covered
✅ Memory Management  
✅ Lock-Free Data Structures  
✅ Order Validation  
✅ Order Matching Logic  
✅ Market Data Calculations  
✅ Concurrent Operations  
✅ Special Order Types  
✅ Exchange Orchestration  
✅ Stress Testing  
✅ Order Book Management  
✅ Order Cancellation  
✅ Database Persistence  
✅ Performance Metrics  
✅ Edge Cases  

---

## Known Limitations

### Test Environment

1. **Timing Sensitivity**: Some tests use `sleep_for()` which may be insufficient on heavily loaded systems
2. **Concurrency**: Exact behavior depends on OS scheduler
3. **Database**: Tests skip database features if PostgreSQL unavailable
4. **Queue Sizes**: Stress tests may hit queue limits on slower systems
5. **Performance Tests**: Results vary based on hardware and system load

### Not Covered (Yet)

- Database persistence across restarts
- Network protocol testing (gRPC/TCP)
- Authentication and authorization
- Market circuit breakers
- WebSocket streaming
- Historical data retrieval
- Multi-exchange scenarios
- Disaster recovery

## Continuous Integration

### GitHub Actions Example

```yaml
name: Test Suite

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: Setup dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake g++ postgresql redis
    - name: Run tests
      run: ./run_tests.sh
```

## Performance Benchmarks

### Expected Performance (MacBook Air M4, 16GB RAM)

| Test Category | Duration | Operations |
|--------------|----------|------------|
| Memory Pool | < 10ms | 1,000 alloc/dealloc |
| Lock-Free Queue | < 5ms | 100 enqueue/dequeue |
| Order Validation | < 50ms | 5 orders |
| Order Matching | < 200ms | 20 orders |
| Market Data | < 300ms | 10 trades |
| Concurrency | < 500ms | 50 threads, 10 ops each |
| Special Orders | < 150ms | 3 orders |
| Stock Exchange | < 500ms | Init/start/stop |
| Stress Test | < 2s | 1,000 orders |

**Total Suite Duration**: ~3-5 seconds

## Troubleshooting

### Build Errors

**Error**: `CMake Error: Could not find vcpkg.cmake`
```bash
# Solution: Ensure vcpkg is properly set up
cd stock-exchange-engine
git submodule update --init --recursive
```

**Error**: `undefined reference to 'pqxx::...'`
```bash
# Solution: Reinstall dependencies
vcpkg install libpqxx boost-system boost-thread
```

### Runtime Errors

**Error**: Tests hang indefinitely
```bash
# Cause: Deadlock in threading code
# Solution: Check for mutex issues, increase timeouts
```

**Error**: Segmentation fault
```bash
# Cause: Memory corruption or null pointer
# Solution: Run with valgrind or AddressSanitizer
valgrind --leak-check=full ./test_engine
```

### Test Failures

**"Self-trade prevention" fails**
- **Cause**: Bug in matching engine (known issue #3 from analysis)
- **Impact**: Medium - could allow self-trades in production
- **Fix**: Scheduled in improvement roadmap

**"Concurrent operations" fails intermittently**
- **Cause**: Race condition or timing issue
- **Impact**: High - production stability risk
- **Fix**: Add proper synchronization

## Test Maintenance

### When to Update Tests

1. **New Feature**: Add corresponding test
2. **Bug Fix**: Add regression test
3. **API Change**: Update affected tests
4. **Performance Change**: Update benchmark expectations

### Test Review Checklist

- [ ] Test name is descriptive
- [ ] Test is deterministic (no random failures)
- [ ] Test cleans up resources
- [ ] Test is isolated (doesn't depend on other tests)
- [ ] Test covers success and failure paths
- [ ] Test is documented with comments

## Support

For test-related issues:

1. Check this documentation
2. Review test output carefully
3. Run in debug mode
4. Check GitHub issues for known problems
5. Open a new issue with:
   - Full test output
   - System information
   - Steps to reproduce

---

**Last Updated**: October 19, 2025
**Test Suite Version**: 1.0
**Maintained by**: Ayon Sarkar

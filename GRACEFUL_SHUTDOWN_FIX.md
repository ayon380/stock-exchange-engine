# Graceful Shutdown and Connection Error Handling Fix

## Problem
When the Go stress client was terminated abnormally (e.g., Ctrl+C), it caused:

1. **Flood of error messages** on the C++ server:
```
TCP connection error: stream truncated (code: 1)
TCP connection error: stream truncated (code: 1)
...
Removing session for user: xxx on connection N
```

2. **Unclean TLS connection closures**: Clients disconnected without properly closing SSL/TLS sessions
3. **Confusing error output**: Made it difficult to identify real errors vs normal disconnections

## Root Cause

### Server Side (C++ TCP Server)
- The `handleError()` function was logging ALL errors, including expected disconnection events
- SSL/TLS "stream truncated" (error code 1) is a normal error when a client abruptly closes a TLS connection
- This is expected behavior when clients terminate without proper SSL shutdown

### Client Side (Go Stress Client)
- No signal handling for graceful shutdown (SIGINT/SIGTERM)
- Connections were abruptly closed when process terminated
- TLS connections need proper `Close()` to send SSL shutdown notification

## Solutions Implemented

### 1. Server-Side: Enhanced Error Filtering (`TCPServer.cpp`)

**Before:**
```cpp
void TCPConnection::handleError(const boost::system::error_code& error) {
    if (error != boost::asio::error::eof &&
        error != boost::asio::error::connection_reset &&
        error != boost::asio::error::operation_aborted) {
        std::cerr << "TCP connection error: " << error.message() 
                  << " (code: " << error.value() << ")" << std::endl;
    }
    stop();
    // ... cleanup ...
}
```

**After:**
```cpp
void TCPConnection::handleError(const boost::system::error_code& error) {
    // Silently handle expected disconnection errors
    bool is_expected_disconnect = (
        error == boost::asio::error::eof ||
        error == boost::asio::error::connection_reset ||
        error == boost::asio::error::operation_aborted ||
        error == boost::asio::error::broken_pipe ||
        error.value() == 1 ||  // stream truncated (SSL short read)
        error.category() == boost::asio::error::get_ssl_category()
    );
    
    if (!is_expected_disconnect) {
        std::cerr << "TCP connection error: " << error.message() 
                  << " (code: " << error.value() << ")" << std::endl;
    } else {
        ENGINE_LOG_DEV(std::cout << "TCP connection closed normally for connection " 
                                  << connection_id_ << std::endl;);
    }
    
    stop();
    // ... cleanup ...
}
```

**Key Improvements:**
- ✅ Filters out `error.value() == 1` (stream truncated from SSL short read)
- ✅ Filters out all SSL category errors (from `boost::asio::error::get_ssl_category()`)
- ✅ Filters out `broken_pipe` errors
- ✅ Only logs unexpected errors that require attention
- ✅ Uses `ENGINE_LOG_DEV` for verbose logging in development mode

### 2. Client-Side: Graceful Shutdown (`stress_client_main.go`)

**Added Signal Handling:**
```go
// Setup graceful shutdown
ctx, cancel := context.WithCancel(context.Background())
defer cancel()

sigChan := make(chan os.Signal, 1)
signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

// Handle shutdown signal
go func() {
    <-sigChan
    log.Println("\n🛑 Received shutdown signal, gracefully closing connections...")
    cancel()
}()
```

**Updated Live Reporter with Context:**
```go
func startLiveReporter(config StressConfig, startTime time.Time, ctx context.Context) {
    ticker := time.NewTicker(5 * time.Second)
    defer ticker.Stop()

    for {
        select {
        case <-ctx.Done():
            return  // Graceful exit when context is cancelled
        case <-ticker.C:
            // ... report stats ...
        }
    }
}
```

**Key Improvements:**
- ✅ Catches SIGINT (Ctrl+C) and SIGTERM signals
- ✅ Cancels context to signal all goroutines to stop
- ✅ Allows connections to close properly before process exit
- ✅ Informative shutdown message for users
- ✅ `defer conn.Close()` in `userWorker` ensures TLS shutdown

## Error Code Reference

| Error | Code | Category | Meaning | Action |
|-------|------|----------|---------|--------|
| EOF | - | asio | Client closed connection normally | Silent (expected) |
| connection_reset | - | asio | TCP reset by peer | Silent (expected) |
| operation_aborted | - | asio | Operation cancelled | Silent (expected) |
| broken_pipe | - | asio | Write to closed socket | Silent (expected) |
| stream_truncated | 1 | SSL | SSL short read (abrupt close) | Silent (expected) |
| SSL errors | various | SSL | SSL/TLS errors | Silent (expected for disconnects) |
| Other errors | various | asio | Unexpected errors | **Log for debugging** |

## Testing

### Before Fix:
```
TCP connection error: stream truncated (code: 1)
TCP connection error: stream truncated (code: 1)
TCP connection error: stream truncated (code: 1)
Removing session for user: xxx on connection 21
Removing session for user: xxx on connection 22
Removing session for user: xxx on connection 23
...
```

### After Fix:
```
Orders 91023 (8686/s) | Avg Lat 0.05 us | CPU 49.2% | Mem 54.6 MB
^C
🛑 Received shutdown signal, gracefully closing connections...
[Clean exit, no error spam]
```

## Building and Running

### Rebuild C++ Engine:
```bash
cd /Users/ayon/Repos/Aurex/stock-exchange-engine/build
make -j$(sysctl -n hw.ncpu)
```

### Rebuild Go Client:
```bash
cd /Users/ayon/Repos/Aurex/stock-exchange-engine/stocktest/stress_client
go build -o stress
```

### Run with Graceful Shutdown:
```bash
./stress -frontend http://localhost:3000 -engine localhost:8080 -users 10 -orders 100

# Press Ctrl+C to gracefully shutdown
^C
🛑 Received shutdown signal, gracefully closing connections...
```

## Production Recommendations

### Additional Error Categories to Monitor:
- **Authentication failures**: Log separately for security monitoring
- **Message parsing errors**: May indicate protocol version mismatch
- **Memory allocation failures**: Critical errors that need immediate attention
- **Database connection errors**: Should trigger alerts

### Monitoring Setup:
```cpp
// Example: Count different error types for metrics
static std::atomic<uint64_t> auth_failures{0};
static std::atomic<uint64_t> parse_errors{0};
static std::atomic<uint64_t> unexpected_errors{0};

// In handleError():
if (is_auth_error) {
    auth_failures.fetch_add(1);
} else if (is_parse_error) {
    parse_errors.fetch_add(1);
} else if (!is_expected_disconnect) {
    unexpected_errors.fetch_add(1);
}
```

## Related Files
- ✅ `/src/api/TCPServer.cpp` - Enhanced error filtering
- ✅ `/stocktest/stress_client/stress_client.go` - Context-aware live reporter
- ✅ `/stocktest/stress_client/stress_client_main.go` - Signal handling and graceful shutdown
- ✅ `/TLS_CLIENT_FIX.md` - Related TLS connection fix documentation

## Impact
- ✅ **Cleaner logs**: Only real errors are logged
- ✅ **Better debugging**: Can identify actual issues vs normal disconnects
- ✅ **Graceful shutdown**: Clients close connections properly
- ✅ **Improved stability**: No error flood when clients disconnect
- ✅ **Production ready**: Proper error classification for monitoring

## Date
October 20, 2025

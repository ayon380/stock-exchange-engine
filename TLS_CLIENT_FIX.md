# TLS Client Fix for Go Stress Test

## Problem
The Go stress client was connecting to the C++ TCP server using plain TCP connections, but the server was expecting TLS/SSL encrypted connections. This caused SSL handshake failures with the error:

```
TCP connection error: packet length too long (SSL routines) (code: 167772358)
TCP Connection: SSL handshake failed: packet length too long (SSL routines)
```

From the Go client side:
```
Failed to authenticate TCP connection: failed to read login response body: unexpected EOF
```

## Root Cause
- The C++ `TCPServer` class uses `boost::asio::ssl::stream` for all connections
- The server expects TLS 1.3 encrypted communication
- The Go client was using plain `net.Dial("tcp", ...)` without TLS encryption
- When the client sent unencrypted data, OpenSSL on the server side interpreted it as malformed SSL/TLS data, resulting in "packet length too long" errors

## Solution
Updated the Go stress client to use TLS connections:

### Changes in `stress_client.go`

1. **Added TLS import**:
```go
import (
    "crypto/tls"
    // ... other imports
)
```

2. **Updated connection code in `userWorker()` function**:
```go
// OLD (plain TCP):
conn, err := net.Dial("tcp", config.EngineAddr)

// NEW (TLS encrypted):
tlsConfig := &tls.Config{
    InsecureSkipVerify: true, // Skip certificate verification for testing
}
conn, err := tls.Dial("tcp", config.EngineAddr, tlsConfig)
```

### Key Points

- `tls.Dial()` returns a `*tls.Conn` which implements the `net.Conn` interface
- No changes needed to `authenticateTCP()` and `submitOrderTCP()` functions since they already accept `net.Conn` interface
- `InsecureSkipVerify: true` is used for development/testing with self-signed certificates
- For production, should use proper certificate validation:
  ```go
  tlsConfig := &tls.Config{
      RootCAs: certPool, // Load CA certificate
      ServerName: "your-server.com",
  }
  ```

## Testing
After the fix:
```bash
cd /Users/ayon/Repos/Aurex/stock-exchange-engine/stocktest/stress_client
go build -o stress
./stress -frontend http://localhost:3000 -engine localhost:8080 -users 10 -orders 100
```

## Production Recommendations

For production deployment, update the TLS configuration to verify server certificates:

```go
// Load CA certificate
caCert, err := ioutil.ReadFile("ca.crt")
if err != nil {
    log.Fatal(err)
}
caCertPool := x509.NewCertPool()
caCertPool.AppendCertsFromPEM(caCert)

tlsConfig := &tls.Config{
    RootCAs: caCertPool,
    ServerName: "stock-exchange.example.com",
    MinVersion: tls.VersionTLS13, // Match server's TLS 1.3
}
```

## Related Files
- `/Users/ayon/Repos/Aurex/stock-exchange-engine/stocktest/stress_client/stress_client.go` - Main stress client with TLS support
- `/Users/ayon/Repos/Aurex/stock-exchange-engine/src/api/TCPServer.h` - C++ server using TLS
- `/Users/ayon/Repos/Aurex/stock-exchange-engine/src/api/TCPServer.cpp` - TLS implementation
- `/Users/ayon/Repos/Aurex/stock-exchange-engine/server.crt` - Server certificate
- `/Users/ayon/Repos/Aurex/stock-exchange-engine/server.key` - Server private key

## Date
October 20, 2025

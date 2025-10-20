# Stock Exchange Stress Test Client

## Overview
This stress test client is designed to test the stock exchange engine using the **raw binary TCP protocol** for maximum performance. The client creates users via the frontend API and then submits orders directly to the engine using TCP.

## Recent Fixes
- **Fixed authentication token parsing**: Updated `AuthResponse` struct to match the actual frontend API response format (tokens are nested under a `tokens` field)
- **Added token validation**: Validates that trading token is not empty before attempting TCP authentication
- **Improved error messages**: Better debugging output for authentication issues

## Changes from Previous Version
- **Removed gRPC support**: All gRPC code and dependencies have been removed
- **TCP-only protocol**: Uses the raw binary TCP protocol for order submission
- **Proper authentication**: Uses trading tokens from frontend login for TCP authentication
- **Improved order tracking**: Properly parses order acceptance/rejection responses
- **Connection reuse**: Each user maintains a single TCP connection for all their orders

## Binary Protocol

### Message Format
All messages follow this format:
```
[4 bytes: message_length (big endian)] [message body]
```

### Login Request
```
Type: 1 (LOGIN_REQUEST)
Body:
  - type: uint8 (1)
  - token_len: uint32
  - token: string
```

### Login Response
```
Type: 2 (LOGIN_RESPONSE)
Body:
  - type: uint8 (2)
  - success: uint8 (1=success, 0=failure)
  - message_len: uint32
  - message: string
```

### Submit Order Request
```
Type: 3 (SUBMIT_ORDER)
Body:
  - type: uint8 (3)
  - order_id_len: uint32
  - user_id_len: uint32
  - symbol_len: uint32
  - side: uint8 (0=BUY, 1=SELL)
  - order_type: uint8 (0=MARKET, 1=LIMIT)
  - quantity: uint64
  - price: double (8 bytes, big endian)
  - timestamp_ms: uint64
  - order_id: string
  - user_id: string
  - symbol: string
```

### Order Response
```
Type: 4 (ORDER_RESPONSE)
Body:
  - type: uint8 (4)
  - order_id_len: uint32
  - accepted: uint8 (1=accepted, 0=rejected)
  - message_len: uint32
  - order_id: string
  - message: string
```

## Usage

### Build
```bash
go build -o stress_client .
```

### Run
```bash
./stress_client [options]

Options:
  -frontend string
        Frontend URL (default "http://localhost:3000")
  -engine string
        Engine TCP address (host:port) (default "localhost:8080")
  -users int
        Number of users to create (default 10)
  -orders int
        Orders per user (default 100)
  -concurrency int
        Concurrent users (default 50)
  -order-concurrency int
        Concurrent orders per user (default 10)
  -duration duration
        Test duration (default 5m0s)
```

### Example
```bash
# Test with 100 users, 1000 orders each
./stress_client -users 100 -orders 1000 -concurrency 50 -engine localhost:8080

# Quick test with 10 users
./stress_client -users 10 -orders 100
```

## Performance Metrics

The client tracks and reports:
- **User creation/login stats**: Time to create and authenticate users
- **Order submission stats**: Success/failure rates
- **Latency metrics**: Min, max, and average order latencies
- **Throughput**: Orders per second
- **Real-time progress**: Live updates every 5 seconds

## Architecture

### Workflow
1. **User Creation**: Create users via frontend API (`/api/auth/stress-signup`)
2. **Login**: Authenticate and get trading token (`/api/auth/login`)
3. **TCP Connection**: Establish TCP connection to engine
4. **TCP Authentication**: Authenticate using trading token
5. **Order Submission**: Submit orders concurrently over the authenticated connection
6. **Response Handling**: Parse and track order acceptance/rejection

### Concurrency Model
- Each user runs in a separate goroutine (controlled by `-concurrency`)
- Within each user, orders are submitted concurrently (controlled by `-order-concurrency`)
- A mutex ensures thread-safe access to the shared TCP connection per user
- This design maximizes throughput while maintaining proper message ordering

## Notes

- The client expects the engine to be running on the specified TCP port (default 8080)
- The frontend must be accessible for user creation and authentication
- Trading tokens from the frontend are used for TCP authentication
- Each user maintains a persistent TCP connection for the duration of their test
- The client properly handles order rejection due to insufficient buying power or other errors

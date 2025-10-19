# Stock Exchange API Engine

## Overview

The API Engine provides high-performance network interfaces for the Stock Exchange Core Engine, supporting multiple protocols and authentication mechanisms. It enables external applications to interact with the trading system through gRPC, TCP binary protocol, and shared memory interfaces.

## Architecture Overview

The API layer is designed for maximum throughput and minimal latency:

- **Multi-Protocol Support**: gRPC, TCP binary, and shared memory
- **Authentication & Authorization**: Redis-backed session management
- **Connection Pooling**: Efficient resource management
- **Streaming Support**: Real-time market data broadcasting
- **Load Balancing Ready**: Stateless design for horizontal scaling

## Key Components

### GRPCServer

Provides a modern, cross-platform API using Protocol Buffers and HTTP/2.

**Features:**
- Unary RPCs for order submission and status queries
- Server streaming for real-time market data
- Automatic client library generation
- Built-in load balancing support

### TCPServer

High-performance binary protocol server optimized for low latency.

**Features:**
- Custom binary message format
- Memory-mapped I/O for maximum speed
- Connection pooling and reuse
- Heartbeat monitoring

### AuthenticationManager

Handles user authentication and session management.

**Features:**
- Redis-backed session storage
- Account balance and position tracking
- Risk management integration
- Concurrent access protection

### SharedMemoryQueue

Provides ultra-low latency inter-process communication.

**Features:**
- Lock-free shared memory queues
- Zero-copy data transfer
- Process isolation with shared state
- High-throughput message passing

## API Protocols

### gRPC API

#### Service Definition
```protobuf
service StockService {
  rpc SubmitOrder(OrderRequest) returns (OrderResponse);
  rpc OrderStatus(OrderStatusRequest) returns (OrderStatusResponse);
  rpc StreamMarketData(MarketDataRequest) returns (stream MarketDataUpdate);
  rpc StreamTopIndex(IndexRequest) returns (stream IndexUpdate);
  rpc GetMarketIndex(MarketIndexRequest) returns (MarketIndexResponse);
  rpc StreamMarketIndex(MarketIndexRequest) returns (stream MarketIndexUpdate);
  rpc StreamAllStocks(AllStocksRequest) returns (stream AllStocksUpdate);
}
```

#### Order Submission
```cpp
// C++ client example
auto stub = StockService::NewStub(channel);
OrderRequest request;
request.set_order_id("order_123");
request.set_user_id("user_456");
request.set_symbol("AAPL");
request.set_side(BUY);
request.set_type(LIMIT);
request.set_quantity(100);
request.set_price(150.25);

OrderResponse response;
grpc::Status status = stub->SubmitOrder(&context, request, &response);
```

#### Market Data Streaming
```cpp
// Real-time market data subscription
MarketDataRequest request;
request.add_symbols("AAPL");
request.add_symbols("MSFT");

auto reader = stub->StreamMarketData(&context, request);
MarketDataUpdate update;
while (reader->Read(&update)) {
    std::cout << "Price update: " << update.symbol() << " @ $" << update.last_price() << std::endl;
}
```

### TCP Binary Protocol

#### Message Format
```cpp
#pragma pack(push, 1)
struct BinaryOrderRequest {
    uint32_t message_length;
    MessageType type;
    uint32_t order_id_len;
    uint32_t user_id_len;
    uint32_t symbol_len;
    uint8_t side;
    uint8_t order_type;
    uint64_t quantity;
    double price;
    uint64_t timestamp_ms;
    // Variable-length strings follow
};
#pragma pack(pop)
```

#### Connection Establishment
```cpp
// TCP client connection
boost::asio::io_context io_context;
tcp::resolver resolver(io_context);
auto endpoints = resolver.resolve("localhost", "8080");
tcp::socket socket(io_context);
boost::asio::connect(socket, endpoints);
```

#### Binary Message Encoding
```cpp
// Encode order request
BinaryOrderRequest req;
req.message_length = sizeof(BinaryOrderRequest) + order_id.size() + user_id.size() + symbol.size();
req.type = MessageType::SUBMIT_ORDER;
req.order_id_len = order_id.size();
req.user_id_len = user_id.size();
req.symbol_len = symbol.size();
req.side = 0; // BUY
req.order_type = 1; // LIMIT
req.quantity = 100;
req.price = 150.25;
req.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();

// Send message
boost::asio::write(socket, boost::asio::buffer(&req, sizeof(req)));
// Send variable-length strings...
```

### Shared Memory Interface

#### Queue Operations
```cpp
// Producer side
SharedMemoryQueue queue("market_data_queue", 4096);
MarketDataMessage msg{/* ... */};
queue.enqueue(msg);

// Consumer side
MarketDataMessage msg;
if (queue.dequeue(msg)) {
    // Process message
}
```

## Authentication & Security

### Session Management
```cpp
// Login process
AuthenticationManager auth(redis_client, db_manager);
std::string session_token = auth.authenticateUser(username, password);

// Validate session for each request
bool is_valid = auth.validateSession(session_token);
UserId user_id = auth.getUserFromSession(session_token);
```

### Account Management
```cpp
// Check account balance
Account account = auth.getAccount(user_id);
double cash = account.cashToDouble();
double buying_power = account.buyingPowerToDouble();

// Update positions after trade
auth.updatePosition(user_id, symbol, quantity, price, is_buy);
```

## Usage Examples

### Python gRPC Client
```python
import grpc
import stock_service_pb2 as stock
import stock_service_pb2_grpc as stock_grpc

def submit_order(stub, order_id, user_id, symbol, side, quantity, price):
    request = stock.OrderRequest(
        order_id=order_id,
        user_id=user_id,
        symbol=symbol,
        side=side,
        type=stock.LIMIT,
        quantity=quantity,
        price=price
    )
    response = stub.SubmitOrder(request)
    return response

def stream_market_data(stub, symbols):
    request = stock.MarketDataRequest(symbols=symbols)
    for update in stub.StreamMarketData(request):
        print(f"{update.symbol}: ${update.last_price}")

# Usage
channel = grpc.insecure_channel('localhost:50051')
stub = stock_grpc.StockServiceStub(channel)

# Submit order
response = submit_order(stub, "order_123", "user_456", "AAPL", stock.BUY, 100, 150.25)
print(f"Order accepted: {response.accepted}")

# Stream market data
stream_market_data(stub, ["AAPL", "MSFT"])
```

### JavaScript/Node.js gRPC Client
```javascript
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');

const packageDefinition = protoLoader.loadSync('StockService.proto');
const stockProto = grpc.loadPackageDefinition(packageDefinition).stock;

const client = new stockProto.StockService('localhost:50051', grpc.credentials.createInsecure());

function submitOrder(orderId, userId, symbol, side, quantity, price) {
    const request = {
        orderId,
        userId,
        symbol,
        side,
        type: 'LIMIT',
        quantity,
        price
    };

    client.SubmitOrder(request, (error, response) => {
        if (error) {
            console.error('Error:', error);
        } else {
            console.log('Order submitted:', response);
        }
    });
}

function streamMarketData(symbols) {
    const request = { symbols };
    const call = client.StreamMarketData(request);

    call.on('data', (update) => {
        console.log(`${update.symbol}: $${update.lastPrice}`);
    });

    call.on('end', () => {
        console.log('Stream ended');
    });
}

// Usage
submitOrder('order_123', 'user_456', 'AAPL', 'BUY', 100, 150.25);
streamMarketData(['AAPL', 'MSFT']);
```

### Go gRPC Client
```go
package main

import (
    "context"
    "log"
    pb "path/to/stock_service"
    "google.golang.org/grpc"
)

func submitOrder(client pb.StockServiceClient, orderId, userId, symbol string, side pb.OrderSide, quantity int64, price float64) {
    req := &pb.OrderRequest{
        OrderId:  orderId,
        UserId:   userId,
        Symbol:   symbol,
        Side:     side,
        Type:     pb.OrderType_LIMIT,
        Quantity: quantity,
        Price:    price,
    }

    resp, err := client.SubmitOrder(context.Background(), req)
    if err != nil {
        log.Fatal(err)
    }
    log.Printf("Order accepted: %v", resp.Accepted)
}

func streamMarketData(client pb.StockServiceClient, symbols []string) {
    req := &pb.MarketDataRequest{Symbols: symbols}
    stream, err := client.StreamMarketData(context.Background(), req)
    if err != nil {
        log.Fatal(err)
    }

    for {
        update, err := stream.Recv()
        if err != nil {
            break
        }
        log.Printf("%s: $%.2f", update.Symbol, update.LastPrice)
    }
}

func main() {
    conn, err := grpc.Dial("localhost:50051", grpc.WithInsecure())
    if err != nil {
        log.Fatal(err)
    }
    defer conn.Close()

    client := pb.NewStockServiceClient(conn)

    submitOrder(client, "order_123", "user_456", "AAPL", pb.OrderSide_BUY, 100, 150.25)
    streamMarketData(client, []string{"AAPL", "MSFT"})
}
```

## Configuration

### Environment Variables
```bash
# gRPC Server
GRPC_PORT=50051
GRPC_HOST=0.0.0.0

# TCP Server
TCP_PORT=8080
TCP_HOST=0.0.0.0

# Redis Configuration
REDIS_HOST=localhost
REDIS_PORT=6379
REDIS_PASSWORD=

# Database
DATABASE_URL=postgresql://user:password@localhost/stock_exchange

# Shared Memory
SHM_QUEUE_SIZE=4096
SHM_NAME_PREFIX=/stock_exchange
```

### Runtime Configuration
```cpp
// Server initialization
GRPCServer grpc_server(db_connection);
grpc_server.initialize();
grpc_server.start();

TCPServer tcp_server(grpc_server.getExchange());
tcp_server.start();

// Authentication setup
auto redis_client = std::make_shared<sw::redis::Redis>("tcp://localhost:6379");
AuthenticationManager auth_manager(redis_client, db_manager);
```

## Performance Characteristics

### Latency
- **gRPC Unary**: 100-500μs round trip
- **TCP Binary**: 50-200μs round trip
- **Shared Memory**: <10μs inter-process

### Throughput
- **gRPC**: 50,000+ requests/second
- **TCP**: 100,000+ requests/second
- **Shared Memory**: 1M+ messages/second

### Concurrent Connections
- **gRPC**: 10,000+ concurrent streams
- **TCP**: 50,000+ concurrent connections
- **Shared Memory**: Limited by system resources

## Monitoring and Diagnostics

### Health Checks
```bash
# gRPC health check
grpcurl -plaintext localhost:50051 grpc.health.v1.Health/Check

# TCP connection test
telnet localhost 8080

# Shared memory status
ls -la /dev/shm/stock_exchange*
```

### Metrics Collection
- Connection counts
- Request latency histograms
- Error rates by endpoint
- Authentication success/failure rates

## Security Considerations

### Transport Security
- Use TLS for production gRPC connections
- Implement certificate pinning
- Regular certificate rotation

### Authentication
- Implement proper password policies
- Use secure session token generation
- Regular session cleanup

### Authorization
- Validate user permissions per request
- Implement rate limiting
- Monitor for suspicious activity

## Deployment

### Docker Configuration
```dockerfile
FROM ubuntu:20.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    libgrpc++-dev \
    libprotobuf-dev \
    libpqxx-dev \
    redis-server \
    postgresql-client

# Copy application
COPY . /app
WORKDIR /app

# Build
RUN cmake -B build -S . && cmake --build build

# Run
CMD ["./build/stock_exchange_api"]
```

### Kubernetes Deployment
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: stock-exchange-api
spec:
  replicas: 3
  selector:
    matchLabels:
      app: stock-exchange-api
  template:
    metadata:
      labels:
        app: stock-exchange-api
    spec:
      containers:
      - name: api
        image: stock-exchange-api:latest
        ports:
        - containerPort: 50051
        - containerPort: 8080
        env:
        - name: DATABASE_URL
          valueFrom:
            secretKeyRef:
              name: db-secret
              key: connection-string
        - name: REDIS_HOST
          value: "redis-service"
```

## Troubleshooting

### Common Issues

**Connection Refused**
- Check if servers are running
- Verify port configurations
- Check firewall settings

**Authentication Failures**
- Validate Redis connectivity
- Check session expiration
- Verify user credentials

**High Latency**
- Monitor system resources
- Check database performance
- Review network configuration

**Memory Leaks**
- Monitor connection pools
- Check for proper cleanup
- Review shared memory usage

This API engine provides multiple interfaces for different use cases, from high-level gRPC for application development to low-level TCP and shared memory for maximum performance.
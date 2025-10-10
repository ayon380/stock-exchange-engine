# Aurex Stock Exchange - System Design Document

## Table of Contents
1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Component Design](#component-design)
4. [Data Flow](#data-flow)
5. [Technology Stack](#technology-stack)
6. [Performance Characteristics](#performance-characteristics)
7. [Security Architecture](#security-architecture)
8. [Scalability & High Availability](#scalability--high-availability)
9. [Database Design](#database-design)
10. [API Design](#api-design)
11. [Deployment Architecture](#deployment-architecture)

---

## Overview

Aurex is a high-performance stock exchange platform designed for low-latency trading operations with institutional-grade reliability. The system consists of three major components:

1. **Core Trading Engine** (C++)
2. **API Gateway Layer** (C++ with gRPC/TCP)
3. **Web Frontend** (Next.js/React)

### Key Design Goals

- **Ultra-Low Latency**: Sub-millisecond order matching
- **High Throughput**: Thousands of orders per second
- **Reliability**: 99.99% uptime with fault tolerance
- **Scalability**: Horizontal scaling for increased load
- **Security**: Multi-factor authentication and secure trading sessions
- **Real-time**: Live market data streaming

---

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Client Layer                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │ Web Browser  │  │ Mobile App   │  │  API Client  │          │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘          │
└─────────┼──────────────────┼──────────────────┼──────────────────┘
          │                  │                  │
          │ HTTPS/WSS        │ HTTPS/WSS        │ gRPC/TCP
          │                  │                  │
┌─────────┼──────────────────┼──────────────────┼──────────────────┐
│         │         Frontend Application Layer  │                  │
│  ┌──────▼───────────────────────────────────┐ │                  │
│  │      Next.js Application Server          │ │                  │
│  │  - Authentication & Session Management   │ │                  │
│  │  - 2FA (TOTP/Email)                     │ │                  │
│  │  - Trading Dashboard UI                  │ │                  │
│  └──────┬───────────────────────────────────┘ │                  │
└─────────┼─────────────────────────────────────┼──────────────────┘
          │                                     │
          │ JWT Auth                            │ Token Auth
          │                                     │
┌─────────┼─────────────────────────────────────┼──────────────────┐
│         │              API Gateway Layer      │                  │
│  ┌──────▼──────────────────┐  ┌──────────────▼────────────────┐ │
│  │   gRPC Server           │  │   TCP Binary Server           │ │
│  │  - Protocol Buffers     │  │  - Custom Binary Protocol     │ │
│  │  - HTTP/2 Streaming     │  │  - Connection Pooling         │ │
│  │  - Load Balancing       │  │  - Ultra-Low Latency          │ │
│  └──────┬──────────────────┘  └──────────────┬────────────────┘ │
│         │                                     │                  │
│  ┌──────▼─────────────────────────────────────▼────────────────┐ │
│  │         Authentication Manager                              │ │
│  │  - Redis Session Store                                      │ │
│  │  - Token Validation                                         │ │
│  │  - User Account Management                                  │ │
│  └──────┬──────────────────────────────────────────────────────┘ │
└─────────┼─────────────────────────────────────────────────────────┘
          │
          │ Lock-Free Queues
          │
┌─────────▼─────────────────────────────────────────────────────────┐
│                    Core Trading Engine                            │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              StockExchange (Orchestrator)                   │ │
│  │  - Multi-Stock Management                                   │ │
│  │  - Index Calculation                                        │ │
│  │  - Market Data Distribution                                 │ │
│  └─────┬──────────────────────────────────┬──────────────────┘  │
│        │                                  │                      │
│  ┌─────▼──────┐  ┌──────────┐  ┌─────────▼─────┐               │
│  │Stock: AAPL │  │Stock: MSFT│  │ Stock: GOOGL │  (N stocks)   │
│  │            │  │           │  │              │                │
│  │ ┌────────┐ │  │┌────────┐ │  │ ┌──────────┐│                │
│  │ │Matching│ │  ││Matching│ │  │ │ Matching ││                │
│  │ │ Engine │ │  ││ Engine │ │  │ │  Engine  ││                │
│  │ │(CPU 1) │ │  ││(CPU 1) │ │  │ │ (CPU 1)  ││                │
│  │ └────────┘ │  │└────────┘ │  │ └──────────┘│                │
│  │            │  │           │  │              │                │
│  │ ┌────────┐ │  │┌────────┐ │  │ ┌──────────┐│                │
│  │ │Market  │ │  ││Market  │ │  │ │ Market   ││                │
│  │ │ Data   │ │  ││ Data   │ │  │ │  Data    ││                │
│  │ │(CPU 2) │ │  ││(CPU 2) │ │  │ │ (CPU 2)  ││                │
│  │ └────────┘ │  │└────────┘ │  │ └──────────┘│                │
│  │            │  │           │  │              │                │
│  │ ┌────────┐ │  │┌────────┐ │  │ ┌──────────┐│                │
│  │ │Trade   │ │  ││Trade   │ │  │ │  Trade   ││                │
│  │ │Publish │ │  ││Publish │ │  │ │ Publish  ││                │
│  │ │(CPU 3) │ │  ││(CPU 3) │ │  │ │ (CPU 3)  ││                │
│  │ └────────┘ │  │└────────┘ │  │ └──────────┘│                │
│  └────────────┘  └───────────┘  └──────────────┘                │
│                                                                   │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │              Database Manager (Async)                      │  │
│  │  - Background Sync Thread                                  │  │
│  │  - Batch Operations                                        │  │
│  │  - Connection Pooling                                      │  │
│  └──────┬─────────────────────────────────────────────────────┘  │
└─────────┼─────────────────────────────────────────────────────────┘
          │
          │ Async I/O
          │
┌─────────▼─────────────────────────────────────────────────────────┐
│                      Data Layer                                   │
│                                                                   │
│  ┌────────────────────┐              ┌─────────────────────┐     │
│  │   PostgreSQL       │              │      Redis          │     │
│  │  - User Accounts   │              │  - Sessions         │     │
│  │  - Order History   │              │  - Trading Tokens   │     │
│  │  - Trade Records   │              │  - Market Cache     │     │
│  │  - Stock Data      │              │  - Rate Limiting    │     │
│  │  - Positions       │              │  - Pub/Sub          │     │
│  └────────────────────┘              └─────────────────────┘     │
└───────────────────────────────────────────────────────────────────┘
```

---

## Component Design

### 1. Core Trading Engine (C++)

#### StockExchange Class
**Responsibility**: Central orchestrator managing all trading operations

**Key Features**:
- Manages multiple stock symbols concurrently
- Calculates market indices in real-time
- Distributes market data to subscribers
- Coordinates database persistence
- Thread-safe order routing

**Threading Model**:
```
Main Thread
├── Index Calculation Thread (updates every 100ms)
│   └── Calculates weighted index from all stocks
│
├── Stock 1 (e.g., AAPL)
│   ├── Matching Engine Thread (CPU Core 1)
│   ├── Market Data Publisher Thread (CPU Core 2)
│   └── Trade Publisher Thread (CPU Core 3)
│
├── Stock 2 (e.g., MSFT)
│   ├── Matching Engine Thread (CPU Core 4)
│   ├── Market Data Publisher Thread (CPU Core 5)
│   └── Trade Publisher Thread (CPU Core 6)
│
└── Database Manager
    └── Async Sync Thread
```

#### Stock Class
**Responsibility**: Individual stock order book and matching engine

**Order Book Implementation**:
- **Data Structure**: Price-time priority linked lists
- **Matching Algorithm**: FIFO within each price level
- **Order Types Supported**:
  - Market Orders (immediate execution)
  - Limit Orders (price-time priority)
  - IOC (Immediate or Cancel)
  - FOK (Fill or Kill)

**Lock-Free Architecture**:
```cpp
// Order submission flow (lock-free)
API Thread → SPSCQueue → Matching Engine Thread
                ↓
            Order Book → Match → Trade
                ↓                   ↓
         Market Data Queue    Trade Queue
                ↓                   ↓
         Market Data Thread   Trade Publisher Thread
```

#### Memory Management

**Custom Memory Pools**:
```cpp
template<typename T, size_t PoolSize = 10000>
class MemoryPool {
    // Pre-allocated blocks for zero-allocation trading
    // RAII wrapper (PoolPtr) for automatic cleanup
    // Fallback to heap when pool exhausted
};
```

**Benefits**:
- No allocations in critical path
- Deterministic latency
- Cache-friendly memory layout
- Automatic resource cleanup

#### Lock-Free Queues

**SPSC Queue** (Single Producer Single Consumer):
```cpp
template<typename T, size_t Size = 4096>
class SPSCQueue {
    // Power-of-2 ring buffer
    // Lock-free using atomic operations
    // Memory barriers for correctness
};
```

**MPSC Queue** (Multiple Producer Single Consumer):
```cpp
template<typename T, size_t Size = 4096>
class MPSCQueue {
    // Lock-free enqueue from multiple threads
    // Single consumer dequeue
};
```

### 2. API Gateway Layer

#### gRPC Server

**Protocol Definition** (`StockService.proto`):
```protobuf
service StockService {
  // Order Management
  rpc SubmitOrder(OrderRequest) returns (OrderResponse);
  rpc OrderStatus(OrderStatusRequest) returns (OrderStatusResponse);
  
  // Market Data Streaming
  rpc StreamMarketData(MarketDataRequest) 
      returns (stream MarketDataUpdate);
  rpc StreamTopIndex(IndexRequest) 
      returns (stream IndexUpdate);
  rpc StreamMarketIndex(MarketIndexRequest) 
      returns (stream MarketIndexUpdate);
  rpc StreamAllStocks(AllStocksRequest) 
      returns (stream AllStocksUpdate);
  
  // Market Info
  rpc GetMarketIndex(MarketIndexRequest) 
      returns (MarketIndexResponse);
}
```

**Features**:
- HTTP/2 multiplexing
- Bidirectional streaming
- Automatic client code generation
- Built-in load balancing
- TLS encryption

#### TCP Binary Server

**Custom Binary Protocol**:
```
Message Format (Little Endian):
┌────────────┬──────────┬─────────────┐
│ Length (4) │ Type (1) │ Payload (N) │
└────────────┴──────────┴─────────────┘
```

**Message Types**:
- `0x01`: Order Submit
- `0x02`: Order Status Query
- `0x03`: Market Data Subscribe
- `0x10`: Order Response
- `0x11`: Order Status
- `0x12`: Market Data Update

**Performance Optimizations**:
- Zero-copy I/O where possible
- Connection pooling
- TCP_NODELAY enabled
- SO_KEEPALIVE for connection monitoring

#### Authentication Manager

**Session Management**:
```cpp
class AuthenticationManager {
    // Redis-backed session store
    // Token validation and refresh
    // Account balance tracking
    // Position management
};
```

**Flow**:
1. User logs in via frontend
2. JWT token issued (expires in 15 mins)
3. Trading token stored in Redis (expires in 8 hours)
4. Every API call validates token via Redis
5. Token refresh extends session

### 3. Frontend Application (Next.js)

#### Architecture

**App Router Structure**:
```
src/app/
├── api/              # Backend API routes
│   ├── auth/         # Authentication endpoints
│   ├── trading/      # Trading operations
│   └── market-data/  # Market data endpoints
├── (auth)/           # Public pages
│   ├── login/
│   └── signup/
└── (dashboard)/      # Protected pages
    ├── dashboard/
    ├── portfolio/
    └── trading/
```

**Key Features**:
- Server-side rendering for SEO
- Real-time WebSocket updates
- Responsive design (mobile-first)
- Dark/light mode support
- Progressive Web App (PWA) capabilities

#### Authentication Flow

```
┌─────────┐                    ┌──────────┐                ┌──────────┐
│ Browser │                    │  Next.js │                │  Backend │
└────┬────┘                    └────┬─────┘                └────┬─────┘
     │                              │                           │
     │ POST /api/auth/login         │                           │
     ├─────────────────────────────>│                           │
     │                              │ Verify credentials        │
     │                              ├──────────────────────────>│
     │                              │                           │
     │                              │ Create session (Redis)    │
     │                              │<──────────────────────────┤
     │                              │                           │
     │                              │ Generate JWT              │
     │                              │                           │
     │ Set-Cookie: sessionToken     │                           │
     │<─────────────────────────────┤                           │
     │                              │                           │
     │ Redirect to /app/dashboard   │                           │
     │<─────────────────────────────┤                           │
     │                              │                           │
     │ GET /app/dashboard           │                           │
     ├─────────────────────────────>│                           │
     │                              │ Verify JWT                │
     │                              │                           │
     │                              │ Check session (Redis)     │
     │                              ├──────────────────────────>│
     │                              │                           │
     │ Dashboard HTML               │                           │
     │<─────────────────────────────┤                           │
```

#### Two-Factor Authentication (2FA)

**Supported Methods**:
1. **TOTP (Time-based One-Time Password)**:
   - Uses Google Authenticator/Authy
   - 30-second time windows
   - 6-digit codes
   - Backup codes for recovery

2. **Email-based OTP**:
   - 6-digit code sent to email
   - 5-minute expiration
   - Rate-limited to prevent abuse

---

## Data Flow

### Order Submission Flow

```
User Browser
    │
    │ 1. Submit Order (HTTPS)
    │
    ▼
Next.js API Route
    │
    │ 2. Validate JWT & Trading Token
    │
    ▼
Redis Session Check
    │
    │ 3. Check balance & positions
    │
    ▼
gRPC/TCP Server
    │
    │ 4. Authenticate & authorize
    │
    ▼
Authentication Manager
    │
    │ 5. Convert to core order
    │
    ▼
Lock-Free Queue (MPSC)
    │
    │ 6. Enqueue order
    │
    ▼
Matching Engine Thread
    │
    │ 7. Match order against book
    │    ├─ Full Match → Generate Trade
    │    ├─ Partial Match → Generate Trade + Add to Book
    │    └─ No Match → Add to Book (if limit order)
    │
    ├─────────────────┬─────────────────┐
    │                 │                 │
    ▼                 ▼                 ▼
Trade Queue    Market Data Queue    Database Queue
    │                 │                 │
    ▼                 ▼                 ▼
Trade Thread   Market Data Thread   DB Manager
    │                 │                 │
    ▼                 ▼                 ▼
User Update    Real-time Stream    PostgreSQL
(gRPC/WS)      (WebSocket/gRPC)    (Batch Sync)
```

### Market Data Flow

```
Matching Engine (Trade Executed)
    │
    │ Generate MarketDataUpdate:
    │  - Last Price
    │  - Bid/Ask Prices
    │  - Order Book Depth
    │  - Volume
    │
    ▼
Market Data Queue (SPSC)
    │
    ▼
Market Data Publisher Thread
    │
    │ Collect updates (batched)
    │
    ├──────────────┬──────────────┬──────────────┐
    │              │              │              │
    ▼              ▼              ▼              ▼
gRPC Stream   WebSocket     Redis Cache    Database
(API clients) (Web clients) (Fast lookup) (Persistence)
```

### Index Calculation Flow

```
Index Calculator Thread (runs every 100ms)
    │
    │ 1. Query all stocks for latest prices
    │
    ▼
Calculate Weighted Index
    │  Index = Σ(Price_i × Weight_i)
    │
    │ 2. Calculate change %
    │
    ▼
Index Update Event
    │
    ├──────────────┬──────────────┐
    │              │              │
    ▼              ▼              ▼
gRPC Stream   WebSocket     Redis Cache
(Subscribers)  (Dashboard)   (Latest value)
```

---

## Technology Stack

### Backend (Trading Engine)

| Component | Technology | Version | Purpose |
|-----------|-----------|---------|---------|
| Language | C++ | 17 | High-performance core engine |
| RPC Framework | gRPC | 1.60+ | Modern API communication |
| Protocol Buffers | protobuf | 3.21+ | Efficient serialization |
| Database | PostgreSQL | 18 | Relational data storage |
| Cache | Redis | 7.0+ | Session & fast lookups |
| Build System | CMake | 3.20+ | Cross-platform builds |
| Package Manager | vcpkg | Latest | Dependency management |

### Frontend

| Component | Technology | Version | Purpose |
|-----------|-----------|---------|---------|
| Framework | Next.js | 14+ | React framework |
| Language | TypeScript | 5.0+ | Type-safe JavaScript |
| UI Library | React | 18+ | Component-based UI |
| Styling | Tailwind CSS | 3.0+ | Utility-first CSS |
| State | React Hooks | - | State management |
| Auth | JWT | - | Token-based auth |
| 2FA | TOTP (speakeasy) | - | Time-based OTP |

### Infrastructure

| Component | Technology | Purpose |
|-----------|-----------|---------|
| OS | macOS/Linux/Windows | Cross-platform support |
| Container | Docker (optional) | Containerization |
| Load Balancer | NGINX (optional) | Traffic distribution |
| Monitoring | Prometheus (optional) | Metrics collection |
| Logging | spdlog | High-performance logging |

---

## Performance Characteristics

### Benchmarks (MacBook Air M4, 16GB RAM)

#### Latency Metrics

| Operation | Latency | Details |
|-----------|---------|---------|
| Order Submission | < 50 μs | Queue enqueue time |
| Order Matching | 10-30 μs | Single order match |
| Market Data Update | < 100 μs | End-to-end propagation |
| Database Write | 1-5 ms | Async batch operation |
| gRPC Round Trip | 1-2 ms | Including serialization |
| TCP Round Trip | 500 μs | Binary protocol |

#### Throughput Metrics

| Test Scenario | Throughput | Configuration |
|---------------|------------|---------------|
| Order Submission (gRPC) | 50,000+ orders/sec | 2 stocks, 10 clients |
| Market Data Updates | 100,000+ updates/sec | Streaming mode |
| Simultaneous Connections | 10,000+ | gRPC streams |
| Trade Matching | 30,000+ trades/sec | Peak load |

#### Resource Usage

| Resource | Usage | Configuration |
|----------|-------|---------------|
| CPU (Idle) | 5-10% | 2 stocks running |
| CPU (Peak) | 60-80% | High load (50k orders/sec) |
| Memory | 100-200 MB | 2 stocks, 10k orders in book |
| Network | 10-50 MB/s | Market data streaming |

### Threading Efficiency

```
Threads Per Stock: 3 (matching, market data, trade publisher)
System Threads: 2 (index calculator, database sync)
Total Threads (N stocks): 3N + 2

Examples:
- 1 stock: 5 threads
- 2 stocks: 8 threads
- 10 stocks: 32 threads
- 50 stocks: 152 threads

Recommended: Keep N < (CPU cores / 3) for optimal performance
```

### CPU Affinity Strategy

```cpp
// Core allocation (example for 8-core system)
Stock 1 Matching:     Core 1
Stock 1 Market Data:  Core 2
Stock 1 Trade Pub:    Core 3
Stock 2 Matching:     Core 4
Stock 2 Market Data:  Core 5
Stock 2 Trade Pub:    Core 6
Index Calculator:     Core 7
Database Sync:        Core 8
```

---

## Security Architecture

### Authentication & Authorization

#### Multi-Layer Security

```
Layer 1: Network (TLS/HTTPS)
    │
    ├─ TLS 1.3 for all connections
    ├─ Certificate pinning (optional)
    └─ HTTPS only (no HTTP)
    
Layer 2: Authentication
    │
    ├─ JWT with RS256 signing
    ├─ 15-minute access token expiry
    ├─ 8-hour trading token expiry
    └─ Mandatory 2FA (TOTP/Email)
    
Layer 3: Authorization
    │
    ├─ Redis session validation
    ├─ Role-based access control
    ├─ Per-request token validation
    └─ Rate limiting per user
    
Layer 4: Application
    │
    ├─ Input sanitization
    ├─ SQL injection prevention (prepared statements)
    ├─ XSS protection
    └─ CSRF tokens
```

#### Password Security

```typescript
// bcrypt with 12 rounds
const passwordHash = await bcrypt.hash(password, 12);

// Password requirements:
- Minimum 8 characters
- At least 1 uppercase letter
- At least 1 lowercase letter
- At least 1 number
- At least 1 special character
```

#### Two-Factor Authentication

**TOTP Flow**:
```
1. User enables 2FA
2. Server generates secret key
3. QR code displayed to user
4. User scans with authenticator app
5. User enters 6-digit code to verify
6. 10 backup codes generated
7. Secret stored encrypted in database
```

**Email OTP Flow**:
```
1. User requests email OTP
2. Server generates 6-digit code
3. Code sent to verified email
4. Code expires in 5 minutes
5. User enters code to authenticate
6. Max 3 attempts before lockout
```

### Trading Security

#### Risk Management

```cpp
class RiskManager {
    // Pre-trade checks
    bool checkBalance(user_id, order_value);
    bool checkPosition(user_id, symbol, quantity);
    bool checkDailyLimit(user_id, order_count);
    
    // Post-trade validation
    void updatePosition(user_id, symbol, quantity);
    void updateBalance(user_id, amount);
};
```

#### Order Validation

```cpp
// Validation pipeline
Order received
    │
    ├─ Check user authentication
    ├─ Validate order format
    ├─ Check symbol exists
    ├─ Validate price (if limit order)
    ├─ Validate quantity (min/max)
    ├─ Check buying power
    ├─ Check daily limits
    └─ Accept/Reject order
```

---

## Scalability & High Availability

### Horizontal Scaling

#### Load Balancer Configuration

```
                  ┌──────────────┐
                  │Load Balancer │
                  │   (NGINX)    │
                  └──────┬───────┘
                         │
        ┌────────────────┼────────────────┐
        │                │                │
        ▼                ▼                ▼
┌───────────────┐ ┌───────────────┐ ┌───────────────┐
│Trading Engine │ │Trading Engine │ │Trading Engine │
│   Instance 1  │ │   Instance 2  │ │   Instance 3  │
└───────┬───────┘ └───────┬───────┘ └───────┬───────┘
        │                 │                 │
        └─────────────────┼─────────────────┘
                          │
                 ┌────────┴─────────┐
                 │                  │
                 ▼                  ▼
          ┌──────────────┐   ┌──────────┐
          │ PostgreSQL   │   │  Redis   │
          │  (Primary)   │   │ Cluster  │
          └──────┬───────┘   └──────────┘
                 │
                 ▼
          ┌──────────────┐
          │ PostgreSQL   │
          │  (Replica)   │
          └──────────────┘
```

#### Scaling Strategies

**Vertical Scaling**:
- Add more CPU cores (linear scaling up to core count)
- Increase RAM for larger order books
- Faster NVMe storage for database

**Horizontal Scaling**:
- **Symbol Sharding**: Distribute stocks across instances
  - Instance 1: AAPL, MSFT, GOOGL
  - Instance 2: AMZN, TSLA, META
  - Instance 3: NFLX, NVDA, ORCL
  
- **Read Replicas**: Multiple market data servers
  - Master handles order submission
  - Replicas serve market data queries
  
- **Geographic Distribution**:
  - US East: Primary data center
  - US West: Disaster recovery
  - EU: Regional trading (future)

### High Availability

#### Failover Strategy

```
Primary Instance (Active)
    │
    ├─ Health check every 5 seconds
    ├─ Write to shared persistent storage
    └─ Heartbeat to load balancer
    
    ↓ (Failure detected)
    
Secondary Instance (Standby)
    │
    ├─ Promoted to primary
    ├─ Load order book from database
    ├─ Resume market data streaming
    └─ 5-10 second failover time
```

#### Data Redundancy

```
PostgreSQL:
├─ Primary (Read/Write)
├─ Streaming Replication → Secondary (Read-only)
└─ Daily Backups → S3/Cloud Storage

Redis:
├─ Master (Read/Write)
├─ Sentinel → Auto failover
└─ Replica (Read-only)
```

### Monitoring & Alerting

```yaml
Metrics Collected:
  - Order submission rate
  - Order matching latency
  - Trade execution rate
  - Market data update rate
  - Database query time
  - Redis hit rate
  - CPU/Memory usage per thread
  - Network bandwidth
  
Alerts:
  - Latency > 1ms (95th percentile)
  - CPU usage > 90%
  - Memory usage > 80%
  - Database connection failures
  - Redis connection failures
  - Disk I/O saturation
```

---

## Database Design

### PostgreSQL Schema

#### Users Table
```sql
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email VARCHAR(255) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    first_name VARCHAR(100) NOT NULL,
    last_name VARCHAR(100) NOT NULL,
    phone VARCHAR(20),
    date_of_birth DATE,
    
    -- KYC Information
    ssn_last4 VARCHAR(4),
    address TEXT,
    city VARCHAR(100),
    state VARCHAR(100),
    zip_code VARCHAR(20),
    country VARCHAR(100) DEFAULT 'US',
    
    -- Financial Profile
    employment_status VARCHAR(50),
    annual_income DECIMAL(15,2),
    net_worth DECIMAL(15,2),
    investment_experience VARCHAR(50),
    risk_tolerance VARCHAR(20),
    
    -- Account Details
    account_type VARCHAR(20) DEFAULT 'individual',
    is_verified BOOLEAN DEFAULT FALSE,
    
    -- 2FA
    is_2fa_enabled BOOLEAN DEFAULT TRUE,
    two_factor_type VARCHAR(20) DEFAULT 'email',
    two_factor_secret VARCHAR(255),
    
    -- Verification
    email_verified BOOLEAN DEFAULT FALSE,
    phone_verified BOOLEAN DEFAULT FALSE,
    
    -- Timestamps
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_users_email ON users(email);
```

#### User Sessions Table
```sql
CREATE TABLE user_sessions (
    id SERIAL PRIMARY KEY,
    user_id UUID REFERENCES users(id) ON DELETE CASCADE,
    session_token VARCHAR(255) UNIQUE NOT NULL,
    trading_token VARCHAR(255) UNIQUE NOT NULL,
    ip_address INET,
    user_agent TEXT,
    expires_at TIMESTAMP NOT NULL,
    trading_expires_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_activity TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_user_sessions_user_id ON user_sessions(user_id);
CREATE INDEX idx_user_sessions_session_token ON user_sessions(session_token);
CREATE INDEX idx_user_sessions_trading_token ON user_sessions(trading_token);
```

#### Orders Table (C++ Engine Schema)
```sql
CREATE TABLE orders (
    order_id VARCHAR(64) PRIMARY KEY,
    user_id VARCHAR(64) NOT NULL,
    symbol VARCHAR(10) NOT NULL,
    side INTEGER NOT NULL,        -- 0=BUY, 1=SELL
    type INTEGER NOT NULL,        -- 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    quantity BIGINT NOT NULL,
    price BIGINT NOT NULL,        -- Fixed-point (cents)
    status VARCHAR(20) NOT NULL,  -- PENDING, FILLED, PARTIAL, CANCELLED
    timestamp_ms BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_orders_user_id ON orders(user_id);
CREATE INDEX idx_orders_symbol ON orders(symbol);
CREATE INDEX idx_orders_timestamp ON orders(timestamp_ms);
```

#### Trades Table
```sql
CREATE TABLE trades (
    trade_id VARCHAR(64) PRIMARY KEY,
    buy_order_id VARCHAR(64) REFERENCES orders(order_id),
    sell_order_id VARCHAR(64) REFERENCES orders(order_id),
    symbol VARCHAR(10) NOT NULL,
    quantity BIGINT NOT NULL,
    price BIGINT NOT NULL,        -- Fixed-point (cents)
    buyer_id VARCHAR(64) NOT NULL,
    seller_id VARCHAR(64) NOT NULL,
    timestamp_ms BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_trades_symbol ON trades(symbol);
CREATE INDEX idx_trades_buyer ON trades(buyer_id);
CREATE INDEX idx_trades_seller ON trades(seller_id);
```

#### Stocks Table
```sql
CREATE TABLE stocks (
    symbol VARCHAR(10) PRIMARY KEY,
    company_name VARCHAR(255) NOT NULL,
    last_price BIGINT NOT NULL,
    bid_price BIGINT,
    ask_price BIGINT,
    volume BIGINT DEFAULT 0,
    market_cap DECIMAL(20,2),
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

### Redis Data Structures

#### Session Storage
```
Key: session:{session_token}
Type: Hash
TTL: 15 minutes (access token)
Fields:
  - user_id
  - email
  - ip_address
  - created_at
  - last_activity
```

#### Trading Token
```
Key: trading:{trading_token}
Type: Hash
TTL: 8 hours
Fields:
  - user_id
  - session_token
  - balance
  - positions (JSON)
```

#### Market Data Cache
```
Key: market:{symbol}
Type: Hash
TTL: 60 seconds
Fields:
  - last_price
  - bid_price
  - ask_price
  - volume
  - timestamp
```

#### Rate Limiting
```
Key: ratelimit:{user_id}:{endpoint}
Type: String (counter)
TTL: 60 seconds
Value: request count
```

---

## API Design

### REST API (Next.js Backend)

#### Authentication Endpoints

```typescript
POST /api/auth/signup
Request:
{
  "email": "user@example.com",
  "password": "SecurePass123!",
  "firstName": "John",
  "lastName": "Doe",
  "country": "US"
}
Response:
{
  "success": true,
  "userId": "uuid",
  "message": "Account created successfully"
}

POST /api/auth/login
Request:
{
  "email": "user@example.com",
  "password": "SecurePass123!"
}
Response:
{
  "success": true,
  "requiresTwoFactor": true,
  "twoFactorType": "totp"
}

POST /api/auth/verify-2fa
Request:
{
  "email": "user@example.com",
  "code": "123456"
}
Response:
{
  "success": true,
  "sessionToken": "jwt_token_here",
  "tradingToken": "trading_token_here",
  "expiresIn": 900,
  "tradingExpiresIn": 28800
}

POST /api/auth/logout
Headers: Authorization: Bearer {sessionToken}
Response:
{
  "success": true,
  "message": "Logged out successfully"
}
```

### gRPC API (Trading Engine)

#### Order Management

```protobuf
message OrderRequest {
  string order_id = 1;
  string user_id = 2;
  string symbol = 3;
  OrderSide side = 4;      // BUY or SELL
  OrderType type = 5;      // MARKET, LIMIT, IOC, FOK
  int64 quantity = 6;
  double price = 7;        // Converted to fixed-point internally
}

message OrderResponse {
  string order_id = 1;
  OrderStatus status = 2;
  string message = 3;
  int64 filled_quantity = 4;
  double avg_fill_price = 5;
}

message OrderStatusRequest {
  string order_id = 1;
  string user_id = 2;
}

message OrderStatusResponse {
  string order_id = 1;
  OrderStatus status = 2;
  int64 quantity = 3;
  int64 filled_quantity = 4;
  double price = 5;
  double avg_fill_price = 6;
}
```

#### Market Data Streaming

```protobuf
message MarketDataRequest {
  repeated string symbols = 1;
}

message MarketDataUpdate {
  string symbol = 1;
  double last_price = 2;
  double bid_price = 3;
  double ask_price = 4;
  int64 volume = 5;
  int64 timestamp_ms = 6;
  repeated PriceLevel bids = 7;
  repeated PriceLevel asks = 8;
}

message PriceLevel {
  double price = 1;
  int64 quantity = 2;
}

// Streaming RPC
rpc StreamMarketData(MarketDataRequest) 
    returns (stream MarketDataUpdate);
```

#### Index Streaming

```protobuf
message MarketIndexRequest {
  string index_name = 1;  // e.g., "TOP10"
}

message MarketIndexUpdate {
  string index_name = 1;
  double index_value = 2;
  double change_percent = 3;
  repeated IndexConstituent constituents = 4;
  int64 timestamp_ms = 5;
}

message IndexConstituent {
  string symbol = 1;
  double last_price = 2;
  double weight = 3;
  double contribution = 4;
  double change_percent = 5;
}

rpc StreamMarketIndex(MarketIndexRequest) 
    returns (stream MarketIndexUpdate);
```

---

## Deployment Architecture

### Development Environment

```bash
# Start databases
./start_databases.sh start

# Build C++ engine
cd stock-exchange-engine/build
cmake ..
make -j$(nproc)

# Run trading engine
./stock_engine

# Start frontend (separate terminal)
cd stockexchange-frontend
npm run dev
```

### Production Deployment

#### Docker Compose Configuration

```yaml
version: '3.8'

services:
  postgres:
    image: postgres:18
    environment:
      POSTGRES_DB: stockexchange
      POSTGRES_USER: ${DB_USER}
      POSTGRES_PASSWORD: ${DB_PASSWORD}
    volumes:
      - postgres_data:/var/lib/postgresql/data
    ports:
      - "5432:5432"
    restart: unless-stopped

  redis:
    image: redis:7-alpine
    ports:
      - "6379:6379"
    volumes:
      - redis_data:/data
    restart: unless-stopped

  trading-engine:
    build:
      context: ./stock-exchange-engine
      dockerfile: Dockerfile
    depends_on:
      - postgres
      - redis
    ports:
      - "50051:50051"  # gRPC
      - "8080:8080"    # TCP
    environment:
      DB_CONNECTION: "postgresql://${DB_USER}:${DB_PASSWORD}@postgres:5432/stockexchange"
      REDIS_HOST: redis
      REDIS_PORT: 6379
    restart: unless-stopped

  frontend:
    build:
      context: ./stockexchange-frontend
      dockerfile: Dockerfile
    depends_on:
      - postgres
      - redis
      - trading-engine
    ports:
      - "3000:3000"
    environment:
      DATABASE_URL: "postgresql://${DB_USER}:${DB_PASSWORD}@postgres:5432/stockexchange"
      REDIS_HOST: redis
      REDIS_PORT: 6379
      GRPC_HOST: trading-engine
      GRPC_PORT: 50051
    restart: unless-stopped

volumes:
  postgres_data:
  redis_data:
```

#### Kubernetes Deployment (Optional)

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: trading-engine
spec:
  replicas: 3
  selector:
    matchLabels:
      app: trading-engine
  template:
    metadata:
      labels:
        app: trading-engine
    spec:
      containers:
      - name: trading-engine
        image: aurex/trading-engine:latest
        ports:
        - containerPort: 50051
          name: grpc
        - containerPort: 8080
          name: tcp
        resources:
          requests:
            memory: "1Gi"
            cpu: "2000m"
          limits:
            memory: "4Gi"
            cpu: "4000m"
        env:
        - name: DB_CONNECTION
          valueFrom:
            secretKeyRef:
              name: db-secret
              key: connection-string
        - name: REDIS_HOST
          value: redis-service
---
apiVersion: v1
kind: Service
metadata:
  name: trading-engine-service
spec:
  selector:
    app: trading-engine
  ports:
  - name: grpc
    port: 50051
    targetPort: 50051
  - name: tcp
    port: 8080
    targetPort: 8080
  type: LoadBalancer
```

### System Requirements (Production)

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | 8 cores | 16+ cores |
| RAM | 16GB | 32GB+ |
| Storage | 500GB SSD | 1TB+ NVMe SSD |
| Network | 1Gbps | 10Gbps |
| OS | Ubuntu 20.04+ | Ubuntu 22.04 LTS |

---

## Future Enhancements

### Planned Features

1. **Advanced Order Types**:
   - Stop-Loss orders
   - Trailing Stop orders
   - Bracket orders
   - OCO (One-Cancels-Other)

2. **Risk Management**:
   - Position limits per user
   - Daily loss limits
   - Margin trading
   - Short selling

3. **Analytics Dashboard**:
   - Historical charts (candlestick, line)
   - Technical indicators
   - Volume analysis
   - P&L reporting

4. **Mobile Applications**:
   - iOS native app
   - Android native app
   - Push notifications for price alerts

5. **Market Maker Program**:
   - Liquidity incentives
   - Rebate structure
   - API for algorithmic trading

6. **Options Trading**:
   - Call/Put options
   - Greeks calculation
   - Option chains
   - Volatility surfaces

7. **Regulatory Compliance**:
   - Trade reporting (SEC/FINRA)
   - Audit trails
   - Compliance monitoring
   - KYC/AML automation

---

## Appendix

### Performance Tuning Guide

#### Linux Kernel Parameters
```bash
# Increase network buffer sizes
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.wmem_max=134217728

# Reduce TCP latency
sudo sysctl -w net.ipv4.tcp_low_latency=1

# Increase file descriptors
ulimit -n 65535
```

#### PostgreSQL Configuration
```postgresql
# postgresql.conf optimizations
shared_buffers = 4GB
effective_cache_size = 12GB
maintenance_work_mem = 1GB
checkpoint_completion_target = 0.9
wal_buffers = 16MB
default_statistics_target = 100
random_page_cost = 1.1
effective_io_concurrency = 200
work_mem = 64MB
min_wal_size = 2GB
max_wal_size = 8GB
max_worker_processes = 8
max_parallel_workers_per_gather = 4
max_parallel_workers = 8
```

#### Redis Configuration
```redis
# redis.conf optimizations
maxmemory 8gb
maxmemory-policy allkeys-lru
save ""  # Disable persistence for cache (optional)
appendonly no
```

### Glossary

- **FIFO**: First In, First Out - order matching priority
- **SPSC**: Single Producer Single Consumer queue
- **MPSC**: Multiple Producer Single Consumer queue
- **IOC**: Immediate or Cancel order type
- **FOK**: Fill or Kill order type
- **TOTP**: Time-based One-Time Password (2FA)
- **JWT**: JSON Web Token
- **gRPC**: Google Remote Procedure Call
- **Fixed-Point**: Integer representation of decimal numbers
- **Order Book**: List of buy and sell orders for a security
- **Latency**: Time delay between action and response
- **Throughput**: Number of operations per unit time

---

## License

Copyright © 2025 Aurex Stock Exchange. All rights reserved.

---

**Document Version**: 1.0  
**Last Updated**: October 10, 2025  
**Authors**: Aurex Engineering Team

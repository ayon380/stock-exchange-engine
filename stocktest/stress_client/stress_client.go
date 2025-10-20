package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// Stress client configuration
type StressConfig struct {
	FrontendURL      string
	EngineAddr       string
	NumUsers         int
	OrdersPerUser    int
	Concurrency      int
	OrderConcurrency int
	TestDuration     time.Duration
	Symbols          []string
}

// TCP Protocol Constants (matching TCPServer.h)
const (
	MessageTypeLoginRequest  = 1
	MessageTypeLoginResponse = 2
	MessageTypeSubmitOrder   = 3
	MessageTypeOrderResponse = 4
	MessageTypeHeartbeat     = 5
	MessageTypeHeartbeatAck  = 6
	OrderSideBuy             = 0
	OrderSideSell            = 1
	OrderTypeMarket          = 0
	OrderTypeLimit           = 1
)

// Binary protocol structures matching C++ implementation
type BinaryLoginRequestBody struct {
	Type     uint8
	TokenLen uint32
}

type BinaryLoginResponse struct {
	MessageLength uint32
	Type          uint8
	Success       uint8
	MessageLen    uint32
}

type BinaryOrderResponse struct {
	MessageLength uint32
	Type          uint8
	OrderIdLen    uint32
	Accepted      uint8
	MessageLen    uint32
}

// Frontend API types
type SignupRequest struct {
	Email         string `json:"email"`
	Password      string `json:"password"`
	FirstName     string `json:"firstName"`
	LastName      string `json:"lastName"`
	Country       string `json:"country"`
	TwoFactorType string `json:"twoFactorType,omitempty"`
}

type LoginRequest struct {
	Email         string `json:"email"`
	Password      string `json:"password"`
	TwoFactorCode string `json:"twoFactorCode,omitempty"`
}

type AuthResponse struct {
	Message string `json:"message"`
	User    struct {
		ID    string `json:"id"`
		Email string `json:"email"`
	} `json:"user"`
	Tokens struct {
		SessionToken     string `json:"sessionToken"`
		TradingToken     string `json:"tradingToken"`
		ExpiresIn        int    `json:"expiresIn"`
		TradingExpiresIn int    `json:"tradingExpiresIn"`
	} `json:"tokens"`
}

// Global stats
type StressStats struct {
	UsersCreated    int64
	UsersLoggedIn   int64
	OrdersSubmitted int64
	OrdersAccepted  int64
	Errors          int64
	// Latency tracking (in nanoseconds)
	SignupLatencies []time.Duration
	LoginLatencies  []time.Duration
	OrderLatencies  []time.Duration
	// Live latency stats
	MinOrderLatency time.Duration
	MaxOrderLatency time.Duration
	AvgOrderLatency time.Duration
}

var stats StressStats
var statsMutex sync.Mutex

// Helper function to calculate average latency
func averageLatency(latencies []time.Duration) time.Duration {
	if len(latencies) == 0 {
		return 0
	}
	var sum time.Duration
	for _, lat := range latencies {
		sum += lat
	}
	return sum / time.Duration(len(latencies))
}

// Live status reporter
func startLiveReporter(config StressConfig, startTime time.Time, ctx context.Context) {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			statsMutex.Lock()
			currentStats := stats
			statsMutex.Unlock()

			elapsed := time.Since(startTime)
			usersCreated := atomic.LoadInt64(&currentStats.UsersCreated)
			usersLoggedIn := atomic.LoadInt64(&currentStats.UsersLoggedIn)
			ordersSubmitted := atomic.LoadInt64(&currentStats.OrdersSubmitted)
			ordersAccepted := atomic.LoadInt64(&currentStats.OrdersAccepted)
			errors := atomic.LoadInt64(&currentStats.Errors)

			avgSignup := averageLatency(currentStats.SignupLatencies)
			avgLogin := averageLatency(currentStats.LoginLatencies)
			_ = avgSignup // Keep for future use
			_ = avgLogin  // Keep for future use

			ordersPerSec := float64(ordersSubmitted) / elapsed.Seconds()

			log.Printf("=== LIVE STATUS (%.1fs) ===", elapsed.Seconds())
			log.Printf("Users: %d created, %d logged in", usersCreated, usersLoggedIn)
			log.Printf("Orders: %d submitted, %d accepted (%.1f%%)", ordersSubmitted, ordersAccepted,
				float64(ordersAccepted)/float64(ordersSubmitted)*100)
			log.Printf("Throughput: %.1f orders/sec", ordersPerSec)
			log.Printf("Errors: %d", errors)
			log.Printf("Order Latencies - Min: %.2fms, Max: %.2fms, Avg: %.2fms",
				float64(currentStats.MinOrderLatency.Nanoseconds())/1e6,
				float64(currentStats.MaxOrderLatency.Nanoseconds())/1e6,
				float64(currentStats.AvgOrderLatency.Nanoseconds())/1e6)
			log.Printf("Progress: %d/%d users completed", usersLoggedIn, config.NumUsers)
			log.Println("==========================")
		}
	}
}

// HTTP client for frontend
func createUser(frontendURL string, userNum int) (string, string, error) {
	email := fmt.Sprintf("stress%d_%d@example.com", userNum, time.Now().UnixNano())
	password := "TestPass123!"

	signupReq := SignupRequest{
		Email:         email,
		Password:      password,
		FirstName:     fmt.Sprintf("Stress%d", userNum),
		LastName:      "User",
		Country:       "US",
		TwoFactorType: "email",
	}

	jsonData, err := json.Marshal(signupReq)
	if err != nil {
		return "", "", fmt.Errorf("failed to marshal signup request: %w", err)
	}

	start := time.Now()
	resp, err := http.Post(frontendURL+"/api/auth/stress-signup", "application/json", bytes.NewBuffer(jsonData))
	latency := time.Since(start)

	if err != nil {
		return "", "", fmt.Errorf("signup request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		body, _ := io.ReadAll(resp.Body)
		return "", "", fmt.Errorf("signup failed with status %d: %s", resp.StatusCode, string(body))
	}

	statsMutex.Lock()
	stats.SignupLatencies = append(stats.SignupLatencies, latency)
	atomic.AddInt64(&stats.UsersCreated, 1)
	statsMutex.Unlock()

	return email, password, nil
}

func loginUser(frontendURL, email, password string) (string, error) {
	loginReq := LoginRequest{
		Email:    email,
		Password: password,
	}

	jsonData, err := json.Marshal(loginReq)
	if err != nil {
		return "", fmt.Errorf("failed to marshal login request: %w", err)
	}

	start := time.Now()
	resp, err := http.Post(frontendURL+"/api/auth/login", "application/json", bytes.NewBuffer(jsonData))
	latency := time.Since(start)

	if err != nil {
		return "", fmt.Errorf("login request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("login failed with status %d: %s", resp.StatusCode, string(body))
	}

	var authResp AuthResponse
	if err := json.NewDecoder(resp.Body).Decode(&authResp); err != nil {
		return "", fmt.Errorf("failed to decode login response: %w", err)
	}

	// Validate that we got a trading token
	if authResp.Tokens.TradingToken == "" {
		return "", fmt.Errorf("empty trading token received from login")
	}

	statsMutex.Lock()
	stats.LoginLatencies = append(stats.LoginLatencies, latency)
	atomic.AddInt64(&stats.UsersLoggedIn, 1)
	statsMutex.Unlock()

	log.Printf("User %s logged in successfully with token: %s...", email, authResp.Tokens.TradingToken[:20])
	return authResp.Tokens.TradingToken, nil
}

// authenticateTCP handles the login handshake for TCP connections.
func authenticateTCP(conn net.Conn, token string) error {
	// Prepare the login request message
	tokenBytes := []byte(token)
	bodyLen := 1 + 4 + len(tokenBytes) // type(1) + token_len(4) + token
	totalLen := 4 + bodyLen            // message_length(4) + body

	buf := new(bytes.Buffer)
	// Write message length (total message size including this field)
	binary.Write(buf, binary.BigEndian, uint32(totalLen))
	// Write message type
	buf.WriteByte(byte(MessageTypeLoginRequest))
	// Write token length
	binary.Write(buf, binary.BigEndian, uint32(len(tokenBytes)))
	// Write token
	buf.Write(tokenBytes)

	// Send the request
	if _, err := conn.Write(buf.Bytes()); err != nil {
		return fmt.Errorf("failed to send login request: %w", err)
	}

	// Read the response header: message_length(4)
	var messageLength uint32
	if err := binary.Read(conn, binary.BigEndian, &messageLength); err != nil {
		return fmt.Errorf("failed to read login response length: %w", err)
	}

	// Read the response body (excluding the 4-byte length we already read)
	bodySize := messageLength - 4
	respBody := make([]byte, bodySize)
	if _, err := io.ReadFull(conn, respBody); err != nil {
		return fmt.Errorf("failed to read login response body: %w", err)
	}

	// Parse response: type(1) + success(1) + message_len(4) + message
	if len(respBody) < 6 {
		return fmt.Errorf("login response too short: %d bytes", len(respBody))
	}

	msgType := respBody[0]
	success := respBody[1]
	messageLen := binary.BigEndian.Uint32(respBody[2:6])

	if msgType != MessageTypeLoginResponse {
		return fmt.Errorf("unexpected response type: %d", msgType)
	}

	// Read message text if present
	var message string
	if messageLen > 0 && len(respBody) >= 6+int(messageLen) {
		message = string(respBody[6 : 6+messageLen])
	}

	// Check for success
	if success != 1 {
		return fmt.Errorf("authentication failed: %s", message)
	}

	log.Printf("TCP authentication successful: %s", message)
	return nil
}

// Helper to convert double to network byte order (as uint64)
func doubleToNetworkBytes(val float64) uint64 {
	bits := *(*uint64)(unsafe.Pointer(&val))
	// Convert to big endian
	return uint64(binary.BigEndian.Uint64((*[8]byte)(unsafe.Pointer(&bits))[:]))
}

// Submit order via TCP binary protocol
func submitOrderTCP(conn net.Conn, userID, symbol string, side, orderType int, quantity int64, price float64) error {
	orderId := fmt.Sprintf("order_%d_%d", time.Now().UnixNano(), rand.Int())

	buf := &bytes.Buffer{}

	// Prepare binary order request
	orderIdBytes := []byte(orderId)
	userIdBytes := []byte(userID)
	symbolBytes := []byte(symbol)

	// Calculate total length: message_length(4) + type(1) + order_id_len(4) + user_id_len(4) +
	// symbol_len(4) + side(1) + order_type(1) + quantity(8) + price(8) + timestamp_ms(8) + strings
	bodyLen := 1 + 4 + 4 + 4 + 1 + 1 + 8 + 8 + 8 + len(orderIdBytes) + len(userIdBytes) + len(symbolBytes)
	totalLen := 4 + bodyLen

	// Write message length
	binary.Write(buf, binary.BigEndian, uint32(totalLen))
	// Write message type
	buf.WriteByte(MessageTypeSubmitOrder)
	// Write string lengths
	binary.Write(buf, binary.BigEndian, uint32(len(orderIdBytes)))
	binary.Write(buf, binary.BigEndian, uint32(len(userIdBytes)))
	binary.Write(buf, binary.BigEndian, uint32(len(symbolBytes)))
	// Write order parameters
	buf.WriteByte(uint8(side))
	buf.WriteByte(uint8(orderType))
	binary.Write(buf, binary.BigEndian, uint64(quantity))

	// Write price as double in network byte order
	priceBits := *(*uint64)(unsafe.Pointer(&price))
	binary.Write(buf, binary.BigEndian, priceBits)

	// Write timestamp
	binary.Write(buf, binary.BigEndian, uint64(time.Now().UnixMilli()))
	// Write strings
	buf.Write(orderIdBytes)
	buf.Write(userIdBytes)
	buf.Write(symbolBytes)

	start := time.Now()
	if _, err := conn.Write(buf.Bytes()); err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("TCP write failed: %w", err)
	}

	// Read response: message_length(4)
	var messageLength uint32
	if err := binary.Read(conn, binary.BigEndian, &messageLength); err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("TCP read response length failed: %w", err)
	}

	// Read response body (excluding the 4-byte length we already read)
	bodySize := messageLength - 4
	respBody := make([]byte, bodySize)
	if _, err := io.ReadFull(conn, respBody); err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("TCP read response body failed: %w", err)
	}

	// Parse response: type(1) + order_id_len(4) + accepted(1) + message_len(4) + order_id + message
	if len(respBody) < 10 {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("order response too short: %d bytes", len(respBody))
	}

	msgType := respBody[0]
	orderIdLen := binary.BigEndian.Uint32(respBody[1:5])
	accepted := respBody[5]
	messageLen := binary.BigEndian.Uint32(respBody[6:10])

	latency := time.Since(start)

	if msgType != MessageTypeOrderResponse {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("unexpected response type: %d", msgType)
	}

	// Extract message if present
	var message string
	offset := 10 + int(orderIdLen)
	if len(respBody) >= offset+int(messageLen) {
		message = string(respBody[offset : offset+int(messageLen)])
	}

	// Update stats
	statsMutex.Lock()
	stats.OrderLatencies = append(stats.OrderLatencies, latency)
	atomic.AddInt64(&stats.OrdersSubmitted, 1)

	if accepted == 1 {
		atomic.AddInt64(&stats.OrdersAccepted, 1)
	} else {
		// Log rejection for debugging
		if rand.Intn(100) < 5 { // Log 5% of rejections to avoid spam
			log.Printf("Order rejected: %s", message)
		}
	}

	// Update live latency stats
	if stats.MinOrderLatency == 0 || latency < stats.MinOrderLatency {
		stats.MinOrderLatency = latency
	}
	if latency > stats.MaxOrderLatency {
		stats.MaxOrderLatency = latency
	}
	// Calculate running average
	totalOrders := len(stats.OrderLatencies)
	if totalOrders > 0 {
		var sum time.Duration
		for _, lat := range stats.OrderLatencies {
			sum += lat
		}
		stats.AvgOrderLatency = sum / time.Duration(totalOrders)
	}
	statsMutex.Unlock()

	return nil
}

// Worker function for each user (legacy, without context)
func userWorker(config StressConfig, userID int, wg *sync.WaitGroup) {
	userWorkerWithContext(context.Background(), config, userID, wg)
}

// Worker function for each user with context support
func userWorkerWithContext(ctx context.Context, config StressConfig, userID int, wg *sync.WaitGroup) {
	defer wg.Done()

	// Check if already cancelled
	select {
	case <-ctx.Done():
		return
	default:
	}

	// Create user
	email, password, err := createUser(config.FrontendURL, userID)
	if err != nil {
		log.Printf("Failed to create user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	// Check cancellation
	select {
	case <-ctx.Done():
		return
	default:
	}

	// Login to get trading token
	tradingToken, err := loginUser(config.FrontendURL, email, password)
	if err != nil {
		log.Printf("Failed to login user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	log.Printf("User %d authenticated successfully", userID)

	// Check cancellation
	select {
	case <-ctx.Done():
		log.Printf("User %d: Cancelled before TCP connection", userID)
		return
	default:
	}

	// Connect to engine via TCP with TLS
	tlsConfig := &tls.Config{
		InsecureSkipVerify: true, // Skip certificate verification for testing
	}

	conn, err := tls.Dial("tcp", config.EngineAddr, tlsConfig)
	if err != nil {
		log.Printf("Failed to connect to TLS TCP server: %v", err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}
	defer func() {
		conn.Close()
		log.Printf("User %d: Connection closed", userID)
	}()

	// Authenticate TCP connection with trading token
	if err := authenticateTCP(conn, tradingToken); err != nil {
		log.Printf("Failed to authenticate TCP connection for user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	// Submit orders concurrently
	var orderWg sync.WaitGroup
	orderSem := make(chan struct{}, config.OrderConcurrency)

	// Use a mutex to serialize TCP writes on the same connection
	var connMutex sync.Mutex

	// Track if we should stop
	stopOrders := make(chan struct{})

	// Monitor context cancellation
	go func() {
		<-ctx.Done()
		close(stopOrders)
	}()

orderLoop:
	for i := 0; i < config.OrdersPerUser; i++ {
		// Check if we should stop
		select {
		case <-stopOrders:
			log.Printf("User %d: Stopping order submission (cancelled)", userID)
			break orderLoop
		default:
		}

		orderWg.Add(1)
		orderSem <- struct{}{} // Acquire

		go func(orderNum int) {
			defer func() {
				<-orderSem // Release
				orderWg.Done()
			}()

			// Check cancellation before submitting
			select {
			case <-stopOrders:
				return
			default:
			}

			symbol := config.Symbols[rand.Intn(len(config.Symbols))]
			side := rand.Intn(2)      // Buy or Sell
			orderType := rand.Intn(2) // Market or Limit
			quantity := int64(rand.Intn(100) + 1)
			price := 100.0 + rand.Float64()*100.0

			// Lock the connection for this order submission
			connMutex.Lock()
			err := submitOrderTCP(conn, fmt.Sprintf("user_%d", userID), symbol, side, orderType, quantity, price)
			connMutex.Unlock()

			if err != nil {
				// Don't log errors if we're shutting down
				select {
				case <-stopOrders:
					return
				default:
					log.Printf("Order submission failed for user %d: %v", userID, err)
				}
			}
		}(i)
	}

	orderWg.Wait()
}

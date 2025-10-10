package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	pb "stress_client/pb"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// Stress client configuration
type StressConfig struct {
	FrontendURL      string
	EngineAddr       string
	UseGRPC          bool
	NumUsers         int
	OrdersPerUser    int
	Concurrency      int
	OrderConcurrency int
	TestDuration     time.Duration
	Symbols          []string
}

// TCP Protocol Constants (copied from test.go)
const (
	MessageTypeLoginRequest  = 1
	MessageTypeLoginResponse = 2
	MessageTypeSubmitOrder   = 3
	OrderSideBuy             = 0
	OrderSideSell            = 1
	OrderTypeMarket          = 0
	OrderTypeLimit           = 1
)

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
	SessionToken     string `json:"sessionToken"`
	TradingToken     string `json:"tradingToken"`
	ExpiresIn        int    `json:"expiresIn"`
	TradingExpiresIn int    `json:"tradingExpiresIn"`
	User             struct {
		ID    string `json:"id"`
		Email string `json:"email"`
	} `json:"user"`
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
func startLiveReporter(config StressConfig, startTime time.Time) {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
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

	statsMutex.Lock()
	stats.LoginLatencies = append(stats.LoginLatencies, latency)
	atomic.AddInt64(&stats.UsersLoggedIn, 1)
	statsMutex.Unlock()

	return authResp.TradingToken, nil
}

// authenticateTCP handles the login handshake for TCP connections.
func authenticateTCP(conn net.Conn, token string) error {
	// Prepare the login request message
	tokenBytes := []byte(token)
	reqLen := 4 + 1 + 4 + len(tokenBytes)

	buf := new(bytes.Buffer)
	binary.Write(buf, binary.BigEndian, uint32(reqLen))
	buf.WriteByte(byte(MessageTypeLoginRequest))
	binary.Write(buf, binary.BigEndian, uint32(len(tokenBytes)))
	buf.Write(tokenBytes)

	// Send the request
	if _, err := conn.Write(buf.Bytes()); err != nil {
		return fmt.Errorf("failed to send login request: %w", err)
	}

	// Read the response header: total_len(4) + type(1) + success(1) + msg_len(4) = 10 bytes
	respHeader := make([]byte, 10)
	if _, err := io.ReadFull(conn, respHeader); err != nil {
		return fmt.Errorf("failed to read login response header: %w", err)
	}

	// Parse the header
	success := respHeader[5] == 1 // 5th byte (0-indexed) is the success flag
	messageLen := binary.BigEndian.Uint32(respHeader[6:10])

	// Read and discard the message body to clear the stream
	if messageLen > 0 {
		if _, err := io.CopyN(io.Discard, conn, int64(messageLen)); err != nil {
			return fmt.Errorf("failed to read login response body: %w", err)
		}
	}

	// Check for success
	if !success {
		return errors.New("authentication failed by server")
	}

	return nil
}

// Submit order via gRPC
func submitOrderGRPC(client pb.StockServiceClient, userID, symbol string, side, orderType int, quantity int64, price float64) error {
	req := &pb.OrderRequest{
		OrderId:     fmt.Sprintf("order_%d_%d", time.Now().UnixNano(), rand.Int()),
		UserId:      userID,
		Symbol:      symbol,
		Side:        pb.OrderSide(side),
		Type:        pb.OrderType(orderType),
		Quantity:    quantity,
		Price:       price,
		TimestampMs: time.Now().UnixMilli(),
	}

	start := time.Now()
	resp, err := client.SubmitOrder(context.Background(), req)
	latency := time.Since(start)

	if err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("gRPC submit order failed: %w", err)
	}

	statsMutex.Lock()
	stats.OrderLatencies = append(stats.OrderLatencies, latency)
	atomic.AddInt64(&stats.OrdersSubmitted, 1)
	if resp.Accepted {
		atomic.AddInt64(&stats.OrdersAccepted, 1)
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

// Submit order via TCP binary protocol
func submitOrderTCP(conn net.Conn, userID, symbol string, side, orderType int, quantity int64, price float64) error {
	orderId := fmt.Sprintf("order_%d_%d", time.Now().UnixNano(), rand.Int())

	buf := &bytes.Buffer{}

	// Prepare binary order request
	orderIdBytes := []byte(orderId)
	userIdBytes := []byte(userID)
	symbolBytes := []byte(symbol)

	totalLen := 4 + 1 + 4 + 4 + 4 + 1 + 1 + 8 + 8 + 8 + len(orderIdBytes) + len(userIdBytes) + len(symbolBytes)

	binary.Write(buf, binary.BigEndian, uint32(totalLen))
	buf.WriteByte(MessageTypeSubmitOrder)
	binary.Write(buf, binary.BigEndian, uint32(len(orderIdBytes)))
	binary.Write(buf, binary.BigEndian, uint32(len(userIdBytes)))
	binary.Write(buf, binary.BigEndian, uint32(len(symbolBytes)))
	buf.WriteByte(uint8(side))
	buf.WriteByte(uint8(orderType))
	binary.Write(buf, binary.BigEndian, uint64(quantity))
	binary.Write(buf, binary.BigEndian, price)
	binary.Write(buf, binary.BigEndian, uint64(time.Now().UnixMilli()))
	buf.Write(orderIdBytes)
	buf.Write(userIdBytes)
	buf.Write(symbolBytes)

	start := time.Now()
	if _, err := conn.Write(buf.Bytes()); err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("TCP write failed: %w", err)
	}

	// Read response header
	respHeader := make([]byte, 4)
	if _, err := io.ReadFull(conn, respHeader); err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("TCP read header failed: %w", err)
	}

	respLen := binary.BigEndian.Uint32(respHeader)
	if respLen < 4 {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("invalid response length: %d", respLen)
	}

	// Read and discard response body
	if _, err := io.CopyN(io.Discard, conn, int64(respLen-4)); err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("TCP read body failed: %w", err)
	}

	latency := time.Since(start)

	statsMutex.Lock()
	stats.OrderLatencies = append(stats.OrderLatencies, latency)
	atomic.AddInt64(&stats.OrdersSubmitted, 1)
	atomic.AddInt64(&stats.OrdersAccepted, 1) // TCP doesn't return acceptance status, assume accepted

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
} // Worker function for each user
func userWorker(config StressConfig, userID int, wg *sync.WaitGroup) {
	defer wg.Done()

	// Create user
	email, password, err := createUser(config.FrontendURL, userID)
	if err != nil {
		log.Printf("Failed to create user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	// Login (for session management, not required for gRPC orders)
	_, err = loginUser(config.FrontendURL, email, password)
	if err != nil {
		log.Printf("Failed to login user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	log.Printf("User %d authenticated successfully", userID)

	if config.UseGRPC {
		// Connect to engine via gRPC
		conn, err := grpc.Dial(config.EngineAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			log.Printf("Failed to connect to gRPC server: %v", err)
			atomic.AddInt64(&stats.Errors, 1)
			return
		}
		defer conn.Close()

		client := pb.NewStockServiceClient(conn)

		// Submit orders concurrently
		var orderWg sync.WaitGroup
		orderSem := make(chan struct{}, config.OrderConcurrency)

		for i := 0; i < config.OrdersPerUser; i++ {
			orderWg.Add(1)
			orderSem <- struct{}{} // Acquire

			go func(orderNum int) {
				defer func() {
					<-orderSem // Release
					orderWg.Done()
				}()

				symbol := config.Symbols[rand.Intn(len(config.Symbols))]
				side := rand.Intn(2)      // Buy or Sell
				orderType := rand.Intn(2) // Market or Limit
				quantity := int64(rand.Intn(100) + 1)
				price := 100.0 + rand.Float64()*100.0

				if err := submitOrderGRPC(client, fmt.Sprintf("user_%d", userID), symbol, side, orderType, quantity, price); err != nil {
					log.Printf("Order submission failed for user %d: %v", userID, err)
				}
			}(i)
		}

		orderWg.Wait()
	} else {
		// Connect to engine via TCP
		conn, err := net.Dial("tcp", config.EngineAddr)
		if err != nil {
			log.Printf("Failed to connect to TCP server: %v", err)
			atomic.AddInt64(&stats.Errors, 1)
			return
		}
		defer conn.Close()

		// For TCP, we need authentication token - use a dummy token for now
		// In a real implementation, you'd get this from login
		authToken := "dummy_token_for_tcp"

		// Authenticate TCP connection
		if err := authenticateTCP(conn, authToken); err != nil {
			log.Printf("Failed to authenticate TCP connection for user %d: %v", userID, err)
			atomic.AddInt64(&stats.Errors, 1)
			return
		}

		// Submit orders
		for i := 0; i < config.OrdersPerUser; i++ {
			symbol := config.Symbols[rand.Intn(len(config.Symbols))]
			side := rand.Intn(2)      // Buy or Sell
			orderType := rand.Intn(2) // Market or Limit
			quantity := int64(rand.Intn(100) + 1)
			price := 100.0 + rand.Float64()*100.0

			if err := submitOrderTCP(conn, fmt.Sprintf("user_%d", userID), symbol, side, orderType, quantity, price); err != nil {
				log.Printf("Order submission failed for user %d: %v", userID, err)
			}
		}
	}
}

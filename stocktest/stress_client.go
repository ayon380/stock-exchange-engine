package stocktest
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	pb "stocktest/pb"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
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

// Stress client configuration
type StressConfig struct {
	FrontendURL   string
	EngineAddr    string
	UseGRPC       bool
	NumUsers      int
	OrdersPerUser int
	Concurrency   int
	TestDuration  time.Duration
	Symbols       []string
}

// Global stats
type StressStats struct {
	UsersCreated    int64
	UsersLoggedIn   int64
	OrdersSubmitted int64
	OrdersAccepted  int64
	Errors          int64
}

var stats StressStats
var statsMutex sync.Mutex

// HTTP client for frontend
func createUser(frontendURL string, userNum int) (string, string, error) {
	email := fmt.Sprintf("stress%d@example.com", userNum)
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

	resp, err := http.Post(frontendURL+"/api/auth/signup", "application/json", bytes.NewBuffer(jsonData))
	if err != nil {
		return "", "", fmt.Errorf("signup request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		body, _ := io.ReadAll(resp.Body)
		return "", "", fmt.Errorf("signup failed with status %d: %s", resp.StatusCode, string(body))
	}

	atomic.AddInt64(&stats.UsersCreated, 1)
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

	resp, err := http.Post(frontendURL+"/api/auth/login", "application/json", bytes.NewBuffer(jsonData))
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

	atomic.AddInt64(&stats.UsersLoggedIn, 1)
	return authResp.TradingToken, nil
}

// authenticateTCP handles the login handshake for TCP connections (if needed).
// Note: This function is kept for compatibility but not used in this stress client.
func authenticateTCP(conn interface{}, token string) error {
	// Implementation removed - use gRPC instead
	return fmt.Errorf("TCP authentication not implemented in this client")
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

	resp, err := client.SubmitOrder(context.Background(), req)
	if err != nil {
		atomic.AddInt64(&stats.Errors, 1)
		return fmt.Errorf("gRPC submit order failed: %w", err)
	}

	atomic.AddInt64(&stats.OrdersSubmitted, 1)
	if resp.Accepted {
		atomic.AddInt64(&stats.OrdersAccepted, 1)
	}

	return nil
}

// Worker function for each user
func userWorker(config StressConfig, userID int, wg *sync.WaitGroup) {
	defer wg.Done()

	// Create user
	email, password, err := createUser(config.FrontendURL, userID)
	if err != nil {
		log.Printf("Failed to create user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	// Login
	token, err := loginUser(config.FrontendURL, email, password)
	if err != nil {
		log.Printf("Failed to login user %d: %v", userID, err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}

	log.Printf("User %d authenticated successfully", userID)

	// Connect to engine via gRPC
	conn, err := grpc.Dial(config.EngineAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Printf("Failed to connect to gRPC server: %v", err)
		atomic.AddInt64(&stats.Errors, 1)
		return
	}
	defer conn.Close()

	client := pb.NewStockServiceClient(conn)

	// Submit orders
	for i := 0; i < config.OrdersPerUser; i++ {
		symbol := config.Symbols[rand.Intn(len(config.Symbols))]
		side := rand.Intn(2)      // Buy or Sell
		orderType := rand.Intn(2) // Market or Limit
		quantity := int64(rand.Intn(100) + 1)
		price := 100.0 + rand.Float64()*100.0

		if err := submitOrderGRPC(client, fmt.Sprintf("user_%d", userID), symbol, side, orderType, quantity, price); err != nil {
			log.Printf("Order submission failed for user %d: %v", userID, err)
		}

		// Small delay between orders
		time.Sleep(time.Millisecond * time.Duration(rand.Intn(100)))
	}
}

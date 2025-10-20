package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

func main() {
	config := StressConfig{}

	flag.StringVar(&config.FrontendURL, "frontend", "http://localhost:3000", "Frontend URL")
	flag.StringVar(&config.EngineAddr, "engine", "localhost:50052", "Engine TCP address (host:port)")
	flag.IntVar(&config.NumUsers, "users", 10, "Number of users to create")
	flag.IntVar(&config.OrdersPerUser, "orders", 100, "Orders per user")
	flag.IntVar(&config.Concurrency, "concurrency", 50, "Concurrent users")
	flag.IntVar(&config.OrderConcurrency, "order-concurrency", 10, "Concurrent orders per user")
	flag.DurationVar(&config.TestDuration, "duration", 5*time.Minute, "Test duration")
	flag.Parse()

	config.Symbols = []string{"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA"}

	log.Printf("Starting stress test with config: %+v", config)

	// Setup graceful shutdown with immediate exit
	ctx, cancel := context.WithCancel(context.Background())

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	// Track if we should force exit
	forceExit := false

	// Handle shutdown signal - IMMEDIATE EXIT
	go func() {
		<-sigChan
		log.Println("\n🛑 Received shutdown signal, exiting immediately...")
		cancel()
		forceExit = true

		// Wait 500ms for graceful cleanup, then force exit
		time.Sleep(500 * time.Millisecond)
		log.Println("Force exiting...")
		os.Exit(0)
	}()

	var wg sync.WaitGroup
	semaphore := make(chan struct{}, config.Concurrency)

	startTime := time.Now()

	// Start live reporter
	go startLiveReporter(config, startTime, ctx)

	// Launch workers
	workersDone := make(chan bool, 1)
	go func() {
		for i := 1; i <= config.NumUsers; i++ {
			// Check if we should exit early
			if forceExit {
				break
			}

			wg.Add(1)
			semaphore <- struct{}{} // Acquire

			go func(userID int) {
				defer func() { <-semaphore }() // Release
				userWorkerWithContext(ctx, config, userID, &wg)
			}(i)
		}

		wg.Wait()
		workersDone <- true
	}()

	// Wait for completion or cancellation
	select {
	case <-workersDone:
		// Normal completion
	case <-ctx.Done():
		// Cancelled by signal
		log.Println("Waiting for active connections to close...")
		// Give 500ms for cleanup
		time.Sleep(500 * time.Millisecond)
	}

	if forceExit {
		log.Println("Exited by user signal")
		os.Exit(0)
	}

	duration := time.Since(startTime)

	// Final stats
	statsMutex.Lock()
	finalStats := stats
	statsMutex.Unlock()

	usersCreated := atomic.LoadInt64(&finalStats.UsersCreated)
	usersLoggedIn := atomic.LoadInt64(&finalStats.UsersLoggedIn)
	ordersSubmitted := atomic.LoadInt64(&finalStats.OrdersSubmitted)
	ordersAccepted := atomic.LoadInt64(&finalStats.OrdersAccepted)
	errors := atomic.LoadInt64(&finalStats.Errors)

	avgSignup := averageLatency(finalStats.SignupLatencies)
	avgLogin := averageLatency(finalStats.LoginLatencies)
	avgOrder := averageLatency(finalStats.OrderLatencies)

	ordersPerSec := float64(ordersSubmitted) / duration.Seconds()

	log.Printf("=== FINAL RESULTS ===")
	log.Printf("Test completed in %v", duration)
	log.Printf("Users: %d created, %d logged in", usersCreated, usersLoggedIn)
	log.Printf("Orders: %d submitted, %d accepted (%.1f%%)", ordersSubmitted, ordersAccepted,
		float64(ordersAccepted)/float64(ordersSubmitted)*100)
	log.Printf("Throughput: %.1f orders/sec", ordersPerSec)
	log.Printf("Errors: %d", errors)
	log.Printf("Average Latencies: Signup=%.2fms, Login=%.2fms, Order=%.2fms",
		float64(avgSignup.Nanoseconds())/1e6,
		float64(avgLogin.Nanoseconds())/1e6,
		float64(avgOrder.Nanoseconds())/1e6)
	log.Printf("=====================")
}

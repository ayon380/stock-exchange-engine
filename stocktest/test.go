package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"

	pb "stocktest/pb" // Assumes your module is 'stocktest' and generated files are in /pb
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

func main() {
	// --- Configuration via Command-Line Flags ---
	// Makes the tool much more flexible to use.
	serverAddr := flag.String("addr", "localhost:50051", "The server address in the format host:port")
	totalRequests := flag.Int("requests", 1_000_000, "Total number of requests to send")
	concurrency := flag.Int("concurrency", 50, "Number of concurrent goroutines")
	flag.Parse()

	log.Printf("Starting gRPC load test with configuration:")
	log.Printf("  - Server Address: %s", *serverAddr)
	log.Printf("  - Total Requests: %d", *totalRequests)
	log.Printf("  - Concurrency:    %d", *concurrency)
	log.Println("-------------------------------------------------")

	// --- gRPC Connection ---
	// Using the modern, non-deprecated way to specify insecure credentials.
	conn, err := grpc.Dial(*serverAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithBlock())
	if err != nil {
		log.Fatalf("dial failed: %v", err)
	}
	defer conn.Close()
	client := pb.NewStockServiceClient(conn)

	// --- Test Data and Metrics Setup ---
	symbols := []string{"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN", "META", "NVDA", "NFLX"}
	var wg sync.WaitGroup

	// Atomic counters for safe concurrent updates
	var sent, errors, sumLat int64
	var minLat int64 = 1<<63 - 1 // Initialize to max value
	var maxLat int64             // Initialize to 0

	// Semaphore to limit concurrency
	sem := make(chan struct{}, *concurrency)
	start := time.Now()

	// --- Real-time Progress Reporter ---
	// This goroutine prints stats periodically so you see what's happening.
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()
	done := make(chan bool)

	go func() {
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
				currentSent := atomic.LoadInt64(&sent)
				currentErrors := atomic.LoadInt64(&errors)
				currentSumLat := atomic.LoadInt64(&sumLat)

				// Avoid division by zero if no requests have completed yet
				if currentSent == 0 {
					continue
				}

				avg := float64(currentSumLat) / float64(currentSent)
				rps := float64(currentSent) / time.Since(start).Seconds()
				log.Printf("Progress: Sent=%d, Errors=%d, RPS=%.f, Avg Latency=%.1fµs",
					currentSent, currentErrors, rps, avg)
			}
		}
	}()

	// --- Main Request Loop ---
	for i := 0; i < *totalRequests; i++ {
		sem <- struct{}{}
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			defer func() { <-sem }() // Release semaphore slot when done

			t0 := time.Now()

			req := &pb.OrderRequest{
				OrderId:     fmt.Sprintf("o%d", i),
				UserId:      fmt.Sprintf("u%d", rand.Intn(1000)),
				Symbol:      symbols[rand.Intn(len(symbols))],
				Side:        pb.OrderSide(rand.Intn(2)),
				Type:        pb.OrderType(rand.Intn(4)),
				Quantity:    int64(rand.Intn(100) + 1),
				Price:       rand.Float64()*1000 + 1,
				TimestampMs: time.Now().UnixMilli(),
			}
			_, err := client.SubmitOrder(context.Background(), req)
			lat := time.Since(t0).Microseconds()

			// --- Handle results and update metrics ---
			if err != nil {
				atomic.AddInt64(&errors, 1)
				// Log the first 10 errors to avoid spamming the console
				if atomic.LoadInt64(&errors) <= 10 {
					log.Printf("ERROR: SubmitOrder failed: %v", err)
				}
				return // Don't process latency for failed requests
			}

			atomic.AddInt64(&sent, 1)
			atomic.AddInt64(&sumLat, lat)

			// Safely update min latency
			for {
				oldMin := atomic.LoadInt64(&minLat)
				if lat >= oldMin {
					break
				}
				if atomic.CompareAndSwapInt64(&minLat, oldMin, lat) {
					break
				}
			}

			// Safely update max latency
			for {
				oldMax := atomic.LoadInt64(&maxLat)
				if lat <= oldMax {
					break
				}
				if atomic.CompareAndSwapInt64(&maxLat, oldMax, lat) {
					break
				}
			}
		}(i)
	}

	wg.Wait()
	// Stop the progress reporter
	done <- true
	
	// --- Final Report ---
	dur := time.Since(start)
	totalSent := atomic.LoadInt64(&sent)
	totalErrors := atomic.LoadInt64(&errors)
	
	if totalSent == 0 {
		log.Println("\n--- FINAL REPORT ---")
		log.Printf("No requests were successfully sent. Total Errors: %d", totalErrors)
		log.Printf("Total time: %s", dur)
		return
	}

	avg := float64(atomic.LoadInt64(&sumLat)) / float64(totalSent)
	rps := float64(totalSent) / dur.Seconds()

	fmt.Printf("\n--- FINAL REPORT ---\n")
	fmt.Printf("Total Time:     %s\n", dur)
	fmt.Printf("Total Requests: %d\n", *totalRequests)
	fmt.Printf("Successful:     %d\n", totalSent)
	fmt.Printf("Errors:         %d\n", totalErrors)
	fmt.Printf("RPS (Overall):  %.2f\n", rps)
	fmt.Printf("Min Latency:    %dµs\n", atomic.LoadInt64(&minLat))
	fmt.Printf("Avg Latency:    %.1fµs\n", avg)
	fmt.Printf("Max Latency:    %dµs\n", atomic.LoadInt64(&maxLat))
	fmt.Println("--------------------")
}

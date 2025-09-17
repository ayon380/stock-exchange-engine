package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// --- Protocol Constants ---
const (
	// Message Types
	MessageTypeLoginRequest  = 1
	MessageTypeLoginResponse = 2
	MessageTypeSubmitOrder   = 3

	// Order Details
	OrderSideBuy    = 0
	OrderSideSell   = 1
	OrderTypeMarket = 0
	OrderTypeLimit  = 1
	OrderTypeIOC    = 2
	OrderTypeFOK    = 3
)

// OPTIMIZATION: Use a sync.Pool to reuse buffers and reduce GC pressure.
var bufferPool = sync.Pool{
	New: func() interface{} {
		return new(bytes.Buffer)
	},
}

// authenticate handles the login handshake for a new connection.
func authenticate(conn net.Conn, token string) error {
	// 1. Prepare the login request message
	tokenBytes := []byte(token)
	// Message format: total_len(4) + type(1) + token_len(4) + token(N)
	reqLen := 4 + 1 + 4 + len(tokenBytes)

	buf := new(bytes.Buffer)
	// Write payload into buffer in BigEndian format
	binary.Write(buf, binary.BigEndian, uint32(reqLen))
	buf.WriteByte(byte(MessageTypeLoginRequest))
	binary.Write(buf, binary.BigEndian, uint32(len(tokenBytes)))
	buf.Write(tokenBytes)

	// 2. Send the request
	if _, err := conn.Write(buf.Bytes()); err != nil {
		return fmt.Errorf("failed to send login request: %w", err)
	}

	// 3. Read the response header: total_len(4) + type(1) + success(1) + msg_len(4) = 10 bytes
	respHeader := make([]byte, 10)
	if _, err := io.ReadFull(conn, respHeader); err != nil {
		return fmt.Errorf("failed to read login response header: %w", err)
	}

	// 4. Parse the header
	success := respHeader[5] == 1 // 5th byte (0-indexed) is the success flag
	messageLen := binary.BigEndian.Uint32(respHeader[6:10])

	// 5. Read and discard the message body to clear the stream for the next message
	if messageLen > 0 {
		if _, err := io.CopyN(io.Discard, conn, int64(messageLen)); err != nil {
			return fmt.Errorf("failed to read login response body: %w", err)
		}
	}

	// 6. Check for success
	if !success {
		return errors.New("authentication failed by server")
	}

	log.Printf("Connection %s -> %s authenticated successfully", conn.LocalAddr(), conn.RemoteAddr())
	return nil
}

func main() {
	// --- Configuration via Command-Line Flags ---
	serverAddr := flag.String("addr", "127.0.0.1:50052", "The server address in the format host:port")
	totalRequests := flag.Int("requests", 1_000_000, "Total number of requests to send")
	concurrency := flag.Int("concurrency", 50, "Number of concurrent goroutines")
	// ADDED: Command-line flag for the auth token
	authToken := flag.String("token", "", "JWT authentication token (required)")
	flag.Parse()

	// ADDED: Validate that the token was provided
	if *authToken == "" {
		log.Fatal("Authentication token is required. Please provide it using the -token flag.")
	}

	log.Printf("Starting TCP load test with configuration:")
	log.Printf("  - Server Address: %s", *serverAddr)
	log.Printf("  - Total Requests: %d", *totalRequests)
	log.Printf("  - Concurrency:    %d", *concurrency)
	// ADDED: Log the first few characters of the token for verification
	if len(*authToken) > 15 {
		log.Printf("  - Auth Token:     %s...", (*authToken)[:15])
	} else {
		log.Printf("  - Auth Token:     %s", *authToken)
	}
	log.Println("-------------------------------------------------")


	// OPTIMIZATION: Create a connection pool to reuse TCP connections.
	// Each connection is authenticated before being added to the pool.
	connPool := make(chan net.Conn, *concurrency)
	for i := 0; i < *concurrency; i++ {
		conn, err := net.Dial("tcp4", *serverAddr)
		if err != nil {
			log.Fatalf("Failed to pre-populate connection pool (dial): %v", err)
		}
		// MODIFIED: Use the token from the command-line flag
		if err := authenticate(conn, *authToken); err != nil {
			log.Fatalf("Failed to authenticate connection for pool: %v. Server might be down or rejecting auth.", err)
		}
		connPool <- conn
	}

	// --- Test Data and Metrics Setup ---
	symbols := []string{"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN", "META", "NVDA", "NFLX"}
	var wg sync.WaitGroup
	var sent, errors, sumLat int64
	var minLat int64 = 1<<63 - 1
	var maxLat int64

	sem := make(chan struct{}, *concurrency)
	start := time.Now()

	// --- Real-time Progress Reporter ---
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
				if currentSent == 0 {
					continue
				}
				currentErrors := atomic.LoadInt64(&errors)
				currentSumLat := atomic.LoadInt64(&sumLat)
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
			defer func() { <-sem }()

			conn := <-connPool

			localRand := rand.New(rand.NewSource(time.Now().UnixNano() + int64(i)))
			t0 := time.Now()

			buf := bufferPool.Get().(*bytes.Buffer)
			buf.Reset()
			defer bufferPool.Put(buf)

			// Prepare binary order request
			orderId := fmt.Sprintf("o%d", i)
			userId := fmt.Sprintf("u%d", localRand.Intn(1000))
			symbol := symbols[localRand.Intn(len(symbols))]
			orderIdBytes := []byte(orderId)
			userIdBytes := []byte(userId)
			symbolBytes := []byte(symbol)

			totalLen := 4 + 1 + 4 + 4 + 4 + 1 + 1 + 8 + 8 + 8 + len(orderIdBytes) + len(userIdBytes) + len(symbolBytes)

			binary.Write(buf, binary.BigEndian, uint32(totalLen))
			buf.WriteByte(MessageTypeSubmitOrder)
			binary.Write(buf, binary.BigEndian, uint32(len(orderIdBytes)))
			binary.Write(buf, binary.BigEndian, uint32(len(userIdBytes)))
			binary.Write(buf, binary.BigEndian, uint32(len(symbolBytes)))
			buf.WriteByte(uint8(localRand.Intn(2)))                               // side
			buf.WriteByte(uint8(localRand.Intn(4)))                               // type
			binary.Write(buf, binary.BigEndian, uint64(localRand.Intn(100)+1))   // quantity
			binary.Write(buf, binary.BigEndian, localRand.Float64()*1000+1)      // price
			binary.Write(buf, binary.BigEndian, uint64(time.Now().UnixMilli()))  // timestamp
			buf.Write(orderIdBytes)
			buf.Write(userIdBytes)
			buf.Write(symbolBytes)

			// This function handles a connection error by creating and authenticating a new connection
			handleConnError := func() {
				conn.Close() // Close the broken connection
				newConn, dialErr := net.Dial("tcp4", *serverAddr)
				if dialErr == nil {
					// MODIFIED: Use the token from the command-line flag
					if authErr := authenticate(newConn, *authToken); authErr == nil {
						connPool <- newConn // Only add if authenticated
					} else {
						log.Printf("Failed to re-authenticate new connection: %v", authErr)
						newConn.Close()
					}
				}
			}

			// Send the request
			if _, err := conn.Write(buf.Bytes()); err != nil {
				atomic.AddInt64(&errors, 1)
				handleConnError()
				return
			}

			// Read response header to get length
			respHeader := make([]byte, 4)
			if _, err := io.ReadFull(conn, respHeader); err != nil {
				atomic.AddInt64(&errors, 1)
				handleConnError()
				return
			}

			respLen := binary.BigEndian.Uint32(respHeader)
			if respLen < 4 {
				atomic.AddInt64(&errors, 1)
				connPool <- conn // Connection is not broken, just a bad response
				return
			}

			// Read and discard response body
			if _, err := io.CopyN(io.Discard, conn, int64(respLen-4)); err != nil {
				atomic.AddInt64(&errors, 1)
				handleConnError()
				return
			}

			// Return the healthy connection to the pool for reuse.
			connPool <- conn

			lat := time.Since(t0).Microseconds()
			atomic.AddInt64(&sent, 1)
			atomic.AddInt64(&sumLat, lat)

			// Update min/max latency using atomic compare-and-swap
			for {
				oldMin := atomic.LoadInt64(&minLat)
				if lat >= oldMin || atomic.CompareAndSwapInt64(&minLat, oldMin, lat) {
					break
				}
			}
			for {
				oldMax := atomic.LoadInt64(&maxLat)
				if lat <= oldMax || atomic.CompareAndSwapInt64(&maxLat, oldMax, lat) {
					break
				}
			}
		}(i)
	}

	wg.Wait()
	done <- true // Stop the progress reporter

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
	fmt.Printf("Total Time:       %s\n", dur)
	fmt.Printf("Total Requests:   %d\n", *totalRequests)
	fmt.Printf("Successful:       %d\n", totalSent)
	fmt.Printf("Errors:           %d\n", totalErrors)
	fmt.Printf("RPS (Overall):    %.2f\n", rps)
	fmt.Printf("Min Latency:      %dµs\n", atomic.LoadInt64(&minLat))
	fmt.Printf("Avg Latency:      %.1fµs\n", avg)
	fmt.Printf("Max Latency:      %dµs\n", atomic.LoadInt64(&maxLat))
	fmt.Println("--------------------")
}
package main

import (
	"bytes"
	"encoding/binary"
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

const (
	MessageTypeSubmitOrder = 1
	OrderSideBuy           = 0
	OrderSideSell          = 1
	OrderTypeMarket        = 0
	OrderTypeLimit         = 1
	OrderTypeIOC           = 2
	OrderTypeFOK           = 3
)

// OPTIMIZATION: Use a sync.Pool to reuse buffers and reduce GC pressure.
var bufferPool = sync.Pool{
	New: func() interface{} {
		return new(bytes.Buffer)
	},
}

func main() {
	// --- Configuration via Command-Line Flags ---
	serverAddr := flag.String("addr", "127.0.0.1:50052", "The server address in the format host:port")
	totalRequests := flag.Int("requests", 1_000_000, "Total number of requests to send")
	concurrency := flag.Int("concurrency", 50, "Number of concurrent goroutines")
	flag.Parse()

	log.Printf("Starting TCP load test with configuration:")
	log.Printf("  - Server Address: %s", *serverAddr)
	log.Printf("  - Total Requests: %d", *totalRequests)
	log.Printf("  - Concurrency:    %d", *concurrency)
	log.Println("-------------------------------------------------")

	// OPTIMIZATION: Create a connection pool to reuse TCP connections.
	connPool := make(chan net.Conn, *concurrency)
	for i := 0; i < *concurrency; i++ {
		conn, err := net.Dial("tcp4", *serverAddr)
		if err != nil {
			log.Fatalf("Failed to pre-populate connection pool: %v", err)
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

			// OPTIMIZATION: Get a connection from the pool instead of dialing.
			conn := <-connPool

			// OPTIMIZATION: Use a local rand source to avoid lock contention.
			localRand := rand.New(rand.NewSource(time.Now().UnixNano() + int64(i)))

			t0 := time.Now()

			// OPTIMIZATION: Get a buffer from the pool.
			buf := bufferPool.Get().(*bytes.Buffer)
			buf.Reset() // Important: reset buffer before use.
			defer bufferPool.Put(buf) // Return buffer to the pool when done.

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
			binary.Write(buf, binary.BigEndian, uint64(localRand.Intn(100)+1))  // quantity
			price := localRand.Float64()*1000 + 1
			binary.Write(buf, binary.BigEndian, price)                                 // price
			binary.Write(buf, binary.BigEndian, uint64(time.Now().UnixMilli()))      // timestamp
			buf.Write(orderIdBytes)
			buf.Write(userIdBytes)
			buf.Write(symbolBytes)

			// Send the request
			_, err := conn.Write(buf.Bytes())
			if err != nil {
				atomic.AddInt64(&errors, 1)
				conn.Close() // Close the broken connection
				// Try to create a new connection to keep the pool healthy
				newConn, dialErr := net.Dial("tcp4", *serverAddr)
				if dialErr == nil {
					connPool <- newConn
				}
				return
			}

			// OPTIMIZATION: Use io.ReadFull for robust reads.
			respHeader := make([]byte, 4)
			_, err = io.ReadFull(conn, respHeader)
			if err != nil {
				atomic.AddInt64(&errors, 1)
				conn.Close()
				newConn, dialErr := net.Dial("tcp4", *serverAddr)
				if dialErr == nil {
					connPool <- newConn
				}
				return
			}
			
			respLen := binary.BigEndian.Uint32(respHeader)
			if respLen < 4 {
				atomic.AddInt64(&errors, 1)
				connPool <- conn // Return valid connection
				return
			}
			
			// Read response body
			respBody := make([]byte, respLen-4)
			_, err = io.ReadFull(conn, respBody)
			if err != nil {
				atomic.AddInt64(&errors, 1)
				conn.Close()
				newConn, dialErr := net.Dial("tcp4", *serverAddr)
				if dialErr == nil {
					connPool <- newConn
				}
				return
			}
			
			// OPTIMIZATION: Return the connection to the pool for reuse.
			connPool <- conn

			lat := time.Since(t0).Microseconds()
			atomic.AddInt64(&sent, 1)
			atomic.AddInt64(&sumLat, lat)

			// Update min latency
			for {
				oldMin := atomic.LoadInt64(&minLat)
				if lat >= oldMin {
					break
				}
				if atomic.CompareAndSwapInt64(&minLat, oldMin, lat) {
					break
				}
			}

			// Update max latency
			for {
				oldMax := atomic.LoadInt64(&maxLat)
				if lat <= oldMax {
					break
				}
				if atomic.CompareAndSwapInt64(&maxLat, oldMax, lat) { // <-- CORRECTED LINE
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
	fmt.Printf("Total Time:      %s\n", dur)
	fmt.Printf("Total Requests:  %d\n", *totalRequests)
	fmt.Printf("Successful:      %d\n", totalSent)
	fmt.Printf("Errors:          %d\n", totalErrors)
	fmt.Printf("RPS (Overall):   %.2f\n", rps)
	fmt.Printf("Min Latency:     %dµs\n", atomic.LoadInt64(&minLat))
	fmt.Printf("Avg Latency:     %.1fµs\n", avg)
	fmt.Printf("Max Latency:     %dµs\n", atomic.LoadInt64(&maxLat))
	fmt.Println("--------------------")
}
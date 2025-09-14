#!/usr/bin/env python3
"""
Performance Comparison Script for Stock Exchange Optimizations

This script compares the performance of different order submission methods:
1. Original gRPC
2. Optimized TCP binary protocol
3. Shared memory (when available)

Usage:
    python performance_comparison.py [host] [port] [orders_per_test]
"""

import subprocess
import time
import sys
import os
from typing import Dict, List, Tuple

def run_test(command: List[str], description: str) -> Tuple[float, int, int]:
    """Run a test command and return (duration_ms, orders_sent, errors)"""
    print(f"\n=== {description} ===")

    start_time = time.time()
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        end_time = time.time()
        duration_ms = (end_time - start_time) * 1000

        # Parse output for metrics
        orders_sent = 0
        errors = 0
        orders_per_sec = 0.0

        for line in result.stdout.split('\n'):
            if 'Orders sent:' in line:
                orders_sent = int(line.split(':')[1].strip())
            elif 'Errors:' in line:
                errors = int(line.split(':')[1].strip())
            elif 'Orders/sec:' in line:
                orders_per_sec = float(line.split(':')[1].strip())

        print(f"Duration: {duration_ms:.2f}ms")
        print(f"Orders sent: {orders_sent}")
        print(f"Errors: {errors}")
        print(f"Orders/sec: {orders_per_sec}")

        if result.stderr:
            print(f"Errors: {result.stderr}")

        return duration_ms, orders_sent, errors

    except subprocess.TimeoutExpired:
        print("Test timed out")
        return 300000, 0, 0  # 5 minutes timeout
    except Exception as e:
        print(f"Test failed: {e}")
        return 0, 0, 0

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = sys.argv[2] if len(sys.argv) > 2 else "50051"
    orders_per_test = int(sys.argv[3]) if len(sys.argv) > 3 else 10000

    print("Stock Exchange Performance Comparison")
    print("=" * 50)
    print(f"Host: {host}")
    print(f"Orders per test: {orders_per_test}")
    print()

    results = {}

    # Test 1: Original gRPC client
    if os.path.exists("./test_client.py"):
        results["gRPC"] = run_test([
            "python", "./test_client.py",
            "--addr", f"{host}:{port}",
            "--requests", str(orders_per_test),
            "--concurrency", "10"
        ], "gRPC Client Test")

    # Test 2: Optimized TCP binary client
    if os.path.exists("./fast_tcp_client"):
        results["TCP Binary"] = run_test([
            "./fast_tcp_client",
            host, "50052",  # TCP port
            "10", str(orders_per_test // 10)  # clients, orders per client
        ], "TCP Binary Client Test")

    # Test 3: Shared Memory client (if available)
    if os.path.exists("./fast_shm_client"):
        results["Shared Memory"] = run_test([
            "./fast_shm_client",
            "stock_exchange_orders",  # shared memory name
            "10", str(orders_per_test // 10)  # clients, orders per client
        ], "Shared Memory Client Test")

    # Summary
    print("\n" + "=" * 50)
    print("PERFORMANCE SUMMARY")
    print("=" * 50)

    print("<20")
    print("-" * 50)

    for method, (duration, orders, errors) in results.items():
        if orders > 0:
            orders_per_sec = orders / (duration / 1000)
            success_rate = (orders - errors) / orders * 100
            print("<20")

    print("\nOptimizations implemented:")
    print("✓ Lock-free queues for order processing")
    print("✓ Memory pools for order objects")
    print("✓ CPU affinity for threads")
    print("✓ TCP binary protocol (no TLS overhead)")
    print("✓ Shared memory ring buffer")
    print("✓ Batching for reduced context switching")
    print("✓ Busy-wait with yield for ultra-low latency")

if __name__ == "__main__":
    main()

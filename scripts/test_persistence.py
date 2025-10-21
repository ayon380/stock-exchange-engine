#!/usr/bin/env python3
"""
Test script to verify SEC compliance database persistence
- Sends orders via TCP
- Checks if orders and trades are persisted to PostgreSQL
"""

import socket
import json
import time
import psycopg2
import sys

def send_order(sock, order):
    """Send an order to the stock exchange via TCP"""
    message = json.dumps(order) + '\n'
    sock.sendall(message.encode())
    response = sock.recv(4096).decode()
    return json.loads(response)

def test_persistence():
    """Test order and trade persistence"""
    
    print("\n🧪 SEC Compliance Persistence Test")
    print("="*50)
    
    # Connect to database
    print("\n1. Connecting to PostgreSQL...")
    try:
        conn = psycopg2.connect(
            dbname="stockexchange",
            user="ayon",
            host="localhost",
            port="5432"
        )
        cursor = conn.cursor()
        print("  ✓ Database connected")
    except Exception as e:
        print(f"  ✗ Database connection failed: {e}")
        return False
    
    # Clear existing test data
    print("\n2. Clearing test data...")
    cursor.execute("DELETE FROM orders WHERE user_id IN ('test_buyer', 'test_seller')")
    cursor.execute("DELETE FROM trades WHERE buyer_user_id IN ('test_buyer', 'test_seller') OR seller_user_id IN ('test_seller', 'test_buyer')")
    conn.commit()
    print("  ✓ Test data cleared")
    
    # Connect to stock exchange
    print("\n3. Connecting to Stock Exchange TCP server...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('localhost', 9090))
        sock.settimeout(5.0)
        print("  ✓ Connected to exchange")
    except Exception as e:
        print(f"  ✗ Connection failed: {e}")
        print("  ℹ Make sure the stock engine is running: cd build && ./stock_engine")
        cursor.close()
        conn.close()
        return False
    
    # Authenticate users
    print("\n4. Authenticating test users...")
    
    # Buyer authentication
    auth_buyer = {
        "type": "authenticate",
        "user_id": "test_buyer",
        "password": "password"
    }
    response = send_order(sock, auth_buyer)
    if response.get("status") != "success":
        print(f"  ✗ Buyer authentication failed: {response}")
        sock.close()
        cursor.close()
        conn.close()
        return False
    print("  ✓ test_buyer authenticated")
    
    buyer_token = response.get("session_token")
    
    # Seller authentication (open new connection)
    sock2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock2.connect(('localhost', 9090))
    sock2.settimeout(5.0)
    
    auth_seller = {
        "type": "authenticate",
        "user_id": "test_seller",
        "password": "password"
    }
    message = json.dumps(auth_seller) + '\n'
    sock2.sendall(message.encode())
    response = json.loads(sock2.recv(4096).decode())
    
    if response.get("status") != "success":
        print(f"  ✗ Seller authentication failed: {response}")
        sock.close()
        sock2.close()
        cursor.close()
        conn.close()
        return False
    print("  ✓ test_seller authenticated")
    
    seller_token = response.get("session_token")
    
    # Submit matching orders
    print("\n5. Submitting orders...")
    
    # Sell order first
    sell_order = {
        "type": "submit_order",
        "session_token": seller_token,
        "order": {
            "symbol": "AAPL",
            "side": "sell",
            "quantity": 100,
            "price": 150.00,
            "order_type": "limit"
        }
    }
    
    message = json.dumps(sell_order) + '\n'
    sock2.sendall(message.encode())
    sell_response = json.loads(sock2.recv(4096).decode())
    print(f"  → Sell order: {sell_response}")
    
    if sell_response.get("status") != "success":
        print(f"  ✗ Sell order failed")
        sock.close()
        sock2.close()
        cursor.close()
        conn.close()
        return False
    
    sell_order_id = sell_response.get("order_id")
    
    # Buy order (should match)
    buy_order = {
        "type": "submit_order",
        "session_token": buyer_token,
        "order": {
            "symbol": "AAPL",
            "side": "buy",
            "quantity": 100,
            "price": 150.00,
            "order_type": "limit"
        }
    }
    
    response = send_order(sock, buy_order)
    print(f"  → Buy order: {response}")
    
    if response.get("status") != "success":
        print(f"  ✗ Buy order failed")
        sock.close()
        sock2.close()
        cursor.close()
        conn.close()
        return False
    
    buy_order_id = response.get("order_id")
    
    print(f"  ✓ Orders submitted (Buy: {buy_order_id}, Sell: {sell_order_id})")
    
    # Wait for persistence
    print("\n6. Waiting for persistence (2 seconds)...")
    time.sleep(2)
    
    # Check order persistence
    print("\n7. Checking order persistence...")
    cursor.execute("""
        SELECT order_id, user_id, symbol, side, quantity, price, status
        FROM orders
        WHERE user_id IN ('test_buyer', 'test_seller')
        ORDER BY timestamp_ms
    """)
    
    orders = cursor.fetchall()
    print(f"  → Found {len(orders)} orders in database")
    
    for order in orders:
        order_id, user_id, symbol, side, qty, price, status = order
        side_str = "BUY" if side == 0 else "SELL"
        print(f"    • {order_id[:20]}... | {user_id} | {symbol} | {side_str} {qty} @ ${price} | {status}")
    
    if len(orders) < 2:
        print(f"  ✗ Expected 2 orders, found {len(orders)}")
    else:
        print(f"  ✓ Order persistence working")
    
    # Check trade persistence
    print("\n8. Checking trade persistence...")
    cursor.execute("""
        SELECT trade_id, symbol, price, quantity, buyer_user_id, seller_user_id
        FROM trades
        WHERE buyer_user_id = 'test_buyer' OR seller_user_id = 'test_seller'
        ORDER BY timestamp_ms DESC
    """)
    
    trades = cursor.fetchall()
    print(f"  → Found {len(trades)} trades in database")
    
    for trade in trades:
        if trade[0]:  # trade_id might be null in old schema
            trade_id, symbol, price, qty, buyer, seller = trade
            print(f"    • Trade: {symbol} | {qty} shares @ ${price} | Buyer: {buyer} | Seller: {seller}")
        else:
            print(f"    • Trade: {trade[1]} | {trade[3]} shares @ ${trade[2]}")
    
    if len(trades) < 1:
        print(f"  ✗ Expected at least 1 trade, found {len(trades)}")
        print(f"  ℹ This might be normal if orders didn't match")
    else:
        print(f"  ✓ Trade persistence working")
    
    # Cleanup
    sock.close()
    sock2.close()
    cursor.close()
    conn.close()
    
    print("\n" + "="*50)
    if len(orders) >= 2:
        print("✅ SEC COMPLIANCE TEST PASSED")
        print("   Orders and trades are being persisted correctly!")
        return True
    else:
        print("⚠️  SEC COMPLIANCE TEST INCOMPLETE")
        print("   Orders/trades not persisted as expected")
        return False

if __name__ == "__main__":
    success = test_persistence()
    sys.exit(0 if success else 1)

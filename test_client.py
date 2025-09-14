import socket
import struct
import time
import random

# --- Server Configuration ---
HOST = '127.0.0.1'
PORT = 50052

# --- Message Constants (from your C++ code) ---
MESSAGE_TYPE_SUBMIT_ORDER = 1
SYMBOLS = ["AAPL", "GOOGL", "MSFT", "TSLA", "AMZN", "META", "NVDA", "NFLX"]

def execute_trade():
    """Connects to the server, sends a single binary trade order, and prints the response."""
    
    print(f"--- Preparing to send trade to {HOST}:{PORT} ---")
    
    try:
        # --- 1. Prepare Order Data ---
        order_id = f"py-order-{random.randint(1000, 9999)}".encode('utf-8')
        user_id = f"py-user-{random.randint(100, 999)}".encode('utf-8')
        symbol = random.choice(SYMBOLS).encode('utf-8')
        
        side = random.randint(0, 1)        # 0 for Buy, 1 for Sell
        order_type = 1                     # 1 for Limit
        quantity = random.randint(1, 100)
        price = round(random.uniform(100.0, 500.0), 2)
        timestamp_ms = int(time.time() * 1000)

        # --- 2. Construct the Binary Message ---
        # ! = Network (big-endian), B = unsigned char(1), I = unsigned int(4), Q = unsigned long long(8), d = double(8)
        
        # ***** THIS LINE IS THE FIX *****
        # The format is now Q (quantity) -> d (price) -> Q (timestamp) to match the variable order.
        body_format = '!BIIIBBQdQ'
        
        packed_body_fixed = struct.pack(
            body_format,
            MESSAGE_TYPE_SUBMIT_ORDER,
            len(order_id),
            len(user_id),
            len(symbol),
            side,
            order_type,
            quantity,
            price,
            timestamp_ms
        )
        
        message_body = packed_body_fixed + order_id + user_id + symbol
        header = struct.pack('!I', 4 + len(message_body))
        full_message = header + message_body

        print(f"   Order ID: {order_id.decode()}")
        print(f"   Action: {'SELL' if side else 'BUY'} {quantity} {symbol.decode()} @ ${price:.2f}")

        # --- 3. Connect and Send ---
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, PORT))
            print("\n✅ Connection successful.")
            
            s.sendall(full_message)
            print("   Trade message sent.")
            
            # --- 4. Receive and Decode Response ---
            print("   Waiting for server response...")
            resp_header_data = s.recv(4)
            if not resp_header_data:
                print("   Connection closed by server before response.")
                return

            resp_len = struct.unpack('!I', resp_header_data)[0]
            response_data = s.recv(resp_len - 4)
            
            print("\n--- Server Response ---")
            print(f"   Received {len(response_data) + 4} bytes.")
            print(f"   Raw data: {response_data}")

    except ConnectionRefusedError:
        print("\n❌ ERROR: Connection was actively refused. Is the server running?")
    except Exception as e:
        print(f"\n❌ An unexpected error occurred: {e}")


if __name__ == "__main__":
    execute_trade()
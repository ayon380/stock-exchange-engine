import socket
import struct
import time
from enum import IntEnum

# Use an Enum for message types for better readability, just like in C++
class MessageType(IntEnum):
    LOGIN_REQUEST = 1
    LOGIN_RESPONSE = 2
    SUBMIT_ORDER = 3
    ORDER_RESPONSE = 4
    HEARTBEAT = 5
    HEARTBEAT_ACK = 6

# --- Configuration ---
SERVER_HOST = '127.0.0.1'
SERVER_PORT = 50052

# --- Binary Structure Definitions ---
# The format strings tell the 'struct' module how to pack/unpack the data.
# '!' means network byte order (big-endian), which is crucial for consistency.
# 'I' is a 4-byte unsigned int (like uint32_t)
# 'B' is a 1-byte unsigned char (like uint8_t)

# Corresponds to C++ BinaryLoginRequest: uint32_t, uint8_t, uint32_t
LOGIN_REQUEST_HEADER_FORMAT = '!IBI'
LOGIN_REQUEST_HEADER_SIZE = struct.calcsize(LOGIN_REQUEST_HEADER_FORMAT)

# Corresponds to C++ BinaryLoginResponse: uint32_t, uint8_t, uint8_t, uint32_t
LOGIN_RESPONSE_HEADER_FORMAT = '!IBBI'
LOGIN_RESPONSE_HEADER_SIZE = struct.calcsize(LOGIN_RESPONSE_HEADER_FORMAT)


def send_login_request(sock, jwt_token: str):
    """Packs and sends the login request message."""
    try:
        token_bytes = jwt_token.encode('utf-8')
        message_size = LOGIN_REQUEST_HEADER_SIZE + len(token_bytes)

        # Pack the header fields into a bytes object
        header = struct.pack(
            LOGIN_REQUEST_HEADER_FORMAT,
            message_size,
            MessageType.LOGIN_REQUEST,
            len(token_bytes)
        )

        # Join the header and the token to form the full message
        full_message = header + token_bytes

        # sendall ensures that all data is sent
        sock.sendall(full_message)
        print(f"Login request sent with token: {jwt_token[:30]}...")
        return True
    except socket.error as e:
        print(f"Error sending login request: {e}")
        return False

def receive_login_response(sock):
    """Receives and unpacks the login response message."""
    try:
        # Read the fixed-size header from the socket
        header_data = sock.recv(LOGIN_RESPONSE_HEADER_SIZE)
        if not header_data:
            print("Connection closed by server (header).")
            return False

        # Unpack the header bytes into Python types
        # The result is a tuple: (message_length, type, success, message_len)
        _, _, success_flag, message_len = struct.unpack(LOGIN_RESPONSE_HEADER_FORMAT, header_data)

        # Read the variable-length message body, if any
        message_body = ""
        if message_len > 0:
            body_data = sock.recv(message_len)
            if not body_data:
                print("Connection closed by server (body).")
                return False
            message_body = body_data.decode('utf-8')
        
        is_success = bool(success_flag)
        print(f"Login response: {'SUCCESS' if is_success else 'FAILURE'}")
        print(f"Message: {message_body}")

        return is_success
    except (socket.error, struct.error) as e:
        print(f"Error receiving/parsing login response: {e}")
        return False

def main():
    """The main function to run the client."""
    # Same test JWT token from your C++ example
    jwt_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOiJlYWJlYTBiYi1jZDRhLTRhMDctYTU3Mi0zYjhlMWM1N2NlYjgiLCJ0eXBlIjoidHJhZGluZyIsImlhdCI6MTc1ODA5NTAxNywiZXhwIjoxNzU4MTgxNDE3fQ.xN8_D9TvA-jF3a94ItZXif9SX993OP_OmCbYkCdoN0U"

    # Using a 'with' statement is the standard Python way to handle sockets,
    # as it ensures the socket is automatically closed even if errors occur.
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            # Connect to the server
            sock.connect((SERVER_HOST, SERVER_PORT))
            print(f"Connected to TCP server on port {SERVER_PORT}")

            # Send the login request
            if not send_login_request(sock, jwt_token):
                return

            # Receive the login response
            if not receive_login_response(sock):
                print("Authentication failed.")
                return
            
            print("Authentication successful!")

            # Keep the connection alive for demonstration
            print("Keeping connection alive for 5 seconds...")
            time.sleep(5)

    except ConnectionRefusedError:
        print(f"Connection refused. Is the server running at {SERVER_HOST}:{SERVER_PORT}?")
    except socket.error as e:
        print(f"A socket error occurred: {e}")
    finally:
        print("Connection closed.")

if __name__ == "__main__":
    main()
#!/bin/zsh

# Script to build and run the stock exchange engine with gRPC FD fix

echo "Building the stock exchange engine..."

# Navigate to the build directory (assuming it's from the stock-exchange-engine root)
cd build

# Build the project
make -j4

# Check if build succeeded
if [ $? -ne 0 ]; then
    echo "Build failed. Exiting."
    exit 1
fi

echo "Build successful. Increasing ulimit and running the engine with GRPC_POLL_STRATEGY=poll to fix FD limit issue..."

# Increase file descriptor limit
ulimit -n 1024

# Run the engine with the environment variable to reduce FD usage
./stock_engine 
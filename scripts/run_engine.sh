#!/bin/zsh

# Script to build and run the stock exchange engine with gRPC FD fix

echo "Building the stock exchange engine..."

# Save the project root directory
PROJECT_ROOT="/Users/ayon/Repos/Aurex/stock-exchange-engine"

# Navigate to the build directory
cd "$PROJECT_ROOT/build"

# Build the project
make -j8

# Check if build succeeded
if [ $? -ne 0 ]; then
    echo "Build failed. Exiting."
    exit 1
fi

echo "Build successful. Preparing runtime environment..."

# Increase file descriptor limit
ulimit -n 1024

# Change back to project root so certificates can be found
cd "$PROJECT_ROOT"

# Load environment overrides if a .env file is present
if [ -f .env ]; then
    echo "Loading environment variables from .env"
    set -a
    source .env
    set +a
fi

# Ensure mandatory variables are present
if [ -z "$AUREX_DB_DSN" ]; then
    echo "AUREX_DB_DSN is not set. Export it or add it to .env before running."
    exit 1
fi

# Provide sane defaults for optional settings if they are not already exported
export AUREX_GRPC_ADDRESS=${AUREX_GRPC_ADDRESS:-"0.0.0.0:50051"}
export AUREX_TCP_ADDRESS=${AUREX_TCP_ADDRESS:-"0.0.0.0"}
export AUREX_TCP_PORT=${AUREX_TCP_PORT:-"50052"}
export AUREX_REDIS_HOST=${AUREX_REDIS_HOST:-"localhost"}
export AUREX_REDIS_PORT=${AUREX_REDIS_PORT:-"6379"}
export AUREX_SHM_NAME=${AUREX_SHM_NAME:-"stock_exchange_orders"}

echo "Launching stock_engine with configured environment..."

# Run the engine from the project root (where certificates are located)
./build/stock_engine 
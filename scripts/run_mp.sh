#!/bin/bash
# ================================================================
# Build and launch Exodia MP (server + 2 clients)
# ================================================================
set -e

cd "$(dirname "$0")/.."

# Kill any existing instances
pkill -f ./server 2>/dev/null || true
pkill -f ./client 2>/dev/null || true
sleep 0.5

# Build
echo "Building..."
make -j$(nproc)

# Create runtime directories
mkdir -p image_cache dump-folder

# Launch
echo "Starting server..."
./server --test &
SERVER_PID=$!
sleep 1

echo "Starting client 1..."
./client &
CLIENT1_PID=$!

echo "Starting client 2..."
./client &
CLIENT2_PID=$!

cleanup() {
    echo "Stopping..."
    kill $CLIENT1_PID $CLIENT2_PID $SERVER_PID 2>/dev/null || true
    wait 2>/dev/null
}
trap cleanup EXIT
trap 'exit 1' INT TERM

echo ""
echo "Server PID: $SERVER_PID"
echo "Client PIDs: $CLIENT1_PID, $CLIENT2_PID"
echo "Press Enter to stop..."
read _

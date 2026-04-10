#!/bin/bash
# ================================================================
# Loop: launch server + client for 15s, kill, scramble seed, repeat
# ================================================================
set -e

cd "$(dirname "$0")/.."

# Build once upfront
echo "Building..."
make -j$(nproc)
mkdir -p image_cache dump-folder

while true; do
    # Scramble the seed
    NEW_SEED=$((RANDOM * RANDOM + RANDOM))
    echo "$NEW_SEED" > seed.txt
    echo "=== Seed: $NEW_SEED ==="

    # Launch server
    ./server --test &
    SERVER_PID=$!
    sleep 1

    # Launch client
    ./client &
    CLIENT_PID=$!

    # Wait 15 seconds
    sleep 20

    # Kill client and server
    echo "Stopping (seed was $NEW_SEED)..."
    kill -9 $CLIENT_PID $SERVER_PID 2>/dev/null || true
    wait $CLIENT_PID $SERVER_PID 2>/dev/null || true

    sleep 1
done

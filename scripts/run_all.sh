#!/bin/bash

# Script to run both client and server executables

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build/wsl-debug"

SERVER_EXE="$BUILD_DIR/server"
CLIENT_EXE="$BUILD_DIR/client"

# Check if executables exist
if [[ ! -x "$SERVER_EXE" ]]; then
    echo "Error: Server executable not found at $SERVER_EXE"
    echo "Please build the project first."
    exit 1
fi

if [[ ! -x "$CLIENT_EXE" ]]; then
    echo "Error: Client executable not found at $CLIENT_EXE"
    echo "Please build the project first."
    exit 1
fi

echo "Starting Server and Client..."

# Run server in background
echo "Starting server..."
"$SERVER_EXE" &
SERVER_PID=$!

# Give server time to start
sleep 1

# Run client in background
echo "Starting client..."
"$CLIENT_EXE" &
CLIENT_PID=$!

# Wait for both processes
echo "Server (PID: $SERVER_PID) and Client (PID: $CLIENT_PID) running..."
echo "Press Ctrl+C to stop both"

# Trap Ctrl+C to kill both processes
trap "kill $SERVER_PID $CLIENT_PID 2>/dev/null; exit" INT TERM

# Wait for both
wait $CLIENT_PID
wait $SERVER_PID

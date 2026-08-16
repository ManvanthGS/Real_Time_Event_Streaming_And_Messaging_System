#!/bin/bash

# Script to run the complete Pub/Sub system (Broker, Consumer, Producer)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build/linux-debug"

BROKER_EXE="$BUILD_DIR/broker"
CONSUMER_EXE="$BUILD_DIR/consumer"
PRODUCER_EXE="$BUILD_DIR/producer"

# Check if executables exist
if [[ ! -x "$BROKER_EXE" || ! -x "$CONSUMER_EXE" || ! -x "$PRODUCER_EXE" ]]; then
    echo "Error: One or more executables not found in $BUILD_DIR"
    echo "Please run scripts/build.sh first."
    exit 1
fi

echo "========================================="
echo "Starting Real-Time Pub/Sub System"
echo "========================================="

# Try to find a terminal emulator or multiplexer to launch separate windows
if command -v tmux &> /dev/null; then
    echo "Tmux found! Launching in a split-terminal session..."
    tmux new-session -d -s pubsub_system "bash -c '$BROKER_EXE 9000; exec bash'"
    tmux split-window -h "bash -c 'sleep 0.5; $CONSUMER_EXE 127.0.0.1 9000 BTC_USD; exec bash'"
    tmux split-window -v "bash -c 'sleep 0.5; $PRODUCER_EXE 127.0.0.1 9000 BTC_USD; exec bash'"
    tmux attach-session -d -t pubsub_system
    exit 0
elif command -v gnome-terminal &> /dev/null; then
    TERM_CMD="gnome-terminal -- "
elif command -v xterm &> /dev/null; then
    TERM_CMD="xterm -hold -e "
else
    echo "Error: Could not find gnome-terminal or xterm."
    echo "To run these in separate terminals, please open 3 separate terminal tabs manually"
    echo "or use the VS Code 'Run Task' feature."
    exit 1
fi

echo "Launching Broker in a new terminal..."
$TERM_CMD bash -c "$BROKER_EXE 9000; exec bash" &
sleep 0.5

echo "Launching Consumer in a new terminal..."
$TERM_CMD bash -c "$CONSUMER_EXE 127.0.0.1 9000 BTC_USD; exec bash" &
sleep 0.5

echo "Launching Producer in a new terminal..."
$TERM_CMD bash -c "$PRODUCER_EXE 127.0.0.1 9000 BTC_USD; exec bash" &

echo "All processes launched in separate windows!"

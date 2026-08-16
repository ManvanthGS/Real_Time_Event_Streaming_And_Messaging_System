#!/bin/bash
# Script to configure and build the project for Linux

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/.."

cd "$PROJECT_ROOT" || exit 1

echo "Configuring the project (linux-debug preset)..."
cmake --preset linux-debug

if [ $? -ne 0 ]; then
    echo "Configuration failed!"
    exit 1
fi

echo "Building the project..."
cmake --build --preset linux-debug -j

if [ $? -eq 0 ]; then
    echo "Build successful! Binaries are in build/linux-debug/"
else
    echo "Build failed!"
    exit 1
fi

#!/bin/bash

# Build script for encode library

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p lib core include

if [ "$1" = "clean" ]; then
    echo "Cleaning encode library..."
    make -f core.mk clean
    rm -rf lib core/*.o include
elif [ "$1" = "all" ] || [ $# -eq 0 ]; then
    echo "Building encode library..."
    make -f core.mk
    
    # Copy output files
    cp core/libencode.a lib/
    cp *.h include/
    
    echo "Encode library built successfully!"
fi
#!/usr/bin/env bash
# Build the repro with debug info and no optimization so line mapping is clean.
set -euo pipefail
cd "$(dirname "$0")"
c++ -g -O0 -std=c++17 repro.cpp -o repro
echo "built ./repro"

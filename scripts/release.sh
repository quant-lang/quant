#!/usr/bin/env bash

set -e

echo "[1/5] Cleaning..."
rm -rf build
rm -rf dist

echo "[2/5] Configuring..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja

echo "[3/5] Building..."
cmake --build build

echo "[4/5] Staging..."
mkdir -p dist
cp build/qu dist/

#echo "[5/5] Packaging..."
#tar -czf qu-linux-x86_64.tar.gz -C dist qu

echo "Done."

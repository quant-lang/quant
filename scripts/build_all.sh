#!/usr/bin/env bash
# Build one dedicated Quant compiler per platform. Each build directory gets
# its own toolchain file from toolchains/ and its own backend set:
#
#   linux     -> runs on Linux x86-64, targets x86_64/aarch64 Linux
#   aarch64   -> runs on AArch64 Linux (cross-built), targets AArch64 Linux
#                and ZeroPoint
#   windows   -> qu.exe built via MinGW-w64, targets x86_64-windows
#   zeropoint -> same x86_64 windows toolchain (per ZeroPoint author's request),
#                targets aarch64-zeropoint only
#
# All builds: Ninja, Release, -j$(nproc). Missing cross toolchains are skipped.

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="$(nproc 2>/dev/null || echo 4)"
cd "$ROOT"

have() { command -v "$1" >/dev/null 2>&1; }

build() {
    name="$1"
    toolchain="$2"
    needs="$3"
    backends="$4"
    default="$5"

    if ! have "$needs"; then
        echo "[$name] SKIP: '$needs' not found in PATH"
        return
    fi

    dir="build-$name"
    echo "[$name] Configuring (toolchain: $toolchain, backends: $backends)..."
    cmake -B "$dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/$toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DQUANT_BACKENDS="$backends" \
        -DQUANT_DEFAULT_TARGET="$default"

    echo "[$name] Building (-j$JOBS)..."
    cmake --build "$dir" -j "$JOBS"

    bin="$dir/bin/qu"
    [ -f "$bin.exe" ] && bin="$bin.exe"
    echo "[$name] Done: $bin"
}

build linux     toolchains/linux-x86_64.cmake      g++                          "x86_64-linux;aarch64-linux" "x86_64-linux"
build aarch64   toolchains/linux-aarch64.cmake     aarch64-unknown-linux-gnu-g++ "aarch64-linux;aarch64-zeropoint" "aarch64-linux"
build windows   toolchains/windows-x86_64.cmake    x86_64-w64-mingw32-g++       "x86_64-windows"             "x86_64-windows"
build zeropoint toolchains/windows-x86_64.cmake    x86_64-w64-mingw32-g++        "aarch64-zeropoint"          "aarch64-zeropoint"

echo
echo "All builds finished:"
ls -la build-*/bin/qu* 2>/dev/null || true

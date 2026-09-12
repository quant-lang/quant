#!/usr/bin/env bash
# check_tests_aarch64.sh - Compile and run all Quant tests on AArch64 via qemu.
#
# Tests ending with _err are expected to fail at compile time.
# All other tests are expected to compile and run to completion.
# Non-zero program exit codes are OK (many tests intentionally return non-zero).
# FAIL = compilation error on a non-_err test, or qemu crash/timeout.
#
# Exit code: 0 if all tests match expectations, 1 otherwise.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QU="$REPO_ROOT/build/bin/qu"
TESTS_DIR="$REPO_ROOT/tests"

if [[ ! -x "$QU" ]]; then
    echo "FATAL: compiler not found at $QU" >&2
    exit 1
fi

if ! command -v qemu-aarch64 &>/dev/null; then
    echo "FATAL: qemu-aarch64 not found" >&2
    exit 1
fi

pass=0
expected_err=0
unexpected_err=0
unexpected_ok=0
crash=0
skipped=0

declare -a unexpected_err_list=()
declare -a unexpected_ok_list=()
declare -a crash_list=()

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

while IFS= read -r -d '' file; do
    if grep -q '^module ' "$file" 2>/dev/null; then
        ((skipped++)) || true
        continue
    fi

    name="${file#$TESTS_DIR/}"
    basename_no_ext="${name%.qu}"
    out="$TMPDIR/out"

    if ! "$QU" --target aarch64 "$file" -o "$out" >/dev/null 2>&1; then
        if [[ "$basename_no_ext" == *_err ]]; then
            ((expected_err++)) || true
        else
            ((unexpected_err++)) || true
            unexpected_err_list+=("$name")
        fi
        continue
    fi

    if [[ "$basename_no_ext" == *_err ]]; then
        ((unexpected_ok++)) || true
        unexpected_ok_list+=("$name")
        continue
    fi

    # Run via qemu. Exit 124 = timeout, >=128 = signal (segfault etc).
    rc=0
    timeout 10 qemu-aarch64 "$out" </dev/null >/dev/null 2>&1 || rc=$?
    if [[ $rc -eq 124 || $rc -ge 128 ]]; then
        ((crash++)) || true
        crash_list+=("$name")
    else
        ((pass++)) || true
    fi
done < <(find "$TESTS_DIR" -name '*.qu' -print0 | sort -z)

total=$((pass + expected_err + unexpected_err + unexpected_ok + crash))

echo "========================================"
echo "  Quant AArch64 Runtime Test Report"
echo "========================================"
echo ""
echo "  Total tests run:    $total"
echo "  Skipped (modules):  $skipped"
echo "  ----------------------------------------"
echo "  Passed:             $pass"
echo "  Expected errors:    $expected_err"
echo "  UNEXPECTED errors:  $unexpected_err"
echo "  UNEXPECTED passes:  $unexpected_ok"
echo "  CRASH/TIMEOUT:      $crash"
echo "========================================"

if [[ ${#unexpected_err_list[@]} -gt 0 ]]; then
    echo ""
    echo "Compile FAIL (not marked _err):"
    for f in "${unexpected_err_list[@]}"; do
        echo "  - $f"
    done
fi

if [[ ${#unexpected_ok_list[@]} -gt 0 ]]; then
    echo ""
    echo "Marked _err but compiled OK:"
    for f in "${unexpected_ok_list[@]}"; do
        echo "  - $f"
    done
fi

if [[ ${#crash_list[@]} -gt 0 ]]; then
    echo ""
    echo "Compiled but CRASHED or TIMED OUT:"
    for f in "${crash_list[@]}"; do
        echo "  - $f"
    done
fi

if [[ $unexpected_err -eq 0 && $unexpected_ok -eq 0 && $crash -eq 0 ]]; then
    echo ""
    echo "All tests match expectations."
    exit 0
else
    echo ""
    echo "Some tests do not match expectations."
    exit 1
fi

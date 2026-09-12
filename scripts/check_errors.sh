#!/usr/bin/env bash
# check_errors.sh - Run all Quant tests and verify expected error status.
#
# Convention:
#   - Tests ending with _err are EXPECTED to fail (compile error).
#   - All other tests are expected to succeed.
#   - Files containing a "module" declaration are library modules and skipped.
#
# Exit code: 0 if all tests match expectations, 1 otherwise.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS_DIR="$REPO_ROOT/tests"

# Overrides:
#   QU       - path to the compiler binary (default: build/bin/qu)
#   QU_FLAGS - extra compiler flags, e.g. "-O0" or "-O3" (default: none)
QU="${QU:-$REPO_ROOT/build/bin/qu}"
read -r -a QU_FLAGS <<< "${QU_FLAGS:-}"

if [[ ! -x "$QU" ]]; then
    echo "FATAL: compiler not found at $QU" >&2
    exit 1
fi

pass=0
expected_err=0
unexpected_err=0
unexpected_ok=0
skipped=0

declare -a unexpected_err_list=()
declare -a unexpected_ok_list=()

while IFS= read -r -d '' file; do
    # Skip library modules (files that contain a 'module' declaration)
    if grep -q '^module ' "$file" 2>/dev/null; then
        ((skipped++)) || true
        continue
    fi

    name="${file#$TESTS_DIR/}"
    basename_no_ext="${name%.qu}"

    if "$QU" "$file" "${QU_FLAGS[@]}" >/dev/null 2>&1; then
        if [[ "$basename_no_ext" == *_err ]]; then
            ((unexpected_ok++)) || true
            unexpected_ok_list+=("$name")
        else
            ((pass++)) || true
        fi
    else
        if [[ "$basename_no_ext" == *_err ]]; then
            ((expected_err++)) || true
        else
            ((unexpected_err++)) || true
            unexpected_err_list+=("$name")
        fi
    fi
done < <(find "$TESTS_DIR" -name '*.qu' -print0 | sort -z)

total=$((pass + expected_err + unexpected_err + unexpected_ok))

echo "========================================"
echo "  Quant Test Report"
echo "========================================"
echo ""
echo "  Total tests run:   $total"
echo "  Skipped (modules): $skipped"
echo "  ----------------------------------------"
echo "  Passed:            $pass"
echo "  Expected errors:   $expected_err"
echo "  UNEXPECTED errors: $unexpected_err"
echo "  UNEXPECTED passes: $unexpected_ok"
echo "========================================"

if [[ ${#unexpected_err_list[@]} -gt 0 ]]; then
    echo ""
    echo "Tests that FAIL but are NOT marked _err (need renaming):"
    for f in "${unexpected_err_list[@]}"; do
        echo "  - $f"
    done
fi

if [[ ${#unexpected_ok_list[@]} -gt 0 ]]; then
    echo ""
    echo "Tests marked _err but PASSING (remove _err or fix test):"
    for f in "${unexpected_ok_list[@]}"; do
        echo "  - $f"
    done
fi

if [[ $unexpected_err -eq 0 && $unexpected_ok -eq 0 ]]; then
    echo ""
    echo "All tests match expectations."
    exit 0
else
    echo ""
    echo "Some tests do not match expectations."
    exit 1
fi

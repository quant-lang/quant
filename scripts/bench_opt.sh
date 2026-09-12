#!/usr/bin/env bash
# bench_opt.sh - End-to-end optimizer benchmark (compile time + runtime).
#
# For every input program and every optimization level the script compiles
# the program K times with the real compiler (reporting the minimum
# "Compilation took" time), runs the produced executable N times (reporting
# the mean wall time) and checks that stdout is identical across levels.
#
# Usage:
#   scripts/bench_opt.sh [file.qu ...]
#
# Overrides:
#   QU          - compiler binary (default: build/bin/qu)
#   QU_LEVELS   - levels, space separated (default: "-O0 -O1 -O2 -O3")
#   QU_COMPILES - compilations per level (default: 5)
#   QU_RUNS     - executions per level (default: 20)
#
# Default inputs: tests/bench_loop_fold.qu tests/loop_fold.qu tests/if_switch_fold.qu

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QU="${QU:-$REPO_ROOT/build/bin/qu}"
read -r -a LEVELS <<< "${QU_LEVELS:--O0 -O1 -O2 -O3}"
COMPILES="${QU_COMPILES:-5}"
RUNS="${QU_RUNS:-20}"

if [[ ! -x "$QU" ]]; then
    echo "FATAL: compiler not found at $QU" >&2
    exit 1
fi

INPUTS=("$@")
if [[ ${#INPUTS[@]} -eq 0 ]]; then
    INPUTS=(tests/bench_loop_fold.qu tests/loop_fold.qu tests/if_switch_fold.qu)
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

now_ms() { date +%s%N | awk '{ printf "%.3f", $1 / 1000000 }'; }

failed=0
for input in "${INPUTS[@]}"; do
    [[ "$input" = /* ]] || input="$REPO_ROOT/$input"
    base="$(basename "${input%.qu}")"
    echo
    echo "### ${input#$REPO_ROOT/}"
    echo
    echo "| level | compile, ms (min of $COMPILES) | run, ms (mean of $RUNS) | exit | output |"
    echo "|---|---|---|---|---|"

    ref_out=""
    have_ref=0
    for lvl in "${LEVELS[@]}"; do
        exe="$WORK/${base}${lvl#-}"
        compile_ms=""
        ok=1
        for ((i = 0; i < COMPILES; i++)); do
            if ! out="$(cd "$REPO_ROOT" && "$QU" "$input" "$lvl" --time -o "$exe" 2>/dev/null)"; then
                ok=0
                break
            fi
            t="$(sed -n 's/.*Compilation took: *\([0-9.]*\) ms.*/\1/p' <<< "$out" | tail -1)"
            if [[ -n "$t" ]] && { [[ -z "$compile_ms" ]] || awk "BEGIN { exit !($t < $compile_ms) }"; }; then
                compile_ms="$t"
            fi
        done
        if [[ $ok -eq 0 ]]; then
            echo "| $lvl | compile failed | | | |"
            failed=1
            continue
        fi

        total=0
        code=0
        run_out=""
        for ((i = 0; i < RUNS; i++)); do
            start="$(now_ms)"
            run_out="$(cd "$WORK" && "$exe" 2>/dev/null)"
            code=$?
            end="$(now_ms)"
            total="$(awk "BEGIN { printf \"%.3f\", $total + ($end - $start) }")"
        done
        mean="$(awk "BEGIN { printf \"%.2f\", $total / $RUNS }")"
        flat="$(tr '\n' ' ' <<< "$run_out" | sed 's/ *$//')"
        [[ ${#flat} -gt 60 ]] && flat="${flat:0:57}..."

        match=""
        if [[ $have_ref -eq 0 ]]; then
            ref_out="$run_out"
            have_ref=1
        elif [[ "$ref_out" != "$run_out" ]]; then
            match=" **DIFFERS**"
            failed=1
        fi

        echo "| $lvl | ${compile_ms:-?} | $mean | $code | $flat$match |"
    done
done

exit $failed

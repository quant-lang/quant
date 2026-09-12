#!/usr/bin/env python3
# bench_opt.py - End-to-end optimizer benchmark (compile time + runtime).
#
# For every input program and every optimization level the script compiles
# the program K times with the real compiler (reporting the minimum
# "Compilation took" time), runs the produced executable N times (reporting
# the mean wall time) and checks that stdout is identical across levels.
#
# Usage:
#   python3 scripts/bench_opt.py [file.qu ...]
#
# Overrides:
#   QU          - compiler binary (default: build/bin/qu[.exe])
#   QU_LEVELS   - levels, space separated (default: "-O0 -O1 -O2 -O3")
#   QU_COMPILES - compilations per level (default: 5)
#   QU_RUNS     - executions per level (default: 20)
#
# Default inputs: tests/bench_loop_fold.qu tests/loop_fold.qu tests/if_switch_fold.qu

import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time


def capture(exe, argv, cwd):
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            [exe, *argv],
            cwd=cwd,
            capture_output=True,
            text=True,
        )
        ms = (time.perf_counter() - start) * 1000.0
    except OSError as exc:
        return {"code": -1, "out": "", "ms": 0.0, "error": str(exc)}
    return {"code": proc.returncode, "out": proc.stdout, "ms": ms}


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    qu = os.environ.get("QU") or os.path.join(
        repo_root, "build", "bin", "qu.exe" if os.name == "nt" else "qu"
    )
    if not os.path.isfile(qu):
        print(f"FATAL: compiler not found at {qu}", file=sys.stderr)
        return 1

    levels = (os.environ.get("QU_LEVELS") or "-O0 -O1 -O2 -O3").split()
    compiles = int(os.environ.get("QU_COMPILES", "5"))
    runs = int(os.environ.get("QU_RUNS", "20"))

    inputs = sys.argv[1:]
    if not inputs:
        inputs = [
            "tests/bench_loop_fold.qu",
            "tests/loop_fold.qu",
            "tests/if_switch_fold.qu",
        ]

    work = tempfile.mkdtemp(prefix="qu_bench_")

    failed = False
    try:
        for input_rel in inputs:
            input_path = input_rel if os.path.isabs(input_rel) else os.path.join(repo_root, input_rel)
            if not os.path.isfile(input_path):
                print(f"SKIP: input not found: {input_rel}", file=sys.stderr)
                continue
            base = os.path.splitext(os.path.basename(input_path))[0]
            print()
            print(f"### {input_rel}")
            print()
            print(f"| level | compile, ms (min of {compiles}) | run, ms (mean of {runs}) | exit | output |")
            print("|---|---|---|---|---|")

            ref_out = None
            for lvl in levels:
                exe = os.path.join(work, f"{base}{lvl.lstrip('-')}.exe")
                compile_ms = float("inf")
                ok = True
                for _ in range(compiles):
                    c = capture(qu, [input_path, lvl, "--time", "-o", exe], repo_root)
                    if c["code"] != 0:
                        ok = False
                        break
                    m = re.search(r"Compilation took:\s*([0-9.]+)\s*ms", c["out"])
                    if m:
                        compile_ms = min(compile_ms, float(m.group(1)))
                if not ok:
                    print(f"| {lvl} | compile failed | | | |")
                    failed = True
                    continue

                total = 0.0
                code = 0
                run_out = ""
                for _ in range(runs):
                    r = capture(exe, [], work)
                    total += r["ms"]
                    code = r["code"]
                    run_out = r["out"]
                mean_ms = total / runs

                out_flat = " ".join(run_out.strip().split())
                if len(out_flat) > 60:
                    out_flat = out_flat[:57] + "..."

                match = ""
                if ref_out is None:
                    ref_out = run_out
                elif ref_out != run_out:
                    match = " **DIFFERS**"
                    failed = True

                print(f"| {lvl} | {compile_ms:.1f} | {mean_ms:.2f} | {code} | {out_flat}{match} |")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
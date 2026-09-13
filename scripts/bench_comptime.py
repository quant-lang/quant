#!/usr/bin/env python3
"""Compare two Release compilers; process startup and all compilation passes are timed."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
parser.add_argument("--runs", type=int, default=9)
args = parser.parse_args()
if args.runs < 1:
    parser.error("--runs must be positive")
compilers = [args.before.resolve(), args.after.resolve()]
workloads = {
    "loop": "i32 calc() { mut i32 i = 0; while (i < 150000) { i = i + 1; } return i; }",
    "calls": "i32 inc(i32 x) { return x + 1; } i32 calc() { mut i32 i = 0; while (i < 60000) { i = inc(i); } return i; }",
    "recursion": "i32 fib(i32 n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } i32 calc() { return fib(22); }",
}
expected = {"loop": 150000, "calls": 60000, "recursion": 17711}
report = {}
with tempfile.TemporaryDirectory(prefix="quant_bench_comptime_") as work:
    work = Path(work)
    for name, body in workloads.items():
        source = work / (name + ".qu")
        source.write_text(body + " i32 main() { if (#calc() == " + str(expected[name]) + ") { return 0; } return 1; }")
        samples = [[], []]
        for iteration in range(args.runs + 2):
            for index in ((0, 1) if iteration % 2 == 0 else (1, 0)):
                exe = work / (name + str(index) + "_" + str(iteration) + ".exe")
                start = time.perf_counter()
                result = subprocess.run([str(compilers[index]), str(source), "-O0", "-o", str(exe)],
                                        capture_output=True, text=True, timeout=120)
                if result.returncode:
                    raise RuntimeError(f"{compilers[index]}: {result.stdout}{result.stderr}")
                elapsed = (time.perf_counter() - start) * 1000
                subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)
                if iteration >= 2:
                    samples[index].append(elapsed)
        report[name] = {label: {"median_ms": statistics.median(values), "min_ms": min(values),
                               "max_ms": max(values), "samples_ms": values}
                        for label, values in zip(("before", "after"), samples)}
print(json.dumps(report, indent=2))

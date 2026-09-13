#!/usr/bin/env python3
"""Compile-time regressions: check values, exit status, and failure diagnostics."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
QU = Path(os.environ.get("QU", ROOT / ("build/bin/qu.exe" if os.name == "nt" else "build/bin/qu"))).resolve()
ERRORS = {
    "comptime_div_overflow_err": "integer division overflow",
    "comptime_float_range_err": "float-to-integer cast is out of range",
    "comptime_recursion_err": "compile-time recursion too deep",
    "comptime_uninitialized_err": "uninitialized",
}
OUTPUTS = {
    "comptime_basic": "14 -5 1 1 0 1 0 0 1 bigger 42 42 1 4".split(),
    "comptime_const": "42 100 140 2 0 ten".split(),
    "comptime_loops": "3628800 15 6 52 18".split(),
    "comptime_switch_generic": "42 20 2 99".split(),
}
with tempfile.TemporaryDirectory(prefix="quant_comptime_") as work:
    for level in ("-O0", "-O1", "-O2", "-O3"):
        exe = Path(work) / "edges.exe"
        subprocess.run([str(QU), str(ROOT / "tests/comptime_edges.qu"), level,
                        "-o", str(exe)], cwd=ROOT, check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, timeout=10)
        assert result.returncode == 0, (level, result.returncode, result.stdout)
        for name, expected in OUTPUTS.items():
            subprocess.run([str(QU), str(ROOT / "tests" / (name + ".qu")), level,
                            "-o", str(exe)], cwd=ROOT, check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 0 and result.stdout.split() == expected, (name, level, result)
        for name, diagnostic in ERRORS.items():
            result = subprocess.run([str(QU), str(ROOT / "tests" / (name + ".qu")),
                                     level, "-o", str(exe)], cwd=ROOT,
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode != 0 and diagnostic in result.stdout + result.stderr, (name, result)
        print(level, "values and diagnostics passed")

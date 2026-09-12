#!/usr/bin/env python3
# check_tests_aarch64.py - Compile and run all Quant tests on AArch64 via qemu.
#
# Tests ending with _err are expected to fail at compile time.
# All other tests are expected to compile and run to completion.
# Non-zero program exit codes are OK (many tests intentionally return non-zero).
# FAIL = compilation error on a non-_err test, or qemu crash/timeout.
#
# Exit code: 0 if all tests match expectations, 1 otherwise.

import os
import shutil
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    qu = os.path.join(repo_root, "build", "bin", "qu")
    tests_dir = os.path.join(repo_root, "tests")

    if not os.path.isfile(qu):
        print(f"FATAL: compiler not found at {qu}", file=sys.stderr)
        return 1

    if shutil.which("qemu-aarch64") is None:
        print("FATAL: qemu-aarch64 not found", file=sys.stderr)
        return 1

    pass_count = 0
    expected_err = 0
    unexpected_err = 0
    unexpected_ok = 0
    crash = 0
    skipped = 0
    unexpected_err_list = []
    unexpected_ok_list = []
    crash_list = []

    work = tempfile.mkdtemp(prefix="qu_aarch64_")

    try:
        files = []
        for dirpath, _dirnames, filenames in os.walk(tests_dir):
            for name in filenames:
                if name.endswith(".qu"):
                    files.append(os.path.join(dirpath, name))
        files.sort()

        for file in files:
            # Skip library modules (files that contain a 'module' declaration)
            try:
                with open(file, encoding="utf-8", errors="replace") as f:
                    has_module = any(line.startswith("module ") for line in f)
            except OSError:
                has_module = False
            if has_module:
                skipped += 1
                continue

            name = os.path.relpath(file, tests_dir)
            base_no_ext = os.path.splitext(name)[0]
            out = os.path.join(work, "out")

            try:
                proc = subprocess.run(
                    [qu, "--target", "aarch64", file, "-o", out],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except OSError as exc:
                print(f"FATAL: cannot run compiler: {exc}", file=sys.stderr)
                return 1

            if proc.returncode != 0:
                if base_no_ext.endswith("_err"):
                    expected_err += 1
                else:
                    unexpected_err += 1
                    unexpected_err_list.append(name)
                continue

            if base_no_ext.endswith("_err"):
                unexpected_ok += 1
                unexpected_ok_list.append(name)
                continue

            # Run via qemu. time-expired = timeout, signal-death = segfault etc.
            try:
                subprocess.run(
                    ["qemu-aarch64", out],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10,
                )
                pass_count += 1
            except subprocess.TimeoutExpired:
                crash += 1
                crash_list.append(name)
            except OSError:
                crash += 1
                crash_list.append(name)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    total = pass_count + expected_err + unexpected_err + unexpected_ok + crash

    print("========================================")
    print("  Quant AArch64 Runtime Test Report")
    print("========================================")
    print("")
    print(f"  Total tests run:    {total}")
    print(f"  Skipped (modules):  {skipped}")
    print("  ----------------------------------------")
    print(f"  Passed:             {pass_count}")
    print(f"  Expected errors:    {expected_err}")
    print(f"  UNEXPECTED errors:  {unexpected_err}")
    print(f"  UNEXPECTED passes:  {unexpected_ok}")
    print(f"  CRASH/TIMEOUT:      {crash}")
    print("========================================")

    if unexpected_err_list:
        print("")
        print("Compile FAIL (not marked _err):")
        for f in unexpected_err_list:
            print(f"  - {f}")

    if unexpected_ok_list:
        print("")
        print("Marked _err but compiled OK:")
        for f in unexpected_ok_list:
            print(f"  - {f}")

    if crash_list:
        print("")
        print("Compiled but CRASHED or TIMED OUT:")
        for f in crash_list:
            print(f"  - {f}")

    if unexpected_err == 0 and unexpected_ok == 0 and crash == 0:
        print("")
        print("All tests match expectations.")
        return 0
    else:
        print("")
        print("Some tests do not match expectations.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
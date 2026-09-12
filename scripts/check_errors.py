#!/usr/bin/env python3
# check_errors.py - Run all Quant tests and verify expected error status.
#
# Convention:
#   - Tests ending with _err are EXPECTED to fail (compile error).
#   - All other tests are expected to succeed.
#   - Files containing a "module" declaration are library modules and skipped.
#
# Exit code: 0 if all tests match expectations, 1 otherwise.
#
# Overrides:
#   QU       - path to the compiler binary (default: build/bin/qu[.exe])
#   QU_FLAGS - extra compiler flags, e.g. "-O0" or "-O3" (default: none)

import os
import subprocess
import sys


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tests_dir = os.path.join(repo_root, "tests")

    qu = os.environ.get("QU")
    if not qu:
        qu = os.path.join(repo_root, "build", "bin", "qu.exe" if os.name == "nt" else "qu")
        if not os.path.isfile(qu):
            # Try Windows-style path (e.g. running from WSL)
            alt = os.path.join(repo_root, "build", "bin", "qu.exe")
            if os.path.isfile(alt):
                qu = alt
    if not os.path.isfile(qu):
        print(f"FATAL: compiler not found at {qu}", file=sys.stderr)
        return 1

    qu_flags = (os.environ.get("QU_FLAGS") or "").split()

    pass_count = 0
    expected_err = 0
    unexpected_err = 0
    unexpected_ok = 0
    skipped = 0
    unexpected_err_list = []
    unexpected_ok_list = []

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
                head = []
                for _ in range(5):
                    line = f.readline()
                    if not line:
                        break
                    head.append(line)
        except OSError:
            head = []
        if any(line.startswith("module ") for line in head):
            skipped += 1
            continue

        rel_path = os.path.relpath(file, tests_dir)
        base_name = os.path.splitext(os.path.basename(file))[0]

        try:
            proc = subprocess.run(
                [qu, file, *qu_flags],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError as exc:
            print(f"FATAL: cannot run compiler: {exc}", file=sys.stderr)
            return 1

        if proc.returncode == 0:
            if base_name.endswith("_err"):
                unexpected_ok += 1
                unexpected_ok_list.append(rel_path)
            else:
                pass_count += 1
        else:
            if base_name.endswith("_err"):
                expected_err += 1
            else:
                unexpected_err += 1
                unexpected_err_list.append(rel_path)

    total = pass_count + expected_err + unexpected_err + unexpected_ok

    print("")
    print("========================================")
    print("  Quant Test Report")
    print("========================================")
    print("")
    print(f"  Total tests run:   {total}")
    print(f"  Skipped (modules): {skipped}")
    print("  ----------------------------------------")
    print(f"  Passed:            {pass_count}")
    print(f"  Expected errors:   {expected_err}")
    print(f"  UNEXPECTED errors: {unexpected_err}")
    print(f"  UNEXPECTED passes: {unexpected_ok}")
    print("========================================")

    if unexpected_err_list:
        print("")
        print("Tests that FAIL but are NOT marked _err (need renaming):")
        for f in unexpected_err_list:
            print(f"  - {f}")

    if unexpected_ok_list:
        print("")
        print("Tests marked _err but PASSING (remove _err or fix test):")
        for f in unexpected_ok_list:
            print(f"  - {f}")

    if unexpected_err == 0 and unexpected_ok == 0:
        print("")
        print("All tests match expectations.")
        return 0
    else:
        print("")
        print("Some tests do not match expectations.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
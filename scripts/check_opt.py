#!/usr/bin/env python3
# check_opt.py - Differential test of the IR optimizer.
#
# Every runnable test (not *_err, not a library module) is compiled at a
# reference level (-O0 by default) and at each level under test (-O1 -O2 -O3
# by default). The produced executables are run and their stdout + exit code
# must be identical to the reference. This catches miscompilations introduced
# by any optimization pass, not just compile errors.
#
# Overrides:
#   QU         - compiler binary (default: build/bin/qu[.exe])
#   QU_REF     - reference level (default: -O0)
#   QU_LEVELS  - levels under test, space separated (default: "-O1 -O2 -O3")
#   QU_TIMEOUT - per-run timeout in seconds (default: 20)
#
# Exit code: 0 if every level matches the reference on every test, 1 otherwise.

import os
import shutil
import subprocess
import sys
import tempfile

# Tests that cannot be driven non-interactively or are not for the host target.
SKIP = {"stdin_read", "zp_hello"}
# Tests whose stdout is nondeterministic (prints OS handles): compare exit codes only.
EXIT_CODE_ONLY = {"file_write"}


def capture(exe, argv, cwd, timeout):
    try:
        proc = subprocess.run(
            [exe, *argv],
            cwd=cwd,
            capture_output=True,
            text=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return {"code": -999, "out": "<timeout>"}
    except OSError as exc:
        return {"code": -1, "out": f"<error: {exc}>"}
    return {"code": proc.returncode, "out": proc.stdout}


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tests_dir = os.path.join(repo_root, "tests")

    qu = os.environ.get("QU") or os.path.join(
        repo_root, "build", "bin", "qu.exe" if os.name == "nt" else "qu"
    )
    if not os.path.isfile(qu):
        print(f"FATAL: compiler not found at {qu}", file=sys.stderr)
        return 1

    ref = os.environ.get("QU_REF", "-O0")
    levels = (os.environ.get("QU_LEVELS") or "-O1 -O2 -O3").split()
    timeout = int(os.environ.get("QU_TIMEOUT", "20"))

    work = tempfile.mkdtemp(prefix="qu_check_opt_")

    pass_count = 0
    fail = 0
    skipped = 0
    compile_fail = 0
    fail_list = []

    try:
        files = []
        for dirpath, _dirnames, filenames in os.walk(tests_dir):
            for name in filenames:
                if name.endswith(".qu"):
                    files.append(os.path.join(dirpath, name))
        files.sort()

        for file in files:
            base = os.path.splitext(os.path.basename(file))[0]
            if base.endswith("_err"):
                continue
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
                continue
            if base in SKIP:
                skipped += 1
                continue

            rel = os.path.relpath(file, tests_dir)

            results = {}
            for lvl in [ref, *levels]:
                exe = os.path.join(work, f"{base}{lvl.lstrip('-')}.exe")
                # Compile from the repo root: tests load sibling modules by path (tests::lib_*).
                c = capture(qu, [file, lvl, "-o", exe], repo_root, timeout)
                if c["code"] != 0 or not os.path.isfile(exe):
                    compile_fail += 1
                    fail_list.append(f"{rel} [{lvl}]: compile failed")
                    break
                results[lvl] = capture(exe, [], work, timeout)
            else:
                ok = True
                for lvl in levels:
                    r = results[ref]
                    t = results[lvl]
                    out_differs = (r["out"] != t["out"]) and base not in EXIT_CODE_ONLY
                    if r["code"] != t["code"] or out_differs:
                        fail_list.append(
                            f"{rel} [{lvl} vs {ref}]: exit {t['code']} vs {r['code']}"
                        )
                        if out_differs:
                            fail_list.append("    stdout differs:")
                            fail_list.append("    ref: " + repr(r["out"]))
                            fail_list.append(f"    {lvl}: " + repr(t["out"]))
                        ok = False
                if ok:
                    pass_count += 1
                else:
                    fail += 1
                continue

            # Compiled failed at some level: the file was broken out of loop above.
            fail += 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    print("========================================")
    print("  Quant Optimizer Differential Report")
    print("========================================")
    print("")
    print(f"  Reference:       {ref}")
    print(f"  Levels:          {' '.join(levels)}")
    print("  ----------------------------------------")
    print(f"  Matching:        {pass_count}")
    print(f"  Mismatching:     {fail}")
    print(f"  Compile failed:  {compile_fail}")
    print(f"  Skipped:         {skipped}")
    print("========================================")

    if fail_list:
        print("")
        print("Failures:")
        for f in fail_list:
            print(f"  {f}")
        print("")
        print("Optimizer output differs from the reference.")
        return 1

    print("")
    print("All levels match the reference.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
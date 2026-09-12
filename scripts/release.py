#!/usr/bin/env python3
# release.py - Clean, configure, build and package a release.

import os
import shutil
import subprocess
import sys


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = os.path.join(repo_root, "build")
    dist = os.path.join(repo_root, "dist")

    print("[1/4] Cleaning...")
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(dist, ignore_errors=True)

    print("[2/4] Configuring...")
    subprocess.run(["cmake", "-B", build, "-DCMAKE_BUILD_TYPE=Release"], check=True, cwd=repo_root)

    print("[3/4] Building...")
    subprocess.run(["cmake", "--build", build, "--config", "Release"], check=True)

    print("[4/4] Packaging...")
    os.makedirs(dist, exist_ok=True)

    exe_src = os.path.join(build, "Release", "qu.exe" if os.name == "nt" else "qu")
    if not os.path.isfile(exe_src):
        exe_src = os.path.join(build, "qu.exe" if os.name == "nt" else "qu")
    if os.path.isfile(exe_src):
        shutil.copy2(exe_src, os.path.join(dist, os.path.basename(exe_src)))
        print("Done.")
        return 0

    print("FATAL: built executable not found", file=sys.stderr)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
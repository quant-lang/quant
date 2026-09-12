#!/usr/bin/env python3
# build_all.py - Build one dedicated Quant compiler per platform. Each build
# directory gets its own toolchain file from toolchains/ and its own backend set:
#
#   linux     -> runs on Linux x86-64, targets x86_64/aarch64 Linux
#   aarch64   -> runs on AArch64 Linux (cross-built), targets AArch64 Linux
#                and ZeroPoint
#   windows   -> qu.exe built via MinGW-w64, targets x86_64-windows
#   zeropoint -> same x86_64 windows toolchain (per ZeroPoint author's request),
#                targets aarch64-zeropoint only
#
# All builds: Ninja, Release, -j$(nproc). Missing cross toolchains are skipped.

import glob
import os
import shutil
import subprocess
import sys


def have(cmd):
    return shutil.which(cmd) is not None


def nproc():
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        try:
            return os.cpu_count() or 1
        except Exception:
            return 1


def run(argv, **kw):
    print("+", " ".join(argv))
    subprocess.run(argv, check=True, **kw)


def build(root, name, toolchain, needs, backends, default, jobs):
    if not have(needs):
        print(f"[{name}] SKIP: '{needs}' not found in PATH")
        return

    dir_name = f"build-{name}"
    bin_ext = ".exe" if os.name == "nt" else ""
    print(f"[{name}] Configuring (toolchain: {toolchain}, backends: {backends})...")
    run([
        "cmake", "-B", dir_name, "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(root, toolchain)}",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DQUANT_BACKENDS={backends}",
        f"-DQUANT_DEFAULT_TARGET={default}",
    ])

    print(f"[{name}] Building (-j{jobs})...")
    run(["cmake", "--build", dir_name, "-j", str(jobs)])

    bin_path = os.path.join(dir_name, "bin", f"qu{bin_ext}")
    if not os.path.isfile(bin_path):
        bin_path = os.path.join(dir_name, "bin", "qu")
    print(f"[{name}] Done: {bin_path}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    jobs = nproc()

    builds = [
        ("linux", "toolchains/linux-x86_64.cmake", "g++",
         "x86_64-linux;aarch64-linux", "x86_64-linux"),
        ("aarch64", "toolchains/linux-aarch64.cmake", "aarch64-unknown-linux-gnu-g++",
         "aarch64-linux;aarch64-zeropoint", "aarch64-linux"),
        ("windows", "toolchains/windows-x86_64.cmake", "x86_64-w64-mingw32-g++",
         "x86_64-windows", "x86_64-windows"),
        ("zeropoint", "toolchains/windows-x86_64.cmake", "x86_64-w64-mingw32-g++",
         "aarch64-zeropoint", "aarch64-zeropoint"),
    ]

    for name, toolchain, needs, backends, default in builds:
        build(root, name, toolchain, needs, backends, default, jobs)

    print()
    print("All builds finished:")
    for path in sorted(glob.glob(os.path.join(root, "build-*", "bin", "qu*"))):
        if os.path.isfile(path):
            print(f"  {path}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
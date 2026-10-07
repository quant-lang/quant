#!/usr/bin/env python3
"""Capture/compare target output bytes and diagnostics against a pre-refactor build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ("x86_64-linux", "aarch64-linux", "x86_64-windows", "aarch64-zeropoint")
LEVELS = (0, 2)


def capture(compiler, output):
    output.mkdir(parents=True, exist_ok=True)
    if (output / "manifest.json").exists():
        raise ValueError("output already contains a capture; choose a fresh directory")
    records = {}
    for target in TARGETS:
        for level in LEVELS:
            for source in sorted((ROOT / "tests").glob("*.qu")):
                key = f"{target}/O{level}/{source.stem}"
                artifact = key + (".exe" if target.endswith("windows") else ".o")
                destination = output / artifact
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.exists():
                    raise ValueError(f"refusing to overwrite {destination}")
                command = [str(compiler), str(source.relative_to(ROOT)), "--target", target,
                           f"-O{level}", "-c", "-o", str(destination)]
                try:
                    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=60)
                    # Hex preserves every diagnostic byte, independent of host encoding.
                    record = {"returncode": result.returncode, "stdout": result.stdout.hex(),
                              "stderr": result.stderr.hex(), "artifact": None, "sha256": None}
                    if destination.is_file():
                        record["artifact"] = artifact
                        record["sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
                except subprocess.TimeoutExpired:
                    record = {"timeout": True}
                records[key] = record
        successes = sum(r.get("returncode") == 0 for k, r in records.items() if k.startswith(target + "/"))
        print(f"{target}: {successes} successful compilations", flush=True)
    (output / "manifest.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("capture", "compare"))
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        parser.error("compiler does not exist")
    if args.mode == "compare" and args.baseline is None:
        parser.error("compare requires --baseline")
    baseline = args.baseline.resolve() if args.baseline else None
    expected = json.loads((baseline / "manifest.json").read_text(encoding="utf-8")) if baseline else None
    records = capture(compiler, args.output.resolve())
    if any(r.get("timeout") for r in records.values()):
        print("A compiler invocation timed out; the comparison is incomplete.", file=sys.stderr)
        return 1
    if args.mode == "capture":
        print(f"Captured {len(records)} cases in {args.output}")
        return 0
    differences = []
    for key in sorted(expected.keys() | records.keys()):
        old, new = expected.get(key), records.get(key)
        if old != new:
            differences.append(key)
        elif new.get("artifact"):
            # Read and compare the actual bytes as well as the saved hashes.
            if (baseline / old["artifact"]).read_bytes() != (args.output / new["artifact"]).read_bytes():
                differences.append(key)
    for key in differences:
        print(f"DIFFERENT: {key}")
    print(f"{len(records)} cases; {len(differences)} differences")
    return int(bool(differences))


if __name__ == "__main__":
    sys.exit(main())

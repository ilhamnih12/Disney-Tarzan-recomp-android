#!/usr/bin/env python3
"""Host checks for ROM identity, atomic save, and touch layout. No APK, no ROM copy."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = [
    ROOT / "runtime/src/host_checks.c",
    ROOT / "runtime/src/rom_id.c",
    ROOT / "runtime/src/save_file.c",
    ROOT / "runtime/src/touch_map.c",
]
OUT = ROOT / "build" / "host_checks"


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-std=gnu11", "-O2", f"-I{ROOT / 'runtime/include'}", "-o", str(OUT), *map(str, SRC)]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        return proc.returncode
    return subprocess.run([str(OUT)], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())

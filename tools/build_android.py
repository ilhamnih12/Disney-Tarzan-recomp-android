#!/usr/bin/env python3
"""Build the ARM64 NativeActivity library with NDK r15 clang.

The ROM is not an input. Output is build/android/libtarzan.so.
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NDK = ROOT / ".toolchains" / "ndk-r15"
CLANG = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
SYSROOT = NDK / "platforms/android-24/arch-arm64"
GCC = NDK / "toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64"
GLUE = NDK / "sources/android/native_app_glue/android_native_app_glue.c"
COMPAT = ROOT / ".toolchains" / "compat"

SRC = [
    ROOT / "runtime/src/os_runtime.c",
    ROOT / "runtime/src/gfx_f3dex.c",
    ROOT / "runtime/src/android_main.c",
    ROOT / "runtime/src/rom_id.c",
    ROOT / "runtime/src/save_file.c",
    ROOT / "runtime/src/touch_map.c",
    ROOT / "build/tarzan/weak_shims.c",
    ROOT / "build/tarzan/lookup.c",
    GLUE,
]
SRC += sorted((ROOT / "build/tarzan/recomp").glob("*.c"))

OUT = ROOT / "build/android"
OBJ = OUT / "obj"


def main() -> int:
    if not CLANG.is_file():
        print("NDK clang missing", file=sys.stderr)
        return 1
    OBJ.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(COMPAT)
    flags = [
        str(CLANG),
        "--target=aarch64-linux-android24",
        f"--sysroot={SYSROOT}",
        f"--gcc-toolchain={GCC}",
        "-std=gnu11",
        "-O2",
        "-fPIC",
        "-fvisibility=hidden",
        "-DANDROID",
        f"-I{ROOT / 'runtime/include'}",
        f"-I{ROOT / 'build/tarzan'}",
        f"-I{ROOT / 'build/tarzan/recomp'}",
        f"-I{NDK / 'sources/android/native_app_glue'}",
        f"-I{SYSROOT / 'usr/include'}",
        "-Wno-implicit-function-declaration",
        "-Wno-int-conversion",
        "-c",
    ]
    objs = []
    failed = 0
    for src in SRC:
        obj = OBJ / (src.stem + ".o")
        objs.append(obj)
        if obj.exists() and obj.stat().st_mtime >= src.stat().st_mtime:
            continue
        print(f"cc {src.name}", flush=True)
        proc = subprocess.run(flags + [str(src), "-o", str(obj)], env=env, cwd=ROOT)
        if proc.returncode != 0:
            failed += 1
            print(f"FAIL {src}", file=sys.stderr)
            if failed > 8:
                return 1
    if failed:
        return 1
    so = OUT / "libtarzan.so"
    link = [
        str(CLANG),
        "--target=aarch64-linux-android24",
        f"--sysroot={SYSROOT}",
        f"--gcc-toolchain={GCC}",
        "-shared",
        "-fPIC",
        "-Wl,-soname,libtarzan.so",
        "-Wl,--no-undefined",
        "-o",
        str(so),
        *[str(o) for o in objs],
        "-llog",
        "-landroid",
        "-lm",
        "-lc",
    ]
    print("link", so, flush=True)
    proc = subprocess.run(link, env=env, cwd=ROOT)
    if proc.returncode != 0:
        return proc.returncode
    print(f"built {so} ({so.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

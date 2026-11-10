#!/usr/bin/env python3
"""Compile the recompiled Tarzan CPU plus the native runtime for the host."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECOMP = ROOT / "build" / "tarzan" / "recomp"
OBJ = ROOT / "build" / "host-obj"
BIN = ROOT / "artifacts" / "tarzan-host"

CFLAGS = [
    "-std=gnu11",
    "-O1",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-pthread",
    "-I",
    str(ROOT / "runtime" / "include"),
    "-I",
    str(RECOMP),
    "-include",
    str(RECOMP / "funcs.h"),
    "-Wno-unused-function",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-unused-label",
]


def rewrite_zero_dest(src: Path) -> None:
    text = src.read_text(errors="replace")
    if "\n    0 = " not in text and not text.startswith("    0 = "):
        return
    out = []
    changed = False
    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith("0 = ") and stripped.rstrip().endswith(";"):
            indent = line[: len(line) - len(stripped)]
            expr = stripped[4:].strip()
            if expr.endswith(";"):
                expr = expr[:-1]
            line = f"{indent}(void)({expr});\n"
            changed = True
        out.append(line)
    if changed:
        src.write_text("".join(out), encoding="utf-8")


def stub_function(src: Path) -> None:
    name = src.stem
    vram = "0"
    if name.startswith("func_") or name.startswith("static_"):
        hexpart = name.split("_")[-1]
        if len(hexpart) == 8:
            vram = "0x" + hexpart + "u"
    src.write_text(
        '#include "tarzan_recomp.h"\n\n'
        f"RECOMP_FUNC void {name}(uint8_t* rdram, recomp_context* ctx) {{\n"
        f"    recomp_unimplemented(rdram, ctx, {vram});\n"
        "}\n",
        encoding="utf-8",
    )


def compile_one(src: Path, obj: Path) -> None:
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", *CFLAGS, "-c", str(src), "-o", str(obj)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main() -> None:
    sources = sorted(RECOMP.glob("*.c"))
    sources += [
        ROOT / "build" / "tarzan" / "lookup.c",
        ROOT / "build" / "tarzan" / "weak_shims.c",
        ROOT / "runtime" / "src" / "os_runtime.c",
        ROOT / "runtime" / "src" / "gfx_f3dex.c",
        ROOT / "runtime" / "src" / "save_file.c",
        ROOT / "runtime" / "src" / "host_main.c",
    ]
    if not (RECOMP / "funcs.h").exists():
        raise SystemExit("missing recompiled funcs.h; run discover + N64Recomp first")
    OBJ.mkdir(parents=True, exist_ok=True)
    jobs = []
    objects = []
    for src in sources:
        obj = OBJ / (src.stem + ".o")
        objects.append(obj)
        if obj.exists() and obj.stat().st_mtime >= src.stat().st_mtime:
            continue
        jobs.append((src, obj))
    for src, _obj in jobs:
        if src.suffix == ".c" and "recomp" in src.parts:
            rewrite_zero_dest(src)
    print(f"compiling {len(jobs)} of {len(sources)} translation units")
    from concurrent.futures import ThreadPoolExecutor, as_completed

    failed_src: list[Path] = []
    with ThreadPoolExecutor(max_workers=2) as pool:
        futs = {pool.submit(compile_one, src, obj): (src, obj) for src, obj in jobs}
        for fut in as_completed(futs):
            src, obj = futs[fut]
            try:
                fut.result()
            except subprocess.CalledProcessError as exc:
                failed_src.append(src)
                err = exc.stderr.decode("utf-8", "replace") if exc.stderr else ""
                print("FAIL", src.name, file=sys.stderr)
                if err:
                    print("\n".join(err.splitlines()[:4]), file=sys.stderr)
                if obj.exists():
                    obj.unlink()
    if failed_src:
        print(f"stubbing {len(failed_src)} functions the recompiler emitted invalid C for")
        for src in failed_src:
            stub_function(src)
            obj = OBJ / (src.stem + ".o")
            compile_one(src, obj)
        (ROOT / "build" / "logs").mkdir(parents=True, exist_ok=True)
        (ROOT / "build" / "logs" / "stubbed-c.txt").write_text(
            "\n".join(sorted(p.name for p in failed_src)) + "\n", encoding="utf-8"
        )
    BIN.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-pthread", "-rdynamic", "-o", str(BIN), *[str(o) for o in objects], "-lm"]
    print("linking", BIN)
    subprocess.run(cmd, check=True)
    print("host binary", BIN)


if __name__ == "__main__":
    main()

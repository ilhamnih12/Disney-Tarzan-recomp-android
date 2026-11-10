#!/usr/bin/env python3
"""Emit weak libultra shims so every renamed OS symbol has a definition.

Strong implementations in os_runtime.c override these.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("symbol_lists", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    text = args.symbol_lists.read_text(errors="replace")
    chunks = re.findall(r"const std::unordered_set<std::string> N64Recomp::(\w+) \{([^}]*)\}", text, re.S)
    names: set[str] = set()
    for name, body in chunks:
        if name in ("reimplemented_funcs", "ignored_funcs"):
            names.update(re.findall(r'"([^"]+)"', body))
    lines = [
        '#include "tarzan_recomp.h"',
        "",
        "/* Weak fallbacks. A strong definition in os_runtime.c wins at link time. */",
    ]
    for name in sorted(names):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            continue
        ret = "-1" if any(tok in name for tok in ("Pfs", "Flash", "Voice", "Motor", "Eeprom")) else "0"
        lines.append(f'__attribute__((weak)) void {name}_recomp(uint8_t* rdram, recomp_context* ctx) {{')
        lines.append("    static int seen;")
        lines.append("    if (!seen) {")
        lines.append("        seen = 1;")
        lines.append(f'        fprintf(stderr, "SHIM {name}\\n");')
        lines.append("    }")
        lines.append("    recomp_unimplemented(rdram, ctx, 0);")
        lines.append(f"    if (ctx) ctx->r2 = (gpr)(int64_t)({ret});")
        lines.append("}")
        lines.append("")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines), encoding="utf-8")
    print(f"weak shims: {len(names)} -> {args.out}")


if __name__ == "__main__":
    main()

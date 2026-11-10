#!/usr/bin/env python3
"""Discover Disney's Tarzan (U) functions and emit an N64Recomp symbol file.

The ROM is read from the user-supplied archive. Nothing in this script copies
the ROM into the application tree that gets packaged.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KNOWN_SHA256 = "e56d09772489a1a7426693396e6cbb24c1bfcd0ced11b92bc1546b081e9f3c06"
ENTRY_VRAM = 0x80000400
ENTRY_ROM = 0x1000
HEADER_DELTA = ENTRY_VRAM - ENTRY_ROM  # vram = rom_off + HEADER_DELTA


def be32(buf: bytes, off: int) -> int:
    return struct.unpack_from(">I", buf, off)[0]


def load_rom(zip_path: Path, out_rom: Path) -> bytes:
    with zipfile.ZipFile(zip_path) as zf:
        infos = [i for i in zf.infolist() if i.filename.lower().endswith((".z64", ".n64", ".v64"))]
        if len(infos) != 1:
            raise SystemExit(f"expected one ROM in {zip_path}, found {len(infos)}")
        data = zf.read(infos[0])
    if data[:4] == b"\x37\x80\x40\x12":
        data = b"".join(data[i + 1 : i + 2] + data[i : i + 1] for i in range(0, len(data), 2))
    elif data[:4] == b"\x40\x12\x37\x80":
        data = b"".join(struct.pack(">I", struct.unpack_from("<I", data, i)[0]) for i in range(0, len(data), 4))
    if data[:4] != b"\x80\x37\x12\x40":
        raise SystemExit("ROM is not a big-endian .z64 image")
    out_rom.parent.mkdir(parents=True, exist_ok=True)
    if not out_rom.exists() or out_rom.read_bytes() != data:
        out_rom.write_bytes(data)
    return data


def parse_symbol_sets(symbol_lists: Path) -> tuple[set[str], set[str], set[str]]:
    text = symbol_lists.read_text(errors="replace")
    chunks = re.findall(r"const std::unordered_set<std::string> N64Recomp::(\w+) \{([^}]*)\}", text, re.S)
    found = {name: set(re.findall(r'"([^"]+)"', body)) for name, body in chunks}
    return found.get("reimplemented_funcs", set()), found.get("ignored_funcs", set()), found.get("renamed_funcs", set())


def load_n64sym(path: Path) -> list[tuple[int, int, str]]:
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(errors="replace").splitlines():
        line = line.split("#", 1)[0].strip()
        parts = line.split()
        if len(parts) < 2:
            continue
        addr = int(parts[0], 16)
        if 0x70000000 <= addr < 0x80000000:
            addr += 0x10000000
        if len(parts) >= 3 and re.fullmatch(r"[0-9A-Fa-f]+", parts[1]):
            size = int(parts[1], 16)
            name = parts[2]
        else:
            size = 0
            name = parts[1]
        rows.append((addr, size, name))
    return rows


def sext16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def is_jr_ra(word: int) -> bool:
    return word == 0x03E00008


def is_jal(word: int) -> bool:
    return (word >> 26) == 3


def jal_target(pc: int, word: int) -> int:
    return (pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def is_sp_prologue(word: int) -> bool:
    return (word & 0xFFFF8000) == 0x27BD8000


def discover(rom: bytes, named: list[tuple[int, int, str]], reimpl: set[str], ignored: set[str], renamed: set[str]) -> dict:
    if len(rom) < ENTRY_ROM + 16:
        raise SystemExit("ROM is too small")
    title = rom[0x20:0x34].split(b"\0", 1)[0].decode("ascii", "replace").strip()
    code = rom[0x3B:0x3F].decode("ascii", "replace")
    entry = be32(rom, 8)
    if entry != ENTRY_VRAM:
        raise SystemExit(f"unsupported entry point {entry:#x}; this recipe expects {ENTRY_VRAM:#x}")

    w0 = be32(rom, ENTRY_ROM)
    w1 = be32(rom, ENTRY_ROM + 4)
    if (w0 >> 26) != 0x0F or (w1 >> 26) != 9:
        raise SystemExit("entry does not match the expected BSS-clear prologue")
    bss = ((w0 & 0xFFFF) << 16) + sext16(w1 & 0xFFFF)
    bss &= 0xFFFFFFFF
    if not (ENTRY_VRAM < bss < ENTRY_VRAM + 0x800000):
        raise SystemExit(f"BSS address {bss:#x} is implausible")
    section_size = bss - ENTRY_VRAM
    if ENTRY_ROM + section_size > len(rom):
        raise SystemExit("BSS-derived section extends past the ROM")

    def word_at_vram(vram: int) -> int:
        return be32(rom, vram - HEADER_DELTA)

    section_end = bss
    starts: set[int] = {ENTRY_VRAM}

    # Direct calls and returns.
    pc = ENTRY_VRAM
    while pc < section_end:
        word = word_at_vram(pc)
        if is_jal(word):
            tgt = jal_target(pc, word)
            if ENTRY_VRAM <= tgt < section_end and (tgt & 3) == 0:
                starts.add(tgt)
        if is_jr_ra(word):
            nxt = pc + 8
            skipped = 0
            while nxt < section_end and word_at_vram(nxt) == 0 and skipped < 8:
                nxt += 4
                skipped += 1
            if ENTRY_VRAM <= nxt < section_end:
                starts.add(nxt)
        pc += 4

    # lui/addiu and lui/ori pairs: function pointers and absolute addresses.
    last_lui: dict[int, tuple[int, int]] = {}
    pc = ENTRY_VRAM
    while pc < section_end:
        word = word_at_vram(pc)
        op = word >> 26
        rs = (word >> 21) & 31
        rt = (word >> 16) & 31
        if op == 0x0F and rt != 0:  # lui
            last_lui[rt] = (pc, word & 0xFFFF)
        elif op in (9, 0x0D) and rs == rt and rt in last_lui:  # addiu / ori
            lui_pc, hi = last_lui[rt]
            if 0 < pc - lui_pc <= 16:
                addr = ((hi << 16) + (sext16(word & 0xFFFF) if op == 9 else (word & 0xFFFF))) & 0xFFFFFFFF
                if ENTRY_VRAM <= addr < section_end and (addr & 3) == 0:
                    starts.add(addr)
        if op != 0x0F:
            # Keep the lui live only for a short window; drop stale entries.
            stale = [reg for reg, (lpc, _) in last_lui.items() if pc - lpc > 16]
            for reg in stale:
                last_lui.pop(reg, None)
        pc += 4

    official = reimpl | ignored | renamed
    trusted_name: dict[int, str] = {}
    for addr, size, name in named:
        if not (ENTRY_VRAM <= addr < section_end) or (addr & 3):
            continue
        c_name = "bzero" if name == "_bzero" else name
        if "_data_" in name or name.endswith("_data"):
            continue
        in_cluster = 0x8003A000 <= addr < 0x80048000
        known = c_name in official or name in official
        if not known:
            continue
        if not in_cluster and size < 0x20:
            continue
        # Prefer a sized match over a zero-size relocation hit.
        prev = trusted_name.get(addr)
        if prev is None or (size > 0 and prev):
            trusted_name[addr] = c_name
            starts.add(addr)

    # Force the boot chain described by the entry's indirect jump.
    boot = None
    for off in range(ENTRY_VRAM, ENTRY_VRAM + 0x40, 4):
        word = word_at_vram(off)
        if (word >> 26) == 0x0F and ((word >> 16) & 31) == 10:  # lui t2
            nxt = word_at_vram(off + 4)
            if (nxt >> 26) == 9 and ((nxt >> 21) & 31) == 10:
                boot = (((word & 0xFFFF) << 16) + sext16(nxt & 0xFFFF)) & 0xFFFFFFFF
                break
    if boot and ENTRY_VRAM <= boot < section_end:
        starts.add(boot)

    ordered = sorted(s for s in starts if ENTRY_VRAM <= s < section_end and (s & 3) == 0)
    # Drop starts that landed inside a 4-byte hole.
    filtered = []
    for addr in ordered:
        if filtered and addr - filtered[-1] < 4:
            continue
        filtered.append(addr)

    functions = []
    used_names: dict[str, int] = {}

    def unique(base: str) -> str:
        base = re.sub(r"[^A-Za-z0-9_]", "_", base)
        if not base or base[0].isdigit():
            base = "fn_" + base
        n = used_names.get(base, 0)
        used_names[base] = n + 1
        return base if n == 0 else f"{base}_{n+1}"

    for i, addr in enumerate(filtered):
        limit = filtered[i + 1] if i + 1 < len(filtered) else section_end
        end = addr
        bad = 0
        pc = addr
        last_good = addr
        while pc < limit:
            word = word_at_vram(pc)
            pointerish = 0x80000000 <= word < 0x80800000
            if word == 0 or pointerish:
                bad += 1
                if bad >= 8 and pc > addr + 8:
                    break
            else:
                bad = 0
                last_good = pc + 4
            end = pc + 4
            pc += 4
        if bad >= 8:
            end = last_good
        end = min(end, limit)
        if end <= addr:
            end = min(addr + 4, limit)
        size = end - addr
        size -= size % 4
        if size <= 0:
            continue
        # Reject ranges that are entirely zeros or pointer tables.
        words = [word_at_vram(addr + n) for n in range(0, size, 4)]
        if all(w == 0 or 0x80000000 <= w < 0x80800000 for w in words):
            continue
        c_name = trusted_name.get(addr)
        if c_name:
            name = unique(c_name)
        else:
            name = unique(f"func_{addr:08X}")
        functions.append({"name": name, "vram": addr, "size": size, "rom": addr - HEADER_DELTA})

    return {
        "title": title,
        "code": code,
        "sha256": hashlib.sha256(rom).hexdigest(),
        "known_us_rom": hashlib.sha256(rom).hexdigest() == KNOWN_SHA256,
        "entry": ENTRY_VRAM,
        "bss": bss,
        "section_rom": ENTRY_ROM,
        "section_vram": ENTRY_VRAM,
        "section_size": section_size,
        "boot": boot,
        "functions": functions,
        "reimplemented": sorted(reimpl),
        "ignored": sorted(ignored),
        "renamed": sorted(renamed),
    }


def final_symbol(name: str, vram: int, sets: dict) -> tuple[str, str]:
    """Return (link_symbol, kind). kind is recomp|renamed|provided|game."""
    if vram == ENTRY_VRAM:
        return "recomp_entrypoint", "entry"
    if name in sets["reimplemented"] or name in sets["ignored"]:
        return name + "_recomp", "provided"
    if name in sets["renamed"]:
        return name + "_recomp", "renamed"
    return name, "game"


def emit(info: dict, out_dir: Path, rom_path: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    sets = {
        "reimplemented": set(info["reimplemented"]),
        "ignored": set(info["ignored"]),
        "renamed": set(info["renamed"]),
    }
    symbols = out_dir / "symbols.toml"
    with symbols.open("w", encoding="utf-8") as fh:
        fh.write("# Generated by tools/discover_tarzan.py — do not edit by hand.\n")
        fh.write("[[section]]\n")
        fh.write('name = "main"\n')
        fh.write(f"rom = 0x{info['section_rom']:08X}\n")
        fh.write(f"vram = 0x{info['section_vram']:08X}\n")
        fh.write(f"size = 0x{info['section_size']:X}\n")
        fh.write("functions = [\n")
        for fn in info["functions"]:
            fh.write(f'  {{ name = "{fn["name"]}", vram = 0x{fn["vram"]:08X}, size = 0x{fn["size"]:X} }},\n')
        fh.write("]\n")

    config = out_dir / "tarzan.toml"
    config.write_text(
        "\n".join(
            [
                "# N64Recomp recipe for Disney's Tarzan (U). ROM path is local and not packaged.",
                "[input]",
                "entrypoint = 0x80000400",
                'symbols_file_path = "symbols.toml"',
                f'rom_file_path = "{rom_path}"',
                'output_func_path = "recomp"',
                "functions_per_output_file = 1",
                'recomp_include = "#include \\"tarzan_recomp.h\\""',
                "single_file_output = false",
                "uses_mips3_float_mode = false",
                "trace_mode = false",
                "",
            ]
        ),
        encoding="utf-8",
    )

    manifest_fns = []
    lookup_lines = [
        '#include "tarzan_recomp.h"',
        '#include "funcs.h"',
        "",
        "int32_t section_addresses_storage[1] = {0};",
        "int32_t* section_addresses = section_addresses_storage;",
        "",
        "typedef struct { uint32_t vram; recomp_func_t* fn; } TzFunc;",
        "static const TzFunc tz_funcs[] = {",
    ]
    for fn in info["functions"]:
        link, kind = final_symbol(fn["name"], fn["vram"], sets)
        manifest_fns.append({**fn, "link": link, "kind": kind})
        lookup_lines.append(f"    {{ 0x{fn['vram']:08X}u, {link} }},")
    lookup_lines += [
        "};",
        "static const uint32_t tz_func_count = sizeof(tz_funcs) / sizeof(tz_funcs[0]);",
        "",
        "recomp_func_t* get_function(int32_t vram) {",
        "    uint32_t key = (uint32_t)vram;",
        "    uint32_t lo = 0, hi = tz_func_count;",
        "    while (lo < hi) {",
        "        uint32_t mid = lo + (hi - lo) / 2;",
        "        uint32_t cur = tz_funcs[mid].vram;",
        "        if (cur == key) return tz_funcs[mid].fn;",
        "        if (cur < key) lo = mid + 1;",
        "        else hi = mid;",
        "    }",
        "    return 0;",
        "}",
        "",
        "gpr get_entrypoint_address(void) { return (gpr)(int32_t)0x80000400u; }",
        "",
    ]
    (out_dir / "lookup.c").write_text("\n".join(lookup_lines) + "\n", encoding="utf-8")
    info_out = dict(info)
    info_out["functions"] = manifest_fns
    info_out.pop("reimplemented")
    info_out.pop("ignored")
    info_out.pop("renamed")
    (out_dir / "manifest.json").write_text(json.dumps(info_out, indent=2) + "\n", encoding="utf-8")
    kinds = {}
    for fn in manifest_fns:
        kinds[fn["kind"]] = kinds.get(fn["kind"], 0) + 1
    print(f"title={info['title']!r} code={info['code']} known_us={info['known_us_rom']}")
    print(f"bss={info['bss']:#x} section_size={info['section_size']:#x} boot={info['boot']}")
    print(f"functions={len(manifest_fns)} kinds={kinds}")
    interesting = [fn for fn in manifest_fns if fn["vram"] in (0x80000400, 0x80021010, 0x80021088, 0x8003E9C0, 0x8003F830, 0x800427E8)]
    for fn in interesting:
        print(f"  {fn['vram']:08X} {fn['size']:4X} {fn['name']} -> {fn['link']} ({fn['kind']})")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--zip", type=Path, default=ROOT / "Disney's Tarzan (U) [!].zip")
    parser.add_argument("--rom", type=Path, default=ROOT / ".work" / "rom" / "tarzan.z64")
    parser.add_argument("--symbols", type=Path, default=ROOT / ".work" / "rom" / "symbols_sized.txt")
    parser.add_argument("--symbol-lists", type=Path, default=ROOT / ".work" / "N64Recomp" / "src" / "symbol_lists.cpp")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "tarzan")
    args = parser.parse_args()
    rom = load_rom(args.zip, args.rom)
    reimpl, ignored, renamed = parse_symbol_sets(args.symbol_lists)
    named = load_n64sym(args.symbols)
    info = discover(rom, named, reimpl, ignored, renamed)
    emit(info, args.out, args.rom.resolve())


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""freeze - pack brickie's Python frontend + templates + **native tools** into an embeddable ELF payload.

## Why it exists

Requirements:
  1. "brickie's python code must also be compiled into an elf" - a host executable just like `brickie-gen`;
  2. "brickie-gen and friends must all be compiled into the python brickie elf" - native child tools
     (today `brickie-gen`, later `brickie-core`/`brickie-verify` ...) are **all embedded in the same ELF**,
     making `brickie` **single-file self-contained**.

Shape (user ruling, see ADR-0004 §7): a **zero-dependency launcher ELF**. A hand-written C++ launcher
(`cxx/launcher.cpp`) embeds the payload **as data**, unpacks it to a temp dir at run time, then uses the
system `python3` to interpret the embedded code and run the embedded native tools. Therefore:

  * the artifact is a real ELF (`build/host/<arch>/<os>/bin/brickie`), not a script;
  * **one file** is enough to distribute: no `PYTHONPATH`, no source tree, no sibling `brickie-gen`,
    no `g++`;
  * **zero new third-party dependencies** - no PyInstaller/Nuitka, only g++ and the Python stdlib.

## Payload layout

    python/brickie/**         L5 frontend (mode 0644)
    templates/**              skeleton templates (mode 0644)
    schema/*.schema.json      declaration-surface shape schemas (mode 0644; shape-gate input for BRV-D2)
    bin/<tool>                **native tools** (mode 0755; given one by one via --embed)

## Payload format: uncompressed ustar tar

Not using zip/compression is deliberate: the launcher must unpack without linking zlib, and tar is a
512-byte fixed header plus fixed-size data, so the parser is ~60 readable lines. The payload is
**uncompressed with normalized metadata (mtime/uid/gid/uname all zero, paths sorted)** => byte-for-byte
reproducible (the seed's "matches this build" gate depends on it).

## Outputs

  --c   <file>          generated C++ source: `const unsigned char brickie_payload[]` + length
  --tar <file>          payload body (optional; for manual `tar tvf` inspection)
  --embed <src>[=<dst>] embed an already-built **native tool** into the payload (repeatable);
                        default destination = `bin/<basename>`, mode 0755.
                        With several tools, pass one flag each, e.g.:
                          --embed build/.../bin/brickie-gen=bin/brickie-gen \
                          --embed build/.../bin/brickie-core=bin/brickie-core
                        Destinations are sorted, mode bits written explicitly, mtime/uid/gid zeroed =>
                        byte-for-byte reproducible; a duplicate destination is rejected before packing
                        (see collect_entries).

Usage:
  python3 tools/brickie/freeze.py --c <out.cpp> --tar <out.tar> \
      --embed build/host/x86-64/linux/bin/brickie-gen=bin/brickie-gen \
      --embed build/host/x86-64/linux/bin/brickie-core=bin/brickie-core
Exit code: 0 success / 2 usage or environment error
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

TOOL_ROOT = Path(__file__).resolve().parent

# Payload roots in source form: L5 frontend + templates + schema (tests, pyproject and build scripts stay out).
# `schema/` must be in the payload: L5's **shape validation** is driven by schema/*.schema.json (BRV-D2),
# and the launcher ELF can only see what is in the payload => without it the shape gate would silently
# degrade in the single-file form.
PAYLOAD_ROOTS = ("python", "templates", "schema")

# One payload entry: (relative path, content, mode)
Entry = tuple[str, bytes, int]

_BLOCK = 512


# ------------------------------------------------------------------ tar writing

def _octal(value: int, field_width: int) -> bytes:
    """Fixed-width octal field (with trailing NUL). field_width is the total field width."""
    text = format(value, "o").rjust(field_width - 1, "0")
    return text.encode("ascii") + b"\0"


def _tar_header(name: str, size: int, mode: int) -> bytes:
    name_b = name.encode("utf-8")
    if len(name_b) > 100:
        raise SystemExit(f"FAIL: payload path exceeds the ustar 100-byte limit: {name}")

    header = bytearray(_BLOCK)
    header[0:len(name_b)] = name_b
    header[100:108] = _octal(mode, 8)          # mode (native tools need the executable bit)
    header[108:116] = _octal(0, 8)             # uid (zeroed => reproducible)
    header[116:124] = _octal(0, 8)             # gid
    header[124:136] = _octal(size, 12)         # size
    header[136:148] = _octal(0, 12)            # mtime (zeroed => reproducible)
    header[148:156] = b" " * 8                 # chksum: spaces first
    header[156:157] = b"0"                     # typeflag: regular file
    header[257:263] = b"ustar\0"               # magic
    header[263:265] = b"00"                    # version

    checksum = sum(header)
    header[148:156] = format(checksum, "o").rjust(6, "0").encode("ascii") + b"\0 "
    return bytes(header)


def _pad(data: bytes) -> bytes:
    remainder = len(data) % _BLOCK
    return b"" if remainder == 0 else b"\0" * (_BLOCK - remainder)


def build_tar(entries: list[Entry]) -> bytes:
    """entries = [(relative path, content, mode)]; the caller sorts them (determinism)."""
    out = bytearray()
    for path, data, mode in entries:
        out += _tar_header(path, len(data), mode)
        out += data
        out += _pad(data)
    out += b"\0" * (_BLOCK * 2)               # end marker
    return bytes(out)


# ------------------------------------------------------------------ payload collection

def _tool_entries(embeds: list[str]) -> list[Entry]:
    """Turn `--embed <src>[=<dst>]` into payload entries (native tools, 0755)."""
    entries: list[Entry] = []
    for spec in embeds:
        src_text, _, dst = spec.partition("=")
        src = Path(src_text)
        if not src.is_absolute():
            src = (Path.cwd() / src).resolve()
        if not src.is_file():
            raise SystemExit(f"FAIL: --embed file does not exist: {src} (build it first)")
        if not dst:
            dst = f"bin/{src.name}"
        if dst.startswith("/") or ".." in Path(dst).parts:
            raise SystemExit(f"FAIL: --embed destination is invalid: {dst}")
        entries.append((dst, src.read_bytes(), 0o755))
    return entries


def collect_entries(embeds: list[str]) -> list[Entry]:
    entries: list[Entry] = []
    for root in PAYLOAD_ROOTS:
        base = TOOL_ROOT / root
        if not base.is_dir():
            raise SystemExit(f"FAIL: payload root directory does not exist: {base}")
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(TOOL_ROOT).as_posix()
            if "__pycache__" in rel or rel.endswith(".pyc"):
                continue
            entries.append((rel, path.read_bytes(), 0o644))

    entries.extend(_tool_entries(embeds))

    entries.sort(key=lambda item: item[0])    # sort by path => byte-for-byte reproducible payload
    if not entries:
        raise SystemExit("FAIL: payload is empty")

    seen: set[str] = set()
    for path, _, _ in entries:
        if path in seen:
            raise SystemExit(f"FAIL: duplicate payload path: {path}")
        seen.add(path)
    return entries


# ------------------------------------------------------------------ C++ generation

def write_c(path: Path, payload: bytes) -> None:
    lines = [
        "// Generated by tools/brickie/freeze.py - **do not edit by hand** (edit freeze.py instead)",
        "//",
        "// Content: brickie's Python frontend (python/brickie/**), skeleton templates (templates/**) and",
        "// native tools (bin/**, e.g. brickie-gen), packed as an uncompressed ustar tar and embedded byte for byte.",
        "// At run time launcher.cpp unpacks it to a temp dir, then uses the system python3 to interpret and run",
        "// the embedded tools - see ADR-0004 §7 and tools/brickie/README.md.",
        "",
        "extern \"C\" {",
        # The definition must be extern: a namespace-scope const in C++ has **internal linkage** by
        # default, and without extern the launcher would fail to link with undefined reference.
        f"extern const unsigned long brickie_payload_size = {len(payload)}UL;",
        "extern const unsigned char brickie_payload[] = {",
    ]
    for offset in range(0, len(payload), 16):
        chunk = payload[offset:offset + 16]
        lines.append("    " + " ".join(f"0x{byte:02x}," for byte in chunk))
    lines.append("};")
    lines.append("}  // extern \"C\"")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


# --------------------------------------------------------------------- entry point

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="freeze", description="pack brickie's self-contained payload")
    parser.add_argument("--c", required=True, metavar="FILE", help="generated C++ source")
    parser.add_argument("--tar", metavar="FILE", help="payload tar (optional, for manual inspection)")
    parser.add_argument(
        "--embed",
        action="append",
        default=[],
        metavar="SRC[=DST]",
        help="embed a built native tool (repeatable); default destination bin/<basename>, mode 0755",
    )
    args = parser.parse_args(argv)

    entries = collect_entries(args.embed)
    payload = build_tar(entries)

    c_path = Path(args.c)
    c_path.parent.mkdir(parents=True, exist_ok=True)
    write_c(c_path, payload)

    if args.tar:
        tar_path = Path(args.tar)
        tar_path.parent.mkdir(parents=True, exist_ok=True)
        tar_path.write_bytes(payload)

    tools = [path for path, _, mode in entries if mode & 0o111]
    detail = ("(" + ", ".join(tools) + ")") if tools else "(none)"
    print(f"freeze: {len(entries)} files, {len(tools)} native tools {detail} -> "
          f"{len(payload)} byte payload -> {c_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

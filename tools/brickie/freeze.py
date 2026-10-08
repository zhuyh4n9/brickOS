#!/usr/bin/env python3
"""freeze — 把 brickie 的 Python 前端 + 模板 + **原生工具**打包成可嵌入 ELF 的载荷。

## 为什么有它

需求:
  1. "brickie 的 python 代码也需要编译为 elf" —— 与 `brickie-gen` 一样成为宿主可执行;
  2. "brickie-gen 等工具均需要编译到 python 的 brickie elf 中" —— 原生子进程工具
     (现在 `brickie-gen`, 将来 `brickie-core`/`brickie-verify` …)**全部嵌进同一个 ELF**,
     使 `brickie` **单文件自包含**。

形态(用户裁定, 见 ADR-0004 §7): **零依赖入口 ELF**。自写 C++ 启动器
(`cxx/launcher.cpp`)把载荷**作为数据嵌入**, 运行时解包到临时目录, 再用系统
`python3` 解释嵌入的代码、执行嵌入的原生工具。于是:

  * 产物是真 ELF(`build/host/<arch>/<os>/bin/brickie`), 不是脚本;
  * **一个文件**即可分发: 不需要 `PYTHONPATH`、不需要源码树、不需要同目录的
    `brickie-gen`、不需要 `g++`;
  * **零新增第三方依赖** —— 不用 PyInstaller/Nuitka, 只借 g++ 与 Python 标准库。

## 载荷布局

    python/brickie/**         L5 前端(模式 0644)
    templates/**              骨架模板(模式 0644)
    schema/*.schema.json      声明面形状 schema(模式 0644; BRV-D2 的形状门输入)
    bin/<tool>                **原生工具**(模式 0755; 由 --embed 逐个指定)

## 载荷格式: 未压缩 ustar tar

不用 zip/压缩是刻意的: 启动器要能在**不链 zlib** 的前提下解包,
而 tar 是 512 字节定长头 + 定长数据, 解析器 ~60 行且可读。载荷用
**未压缩 + 归一化元数据(mtime/uid/gid/uname 全零, 路径排序)** ⇒ 逐字节可复现
(自举种子的"与本次构建一致"门禁依赖这条)。

## 产物

  --c   <file>          生成的 C++ 源: `const unsigned char brickie_payload[]` + 长度
  --tar <file>          载荷本体(可选; 供 `tar tvf` 人工检查)
  --embed <src>[=<dst>] 把一个**已构建的原生工具**嵌进载荷(可重复);
                        缺省落点 = `bin/<basename>`, 模式 0755。
                        多工具时逐条给, 如:
                          --embed build/.../bin/brickie-gen=bin/brickie-gen \
                          --embed build/.../bin/brickie-core=bin/brickie-core
                        落点按路径排序、模式位显式写入、mtime/uid/gid 归零 ⇒ 逐字节可复现;
                        同一落点重复嵌入会在打包前报错(见 collect_entries)。

用法:
  python3 tools/brickie/freeze.py --c <out.cpp> --tar <out.tar> \
      --embed build/host/x86-64/linux/bin/brickie-gen=bin/brickie-gen \
      --embed build/host/x86-64/linux/bin/brickie-core=bin/brickie-core
退出码: 0 成功 / 2 用法或环境错
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

TOOL_ROOT = Path(__file__).resolve().parent

# 源码形态的载荷根: L5 前端 + 模板 + schema(测试、pyproject、构建脚本都不进)。
# `schema/` 必须在载荷里: L5 的**形状校验**按 schema/*.schema.json 做(BRV-D2),
# 而入口 ELF 运行时只有载荷里的东西可用 ⇒ 不嵌进去, 形状门在单文件形态下会静默降级。
PAYLOAD_ROOTS = ("python", "templates", "schema")

# 一个载荷条目: (相对路径, 内容, 模式)
Entry = tuple[str, bytes, int]

_BLOCK = 512


# ------------------------------------------------------------------ tar 写入

def _octal(value: int, field_width: int) -> bytes:
    """定长八进制字段(含结尾 NUL)。field_width 是字段总宽。"""
    text = format(value, "o").rjust(field_width - 1, "0")
    return text.encode("ascii") + b"\0"


def _tar_header(name: str, size: int, mode: int) -> bytes:
    name_b = name.encode("utf-8")
    if len(name_b) > 100:
        raise SystemExit(f"FAIL: 载荷路径超过 ustar 的 100 字节上限: {name}")

    header = bytearray(_BLOCK)
    header[0:len(name_b)] = name_b
    header[100:108] = _octal(mode, 8)          # mode(原生工具需要可执行位)
    header[108:116] = _octal(0, 8)             # uid(归零 ⇒ 可复现)
    header[116:124] = _octal(0, 8)             # gid
    header[124:136] = _octal(size, 12)         # size
    header[136:148] = _octal(0, 12)            # mtime(归零 ⇒ 可复现)
    header[148:156] = b" " * 8                 # chksum 先填空格
    header[156:157] = b"0"                     # typeflag: 普通文件
    header[257:263] = b"ustar\0"               # magic
    header[263:265] = b"00"                    # version

    checksum = sum(header)
    header[148:156] = format(checksum, "o").rjust(6, "0").encode("ascii") + b"\0 "
    return bytes(header)


def _pad(data: bytes) -> bytes:
    remainder = len(data) % _BLOCK
    return b"" if remainder == 0 else b"\0" * (_BLOCK - remainder)


def build_tar(entries: list[Entry]) -> bytes:
    """entries = [(相对路径, 内容, 模式)]; 调用方负责排序(确定性)。"""
    out = bytearray()
    for path, data, mode in entries:
        out += _tar_header(path, len(data), mode)
        out += data
        out += _pad(data)
    out += b"\0" * (_BLOCK * 2)               # 结束标记
    return bytes(out)


# ------------------------------------------------------------------ 载荷收集

def _tool_entries(embeds: list[str]) -> list[Entry]:
    """把 `--embed <src>[=<dst>]` 解析成载荷条目(原生工具, 0755)。"""
    entries: list[Entry] = []
    for spec in embeds:
        src_text, _, dst = spec.partition("=")
        src = Path(src_text)
        if not src.is_absolute():
            src = (Path.cwd() / src).resolve()
        if not src.is_file():
            raise SystemExit(f"FAIL: --embed 指向的文件不存在: {src}(先构建它)")
        if not dst:
            dst = f"bin/{src.name}"
        if dst.startswith("/") or ".." in Path(dst).parts:
            raise SystemExit(f"FAIL: --embed 落点非法: {dst}")
        entries.append((dst, src.read_bytes(), 0o755))
    return entries


def collect_entries(embeds: list[str]) -> list[Entry]:
    entries: list[Entry] = []
    for root in PAYLOAD_ROOTS:
        base = TOOL_ROOT / root
        if not base.is_dir():
            raise SystemExit(f"FAIL: 载荷根目录不存在: {base}")
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(TOOL_ROOT).as_posix()
            if "__pycache__" in rel or rel.endswith(".pyc"):
                continue
            entries.append((rel, path.read_bytes(), 0o644))

    entries.extend(_tool_entries(embeds))

    entries.sort(key=lambda item: item[0])    # 路径排序 ⇒ 载荷逐字节可复现
    if not entries:
        raise SystemExit("FAIL: 载荷为空")

    seen: set[str] = set()
    for path, _, _ in entries:
        if path in seen:
            raise SystemExit(f"FAIL: 载荷路径重复: {path}")
        seen.add(path)
    return entries


# ------------------------------------------------------------------ 生成 C++

def write_c(path: Path, payload: bytes) -> None:
    lines = [
        "// 由 tools/brickie/freeze.py 生成 —— **勿手改**(改它请改 freeze.py)",
        "//",
        "// 内容: brickie 的 Python 前端(python/brickie/**)、骨架模板(templates/**)与",
        "// 原生工具(bin/**, 如 brickie-gen), 打包成未压缩 ustar tar 后逐字节嵌入。",
        "// 运行时由 launcher.cpp 解包到临时目录, 再用系统 python3 解释并执行嵌入工具",
        "// —— 详见 ADR-0004 §7 与 tools/brickie/README.md。",
        "",
        "extern \"C\" {",
        # 定义必须带 extern: C++ 里命名空间作用域的 const 默认是**内部链接**,
        # 不带 extern 的话启动器链接时会报 undefined reference。
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


# --------------------------------------------------------------------- 入口

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="freeze", description="打包 brickie 的自包含载荷")
    parser.add_argument("--c", required=True, metavar="FILE", help="生成的 C++ 源")
    parser.add_argument("--tar", metavar="FILE", help="载荷 tar(可选, 供人工检查)")
    parser.add_argument(
        "--embed",
        action="append",
        default=[],
        metavar="SRC[=DST]",
        help="嵌入一个已构建的原生工具(可重复); 缺省落点 bin/<basename>, 模式 0755",
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
    detail = ("(" + ", ".join(tools) + ")") if tools else "(无)"
    print(f"freeze: {len(entries)} 个文件, 原生工具 {len(tools)} 个{detail} → "
          f"{len(payload)} 字节载荷 → {c_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

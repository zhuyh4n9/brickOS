"""文件编排(§9.1: IO 归 L5 前端)。

两条纪律:
1. **只写原生侧返回的 artifacts** —— "该生成哪些路径""能否覆盖"是业务规则,
   留在 C++ 侧;这里不重新判断。
2. **逐字节写入, 不加时间戳/不换行归一** —— 否则 V-2(生成物幂等, 逐字节一致)
   会被写盘层自己破坏。
"""

from __future__ import annotations

from pathlib import Path

# 判"这个路径上是不是生成物"只需首行; 生成的标记一定在首行
_HEAD_BYTES = 512


def _decode_first_line(path: Path) -> str:
    try:
        with path.open("rb") as handle:
            raw = handle.readline(_HEAD_BYTES)
    except OSError:
        return ""
    return raw.decode("utf-8", errors="replace").rstrip("\r\n")


def scan_existing(root: Path, paths: list[str]) -> list[dict]:
    """把"计划里已存在的路径"整理成原生侧要的 ``existing`` 上下文。

    只读首行: C++ 侧用首行里的生成物标记判"这是不是我们的生成物"(§8.4 的
    ``--force`` 语义依赖这个判定), 判定本身在原生侧做。
    """
    existing: list[dict] = []
    for rel in paths:
        absolute = root / rel
        if absolute.is_file():
            existing.append({"path": rel, "first_line": _decode_first_line(absolute)})
    return existing


def write_artifacts(root: Path, artifacts: list[dict]) -> list[str]:
    """落盘, 返回实际写出的仓库相对路径(保持原生侧给出的顺序)。"""
    written: list[str] = []
    for artifact in artifacts:
        target = root / artifact["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(artifact["content"], encoding="utf-8")
        written.append(artifact["path"])
    return written

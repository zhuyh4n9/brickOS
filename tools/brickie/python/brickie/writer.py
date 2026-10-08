"""文件编排(§9.1: IO 归 L5 前端)。

四条纪律:

1. **只写原生侧返回的 ``files``** —— "该生成哪些路径""能否覆盖"是业务规则, 留在
   原生侧; 这里不重新判断(唯一例外是 ``human`` 文件的**永不覆盖**, 见 contract §4)。
2. **幂等**: 只在内容与现有文件**逐字节不同**时写; 相同 ⇒ 既不写盘也不碰 mtime
   (V-2: 重复 ``gen`` 零 diff)。
3. **``--check`` 不写盘**: 逐字节比对, 把差异交给编排层做成退出码 1。
4. **逐字节写入, 不做编码/换行归一** —— 否则写盘层自己就会破坏"可复现"。

``kind`` 的语义(contract §4):

* ``human``: 人写文件(``plugin.toml`` / ``product.toml`` / 骨架的其余部分)——
  **永不被覆盖**; 只有 ``new``/``init`` 在文件**不存在**时创建。
* ``machine`` / ``rendered`` / ``generated``: 机器拥有的文件, 按内容幂等落盘。
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

# 判"这个路径上是不是生成物"只需首行; 生成的标记一定在首行
_HEAD_BYTES = 512

# 扫声明面时要跳过的目录: 状态目录 / VCS / 依赖缓存(不是插件树的一部分)
_EXCLUDED_DIRS = frozenset(
    {"build", ".git", ".hg", ".svn", ".scratch", "node_modules", "__pycache__", "prebuilts", "target"}
)


class WriteError(RuntimeError):
    """落盘失败(权限 / 目录占位 / 磁盘满) —— 对应 BRV-D9 的退出码 2。"""

    def __init__(self, path: str, cause: OSError) -> None:
        super().__init__(f"写入 {path} 失败: {cause}")
        self.path = path


@dataclass
class WriteOutcome:
    """一次落盘/比对的结果(呈现层与退出码都从这里取)。"""

    written: list[str] = field(default_factory=list)
    unchanged: list[str] = field(default_factory=list)
    skipped_human: list[str] = field(default_factory=list)
    # ``--check`` 专用
    stale: list[str] = field(default_factory=list)   # 文件在, 但内容与重算不同
    missing: list[str] = field(default_factory=list) # 文件不在

    def conflicting(self) -> list[str]:
        return [*self.stale, *self.missing]


# ------------------------------------------------------------------ 读侧


def _decode_first_line(path: Path) -> str:
    try:
        with path.open("rb") as handle:
            raw = handle.readline(_HEAD_BYTES)
    except OSError:
        return ""
    return raw.decode("utf-8", errors="replace").rstrip("\r\n")


def scan_existing(root: Path, paths: list[str]) -> list[dict]:
    """把"计划里已存在的路径"整理成原生侧要的 ``context.existing``。

    只读首行: 原生侧用首行里的生成物标记判"这是不是我们的生成物"(§8.4 的 ``--force``
    语义依赖这个判定), 判定本身在原生侧做。
    """
    existing: list[dict] = []
    for rel in paths:
        if not isinstance(rel, str):
            continue
        absolute = root / rel
        if absolute.is_file():
            existing.append({"path": rel, "first_line": _decode_first_line(absolute)})
    return existing


def read_text(path: Path) -> str:
    """读一个文本文件(声明面扫描用); 读不了 ⇒ 空串(由校验器报形状错)。"""
    try:
        return path.read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return ""


def _iter_named(root: Path, name: str) -> list[Path]:
    """在插件树里找同名文件, 跳过状态目录/VCS/依赖缓存; 结果排序(可复现)。"""
    found: list[Path] = []
    try:
        candidates = root.rglob(name)
        for path in candidates:
            if not path.is_file():
                continue
            parts = path.relative_to(root).parts
            if any(part in _EXCLUDED_DIRS for part in parts[:-1]):
                continue
            found.append(path)
    except OSError:
        return []
    return sorted(found)


def iter_declaration_files(root: Path, kinds: tuple[str, ...]) -> list[tuple[str, str]]:
    """列出要交给形状校验的声明文件: ``[(相对 root 的路径, 记录类型)]``。

    记录类型与 ``schema.SCHEMA_FILES`` 对齐: ``plugin`` / ``product`` / ``lock`` / ``index``。
    """
    pairs: list[tuple[str, str]] = []
    if "product" in kinds:
        pairs += [(p.relative_to(root).as_posix(), "product") for p in _iter_named(root, "product.toml")]
    if "plugin" in kinds:
        pairs += [(p.relative_to(root).as_posix(), "plugin") for p in _iter_named(root, "plugin.toml")]
    if "lock" in kinds:
        lock = root / "brickie.lock"
        if lock.is_file():
            pairs.append(("brickie.lock", "lock"))
    if "index" in kinds:
        index = root / "build" / "index" / "dependents.json"
        if index.is_file():
            pairs.append(("build/index/dependents.json", "index"))
    return pairs


# ------------------------------------------------------------------ 写侧


def _chmod(path: Path, mode: object) -> None:
    if not isinstance(mode, str) or not mode:
        return
    try:
        bits = int(mode, 8)
    except ValueError:
        return
    try:
        os.chmod(path, bits)
    except OSError:
        # Windows / 只读挂载: 权限位不是关键路径, 不因此判环境错
        pass


def write_files(
    root: Path,
    files: list[dict],
    *,
    kinds: dict[str, str] | None = None,
    check: bool = False,
) -> WriteOutcome:
    """落盘(或在 ``check`` 时只比对)原生侧返回的 ``files``。

    ``kinds`` 把路径映射到计划里的 ``kind``(``new``/``init``/``gen`` 的计划用
    ``human``/``generated``; ``brickie-gen`` 自己回的是 ``rendered``)。缺省按
    ``files[i].kind`` 取, 再缺省当 ``rendered``。

    ``--check``(``check=True``)时**绝不写盘**, 差异进 ``stale``/``missing``。
    """
    kinds = kinds or {}
    outcome = WriteOutcome()

    for entry in files:
        rel = entry.get("path")
        if not isinstance(rel, str) or not rel:
            continue
        content = entry.get("content", "")
        if not isinstance(content, str):
            content = str(content)
        kind = kinds.get(rel) or entry.get("kind") or "rendered"
        target = root / rel
        payload = content.encode("utf-8")

        # 人写文件永不覆盖(contract §4; --force 也不例外)
        if kind == "human" and target.exists():
            outcome.skipped_human.append(rel)
            continue

        if target.is_file():
            try:
                current = target.read_bytes()
            except OSError as exc:
                raise WriteError(rel, exc) from exc
            if current == payload:
                outcome.unchanged.append(rel)
                if not check:
                    _chmod(target, entry.get("mode"))
                continue
            if check:
                outcome.stale.append(rel)
                continue
        elif check:
            outcome.missing.append(rel)
            continue

        try:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
        except OSError as exc:
            raise WriteError(rel, exc) from exc
        _chmod(target, entry.get("mode"))
        outcome.written.append(rel)

    return outcome

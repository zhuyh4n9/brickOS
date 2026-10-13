"""File orchestration (IO is the L5 front end's duty).
Write only the ``files`` the native side returns; business rules, including never overwriting
``human`` files, stay native. Writes are byte-for-byte with no encoding/newline normalization and
idempotent: identical content means no write and no mtime touch. ``--check`` writes nothing and
reports byte differences as exit code 1. Stale generated files -- only tool-owned names under
``build/gen/`` and absent from this run's plan -- are deleted here as an IO duty. ``kind``:
``human`` files are created only by ``new``/``init`` and only when absent;
``machine``/``rendered``/``generated`` are machine-owned and written idempotently by content.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

# Generated-marker check reads line 1 only (the marker is always there).
_HEAD_BYTES = 512

_EXCLUDED_DIRS = frozenset(
    {"build", ".git", ".hg", ".svn", ".scratch", "node_modules", "__pycache__", "prebuilts", "target"}
)


class WriteError(RuntimeError):
    """Write failure (permissions / directory in the way / disk full); maps to BRV-D9 code 2."""

    def __init__(self, path: str, cause: OSError) -> None:
        super().__init__(f"failed to write {path}: {cause}")
        self.path = path


@dataclass
class WriteOutcome:
    """Result of one write/compare pass (both presentation and exit codes read from here)."""

    written: list[str] = field(default_factory=list)
    unchanged: list[str] = field(default_factory=list)
    skipped_human: list[str] = field(default_factory=list)
    stale: list[str] = field(default_factory=list)   # file exists but content differs from recomputation
    missing: list[str] = field(default_factory=list) # file is absent

    def conflicting(self) -> list[str]:
        return [*self.stale, *self.missing]


def _decode_first_line(path: Path) -> str:
    try:
        with path.open("rb") as handle:
            raw = handle.readline(_HEAD_BYTES)
    except OSError:
        return ""
    return raw.decode("utf-8", errors="replace").rstrip("\r\n")


def scan_existing(root: Path, paths: list[str]) -> list[dict]:
    """Build the native side's ``context.existing``: only the first line is read, from which the
    native side recognizes its own generated files (``--force`` semantics depend on that decision).
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
    """Read one text file for declaration-surface scanning; unreadable -> empty string."""
    try:
        return path.read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return ""


def _iter_named(root: Path, name: str) -> list[Path]:
    """Same-named files in the plugin tree, skipping state/VCS/caches; sorted (reproducible)."""
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
    """Declaration files to shape-validate: ``[(rel path, kind)]``, kinds from ``schema.py``."""
    pairs: list[tuple[str, str]] = []
    if "product" in kinds:
        pairs += [(p.relative_to(root).as_posix(), "product") for p in _iter_named(root, "product.toml")]
    if "plugin" in kinds:
        pairs += [(p.relative_to(root).as_posix(), "plugin") for p in _iter_named(root, "brick.toml")]
    if "lock" in kinds:
        lock = root / "brickie.lock"
        if lock.is_file():
            pairs.append(("brickie.lock", "lock"))
    if "index" in kinds:
        index = root / "build" / "index" / "dependents.json"
        if index.is_file():
            pairs.append(("build/index/dependents.json", "index"))
    return pairs


#: Generated file name (tool-owned): `build/gen/<plugin name>/plugin_desc.c`
_DESCRIPTOR_NAME = "plugin_desc.c"


def prune_orphan_descriptors(
    root: Path, planned: set[str], *, check: bool = False
) -> list[str]:
    """Find (when ``check``) or delete the ``build/gen/`` descriptors absent from this run's plan.

    ``brickie gen`` never deletes, so a renamed/dropped plugin leaves its old descriptor on disk and
    the ``gen_sources`` glob sweeps it in as a ghost plugin (measured: two descriptors on one
    ``uboot_mmc_start`` after the ``uboot-mmc`` rename, registered twice, second ``start`` hit
    ``-ENOSPC`` -> boot panic). Only the tool's own ``plugin_desc.c`` under ``build/gen/`` is
    touched; human and backend files (``build.mk``/``build.ninja``/``build-state.json``) are out of
    scope. ``check=True`` deletes nothing; the stale list goes to the orchestration layer (code 1).
    """
    base = root / "build" / "gen"
    if not base.is_dir():
        return []
    orphans: list[str] = []
    try:
        candidates = sorted(base.rglob(_DESCRIPTOR_NAME))
    except OSError:
        return []
    for path in candidates:
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if rel in planned:
            continue
        orphans.append(rel)
        if check:
            continue
        try:
            path.unlink()
        except OSError:
            # Failed delete is not a build error; next gen retries and filter_gen_sources drops it.
            continue
    if not check:
        _prune_empty_dirs(base)
    return orphans


def _prune_empty_dirs(base: Path) -> None:
    """After deleting descriptors, collapse empty directories (only empty dirs inside ``build/gen/``)."""
    try:
        dirs = sorted((p for p in base.rglob("*") if p.is_dir()), reverse=True)
    except OSError:
        return
    for path in dirs:
        try:
            path.rmdir()  # non-empty ⇒ OSError, which is exactly "stop here"
        except OSError:
            continue


def _chmod(path: Path, mode: object) -> None:
    """Apply an octal ``mode`` string; unsupported platforms are ignored."""
    if not isinstance(mode, str) or not mode:
        return
    try:
        bits = int(mode, 8)
    except ValueError:
        return
    try:
        os.chmod(path, bits)
    except OSError:
        # Windows / read-only mounts: permission bits are not on the critical path.
        pass


def write_files(
    root: Path,
    files: list[dict],
    *,
    kinds: dict[str, str] | None = None,
    check: bool = False,
) -> WriteOutcome:
    """Write (or, under ``check``, only compare) the ``files`` returned by the native side.

    ``kinds`` maps a path to its plan ``kind`` (``new``/``init``/``gen`` plans use ``human``/``generated``;
    ``brickie-gen`` itself returns ``rendered``). Defaults to ``files[i].kind``, then to ``rendered``.
    Under ``check=True`` nothing is written; differences go to ``stale``/``missing``.
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

        # Human-written files are never overwritten (--force is no exception).
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

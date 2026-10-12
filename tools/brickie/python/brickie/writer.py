"""File orchestration (§9.1: IO belongs to the L5 front end).

Five disciplines:

1. **Write only the ``files`` returned by the native side** -- "which paths to generate" and "may this be
   overwritten" are business rules that stay native; nothing is re-judged here (the one exception is the
   **never-overwrite** rule for ``human`` files, see contract §4).
2. **Idempotent**: write only when the content differs **byte for byte** from the existing file; identical ⇒
   neither write nor touch mtime (V-2: a repeated ``gen`` produces zero diff).
3. **``--check`` writes nothing**: compare byte for byte and hand the differences to the orchestration layer as exit code 1.
4. **Byte-for-byte writes, no encoding/newline normalization** -- otherwise the write layer itself breaks reproducibility.
5. **Cleaning up stale generated files is an IO duty, not a second set of business rules**: ``build/gen/`` is a
   machine-owned directory, and ``prune_orphan_descriptors`` deletes only entries whose file name is the tool's own
   (``plugin_desc.c``) and which are absent from this run's **native-side plan** -- "which paths to generate" is still
   decided entirely by the plan (same shape as ``clean``: core gives paths / L5 deletes).

``kind`` semantics (contract §4):

* ``human``: human-written files (``brick.toml`` / ``product.toml`` / the rest of the skeleton) --
  **never overwritten**; only ``new``/``init`` create them when the file does **not exist**.
* ``machine`` / ``rendered`` / ``generated``: machine-owned files, written idempotently by content.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

# Checking "is this path a generated file" needs only the first line; the generated marker is always on line 1
_HEAD_BYTES = 512

# Directories to skip when scanning the declaration surface: state dirs / VCS / dependency caches (not part of the plugin tree)
_EXCLUDED_DIRS = frozenset(
    {"build", ".git", ".hg", ".svn", ".scratch", "node_modules", "__pycache__", "prebuilts", "target"}
)


class WriteError(RuntimeError):
    """Write failure (permissions / directory in the way / disk full) -- maps to BRV-D9 exit code 2."""

    def __init__(self, path: str, cause: OSError) -> None:
        super().__init__(f"failed to write {path}: {cause}")
        self.path = path


@dataclass
class WriteOutcome:
    """Result of one write/compare pass (both presentation and exit codes read from here)."""

    written: list[str] = field(default_factory=list)
    unchanged: list[str] = field(default_factory=list)
    skipped_human: list[str] = field(default_factory=list)
    # ``--check`` only
    stale: list[str] = field(default_factory=list)   # file exists but content differs from recomputation
    missing: list[str] = field(default_factory=list) # file is absent

    def conflicting(self) -> list[str]:
        return [*self.stale, *self.missing]


# ------------------------------------------------------------------ read side


def _decode_first_line(path: Path) -> str:
    try:
        with path.open("rb") as handle:
            raw = handle.readline(_HEAD_BYTES)
    except OSError:
        return ""
    return raw.decode("utf-8", errors="replace").rstrip("\r\n")


def scan_existing(root: Path, paths: list[str]) -> list[dict]:
    """Turn "paths that already exist per the plan" into the native side's ``context.existing``.

    Only the first line is read: the native side uses the generated marker on it to decide whether a file is
    one of ours (§8.4's ``--force`` semantics depend on that decision); the decision itself is made natively.
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
    """Read one text file (for declaration-surface scanning); unreadable ⇒ empty string (the validator reports the shape error)."""
    try:
        return path.read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return ""


def _iter_named(root: Path, name: str) -> list[Path]:
    """Find same-named files in the plugin tree, skipping state dirs/VCS/dependency caches; sorted result (reproducible)."""
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
    """List the declaration files to hand to shape validation: ``[(path relative to root, record kind)]``.

    Record kinds align with ``schema.SCHEMA_FILES``: ``plugin`` / ``product`` / ``lock`` / ``index``.
    """
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


# ------------------------------------------------------------------ stale generated files


#: Generated file name (tool-owned): `build/gen/<plugin name>/plugin_desc.c`
_DESCRIPTOR_NAME = "plugin_desc.c"


def prune_orphan_descriptors(
    root: Path, planned: set[str], *, check: bool = False
) -> list[str]:
    """Find (when ``check``) or delete the descriptors under ``build/gen/`` that are **not in this run's plan**.

    Why this half is needed: ``brickie gen`` only writes descriptors for plugins currently in the tree and
    **never deletes anything**. Rename or drop a plugin and the old directory stays on disk forever; the
    ``[build].gen_sources`` glob ``build/gen/**/plugin_desc.c`` then sweeps it into the image ⇒ **ghost plugins**.

    Measured (2026-10, after ADR-0029 renamed ``third_party/uboot-mmc`` to ``adapter/uboot-mmc``): two
    descriptors had their ``start`` pointing at the **same** ``uboot_mmc_start`` (same symbol name), and
    plugin_manager registered the same thing twice -- on the second start the 4 cards were already taken, so it
    returned ``-ENOSPC`` (``-28``) and, under "stop on first failure" (ruling G6), **panicked at boot**:
    ``PLUGIN FAIL third_party/uboot-mmc phase=start rc=-28``.

    The scope is deliberately narrow: **only the tool's own file name** ``plugin_desc.c``, and only under
    ``build/gen/`` -- that directory is machine-owned (contract §4), so human-written files and brickie's
    backend files (``build.mk``/``build.ninja``/``build-state.json``) fall outside this set.
    With ``check=True`` nothing is deleted; the stale list goes to the orchestration layer (as exit code 1).
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
            # A failed delete is not a build error (the next gen retries); the file is still kept out of the
            # image by the build side's `filter_gen_sources` -- the last gate against ghost plugins.
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


# ------------------------------------------------------------------ write side


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
        # Windows / read-only mounts: permission bits are not on the critical path, so this is not an environment error
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

    Under ``--check`` (``check=True``) nothing is **ever** written; differences go to ``stale``/``missing``.
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

        # Human-written files are never overwritten (contract §4; --force is no exception)
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

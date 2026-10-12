"""host platform triple — coordinates for tool output under ``build/host/<host-arch>/<host-os>/``.

Layout (inspired by Android's ``out/host/...``)::

    build/host/<host-arch>/<host-os>/bin/brickie-gen
    build/host/<host-arch>/<host-os>/lib/libbrickie-gen.a

    host-arch: x86-64 / aarch64 / ...
    host-os:   linux / darwin / win

**The single source of truth is** ``tools/host-detect.sh`` (consumed by make via ``mk/host.mk``).
This module is the **Python mirror** of that mapping — the frontend must compute the
same paths **without starting a shell** (installed layout / CI jobs with an empty ``PATH``). The two mappings are
asserted to match by the "same rules" case in ``tests/run.sh`` and must not drift.

Overrides (same names as the script): ``BRICKIE_HOST_ARCH`` / ``BRICKIE_HOST_OS``.
"""

from __future__ import annotations

import os
import platform
import sys
from functools import lru_cache
from pathlib import Path

# One-to-one with the case branches in tools/host-detect.sh (changing one requires changing the other; tests guard this).
_ARCH_CANONICAL = frozenset({"x86-64", "aarch64", "x86", "arm", "riscv64", "ppc64le", "s390x"})
_ARCH_ALIASES = {
    "x86_64": "x86-64",
    "amd64": "x86-64",
    "arm64": "aarch64",
    "i386": "x86",
    "i486": "x86",
    "i586": "x86",
    "i686": "x86",
    "armv6l": "arm",
    "armv7l": "arm",
    "armv7": "arm",
}

_OS_CANONICAL = frozenset({"linux", "darwin", "win", "freebsd"})


def normalize_arch(raw: str) -> str:
    """Normalize a ``uname -m`` / ``platform.machine()`` value to host-arch."""
    value = raw.strip()
    if value in _ARCH_CANONICAL:
        return value
    return _ARCH_ALIASES.get(value.lower(), value.lower())


def normalize_os(raw: str) -> str:
    """Normalize a ``uname -s`` / ``sys.platform`` value to host-os."""
    value = raw.strip()
    if value in _OS_CANONICAL:
        return value
    low = value.lower()
    if low.startswith("linux"):
        return "linux"
    if low.startswith("darwin"):
        return "darwin"
    if low.startswith(("mingw", "msys", "cygwin", "windows", "win")):
        return "win"
    if low.startswith("freebsd"):
        return "freebsd"
    return low


@lru_cache(maxsize=1)
def host_triple() -> tuple[str, str]:
    """Return ``(host_arch, host_os)``; cached (the host cannot change within a process)."""
    arch = os.environ.get("BRICKIE_HOST_ARCH") or platform.machine() or "unknown"
    osname = os.environ.get("BRICKIE_HOST_OS") or sys.platform
    return normalize_arch(arch), normalize_os(osname)


def host_triple_str() -> str:
    """``"<host-arch>/<host-os>"`` — the first-level path under ``build/host/``."""
    return "/".join(host_triple())


def host_out_dir(repo_root: Path) -> Path:
    """``<repo_root>/build/host/<host-arch>/<host-os>``."""
    arch, osname = host_triple()
    return Path(repo_root) / "build" / "host" / arch / osname


def host_bin_dir(repo_root: Path) -> Path:
    """``<repo_root>/build/host/<host-arch>/<host-os>/bin`` (this build's output, derived)."""
    return host_out_dir(repo_root) / "bin"


def prebuilts_bin_dir(repo_root: Path) -> Path:
    """``<repo_root>/prebuilts/seed/brickie/<host-arch>/<host-os>/bin`` (bootstrap seed, committed).

    **Two** sets of host artifacts exist alongside ``host_bin_dir()``: the former is "just built locally", the latter is
    "committed with the source so a new checkout without a compiler can use it directly". Lookup order is in
    ``native.generator_binary()``.
    """
    arch, osname = host_triple()
    return Path(repo_root) / "prebuilts" / "seed" / "brickie" / arch / osname / "bin"

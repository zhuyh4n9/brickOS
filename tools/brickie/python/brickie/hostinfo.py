"""host 平台三元组 —— 工具产物落点 ``build/host/<host-arch>/<host-os>/`` 的坐标。

布局(参考 Android 的 ``out/host/...``)::

    build/host/<host-arch>/<host-os>/bin/brickie-gen
    build/host/<host-arch>/<host-os>/lib/libbrickie-gen.a

    host-arch: x86-64 / aarch64 / ...
    host-os:   linux / darwin / win

**单一真值在** ``tools/host-detect.sh``(make 侧经 ``mk/host.mk`` 消费它)。
本模块是那份映射的 **Python 镜像** —— 前端要能在**不启动 shell** 的情况下算出
同一个路径(安装形态 / ``PATH`` 被清空的 CI 作业)。两份映射由
``tests/run.sh`` 的"同口径"用例断言, 不许漂移。

覆盖(与脚本同名): ``BRICKIE_HOST_ARCH`` / ``BRICKIE_HOST_OS``。
"""

from __future__ import annotations

import os
import platform
import sys
from functools import lru_cache
from pathlib import Path

# 与 tools/host-detect.sh 的 case 分支一一对应(改一处必须改另一处, 有用例兜底)。
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
    """把 ``uname -m`` / ``platform.machine()`` 的值归一成 host-arch。"""
    value = raw.strip()
    if value in _ARCH_CANONICAL:
        return value
    return _ARCH_ALIASES.get(value.lower(), value.lower())


def normalize_os(raw: str) -> str:
    """把 ``uname -s`` / ``sys.platform`` 的值归一成 host-os。"""
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
    """返回 ``(host_arch, host_os)``; 结果缓存(同进程内宿主不会变)。"""
    arch = os.environ.get("BRICKIE_HOST_ARCH") or platform.machine() or "unknown"
    osname = os.environ.get("BRICKIE_HOST_OS") or sys.platform
    return normalize_arch(arch), normalize_os(osname)


def host_triple_str() -> str:
    """``"<host-arch>/<host-os>"`` —— 即 ``build/host/`` 下的一级路径。"""
    return "/".join(host_triple())


def host_out_dir(repo_root: Path) -> Path:
    """``<repo_root>/build/host/<host-arch>/<host-os>``。"""
    arch, osname = host_triple()
    return Path(repo_root) / "build" / "host" / arch / osname


def host_bin_dir(repo_root: Path) -> Path:
    """``<repo_root>/build/host/<host-arch>/<host-os>/bin``(本次构建产物, 派生)。"""
    return host_out_dir(repo_root) / "bin"


def prebuilts_bin_dir(repo_root: Path) -> Path:
    """``<repo_root>/prebuilts/seed/brickie/<host-arch>/<host-os>/bin``(自举种子, 进库)。

    与 ``host_bin_dir()`` 是**两份**宿主产物: 前者是"本机刚编出来的", 后者是
    "随源码提交、供没有编译器的新 checkout 直接用的种子"。查找顺序见
    ``native.generator_binary()``。
    """
    arch, osname = host_triple()
    return Path(repo_root) / "prebuilts" / "seed" / "brickie" / arch / osname / "bin"

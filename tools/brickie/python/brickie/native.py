"""L5 → L2 的进程边界: 调用 ``brickie-gen``, JSON over stdio(§9.1 / C9)。

Python 侧只负责"把请求变成 JSON、把 JSON 变回对象"; 判定一律不在这里。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from .hostinfo import host_bin_dir, prebuilts_bin_dir


class NativeError(RuntimeError):
    """原生侧故障(环境错) —— 对应 BRV-D9 的退出码 2。"""


def tool_root() -> Path:
    """工具自身根目录(``tools/brickie/``)。

    源码树布局: ``tools/brickie/python/brickie/native.py`` → 上溯三层。
    可用 ``BRICKIE_TOOL_ROOT`` 覆盖(安装形态 / 测试用)。
    """
    override = os.environ.get("BRICKIE_TOOL_ROOT")
    if override:
        return Path(override).resolve()
    return Path(__file__).resolve().parents[2]


def repo_root() -> Path:
    """仓库根(``build/host/...`` 的锚点)。

    源码树布局: ``tools/brickie/`` 的上一级。可用 ``BRICKIE_REPO_ROOT`` 覆盖。
    """
    override = os.environ.get("BRICKIE_REPO_ROOT")
    if override:
        return Path(override).resolve()
    return tool_root().resolve().parent.parent


def templates_root() -> Path:
    return tool_root() / "templates"


def _is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def generator_binary() -> Path:
    """定位 L2 生成器 ``brickie-gen``。

    查找顺序(先"本树刚编的", 再"随源码提交的种子", 最后"环境里的"):
      1. ``$BRICKIE_GEN`` 显式覆盖(用例 / 安装形态);
      2. ``build/host/<host-arch>/<host-os>/bin/brickie-gen`` —— **本次构建落点**
         (参考 Android 的 ``out/host/...``; 见 ``mk/host.mk``);
      3. ``build/host/*/*/bin/brickie-gen`` —— 宿主映射口径万一不一致时的兜底;
      4. ``prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie-gen`` —— **自举种子**
         (进版本库; 没有 g++ / 全新 checkout 时直接可用, 见 ADR 0004);
      5. ``prebuilts/seed/brickie/*/*/bin/brickie-gen`` —— 同上兜底;
      6. ``tools/brickie/cxx/brickie-gen`` —— 首刀遗留的旧落点(迁移期);
      7. ``PATH`` 上的 ``brickie-gen``。

    本地构建优先于种子: 开发者刚改完源码跑 ``make tools``, 不该被旧种子盖过去。
    """
    exe = "brickie-gen.exe" if os.name == "nt" else "brickie-gen"

    override = os.environ.get("BRICKIE_GEN")
    if override:
        candidate = Path(override).expanduser()
        if _is_executable(candidate):
            return candidate
        raise NativeError(f"$BRICKIE_GEN 指向的不是可执行文件: {candidate}")

    expected = host_bin_dir(repo_root()) / exe
    if _is_executable(expected):
        return expected

    for candidate in sorted((repo_root() / "build" / "host").glob(f"*/*/bin/{exe}")):
        if _is_executable(candidate):
            return candidate

    seed = prebuilts_bin_dir(repo_root()) / exe
    if _is_executable(seed):
        return seed

    for candidate in sorted((repo_root() / "prebuilts" / "seed" / "brickie").glob(f"*/*/bin/{exe}")):
        if _is_executable(candidate):
            return candidate

    legacy = tool_root() / "cxx" / exe
    if _is_executable(legacy):
        return legacy

    found = shutil.which("brickie-gen")
    if found:
        return Path(found)

    raise NativeError(
        f"找不到生成器 brickie-gen(期望 {expected} 或种子 {seed});"
        f"先在仓库根跑 `make tools`(或 tools/brickie 下跑 `make`)"
    )


def call(request: dict) -> dict:
    """把请求交给 brickie-gen, 返回它的响应对象。

    brickie-gen 的退出码只有两种: 0 = 协议处理成功(诊断与业务退出码在响应里),
    非 0 = 它自己故障。因此这里把"非 0"一律当环境错抛 NativeError,
    业务结论只从响应的 ``exit_code`` 读 —— 两者不会被混为一谈。
    """
    binary = generator_binary()
    payload = json.dumps(request, ensure_ascii=False)
    try:
        proc = subprocess.run(
            [str(binary)],
            input=payload,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:  # 权限 / 不是可执行文件 / ...
        raise NativeError(f"无法执行 {binary}: {exc}") from exc

    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "(无输出)"
        raise NativeError(f"brickie-gen 故障(exit {proc.returncode}): {detail}")

    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise NativeError(f"brickie-gen 输出不是合法 JSON: {exc}") from exc

    if not isinstance(response, dict):
        raise NativeError("brickie-gen 响应不是 JSON 对象")
    if response.get("status") == "internal_error":
        msgs = "; ".join(d.get("message", "") for d in response.get("diagnostics", []))
        raise NativeError(msgs or "brickie-gen 内部错误")
    return response

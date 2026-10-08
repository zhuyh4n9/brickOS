"""L5 → L0/L1/L2 的进程边界: 调 ``brickie-core`` / ``brickie-gen``, JSON over stdio。

契约见 ``docs/contract.md`` §1/§2/§6: 两个原生工具共用**同一个信封** —— 一行 JSON
从 stdin 读入、一行 JSON 从 stdout 写出。Python 侧只负责"把请求变成 JSON、把 JSON
变回对象", 一切判定与错误码都在原生侧(§8 的允许/禁止边界)。

两条不变式:

* **进程退出码**: 原生工具的进程退出码恒为 0(协议处理成功), 业务退出码在响应的
  ``exit_code`` 里; 非 0 只表示"工具自己故障" ⇒ 一律抛 :class:`NativeError`(L5 退出码 2)。
* **协议版本**: 响应 ``protocol`` 必须等于本包的 ``PROTOCOL``; 不等即握手失败 ⇒
  ``NativeError``(带码 ``BRV-PROTO-0001``), **不自动迁移**(contract §2)。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from . import PROTOCOL
from .hostinfo import host_bin_dir, prebuilts_bin_dir


class NativeError(RuntimeError):
    """原生侧故障(协议失败 / 环境错 / 工具自身故障) —— 对应 BRV-D9 的退出码 2。

    ``code`` 为可选的 BRV 诊断码(如协议不匹配的 ``BRV-PROTO-0001``); 无法归类时
    为 ``None``(contract §2: 无码诊断用 ``code: null``)。
    """

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


# ------------------------------------------------------------------ 路径锚点


def tool_root() -> Path:
    """工具自身根目录(``tools/brickie/``)。

    源码树布局: ``tools/brickie/python/brickie/native.py`` → 上溯三层。
    入口 ELF 形态下是解包出来的载荷根(仍有 ``python/``、``templates/``)。
    可用 ``BRICKIE_TOOL_ROOT`` 覆盖(安装形态 / 用例)。
    """
    override = os.environ.get("BRICKIE_TOOL_ROOT")
    if override:
        return Path(override).expanduser().resolve()
    return Path(__file__).resolve().parents[2]


def repo_root() -> Path:
    """仓库根(``build/host/...`` 与 ``prebuilts/seed/...`` 的锚点)。

    源码树布局: ``tools/brickie/`` 的上一级。可用 ``BRICKIE_REPO_ROOT`` 覆盖。
    """
    override = os.environ.get("BRICKIE_REPO_ROOT")
    if override:
        return Path(override).expanduser().resolve()
    return tool_root().resolve().parent.parent


def templates_root() -> Path:
    """骨架模板根(``templates/``); 作为 ``brickie-gen render`` 的入参。"""
    return tool_root() / "templates"


# ------------------------------------------------------------------ 二进制定位

_CORE_ENV = "BRICKIE_CORE"
_GEN_ENV = "BRICKIE_GEN"


def _is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def _find_tool(name: str, env_var: str) -> Path:
    """按**固定顺序**定位一个原生工具, 先"刚编的"再"种子"最后"环境里的"。

      1. ``$BRICKIE_CORE`` / ``$BRICKIE_GEN`` 显式覆盖(用例 / 入口 ELF 解包件);
      2. ``build/host/<host-arch>/<host-os>/bin/<name>`` —— 本次构建落点;
      3. ``build/host/*/*/bin/<name>`` —— 宿主三元组口径万一不一致时的兜底;
      4. ``prebuilts/seed/brickie/<host-arch>/<host-os>/bin/<name>`` —— 自举种子;
      5. ``prebuilts/seed/brickie/*/*/bin/<name>`` —— 同上兜底;
      6. ``PATH`` 上的同名可执行。

    本机构建优先于种子: 刚改完源码跑 ``make tools`` 不该被旧种子盖过去。
    """
    exe = f"{name}.exe" if os.name == "nt" else name

    override = os.environ.get(env_var)
    if override:
        candidate = Path(override).expanduser()
        if _is_executable(candidate):
            return candidate
        raise NativeError(f"${env_var} 指向的不是可执行文件: {candidate}")

    root = repo_root()
    expected = host_bin_dir(root) / exe
    candidates: list[Path] = [expected]
    candidates += sorted((root / "build" / "host").glob(f"*/*/bin/{exe}"))
    seed_dir = prebuilts_bin_dir(root)
    candidates.append(seed_dir / exe)
    candidates += sorted((root / "prebuilts" / "seed" / "brickie").glob(f"*/*/bin/{exe}"))
    for candidate in candidates:
        if _is_executable(candidate):
            return candidate

    found = shutil.which(name)
    if found:
        return Path(found)

    raise NativeError(
        f"找不到原生工具 {name}(期望 {expected} 或种子 {seed_dir / exe});"
        f"先在仓库根跑 `make tools`(或设 ${env_var} 指向已有的可执行文件)"
    )


def core_binary() -> Path:
    """``brickie-core``(L0 模型 + L1 求解/版本/接口引擎)的位置。"""
    return _find_tool("brickie-core", _CORE_ENV)


def gen_binary() -> Path:
    """``brickie-gen``(L2 生成器)的位置。"""
    return _find_tool("brickie-gen", _GEN_ENV)


# ------------------------------------------------------------------ 请求/响应


def make_request(
    command: str,
    root: Path,
    args: dict | None = None,
    context: dict | None = None,
    *,
    templates: Path | None = None,
) -> dict:
    """组装标准的请求信封(contract §2)。

    ``templates_root`` 是首刀遗留的顶层字段: 现在按 §6 只属于 ``brickie-gen render``
    的 ``args``, 这里顺带保留顶层一份, 供尚未迁到新契约的原生实现读取(多余字段被忽略)。
    """
    request: dict = {
        "protocol": PROTOCOL,
        "command": command,
        "root": str(root),
        "args": dict(args or {}),
        "context": dict(context or {}),
    }
    if templates is not None:
        request["templates_root"] = str(templates)
    return request


def _call(binary: Path, request: dict, label: str) -> dict:
    """把一个请求交给原生工具, 校验并规范化它的响应。"""
    payload = json.dumps(request, ensure_ascii=False)
    try:
        proc = subprocess.run(
            [str(binary)],
            input=payload,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:  # 权限 / 不是可执行文件 / 被删掉
        raise NativeError(f"无法执行 {binary}: {exc}") from exc

    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "(无输出)"
        raise NativeError(f"{label} 故障(进程退出码 {proc.returncode}): {detail}")

    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise NativeError(f"{label} 输出不是合法 JSON: {exc}") from exc

    if not isinstance(response, dict):
        raise NativeError(f"{label} 响应不是 JSON 对象")

    protocol = response.get("protocol")
    if protocol != PROTOCOL:
        raise NativeError(
            f"{label} 协议版本不匹配: 期望 protocol={PROTOCOL}, 实际 {protocol!r}"
            f"(BRV-PROTO-0001; 不做自动迁移 —— 见 contract §2)",
            code="BRV-PROTO-0001",
        )

    status = response.get("status")
    if status == "internal_error":
        detail = "; ".join(
            d.get("message", "") for d in response.get("diagnostics", []) if isinstance(d, dict)
        )
        raise NativeError(f"{label} 内部错误: {detail or '(无消息)'}")
    if status != "ok":
        raise NativeError(f"{label} 响应 status 非法: {status!r}(应为 ok/internal_error)")

    exit_code = response.get("exit_code")
    if not isinstance(exit_code, int) or isinstance(exit_code, bool):
        raise NativeError(f"{label} 响应 exit_code 非法: {exit_code!r}")

    diagnostics = response.get("diagnostics", [])
    files = response.get("files", [])
    data = response.get("data", {})
    if not isinstance(diagnostics, list):
        raise NativeError(f"{label} 响应 diagnostics 不是数组")
    if not isinstance(files, list):
        raise NativeError(f"{label} 响应 files 不是数组")
    if not isinstance(data, dict):
        raise NativeError(f"{label} 响应 data 不是对象")

    return {
        "protocol": PROTOCOL,
        "status": "ok",
        "exit_code": exit_code,
        "diagnostics": [d for d in diagnostics if isinstance(d, dict)],
        "files": [f for f in files if isinstance(f, dict)],
        "data": data,
    }


def call_core(request: dict) -> dict:
    """把请求交给 ``brickie-core``(一切判定与码的来源)。"""
    return _call(core_binary(), request, "brickie-core")


def call_gen(request: dict) -> dict:
    """把请求交给 ``brickie-gen``(只做模板渲染, 不判定)。"""
    return _call(gen_binary(), request, "brickie-gen")


def render(artifacts: list[dict], *, root: Path) -> dict:
    """请 ``brickie-gen render`` 把计划里的 ``artifacts`` 渲染成文件内容(contract §6)。

    ``artifacts[i] = {path, template, vars}``(``kind`` 会被忽略); 响应 ``files``
    = ``[{path, kind:"rendered", content, mode}]``。
    """
    request = make_request(
        "render",
        root,
        {"templates_root": str(templates_root()), "artifacts": artifacts},
        templates=templates_root(),
    )
    return call_gen(request)

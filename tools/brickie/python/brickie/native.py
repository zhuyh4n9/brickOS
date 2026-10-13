"""L5 -> L0/L1/L2 process boundary: calls ``brickie-core`` / ``brickie-gen``, JSON over stdio.

Both native tools share one envelope: one JSON line from stdin, one JSON line to stdout. Python only
turns requests into JSON and JSON back into objects; all decisions and codes live natively.

* A native tool's process exit code is always 0 (protocol handled); the business exit code is in the
  response's ``exit_code``. Non-zero means the tool itself failed -> ``NativeError`` (L5 code 2).
* The response ``protocol`` must equal this package's ``PROTOCOL``; a mismatch is a failed handshake
  -> ``NativeError`` with code ``BRV-PROTO-0001``, no automatic migration.
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
    """Native-side failure (protocol / environment / tool fault); maps to BRV-D9 exit code 2.
    ``code`` is an optional BRV diagnostic code, ``None`` when unclassifiable (``code: null``).
    """

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


def tool_root() -> Path:
    """The tool's own root directory (``tools/brickie/``); in the entry ELF this is the unpacked
    payload root (still has ``python/``, ``templates/``). Overridable via ``BRICKIE_TOOL_ROOT``.
    """
    override = os.environ.get("BRICKIE_TOOL_ROOT")
    if override:
        return Path(override).expanduser().resolve()
    return Path(__file__).resolve().parents[2]


def repo_root() -> Path:
    """Repo root, anchor of ``build/host/...`` and ``prebuilts/seed/...``; ``BRICKIE_REPO_ROOT`` wins."""
    override = os.environ.get("BRICKIE_REPO_ROOT")
    if override:
        return Path(override).expanduser().resolve()
    return tool_root().resolve().parent.parent


def templates_root() -> Path:
    """Skeleton template root (``templates/``); passed to ``brickie-gen render``."""
    return tool_root() / "templates"


_CORE_ENV = "BRICKIE_CORE"
_GEN_ENV = "BRICKIE_GEN"


def _is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def _find_tool(name: str, env_var: str) -> Path:
    """Locate a native tool in a fixed order: the ``$BRICKIE_CORE``/``$BRICKIE_GEN`` override, this
    build's ``build/host/<arch>/<os>/bin/<name>``, a ``*/*`` host fallback, the
    ``prebuilts/seed/brickie`` seed (and its ``*/*`` fallback), then ``PATH``. Local builds win over
    seeds, so ``make tools`` output is never shadowed by an old seed.
    """
    exe = f"{name}.exe" if os.name == "nt" else name

    override = os.environ.get(env_var)
    if override:
        candidate = Path(override).expanduser()
        if _is_executable(candidate):
            return candidate
        raise NativeError(f"${env_var} does not point to an executable: {candidate}")

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
        f"native tool not found: {name} (expected {expected} or seed {seed_dir / exe}); "
        f"run `make tools` at the repo root first (or set ${env_var} to an existing executable)"
    )


def core_binary() -> Path:
    """Location of ``brickie-core`` (L0 model + L1 solve/version/interface engine)."""
    return _find_tool("brickie-core", _CORE_ENV)


def gen_binary() -> Path:
    """Location of ``brickie-gen`` (L2 generator)."""
    return _find_tool("brickie-gen", _GEN_ENV)


def make_request(
    command: str,
    root: Path,
    args: dict | None = None,
    context: dict | None = None,
    *,
    templates: Path | None = None,
) -> dict:
    """Assemble the standard request envelope; ``templates_root`` is a legacy top-level field kept
    for native implementations not yet migrated (extras are ignored).
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
    """Hand a request to a native tool, validate and normalize its response."""
    payload = json.dumps(request, ensure_ascii=False)
    try:
        proc = subprocess.run(
            [str(binary)],
            input=payload,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:  # permissions / not an executable / deleted
        raise NativeError(f"cannot execute {binary}: {exc}") from exc

    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "(no output)"
        raise NativeError(f"{label} failed (process exit code {proc.returncode}): {detail}")

    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise NativeError(f"{label} output is not valid JSON: {exc}") from exc

    if not isinstance(response, dict):
        raise NativeError(f"{label} response is not a JSON object")

    protocol = response.get("protocol")
    if protocol != PROTOCOL:
        raise NativeError(
            f"{label} protocol version mismatch: expected protocol={PROTOCOL}, got {protocol!r}"
            f" (BRV-PROTO-0001; no automatic migration — see contract §2)",
            code="BRV-PROTO-0001",
        )

    status = response.get("status")
    if status == "internal_error":
        detail = "; ".join(
            d.get("message", "") for d in response.get("diagnostics", []) if isinstance(d, dict)
        )
        raise NativeError(f"{label} internal error: {detail or '(no message)'}")
    if status != "ok":
        raise NativeError(f"{label} response status invalid: {status!r} (expected ok/internal_error)")

    exit_code = response.get("exit_code")
    if not isinstance(exit_code, int) or isinstance(exit_code, bool):
        raise NativeError(f"{label} response exit_code invalid: {exit_code!r}")

    diagnostics = response.get("diagnostics", [])
    files = response.get("files", [])
    data = response.get("data", {})
    if not isinstance(diagnostics, list):
        raise NativeError(f"{label} response diagnostics is not an array")
    if not isinstance(files, list):
        raise NativeError(f"{label} response files is not an array")
    if not isinstance(data, dict):
        raise NativeError(f"{label} response data is not an object")

    return {
        "protocol": PROTOCOL,
        "status": "ok",
        "exit_code": exit_code,
        "diagnostics": [d for d in diagnostics if isinstance(d, dict)],
        "files": [f for f in files if isinstance(f, dict)],
        "data": data,
    }


def call_core(request: dict) -> dict:
    """Hand the request to ``brickie-core`` (the source of all decisions and codes)."""
    return _call(core_binary(), request, "brickie-core")


def call_gen(request: dict) -> dict:
    """Hand the request to ``brickie-gen`` (template rendering only, no decisions)."""
    return _call(gen_binary(), request, "brickie-gen")


def render(artifacts: list[dict], *, root: Path) -> dict:
    """Ask ``brickie-gen render`` for file contents: ``artifacts[i] = {path, template, vars}``;
    the response ``files`` is ``[{path, kind:"rendered", content, mode}]`` (``kind`` is ignored).
    """
    request = make_request(
        "render",
        root,
        {"templates_root": str(templates_root()), "artifacts": artifacts},
        templates=templates_root(),
    )
    return call_gen(request)

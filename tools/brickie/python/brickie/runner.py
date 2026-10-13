"""brickie's executor (L5): run core's ``steps`` as processes; make no judgments.

argv, candidate order, incrementality and log pass/fail all come from core. Here we validate
executables (incl. the ``check_device_models`` capability precheck), schedule concurrency, time out
and collect logs, and show progress. ``steps`` is ordered: consecutive steps sharing a ``group`` may
run concurrently, groups run serially; ``--jobs N`` caps concurrency (N <= 0 = CPU count). QEMU gate
steps declare ``expect_timeout: true``, so their timeout is an expected result, not a failure.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

#: Per-step log cap (truncate beyond this, so an 8 MiB log never enters the JSON envelope).
MAX_LOG_BYTES = 4 * 1024 * 1024

#: Minimum QEMU with a hotpluggable `-device emmc`: the model landed in 9.1 but
#: `dc->user_creatable` was cleared only in 10.2.0 (older builds just reject the device name).
EMMC_MIN_QEMU = (10, 2)

_QEMU_VERSION_CACHE: dict[str, tuple[int, int] | None] = {}


class StepError(RuntimeError):
    """A step could not start (missing executable / wrong cwd); environment error, exit code 2."""

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


@dataclass
class StepResult:
    label: str
    kind: str
    argv: list[str]
    rc: int | None = None
    timed_out: bool = False
    ok: bool = False
    log: str | None = None
    text: str = ""
    stdout: str = ""
    expect_timeout: bool = False
    judged: bool = False
    judge_failed: int = 0
    diagnostics: list[dict] = field(default_factory=list)

    def line(self) -> str:
        state = "ok  " if self.ok else "FAIL"
        return f"  {state} {self.label}"


def _truncate(text: str) -> str:
    if len(text) <= MAX_LOG_BYTES:
        return text
    return text[:MAX_LOG_BYTES] + f"\n...(log exceeds {MAX_LOG_BYTES} bytes, truncated)\n"


def resolve_exe(root: Path, exe: str, what: str, code: str | None) -> str:
    """Last check that ``argv[0]`` is executable; core already resolved it in candidate order.

    Relative paths resolve against ``root``, bare names through PATH."""
    if os.sep in exe or (os.altsep and os.altsep in exe):
        candidate = root / exe
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
        raise StepError(f"{what}: `{exe}` is not an executable file (relative to {root})", code)
    found = shutil.which(exe)
    if found:
        return found
    raise StepError(f"{what}: no executable `{exe}` found on PATH", code)


def _device_drivers(argv: list[str]) -> list[str]:
    """Driver from each ``-device <driver>[,prop=...]`` pair (only the arg after ``-device``)."""
    drivers: list[str] = []
    for i, arg in enumerate(argv):
        if arg == "-device" and i + 1 < len(argv):
            drivers.append(argv[i + 1].split(",", 1)[0])
    return drivers


def _qemu_version(exe: str) -> tuple[int, int] | None:
    """Return ``(major, minor)``, or ``None`` when undeterminable (not QEMU / no ``--version``)."""
    if exe in _QEMU_VERSION_CACHE:
        return _QEMU_VERSION_CACHE[exe]
    version: tuple[int, int] | None = None
    try:
        proc = subprocess.run(
            [exe, "--version"],
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        pass
    else:
        match = re.search(r"version (\d+)\.(\d+)", proc.stdout or "")
        if match:
            version = (int(match.group(1)), int(match.group(2)))
    _QEMU_VERSION_CACHE[exe] = version
    return version


def check_device_models(exe: str, argv: list[str], label: str, code: str | None) -> None:
    """Precheck the device models ``argv`` names; raises StepError before the process starts.

    Only ``-device emmc`` is checked (needs QEMU >= EMMC_MIN_QEMU). An undeterminable version
    does not block, so the precheck never becomes a second judge.
    """
    if "emmc" not in _device_drivers(argv):
        return
    got = _qemu_version(exe)
    if got is None or got >= EMMC_MIN_QEMU:
        return
    want = f"{EMMC_MIN_QEMU[0]}.{EMMC_MIN_QEMU[1]}"
    raise StepError(
        f"{label}: this QEMU is too old ({got[0]}.{got[1]}); it has no hotpluggable `-device emmc`\n"
        f"  binary: {exe}\n"
        f"  need:   QEMU >= {want} -- the eMMC card model landed in QEMU 9.1, but back then\n"
        f"        `emmc_class_init()` set `dc->user_creatable = false`, removed only in 10.2;\n"
        f"        older builds only say \"'emmc' is not a valid device model name\".\n"
        f"  fix:   bash tools/fetch-qemu.sh"
        f" (unpacks 10.2.1 into prebuilts/toolchain/, no root needed),\n"
        f"        or point [build.target.qemu].binary at a qemu-system-aarch64 >= {want}",
        code,
    )


def _exec_one(
    root: Path,
    step: dict,
    *,
    dry_run: bool,
    job_count: int,
    judge=None,
    stream: bool = True,
) -> StepResult:
    argv = [str(a) for a in step.get("argv") or []]
    label = str(step.get("label") or (argv[0] if argv else "?"))
    kind = str(step.get("kind") or "")
    result = StepResult(
        label=label,
        kind=kind,
        argv=argv,
        expect_timeout=bool(step.get("expect_timeout")),
    )
    if not argv:
        result.ok = True
        return result
    if dry_run:
        result.ok = True
        result.text = " ".join(argv)
        return result

    exe = resolve_exe(root, argv[0], label, step.get("tool_code"))
    argv = [exe] + argv[1:]
    result.argv = argv
    # Device-model precheck (EMMC_MIN_QEMU); failure is an environment error, not a judgment.
    check_device_models(exe, argv, label, step.get("tool_code"))

    # Create output dirs ourselves (compilers will not); core declares which files a step writes.
    for out in step.get("outputs") or []:
        parent = (root / str(out)).parent
        if not parent.is_dir():
            parent.mkdir(parents=True, exist_ok=True)

    log_rel = step.get("log")
    workdir = root / str(step.get("cwd") or ".")
    timeout = step.get("timeout_s")
    timeout = float(timeout) if isinstance(timeout, (int, float)) and timeout else None
    stdout_lines = step.get("stdout_lines")
    # `run` is interactive (the user's QEMU console) -- inherit stdio directly.
    inherit = kind == "run"
    if kind == "print" or dry_run:
        inherit = False

    try:
        proc = subprocess.run(
            argv,
            cwd=str(workdir),
            timeout=timeout,
            check=False,
            text=True,
            capture_output=not inherit,
        )
    except subprocess.TimeoutExpired as exc:
        result.timed_out = True
        result.rc = None
        out = exc.stdout or ""
        err = exc.stderr or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        if isinstance(err, bytes):
            err = err.decode("utf-8", "replace")
        result.stdout = out
        result.text = _truncate(out + err)
        result.ok = result.expect_timeout
    except OSError as exc:
        raise StepError(f"{label}: cannot execute {argv[0]}: {exc}", step.get("tool_code")) from exc
    else:
        result.rc = proc.returncode
        result.stdout = proc.stdout or ""
        result.text = _truncate((proc.stdout or "") + (proc.stderr or ""))
        result.ok = proc.returncode == 0

    if log_rel:
        path = root / str(log_rel)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(result.text, encoding="utf-8")
        result.log = str(log_rel)

    # Criteria live in core: hand the log to the judge (its name comes from core).
    judge_name = step.get("judge")
    if judge_name and judge is not None and not dry_run:
        verdict = judge(str(judge_name), result.text)
        result.judged = True
        result.judge_failed = int(verdict.get("failed") or 0)
        result.diagnostics = list(verdict.get("diagnostics") or [])
        if result.judge_failed > 0:
            result.ok = False

    if kind == "print" and stdout_lines:
        lines = result.stdout.splitlines()
        result.stdout = "\n".join(lines[: int(stdout_lines)])
    return result


def run_steps(
    root: Path,
    steps: list[dict],
    *,
    jobs: int = 0,
    dry_run: bool = False,
    judge=None,
    stream: bool = True,
    out=sys.stdout,
    err=sys.stderr,
) -> tuple[list[StepResult], bool]:
    """Run steps concurrently within a ``group``, serially between groups; return (results, ok).

    A failure does not abort the batch: in-flight steps finish and later groups run; only steps
    consuming a failed step's outputs are skipped. The caller decides the exit code."""
    results: list[StepResult] = []
    workers = jobs if jobs > 0 else (os.cpu_count() or 4)
    index = 0
    ok_all = True
    total = len(steps)
    # Upstream failure ⇒ downstream must be skipped: a previous run's stale objects link fine,
    # so only the final exit code would reveal it. core's per-step `consumes` drives the check.
    failed_outputs: set[str] = set()
    while index < total:
        step = steps[index]
        group = step.get("group")
        batch: list[dict] = []
        while index < total and steps[index].get("group") == group:
            batch.append(steps[index])
            index += 1
        runnable: list[dict] = []
        skipped: list[StepResult] = []
        for st in batch:
            missing = [c for c in (st.get("consumes") or []) if c in failed_outputs]
            if not missing:
                runnable.append(st)
                continue
            res = StepResult(
                label=str(st.get("label") or "?"),
                kind=str(st.get("kind") or ""),
                argv=[str(a) for a in (st.get("argv") or [])],
                ok=False,
                text=f"upstream step failed, not run (avoiding stale artifacts): {', '.join(missing)}",
                expect_timeout=bool(st.get("expect_timeout")),
            )
            skipped.append(res)
            for art in st.get("outputs") or []:
                failed_outputs.add(str(art))
        batch = runnable

        parallel = len(batch) > 1 and all(b.get("kind") in ("compile", "hosttest") for b in batch)
        if parallel:
            with ThreadPoolExecutor(max_workers=min(workers, len(batch))) as pool:
                batch_results = list(
                    pool.map(
                        lambda s: _exec_one(root, s, dry_run=dry_run, job_count=workers, judge=judge),
                        batch,
                    )
                )
        else:
            batch_results = [
                _exec_one(root, s, dry_run=dry_run, job_count=workers, judge=judge)
                for s in batch
            ]
        for res in skipped:
            results.append(res)
            ok_all = False
            if dry_run:
                continue
            print(f"  skip {res.label}  (upstream step failed, not run)", file=err if stream else out, flush=True)
        for res in batch_results:
            results.append(res)
            if not res.ok:
                ok_all = False
                for step in batch:
                    if str(step.get("label")) == res.label:
                        for art in step.get("outputs") or []:
                            failed_outputs.add(str(art))
            if dry_run:
                print(f"  dry-run {res.label}\n           {' '.join(res.argv)}", file=out)
                continue
            print(res.line(), file=out, flush=True)
            if res.kind == "print":
                if res.stdout:
                    print(res.stdout.rstrip("\n"), file=out)
                continue
            if not res.ok:
                head = "\n".join(res.text.splitlines()[:40])
                print(f"        -- {res.label} output (first 40 lines) --", file=err)
                print("\n".join(f"        {line}" for line in head.splitlines()), file=err)
                if res.timed_out and not res.expect_timeout:
                    print(f"        timeout ({res.label}): the criterion is timeout_s in tests/gates.toml", file=err)
    return results, ok_all

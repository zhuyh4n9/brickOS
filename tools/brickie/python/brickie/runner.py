"""brickie 的**执行器**(L5): 把 core 给的 ``steps`` 变成进程。

## 纪律(contract §8 / V-18c)

本模块**没有判定**:

* ``argv`` 来自 core(``data.steps[i].argv``)—— 连"用哪个编译器"都是 core 解析好的
  (裁定 S-B1: 生成物 make/ninja 要把解析结果烧进去, 所以解析只能在 core);
* 工具候选序、增量口径、日志红绿判据同样在 core;
* 本模块只做四件事: **校验可执行**、**并发编排**、**超时/收日志**、**呈现进度**。

## 编排口径

``steps`` 是有序的; ``group`` 相同的**连续**步骤可并发(编译步骤同组), 组间串行
(链接必须等编译)。``--jobs N`` 限流; ``N <= 0`` ⇒ 用 CPU 数。

QEMU 门禁步骤声明 ``expect_timeout: true``: 镜像本来就跑到被掐 —— 于是"超时"是
**预期结果**而不是失败(等价于旧 Makefile 里的 ``rc == 124`` 判据)。日志判据由
``judge`` 字段指向 core 的 ``judge`` 命令(正则来自 ``tests/gates.toml``)。
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

#: 单个步骤的日志上限(超过就截断, 免得 8 MiB 的日志进 JSON 信封)。
MAX_LOG_BYTES = 4 * 1024 * 1024


class StepError(RuntimeError):
    """步骤**无法启动**(可执行不在场 / cwd 不对) —— 环境错(退出码 2)。"""

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
    return text[:MAX_LOG_BYTES] + f"\n…(日志超过 {MAX_LOG_BYTES} 字节, 已截断)\n"


def resolve_exe(root: Path, exe: str, what: str, code: str | None) -> str:
    """校验``argv[0]``真的可执行。

    ``core`` 已按候选序解析过一次(可能是 ``prebuilts/toolchain/bin/...`` 这样的相对
    路径, 也可能只是 PATH 上的裸名)。这里只做**最后一道**校验 —— 把"运行期它不见了"
    与"core 压根没解析出来"分开报。
    """
    if os.sep in exe or (os.altsep and os.altsep in exe):
        candidate = root / exe
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
        raise StepError(f"{what}: `{exe}` 不是可执行文件(相对 {root})", code)
    found = shutil.which(exe)
    if found:
        return found
    raise StepError(f"{what}: PATH 上找不到可执行文件 `{exe}`", code)


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

    # 产物目录的父目录要自己建: 编译器不会替我们建(第一次编一个新目录时踩到过)。
    # 这是**编排**而不是判定 —— core 只声明"这个步骤写出哪些文件"。
    for out in step.get("outputs") or []:
        parent = (root / str(out)).parent
        if not parent.is_dir():
            parent.mkdir(parents=True, exist_ok=True)

    log_rel = step.get("log")
    workdir = root / str(step.get("cwd") or ".")
    timeout = step.get("timeout_s")
    timeout = float(timeout) if isinstance(timeout, (int, float)) and timeout else None
    stdout_lines = step.get("stdout_lines")
    # `run` 是**交互式**的(用户的 QEMU 控制台) —— 直接继承 stdio。
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
        raise StepError(f"{label}: 无法执行 {argv[0]}: {exc}", step.get("tool_code")) from exc
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

    # 判据在 core: 把日志交回去判(judge 字段由 core 填)。
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
    """按 ``group`` 并发、组间串行地执行全部步骤; 返回 (结果, 是否全绿)。

    失败**不中断整批**: 已经起来的同组步骤跑完(它们的错误信息同样有价值),
    后续组照跑 —— 由调用方决定退出码。
    """
    results: list[StepResult] = []
    workers = jobs if jobs > 0 else (os.cpu_count() or 4)
    index = 0
    ok_all = True
    total = len(steps)
    # 上游失败 ⇒ 下游**必须跳过**, 不能用上一次的旧产物接着跑:
    # 旧对象照样能链接成功, 于是日志会打印 "ok ld", 而 `build/brick.elf` 其实是陈旧的
    # (报出问题的只有最后的退出码)。core 在步骤里给了 `consumes`(它消费哪些"本计划里
    # 别的步骤产出的文件"), 这里据此判。
    failed_outputs: set[str] = set()
    while index < total:
        step = steps[index]
        group = step.get("group")
        batch: list[dict] = []
        while index < total and steps[index].get("group") == group:
            batch.append(steps[index])
            index += 1
        # 上游失败的下游步骤: 先摘出来(不执行), 并把它们的产物也算失败, 让链条继续短路。
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
                text=f"上游步骤失败, 未执行(避免用旧产物): {', '.join(missing)}",
                expect_timeout=bool(st.get("expect_timeout")),
            )
            skipped.append(res)
            for art in st.get("outputs") or []:
                failed_outputs.add(str(art))
        batch = runnable

        # 只有"编译"这种天然独立的步骤值得并发; 其余按序单跑(group 相同但需顺序)。
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
            print(f"  skip {res.label}  (上游步骤失败, 未执行)", file=err if stream else out, flush=True)
        for res in batch_results:
            results.append(res)
            if not res.ok:
                ok_all = False
                # 记下"这一步没产出": 依赖它的下游会被短路(见上面的 consumes 判定)。
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
                # 失败现场: 尾巴对着看最有用(编译错误的头几行里有真话)
                head = "\n".join(res.text.splitlines()[:40])
                print(f"        ── {res.label} 输出(前 40 行) ──", file=err)
                print("\n".join(f"        {line}" for line in head.splitlines()), file=err)
                if res.timed_out and not res.expect_timeout:
                    print(f"        超时({res.label}): 判据是 tests/gates.toml 的 timeout_s", file=err)
    return results, ok_all

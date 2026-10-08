"""呈现层: 把 ``data`` 渲染成人读文本; ``--json`` 输出同一 ``data`` + 诊断(同源)。

**呈现纪律**(contract §8 / BRV-D2 纪律 2): 文本输出与 ``--json`` 是**同一 ``data``
的两个序列化**。因此本模块里的每个渲染器都只读 ``data`` 与"落盘结果"(``extra``),
**不重新判定任何东西** —— 判定与码都来自原生侧。

诊断的文本格式沿用首刀的 ``_emit_diagnostics_text``(severity 标签 + 码 + 位置);
``--json`` 下诊断数组按 ``(code, file, span, target)`` **稳定排序**(contract §3)。
"""

from __future__ import annotations

import json

_SEVERITY_LABEL = {"error": "error", "warning": "warning", "info": "note"}


# ------------------------------------------------------------------ 诊断


def sort_diagnostics(diagnostics: list[dict]) -> list[dict]:
    """按 ``(code, file, span, target)`` 稳定排序(contract §3; V-9 的快照前提)。"""

    def key(diag: dict) -> tuple[str, str, str, str]:
        return (
            str(diag.get("code") or ""),
            str(diag.get("file") or ""),
            str(diag.get("span") or ""),
            str(diag.get("target") or ""),
        )

    return sorted(diagnostics, key=key)


def emit_diagnostics_text(diagnostics: list[dict]) -> str:
    """人读诊断文本(打印到 stderr)。"""
    lines: list[str] = []
    for diag in diagnostics:
        label = _SEVERITY_LABEL.get(str(diag.get("severity", "")), str(diag.get("severity", "?")))
        code = diag.get("code") or "-"
        where = diag.get("file") or diag.get("target") or ""
        head = f"{label}: {code}"
        if where:
            head += f" [{where}]"
        span = diag.get("span")
        if span:
            head += f" ({span})"
        lines.append(f"{head} {diag.get('message', '')}")
        if diag.get("hint"):
            lines.append(f"       提示: {diag['hint']}")
    return "\n".join(lines)


# ------------------------------------------------------------------ JSON


def json_payload(
    *,
    command: str,
    exit_code: int,
    diagnostics: list[dict],
    data: dict,
    written: list[str] | None = None,
) -> dict:
    """``--json`` 的稳定信封: 同一 ``data`` + 诊断 + 实际落盘清单。"""
    return {
        "protocol": 1,
        "status": "ok",
        "command": command,
        "exit_code": exit_code,
        "diagnostics": sort_diagnostics(diagnostics),
        "data": data or {},
        "written": list(written or []),
    }


def emit_json(payload: dict) -> str:
    return json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=False)


# ------------------------------------------------------------------ 小工具


def _as_list(value: object) -> list:
    return value if isinstance(value, list) else []


def _as_dict(value: object) -> dict:
    return value if isinstance(value, dict) else {}


def _fmt(value: object) -> str:
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "是" if value else "否"
    if isinstance(value, (int, float, str)):
        return str(value)
    return json.dumps(value, ensure_ascii=False)


def _short_hash(value: object, width: int = 12) -> str:
    if isinstance(value, str) and value:
        return value[:width]
    return "-"


def _table(headers: list[str], rows: list[list[object]]) -> list[str]:
    """等宽对齐的极简表格(空表返回提示行)。"""
    if not rows:
        return ["  (空)"]
    cells = [[_fmt(c) for c in row] for row in rows]
    widths = [len(h) for h in headers]
    for row in cells:
        for index, cell in enumerate(row):
            if index < len(widths):
                widths[index] = max(widths[index], len(cell))
    out = ["  " + "  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)).rstrip()]
    for row in cells:
        out.append("  " + "  ".join(
            (row[i] if i < len(row) else "").ljust(widths[i]) for i in range(len(widths))
        ).rstrip())
    return out


def _footer(extra: dict, lines: list[str]) -> list[str]:
    """统一的"落盘结果"小节(同一份 written/unchanged/skipped 也进 ``--json``)。"""
    written = _as_list(extra.get("written"))
    unchanged = _as_list(extra.get("unchanged"))
    skipped = _as_list(extra.get("skipped"))
    if written:
        lines.append(f"落盘: {len(written)} 个文件")
        lines.extend(f"  写入 {path}" for path in written)
    if unchanged:
        lines.append(f"未变: {len(unchanged)} 个文件(逐字节相同, 不写盘)")
    if skipped:
        lines.append(f"跳过: {len(skipped)} 个人写文件(已存在, 永不覆盖)")
    return lines


def _section(title: str, lines: list[str]) -> list[str]:
    if lines:
        return [title, *lines]
    return []


# ------------------------------------------------------------------ 渲染器


def _render_plan(data: dict, extra: dict, title: str, title_failed: str) -> list[str]:
    artifacts = [a for a in _as_list(data.get("artifacts")) if isinstance(a, dict)]
    written = set(_as_list(extra.get("written")))
    unchanged = set(_as_list(extra.get("unchanged")))
    skipped = set(_as_list(extra.get("skipped")))

    # 退出码非 0 时不能说"已生成" —— 标题与 JSON 的 exit_code 同源(同一个 data)
    ok = bool(extra.get("ok", True))
    lines = [title if ok else title_failed]
    if not artifacts:
        lines.append("  (计划里没有产物)")
    for artifact in artifacts:
        path = artifact.get("path")
        if not isinstance(path, str):
            continue
        kind = "人写" if artifact.get("kind") == "human" else "生成"
        state = ""
        if path in written:
            state = "已写入"
        elif path in unchanged:
            state = "已是最新"
        elif path in skipped:
            state = "跳过(已存在, 不覆盖)"
        suffix = f"   [{state}]" if state else ""
        lines.append(f"  {kind:<4} {path}{suffix}")
    lines = _footer(extra, lines)
    if written:
        lines.append("")
        lines.append("下一步: brickie check")
    return lines


def _render_check(data: dict, extra: dict) -> list[str]:
    summary = _as_dict(data.get("summary"))
    errors = summary.get("errors", 0)
    warnings = summary.get("warnings", 0)
    infos = summary.get("infos", 0)
    profile = summary.get("profile") or extra.get("profile") or "dev"
    closure = _as_dict(summary.get("closure"))

    verdict = "通过" if not errors else "未通过"
    lines = [f"检查{verdict}: {errors} 错误 / {warnings} 警告 / {infos} 提示 (profile={profile})"]
    if closure:
        plugins = closure.get("plugins", "-")
        ram = closure.get("ram_kib", "-")
        stack = closure.get("stack_kib", "-")
        lines.append(f"  闭包: {plugins} 个插件, RAM 合计 {ram} KiB, 栈合计 {stack} KiB")

    scopes = data.get("scope_results")
    rows: list[list[object]] = []
    if isinstance(scopes, dict):
        for scope, result in scopes.items():
            info = _as_dict(result)
            rows.append([scope, info.get("errors", 0), info.get("warnings", 0), info.get("infos", 0)])
    elif isinstance(scopes, list):
        for result in scopes:
            info = _as_dict(result)
            if info:
                rows.append([
                    info.get("scope", "?"), info.get("errors", 0),
                    info.get("warnings", 0), info.get("infos", 0),
                ])
    if rows:
        lines.append("  分域:")
        lines.extend(_table(["域", "错误", "警告", "提示"], rows))
    return _footer(extra, lines)


def _render_closure(data: dict, extra: dict) -> list[str]:
    plugins = [p for p in _as_list(data.get("plugins")) if isinstance(p, dict)]
    rows = [
        [
            p.get("name", "?"), p.get("version", "-"), p.get("compat_gen", 0),
            p.get("plugin_type", "-"), p.get("phase", "-"),
            p.get("ram_kib", "-"), p.get("stack_kib", "-"),
            "选中" if p.get("selected", True) else "剔除",
        ]
        for p in plugins
    ]
    lines = [f"产品闭包(profile={data.get('profile', 'dev')}): {len(plugins)} 个插件"]
    lines.append("  cg = COMPAT_GEN(精确钉代, 不参与 range 比较)")
    lines.extend(_table(["插件", "版本", "cg", "类型", "相位", "RAM", "栈", "状态"], rows))

    order = _as_list(data.get("topo_order"))
    if order:
        lines.extend(_section("初始化拓扑序:", [f"  {' → '.join(str(x) for x in order)}"]))

    totals = _as_dict(data.get("totals"))
    if totals:
        budget = _as_dict(totals.get("budget"))
        capacity = _as_dict(totals.get("platform_capacity"))
        lines.append(
            f"合计: RAM {totals.get('ram_kib', '-')} KiB / 栈 {totals.get('stack_kib', '-')} KiB"
            f"(平台容量 RAM {capacity.get('ram_kib', '-')} / 栈 {capacity.get('stack_kib', '-')};"
            f" 产品预算 RAM {budget.get('ram_kib', '-')} / 栈 {budget.get('stack_kib', '-')})"
        )

    cycles = _as_list(data.get("cycles"))
    if cycles:
        lines.append(f"环: {len(cycles)}")
        for cycle in cycles:
            path = cycle if isinstance(cycle, list) else _as_dict(cycle).get("path", cycle)
            lines.append(f"  {' → '.join(str(x) for x in path) if isinstance(path, list) else _fmt(path)}")
    return _footer(extra, lines)


def _render_dep_tree(data: dict, extra: dict) -> list[str]:
    nodes = _as_dict(data.get("nodes"))
    roots = [str(r) for r in _as_list(data.get("roots"))]
    kind = data.get("kind", "all")
    lines = [f"依赖树(kind={kind}):"]
    if not roots:
        lines.append("  (空)")
        return _footer(extra, lines)

    def walk(name: str, prefix: str, seen: frozenset[str]) -> None:
        entry = _as_dict(nodes.get(name))
        deps = [d for d in _as_list(entry.get("deps")) if isinstance(d, dict)]
        for index, dep in enumerate(deps):
            dep_name = str(dep.get("name", "?"))
            last = index == len(deps) - 1
            branch = "└── " if last else "├── "
            detail = f" [{dep.get('kind', '?')}"
            if dep.get("range"):
                detail += f" {dep['range']}"
            if dep.get("phase"):
                detail += f" phase={dep['phase']}"
            detail += "]"
            if dep_name in seen:
                lines.append(f"  {prefix}{branch}{dep_name}{detail} (环)")
                continue
            lines.append(f"  {prefix}{branch}{dep_name}{detail}")
            walk(dep_name, prefix + ("    " if last else "│   "), seen | {dep_name})

    for root in roots:
        lines.append(f"  {root}")
        walk(root, "  ", frozenset({root}))
    return _footer(extra, lines)


def _render_dep_graph(data: dict, extra: dict) -> list[str]:
    text = data.get("text")
    lines = [f"依赖图(format={data.get('format', '?')}):"]
    if isinstance(text, str) and text:
        lines.extend(f"  {line}" for line in text.splitlines())
    else:
        nodes = _as_list(data.get("nodes"))
        edges = [e for e in _as_list(data.get("edges")) if isinstance(e, dict)]
        lines.append(f"  节点 {len(nodes)} 个, 边 {len(edges)} 条")
        lines.extend(
            f"  {e.get('from', '?')} -[{e.get('kind', '?')}]-> {e.get('to', '?')}" for e in edges
        )
    return _footer(extra, lines)


def _render_dep_why(data: dict, extra: dict) -> list[str]:
    if not data.get("found"):
        return _footer(extra, [f"未找到依赖路径: {extra.get('from', '?')} → {extra.get('to', '?')}"])
    path = _as_list(data.get("path"))
    lines = [f"依赖路径({' → '.join(str(p) for p in path)}):"]
    for edge in _as_list(data.get("edges")):
        info = _as_dict(edge)
        lines.append(f"  {info.get('from', '?')} -[{info.get('kind', '?')}]-> {info.get('to', '?')}")
    return _footer(extra, lines)


def _render_dep_index(data: dict, extra: dict) -> list[str]:
    dependents = _as_dict(data.get("dependents"))
    edges = sum(len(v) for v in dependents.values() if isinstance(v, list))
    lines = [f"反向依赖索引: {len(dependents)} 个被依赖方, {edges} 条边"]
    rows = []
    for target, sources in sorted(dependents.items()):
        for source in _as_list(sources):
            info = _as_dict(source)
            rows.append([target, info.get("from", "?"), info.get("kind", "?")])
    if rows:
        lines.extend(_table(["被依赖方", "依赖方", "类型"], rows))
    return _footer(extra, lines)


def _render_dep_snippet(data: dict, extra: dict, verb: str) -> list[str]:
    lines = [f"dep {verb} 只打印片段, **不改写 plugin.toml**(裁定 R-2: 人写文件永不被改写)"]
    snippet = data.get("snippet")
    if isinstance(snippet, str) and snippet:
        lines.append("")
        lines.extend(f"  {line}" for line in snippet.splitlines())
    else:
        lines.append("  (core 未返回 snippet)")
    if data.get("locator"):
        lines.extend(["", f"  定位: {_fmt(data['locator'])}"])
    return _footer(extra, lines)


def _render_ver_show(data: dict, extra: dict) -> list[str]:
    segments = _as_dict(data.get("segments"))
    lines = [f"接口单元 {data.get('id', extra.get('id', '?'))}"]
    lines.append(f"  版本: {data.get('version', '-')}"
                 f"(COMPAT_GEN.{'*'}= {segments.get('compat_gen', data.get('compat_gen', '-'))}"
                 f" / MAJOR {segments.get('major', '-')}"
                 f" / MINOR {segments.get('minor', '-')}"
                 f" / REVISE {segments.get('revise', '-')})")
    lines.append("  注: COMPAT_GEN 不参与 range 比较(contract §5; RV-11)")
    lines.append(f"  插件: {data.get('plugin', '-')}   来源: {data.get('source', '-')}")
    lines.append(f"  状态: {data.get('status', '-')} / 冻结态: {data.get('freeze_state', '-')}"
                 f" / 已发布: {_fmt(data.get('published'))}")
    source = _as_dict(data.get("compat_gen_source"))
    if source:
        note = f"({source.get('note')})" if source.get("note") else ""
        lines.append(f"  COMPAT_GEN 来源: {source.get('kind', '-')}{note}")
    return _footer(extra, lines)


def _render_ver_bump(data: dict, extra: dict) -> list[str]:
    lines = [f"版本推进: {data.get('id', extra.get('id', '?'))}"]
    if data.get("from") is not None or data.get("to") is not None:
        lines.append(f"  {data.get('from', '-')} → {data.get('to', '-')}(rule={data.get('rule', '-')})")
    else:
        lines.append(f"  rule={data.get('rule', '-')}   版本: {data.get('version', '-')}")
    segments = _as_dict(data.get("segments"))
    if segments:
        lines.append(
            f"  四段: COMPAT_GEN {segments.get('compat_gen', '-')}"
            f" / MAJOR {segments.get('major', '-')}"
            f" / MINOR {segments.get('minor', '-')}"
            f" / REVISE {segments.get('revise', '-')}"
        )
    lines.append("  注: plugin.toml 里的 version 由人维护 —— 工具只把它应改成什么作为提示报出(裁定 R-3)")
    return _footer(extra, lines)


def _render_iface_list(data: dict, extra: dict) -> list[str]:
    units = [u for u in _as_list(data.get("units")) if isinstance(u, dict)]
    lines = [f"接口单元: {len(units)} 个"]
    rows = [
        [
            u.get("id", "?"), u.get("api_iface", "-"), u.get("form", "-"),
            u.get("version", "-"), u.get("compat_gen", 0),
            u.get("freeze_state", "-"), u.get("status", "-"),
            _fmt(u.get("published")), _short_hash(u.get("hash")),
        ]
        for u in units
    ]
    lines.extend(_table(["单元", "分类", "形态", "版本", "cg", "冻结态", "状态", "已发布", "hash"], rows))
    return _footer(extra, lines)


def _render_iface_show(data: dict, extra: dict) -> list[str]:
    unit = _as_dict(data.get("unit"))
    lines = [f"接口单元 {unit.get('id', extra.get('id', '?'))}"]
    for key, label in (
        ("api_iface", "分类"), ("form", "形态"), ("version", "版本"),
        ("compat_gen", "COMPAT_GEN"), ("freeze_state", "冻结态"), ("status", "状态"),
        ("hash_scope", "hash_scope"), ("truth", "truth"),
        ("declaration_hash", "声明面 hash"), ("snapshot_hash", "快照 hash"),
        ("published", "已发布"),
    ):
        value = unit.get(key, data.get(key))
        lines.append(f"  {label}: {_fmt(value)}")
    lines.append("  NOT_ABI: 声明面 hash ≠ ABI 兼容证明(v0.1 真值 = 声明面; contract §6.6)")
    if data.get("not_abi") is False:
        lines.append("  ⚠ core 报告 not_abi=false: v0.1 不支持符号面真值")

    entries = [e for e in _as_list(data.get("entries")) if isinstance(e, dict)]
    lines.extend(_section(f"条目({len(entries)}):", [
        f"  {e.get('kind', '?'):<14} {e.get('name', '?'):<28} {e.get('status', '-')}"
        for e in entries
    ]))
    if data.get("source"):
        lines.append(f"  来源: {data['source']}")
    return _footer(extra, lines)


def _render_iface_diff(data: dict, extra: dict) -> list[str]:
    lines = [f"接口变更集 {data.get('id', extra.get('id', '?'))}: 判定 {data.get('verdict', '-')}"]
    version = _as_dict(data.get("version"))
    if version:
        lines.append(f"  版本: {version.get('from', '-')} → {version.get('to', '-')}")
        reasons = _as_list(version.get("reasons"))
        if reasons:
            lines.append(f"  理由: {', '.join(str(r) for r in reasons)}")
    lines.append(f"  COMPAT_GEN 变化: {_fmt(data.get('compat_gen_changed'))}")
    changes = [c for c in _as_list(data.get("changes")) if isinstance(c, dict)]
    if changes:
        lines.append(f"  变更({len(changes)}):")
        lines.extend(
            f"    {c.get('kind', '?'):<10} {c.get('name', c.get('entry', '?'))}"
            + (f"  {c.get('from')} → {c.get('to')}" if c.get("from") is not None else "")
            for c in changes
        )
    dependents = _as_dict(data.get("dependents"))
    if dependents:
        for key, label in (("direct", "直接依赖者"), ("transitive", "传递依赖者"), ("unsatisfied", "失配者")):
            names = _as_list(dependents.get(key))
            if names:
                lines.append(f"  {label}({len(names)}): {', '.join(str(n) for n in names)}")
    return _footer(extra, lines)


def _render_iface_status(data: dict, extra: dict) -> list[str]:
    consistent = data.get("consistent")
    lines = [f"接口面一致性 {'一致' if consistent else '不一致'}: {data.get('id', extra.get('id', '?'))}"]
    lines.append(f"  重算 hash: {_fmt(data.get('recomputed_hash'))}")
    lines.append(f"  快照 hash: {_fmt(data.get('snapshot_hash'))}")
    if data.get("declaration_hash") is not None:
        lines.append(f"  声明 hash: {_fmt(data.get('declaration_hash'))}")
    return _footer(extra, lines)


def _render_iface_publish(data: dict, extra: dict) -> list[str]:
    if data.get("noop"):
        lines = ["接口发布: 面未变 ⇒ 空操作(不写盘、不推进版本, 退出 0; contract §6.5)"]
        return _footer(extra, lines)
    lines = [f"接口发布: {data.get('id', extra.get('id', '?'))}"]
    for key, label in (
        ("version", "版本"), ("from", "原版本"), ("to", "新版本"),
        ("compat_gen", "COMPAT_GEN"), ("verdict", "变更判定"), ("published", "已发布"),
    ):
        if data.get(key) is not None:
            lines.append(f"  {label}: {_fmt(data[key])}")
    report = _as_dict(data.get("report"))
    if report:
        dependents = _as_dict(report.get("dependents"))
        for key, label in (("direct", "直接依赖者"), ("transitive", "传递依赖者"), ("unsatisfied", "失配者")):
            names = _as_list(dependents.get(key))
            if names:
                lines.append(f"  {label}({len(names)}): {', '.join(str(n) for n in names)}")
    return _footer(extra, lines)


def _render_iface_state(data: dict, extra: dict, verb: str) -> list[str]:
    target = data.get("id") or extra.get("id") or "?"
    entry = data.get("entry") or extra.get("entry")
    where = f"{target}#{entry}" if entry else str(target)
    lines = [f"接口{verb}: {where}"]
    if data.get("proposal"):
        lines.append(f"  待升格提案: {data['proposal']}(v0.1 只出提案, 不落 frozen 快照、不 bump COMPAT_GEN —— A-26 例外)")
    if data.get("freeze_state"):
        lines.append(f"  冻结态: {data['freeze_state']}")
    if data.get("status"):
        lines.append(f"  状态: {data['status']}")
    if data.get("compat_gen") is not None:
        lines.append(f"  COMPAT_GEN: {_fmt(data['compat_gen'])}")
    return _footer(extra, lines)


def _render_version(data: dict, extra: dict) -> list[str]:
    lines = [f"brickie {data.get('version', '-')}"]
    if data.get("protocol") is not None:
        lines.append(f"  协议: {data['protocol']}")
    languages = [item for item in _as_list(data.get("languages")) if isinstance(item, dict)]
    if languages:
        lines.append("  语言:")
        for item in languages:
            lines.append(f"    {item.get('name', '?'):<6} {item.get('version', '-')}")
    deps = _as_list(data.get("deps"))
    if deps:
        lines.append("  依赖指纹:")
        for dep in deps:
            if isinstance(dep, dict):
                name = dep.get("name", dep.get("id", "?"))
                value = dep.get("version", dep.get("value", dep.get("hash", "-")))
                lines.append(f"    {name:<24} {_fmt(value)}")
            else:
                lines.append(f"    {_fmt(dep)}")
    elif extra.get("deps_requested"):
        lines.append("  依赖指纹: (core 未返回 deps)")
    return lines


def _render_build(data: dict, extra: dict) -> list[str]:
    """`brickie build` 的人读呈现: 目标事实 / 单元 / 步骤统计 / 工具 / 产物。"""
    ok = bool(extra.get("ok", True))
    profile = _fmt(data.get("profile"))
    backend = _fmt(data.get("backend"))
    lines = [f"构建{'完成' if ok else '失败'}(profile={profile}, backend={backend})"]

    target = _as_dict(data.get("target"))
    outputs = _as_dict(data.get("outputs"))
    lines.append(f"  目标: {_fmt(target.get('arch'))} / 交叉前缀 {_fmt(target.get('cross'))}"
                 f"  (来自 {_fmt(target.get('owner'))})")
    if target.get("linker_script"):
        lines.append(f"  链接脚本: {_fmt(target.get('linker_script'))}")
    lines.append(f"  产物: {_fmt(outputs.get('elf'))} + {_fmt(outputs.get('bin'))}"
                 f"  (对象 {_fmt(outputs.get('obj_dir'))})")

    units = [u for u in _as_list(data.get("units")) if isinstance(u, dict)]
    lines.append(f"  单元: {len(units)} 个 / 源文件 {_fmt(data.get('source_count'))} 个")
    for unit in units:
        lines.append(f"    {_fmt(unit.get('name'))}: {_fmt(unit.get('sources'))} 个源"
                     + (f"  defines={unit.get('defines')}" if unit.get("defines") else ""))

    total = data.get("steps_total")
    todo = data.get("steps_todo")
    upto = data.get("up_to_date")
    lines.append(f"  步骤: 共 {_fmt(total)} / 本次要跑 {_fmt(todo)} / 已是最新 {_fmt(upto)}")

    results = [r for r in _as_list(extra.get("results")) if isinstance(r, dict)]
    for res in results[:24]:
        state = "ok  " if res.get("ok") else "FAIL"
        lines.append(f"    {state} {_fmt(res.get('label'))}")
    if len(results) > 24:
        lines.append(f"    …(另有 {len(results) - 24} 条步骤结果, --json 里全有)")

    stale = _as_list(data.get("stale_outputs"))
    if stale:
        lines.append(f"  过期产物: {len(stale)} 个(状态文件里已不在计划内的对象)")
    if extra.get("post"):
        lines.append(f"  构建后门禁: {', '.join(str(p) for p in extra['post'])}")
    lines.append(f"  计划指纹: {_short_hash(_fmt(data.get('plan_hash')))}")
    return _footer(extra, lines)


def _render_clean(data: dict, extra: dict) -> list[str]:
    removed = _as_list(extra.get("removed"))
    lines = [f"清理完成: 删掉 {len(removed)} 个路径(镜像侧; 不动 build/host 的工具)"]
    for path in removed:
        lines.append(f"  删除 {_fmt(path)}")
    if not removed:
        lines.append("  (没有需要删的东西)")
    for path in _as_list(data.get("paths")):
        if path not in removed:
            lines.append(f"  跳过 {_fmt(path)}(不存在)")
    return _footer(extra, lines)


def _render_test(data: dict, extra: dict) -> list[str]:
    if extra.get("list"):
        lines = [f"门禁声明面: {_fmt(data.get('file'))}"
                 + ("" if data.get("present") else "  (文件不存在)")]
        for key, label in (("hosttests", "宿主用例"), ("gates", "QEMU 门禁"), ("scripts", "脚本门禁")):
            names = [str(x) for x in _as_list(data.get(key))]
            lines.append(f"  {label}({len(names)}): {', '.join(names) if names else '(空)'}")
        post = [str(x) for x in _as_list(data.get("build_post"))]
        lines.append(f"  构建后自动跑: {', '.join(post) if post else '(无)'}")
        return lines
    ok = bool(extra.get("ok", True))
    lines = [f"门禁{'全绿' if ok else '红'}: {_fmt(data.get('name')) or '(全部)'}"]
    gates = _as_dict(data.get("gates"))
    if gates:
        for key, label in (("hosttests", "宿主用例"), ("gates", "QEMU 门禁"), ("scripts", "脚本门禁")):
            names = [str(x) for x in _as_list(gates.get(key))]
            if names:
                lines.append(f"  {label}: {', '.join(names)}")
    results = [r for r in _as_list(extra.get("results")) if isinstance(r, dict)]
    for res in results:
        state = "ok  " if res.get("ok") else "FAIL"
        judged = ""
        if res.get("judged"):
            judged = f"  [判据: {res.get('judge_failed', 0)} 条不满足]" if res.get("judge_failed") else "  [判据通过]"
        lines.append(f"    {state} {_fmt(res.get('label'))}{judged}")
    if not results:
        lines.append("  (没有步骤 —— 门禁名单为空?)")
    return _footer(extra, lines)


def _render_size(data: dict, extra: dict) -> list[str]:
    # 体积表已由执行器直接打到 stdout(那是 `size` 的真实输出) —— 这里不再重复。
    return []


def _render_disasm(data: dict, extra: dict) -> list[str]:
    return []


def _render_run(data: dict, extra: dict) -> list[str]:
    return []


def _render_generic(data: dict, extra: dict) -> list[str]:
    """未定型 ``data`` 的兜底呈现: 标量逐行, 复合结构缩进 JSON。

    这样即使 core 的某个命令还没实现, 编排/呈现也不会抛异常; 字段名按 contract §5
    写死的命令走上面的专用渲染器。
    """
    lines = [f"{extra.get('title', '结果')}:"]
    if not data:
        return _footer(extra, lines + ["  (data 为空)"])
    for key in data:
        value = data[key]
        if isinstance(value, (str, int, float, bool)) or value is None:
            lines.append(f"  {key}: {_fmt(value)}")
        else:
            lines.append(f"  {key}:")
            block = json.dumps(value, ensure_ascii=False, indent=2)
            lines.extend(f"    {line}" for line in block.splitlines())
    return _footer(extra, lines)


# ------------------------------------------------------------------ 分派

_RENDERERS = {
    "check": _render_check,
    "dep tree": _render_dep_tree,
    "dep graph": _render_dep_graph,
    "dep why": _render_dep_why,
    "dep index": _render_dep_index,
    "dep closure": _render_closure,
    "ver show": _render_ver_show,
    "ver bump": _render_ver_bump,
    "iface list": _render_iface_list,
    "iface show": _render_iface_show,
    "iface diff": _render_iface_diff,
    "iface status": _render_iface_status,
    "iface publish": _render_iface_publish,
    "version": _render_version,
    "build": _render_build,
    "clean": _render_clean,
    "test": _render_test,
    "size": _render_size,
    "disasm": _render_disasm,
    "run": _render_run,
}


def render_text(command: str, data: dict, extra: dict | None = None) -> str:
    """把一条命令的 ``data`` 渲染成人读文本(与 ``--json`` 同源)。"""
    extra = extra or {}
    if command == "new":
        args = _as_dict(extra.get("args"))
        title = (
            f"{args.get('name', '?')}  插件骨架已生成"
            f"({args.get('plugin_type', '?')} / {args.get('api_type', '?')} / {args.get('lang', '?')})"
        )
        failed = f"{args.get('name', '?')}  插件骨架计划(未生成)"
        return "\n".join(_render_plan(data, extra, title, failed))
    if command == "init":
        args = _as_dict(extra.get("args"))
        return "\n".join(_render_plan(
            data, extra,
            f"产品 {args.get('name', '?')} 骨架已生成",
            f"产品 {args.get('name', '?')} 骨架计划(未生成)",
        ))
    if command == "gen":
        if extra.get("check"):
            conflicts = _as_list(extra.get("stale")) + _as_list(extra.get("missing"))
            lines = [
                "gen --check: 生成物与重算逐字节一致"
                if not conflicts
                else f"gen --check: {len(conflicts)} 个生成物与重算不一致(未写盘)"
            ]
            lines.extend(f"  过期 {path}" for path in _as_list(extra.get("stale")))
            lines.extend(f"  缺失 {path}" for path in _as_list(extra.get("missing")))
            return "\n".join(_footer(extra, lines))
        return "\n".join(_render_plan(
            data, extra,
            "gen: 生成物重建计划(已落盘)",
            "gen: 生成物重建计划(未落盘)",
        ))
    if command == "dep add":
        return "\n".join(_render_dep_snippet(data, extra, "add"))
    if command == "dep rm":
        return "\n".join(_render_dep_snippet(data, extra, "rm"))
    if command in ("iface freeze",):
        return "\n".join(_render_iface_state(data, extra, "冻结(待升格提案)"))
    if command in ("iface deprecate",):
        return "\n".join(_render_iface_state(data, extra, "弃用"))
    if command in ("iface undeprecate",):
        return "\n".join(_render_iface_state(data, extra, "取消弃用"))
    if command in ("iface unfreeze",):
        return "\n".join(_render_iface_state(data, extra, "解冻(进入窗口)"))
    if command in ("iface refreeze",):
        return "\n".join(_render_iface_state(data, extra, "重新冻结(退出窗口)"))

    renderer = _RENDERERS.get(command)
    if renderer is not None:
        return "\n".join(renderer(data, extra))
    return "\n".join(_render_generic(data, {**extra, "title": f"{command} 结果"}))

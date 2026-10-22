"""Presentation layer: render ``data`` as text; ``--json`` serializes the same ``data``.

Renderers read only ``data`` and the write results (``extra``) and re-decide nothing -- verdicts and
codes come from core. Diagnostic text is one line per diagnostic (severity + code + location).
"""

from __future__ import annotations

import json

_SEVERITY_LABEL = {"error": "error", "warning": "warning", "info": "note"}


def sort_diagnostics(diagnostics: list[dict]) -> list[dict]:
    """Stable sort by ``(code, file, span, target)``; snapshot precondition for V-9."""

    def key(diag: dict) -> tuple[str, str, str, str]:
        return (
            str(diag.get("code") or ""),
            str(diag.get("file") or ""),
            str(diag.get("span") or ""),
            str(diag.get("target") or ""),
        )

    return sorted(diagnostics, key=key)


def emit_diagnostics_text(diagnostics: list[dict]) -> str:
    """Human-readable diagnostics text (printed to stderr)."""
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
            lines.append(f"       hint: {diag['hint']}")
    return "\n".join(lines)


def json_payload(
    *,
    command: str,
    exit_code: int,
    diagnostics: list[dict],
    data: dict,
    written: list[str] | None = None,
) -> dict:
    """Stable ``--json`` envelope: the same ``data`` + diagnostics + the files actually written."""
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


def _as_list(value: object) -> list:
    return value if isinstance(value, list) else []


def _as_dict(value: object) -> dict:
    return value if isinstance(value, dict) else {}


def _fmt(value: object) -> str:
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, (int, float, str)):
        return str(value)
    return json.dumps(value, ensure_ascii=False)


def _short_hash(value: object, width: int = 12) -> str:
    if isinstance(value, str) and value:
        return value[:width]
    return "-"


def _table(headers: list[str], rows: list[list[object]]) -> list[str]:
    """Minimal monospace-aligned table (an empty table yields a hint line)."""
    if not rows:
        return ["  (empty)"]
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
    """Write-results section: written/unchanged/skipped counts (same facts go into ``--json``)."""
    written = _as_list(extra.get("written"))
    unchanged = _as_list(extra.get("unchanged"))
    skipped = _as_list(extra.get("skipped"))
    if written:
        lines.append(f"written: {len(written)} files")
        lines.extend(f"  write {path}" for path in written)
    if unchanged:
        lines.append(f"unchanged: {len(unchanged)} files (byte-identical, not written)")
    if skipped:
        lines.append(f"skipped: {len(skipped)} human-written files (already exist, never overwritten)")
    return lines


def _section(title: str, lines: list[str]) -> list[str]:
    if lines:
        return [title, *lines]
    return []


def _render_plan(data: dict, extra: dict, title: str, title_failed: str) -> list[str]:
    artifacts = [a for a in _as_list(data.get("artifacts")) if isinstance(a, dict)]
    written = set(_as_list(extra.get("written")))
    unchanged = set(_as_list(extra.get("unchanged")))
    skipped = set(_as_list(extra.get("skipped")))

    # Non-zero exit must not claim "generated": title and JSON exit_code share one source.
    ok = bool(extra.get("ok", True))
    lines = [title if ok else title_failed]
    if not artifacts:
        lines.append("  (no artifacts in the plan)")
    for artifact in artifacts:
        path = artifact.get("path")
        if not isinstance(path, str):
            continue
        kind = "human" if artifact.get("kind") == "human" else "gen"
        state = ""
        if path in written:
            state = "written"
        elif path in unchanged:
            state = "up to date"
        elif path in skipped:
            state = "skipped (exists, not overwritten)"
        suffix = f"   [{state}]" if state else ""
        lines.append(f"  {kind:<4} {path}{suffix}")
    lines = _footer(extra, lines)
    if written:
        lines.append("")
        lines.append("next: brickie check")
    return lines


def _render_check(data: dict, extra: dict) -> list[str]:
    summary = _as_dict(data.get("summary"))
    errors = summary.get("errors", 0)
    warnings = summary.get("warnings", 0)
    infos = summary.get("infos", 0)
    profile = summary.get("profile") or extra.get("profile") or "dev"
    closure = _as_dict(summary.get("closure"))

    verdict = "passed" if not errors else "failed"
    lines = [f"check {verdict}: {errors} errors / {warnings} warnings / {infos} infos (profile={profile})"]
    if closure:
        plugins = closure.get("plugins", "-")
        ram = closure.get("ram_kib", "-")
        stack = closure.get("stack_kib", "-")
        lines.append(f"  closure: {plugins} plugins, RAM total {ram} KiB, stack total {stack} KiB")

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
        lines.append("  per scope:")
        lines.extend(_table(["scope", "errors", "warnings", "infos"], rows))
    return _footer(extra, lines)


def _render_closure(data: dict, extra: dict) -> list[str]:
    plugins = [p for p in _as_list(data.get("plugins")) if isinstance(p, dict)]
    rows = [
        [
            p.get("name", "?"), p.get("version", "-"), p.get("compat_gen", 0),
            p.get("plugin_type", "-"), p.get("phase", "-"),
            p.get("ram_kib", "-"), p.get("stack_kib", "-"),
            "selected" if p.get("selected", True) else "pruned",
        ]
        for p in plugins
    ]
    lines = [f"product closure (profile={data.get('profile', 'dev')}): {len(plugins)} plugins"]
    lines.append("  cg = COMPAT_GEN (exact pin, not part of range comparison)")
    lines.extend(_table(["plugin", "version", "cg", "type", "phase", "RAM", "stack", "status"], rows))

    order = _as_list(data.get("topo_order"))
    if order:
        lines.extend(_section("dependency topological order (name tie-break; the runtime order is `show brick-init-flow`):",
                              [f"  {' → '.join(str(x) for x in order)}"]))

    totals = _as_dict(data.get("totals"))
    if totals:
        budget = _as_dict(totals.get("budget"))
        capacity = _as_dict(totals.get("runtime_capacity"))
        lines.append(
            f"total: RAM {totals.get('ram_kib', '-')} KiB / stack {totals.get('stack_kib', '-')} KiB"
            f" (platform capacity RAM {capacity.get('ram_kib', '-')} / stack {capacity.get('stack_kib', '-')};"
            f" product budget RAM {budget.get('ram_kib', '-')} / stack {budget.get('stack_kib', '-')})"
        )

    cycles = _as_list(data.get("cycles"))
    if cycles:
        lines.append(f"cycles: {len(cycles)}")
        for cycle in cycles:
            path = cycle if isinstance(cycle, list) else _as_dict(cycle).get("path", cycle)
            lines.append(f"  {' → '.join(str(x) for x in path) if isinstance(path, list) else _fmt(path)}")
    return _footer(extra, lines)


def _render_show_bricks(data: dict, extra: dict) -> list[str]:
    """``show bricks``: the closure as a reading aid -- grouped by namespace, with origins/cost."""
    bricks = [b for b in _as_list(data.get("bricks")) if isinstance(b, dict)]
    totals = _as_dict(data.get("totals"))
    product = _as_dict(data.get("product"))
    head = "product bricks"
    if product:
        head += (
            f" ({product.get('name', '?')} {product.get('version', '?')},"
            f" profile={product.get('profile', 'dev')})"
        )
    lines = [
        f"{head}: {totals.get('plugins', len(bricks))} plugins, {totals.get('roots', 0)} roots",
        "  origin: root = named in product.toml, dep = pulled in by a [[dep]] closure",
    ]
    rows = [
        [
            b.get("name", "?"),
            b.get("plugin_type", "-"),
            b.get("subkind") or "-",
            b.get("phase", "-"),
            b.get("ram_kib", "-"),
            b.get("stack_kib", "-"),
            "root:" + str(b.get("root_kind", "")) if b.get("origin") == "root" else "dep",
        ]
        for b in bricks
    ]
    lines.extend(_table(["plugin", "type", "subkind", "phase", "RAM", "stack", "origin"], rows))

    budget = _as_dict(totals.get("budget"))
    capacity = _as_dict(totals.get("runtime_capacity"))
    lines.append(
        f"total: RAM {totals.get('ram_kib', '-')} KiB / stack {totals.get('stack_kib', '-')} KiB"
        f"   (product budget RAM {budget.get('ram_kib', '-')} / stack {budget.get('stack_kib', '-')};"
        f" runtime capacity RAM {capacity.get('ram_kib', '-')} / stack {capacity.get('stack_kib', '-')})"
    )
    by_type = _as_dict(totals.get("by_type"))
    parts = [f"{k} {v}" for k, v in by_type.items() if v]
    if parts:
        lines.append("by type: " + ", ".join(parts))
    seeds = _as_list(data.get("seeds"))
    if seeds:
        lines.extend(_section("closure roots:", ["  " + " → ".join(str(s) for s in seeds)]))
    return _footer(extra, lines)


def _render_show_init_flow(data: dict, extra: dict) -> list[str]:
    """``show brick-init-flow``: the runtime boot chain, step by step (phase / hook / plugin)."""
    steps = [s for s in _as_list(data.get("steps")) if isinstance(s, dict)]
    product = _as_dict(data.get("product"))
    counts = _as_dict(data.get("counts"))
    head = "brick init flow (runtime order)"
    if product:
        head += (
            f" -- {product.get('name', '?')} {product.get('version', '?')},"
            f" profile={product.get('profile', 'dev')}"
        )
    lines = [
        f"{head}: {len(steps)} steps",
        f"  order basis: {data.get('order_basis', '?')}",
    ]
    rows = [
        [
            index,
            s.get("phase", "-"),
            s.get("hook") or "-",
            s.get("plugin") or "-",
            s.get("note", ""),
        ]
        for index, s in enumerate(steps)
    ]
    lines.extend(_table(["#", "phase", "hook", "plugin", "note"], rows))
    keys = ("early", "core", "late", "app", "irq", "start", "selftest")
    parts = [f"{k} {counts.get(k, 0)}" for k in keys if k in counts]
    if parts:
        lines.append("per phase: " + ", ".join(parts))
    lines.append(
        "selftest pass: "
        + (
            "on (after START, before the scheduler takes over)"
            if data.get("selftest_on")
            else "off (product switch)"
        )
    )
    unsorted = _as_list(data.get("unsorted"))
    if unsorted:
        lines.append(f"UNSCHEDULABLE (init cycle): {len(unsorted)} plugin(s)")
        lines.extend(f"  {name}" for name in unsorted)
    return _footer(extra, lines)


def _render_dep_tree(data: dict, extra: dict) -> list[str]:
    nodes = _as_dict(data.get("nodes"))
    roots = [str(r) for r in _as_list(data.get("roots"))]
    kind = data.get("kind", "all")
    lines = [f"dependency tree (kind={kind}):"]
    if not roots:
        lines.append("  (empty)")
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
                lines.append(f"  {prefix}{branch}{dep_name}{detail} (cycle)")
                continue
            lines.append(f"  {prefix}{branch}{dep_name}{detail}")
            walk(dep_name, prefix + ("    " if last else "│   "), seen | {dep_name})

    for root in roots:
        lines.append(f"  {root}")
        walk(root, "  ", frozenset({root}))
    return _footer(extra, lines)


def _render_dep_graph(data: dict, extra: dict) -> list[str]:
    text = data.get("text")
    lines = [f"dependency graph (format={data.get('format', '?')}):"]
    if isinstance(text, str) and text:
        lines.extend(f"  {line}" for line in text.splitlines())
    else:
        nodes = _as_list(data.get("nodes"))
        edges = [e for e in _as_list(data.get("edges")) if isinstance(e, dict)]
        lines.append(f"  {len(nodes)} nodes, {len(edges)} edges")
        lines.extend(
            f"  {e.get('from', '?')} -[{e.get('kind', '?')}]-> {e.get('to', '?')}" for e in edges
        )
    return _footer(extra, lines)


def _render_dep_why(data: dict, extra: dict) -> list[str]:
    if not data.get("found"):
        return _footer(extra, [f"dependency path not found: {extra.get('from', '?')} → {extra.get('to', '?')}"])
    path = _as_list(data.get("path"))
    lines = [f"dependency path ({' → '.join(str(p) for p in path)}):"]
    for edge in _as_list(data.get("edges")):
        info = _as_dict(edge)
        lines.append(f"  {info.get('from', '?')} -[{info.get('kind', '?')}]-> {info.get('to', '?')}")
    return _footer(extra, lines)


def _render_dep_index(data: dict, extra: dict) -> list[str]:
    dependents = _as_dict(data.get("dependents"))
    edges = sum(len(v) for v in dependents.values() if isinstance(v, list))
    lines = [f"reverse dependency index: {len(dependents)} targets, {edges} edges"]
    rows = []
    for target, sources in sorted(dependents.items()):
        for source in _as_list(sources):
            info = _as_dict(source)
            rows.append([target, info.get("from", "?"), info.get("kind", "?")])
    if rows:
        lines.extend(_table(["target", "dependent", "kind"], rows))
    return _footer(extra, lines)


def _render_dep_snippet(data: dict, extra: dict, verb: str) -> list[str]:
    lines = [f"dep {verb} only prints a snippet, **does not rewrite brick.toml** (ruling R-2: human-written files are never rewritten)"]
    snippet = data.get("snippet")
    if isinstance(snippet, str) and snippet:
        lines.append("")
        lines.extend(f"  {line}" for line in snippet.splitlines())
    else:
        lines.append("  (core returned no snippet)")
    if data.get("locator"):
        lines.extend(["", f"  locator: {_fmt(data['locator'])}"])
    return _footer(extra, lines)


def _render_ver_show(data: dict, extra: dict) -> list[str]:
    segments = _as_dict(data.get("segments"))
    lines = [f"interface unit {data.get('id', extra.get('id', '?'))}"]
    lines.append(f"  version: {data.get('version', '-')}"
                 f" (COMPAT_GEN.{'*'}= {segments.get('compat_gen', data.get('compat_gen', '-'))}"
                 f" / MAJOR {segments.get('major', '-')}"
                 f" / MINOR {segments.get('minor', '-')}"
                 f" / REVISE {segments.get('revise', '-')})")
    lines.append("  note: COMPAT_GEN is not part of range comparison (contract §5; RV-11)")
    lines.append(f"  plugin: {data.get('plugin', '-')}   source: {data.get('source', '-')}")
    lines.append(f"  status: {data.get('status', '-')} / freeze state: {data.get('freeze_state', '-')}"
                 f" / published: {_fmt(data.get('published'))}")
    source = _as_dict(data.get("compat_gen_source"))
    if source:
        note = f"({source.get('note')})" if source.get("note") else ""
        lines.append(f"  COMPAT_GEN source: {source.get('kind', '-')}{note}")
    return _footer(extra, lines)


def _render_ver_bump(data: dict, extra: dict) -> list[str]:
    lines = [f"version bump: {data.get('id', extra.get('id', '?'))}"]
    if data.get("from") is not None or data.get("to") is not None:
        lines.append(f"  {data.get('from', '-')} → {data.get('to', '-')} (rule={data.get('rule', '-')})")
    else:
        lines.append(f"  rule={data.get('rule', '-')}   version: {data.get('version', '-')}")
    segments = _as_dict(data.get("segments"))
    if segments:
        lines.append(
            f"  four segments: COMPAT_GEN {segments.get('compat_gen', '-')}"
            f" / MAJOR {segments.get('major', '-')}"
            f" / MINOR {segments.get('minor', '-')}"
            f" / REVISE {segments.get('revise', '-')}"
        )
    lines.append("  note: version in brick.toml is human-maintained -- the tool only reports what it should become (ruling R-3)")
    return _footer(extra, lines)


def _render_iface_list(data: dict, extra: dict) -> list[str]:
    units = [u for u in _as_list(data.get("units")) if isinstance(u, dict)]
    lines = [f"interface units: {len(units)}"]
    rows = [
        [
            u.get("id", "?"), u.get("api_iface", "-"), u.get("form", "-"),
            u.get("version", "-"), u.get("compat_gen", 0),
            u.get("freeze_state", "-"), u.get("status", "-"),
            _fmt(u.get("published")), _short_hash(u.get("hash")),
        ]
        for u in units
    ]
    lines.extend(_table(["unit", "category", "form", "version", "cg", "freeze", "status", "published", "hash"], rows))
    return _footer(extra, lines)


def _render_iface_show(data: dict, extra: dict) -> list[str]:
    unit = _as_dict(data.get("unit"))
    lines = [f"interface unit {unit.get('id', extra.get('id', '?'))}"]
    for key, label in (
        ("api_iface", "category"), ("form", "form"), ("version", "version"),
        ("compat_gen", "COMPAT_GEN"), ("freeze_state", "freeze state"), ("status", "status"),
        ("hash_scope", "hash_scope"), ("truth", "truth"),
        ("declaration_hash", "declaration hash"), ("snapshot_hash", "snapshot hash"),
        ("published", "published"),
    ):
        value = unit.get(key, data.get(key))
        lines.append(f"  {label}: {_fmt(value)}")
    lines.append("  NOT_ABI: declaration hash ≠ proof of ABI compatibility (v0.1 truth = declaration side; contract §6.6)")
    if data.get("not_abi") is False:
        lines.append("  ⚠ core reports not_abi=false: v0.1 does not support symbol-side truth")

    entries = [e for e in _as_list(data.get("entries")) if isinstance(e, dict)]
    lines.extend(_section(f"entries ({len(entries)}):", [
        f"  {e.get('kind', '?'):<14} {e.get('name', '?'):<28} {e.get('status', '-')}"
        for e in entries
    ]))
    if data.get("source"):
        lines.append(f"  source: {data['source']}")
    return _footer(extra, lines)


def _render_iface_diff(data: dict, extra: dict) -> list[str]:
    lines = [f"interface change set {data.get('id', extra.get('id', '?'))}: verdict {data.get('verdict', '-')}"]
    version = _as_dict(data.get("version"))
    if version:
        lines.append(f"  version: {version.get('from', '-')} → {version.get('to', '-')}")
        reasons = _as_list(version.get("reasons"))
        if reasons:
            lines.append(f"  reasons: {', '.join(str(r) for r in reasons)}")
    lines.append(f"  COMPAT_GEN changed: {_fmt(data.get('compat_gen_changed'))}")
    changes = [c for c in _as_list(data.get("changes")) if isinstance(c, dict)]
    if changes:
        lines.append(f"  changes ({len(changes)}):")
        lines.extend(
            f"    {c.get('kind', '?'):<10} {c.get('name', c.get('entry', '?'))}"
            + (f"  {c.get('from')} → {c.get('to')}" if c.get("from") is not None else "")
            for c in changes
        )
    dependents = _as_dict(data.get("dependents"))
    if dependents:
        for key, label in (("direct", "direct dependents"), ("transitive", "transitive dependents"), ("unsatisfied", "unsatisfied")):
            names = _as_list(dependents.get(key))
            if names:
                lines.append(f"  {label} ({len(names)}): {', '.join(str(n) for n in names)}")
    return _footer(extra, lines)


def _render_iface_status(data: dict, extra: dict) -> list[str]:
    consistent = data.get("consistent")
    lines = [f"interface surface consistency {'consistent' if consistent else 'inconsistent'}: {data.get('id', extra.get('id', '?'))}"]
    lines.append(f"  recomputed hash: {_fmt(data.get('recomputed_hash'))}")
    lines.append(f"  snapshot hash: {_fmt(data.get('snapshot_hash'))}")
    if data.get("declaration_hash") is not None:
        lines.append(f"  declaration hash: {_fmt(data.get('declaration_hash'))}")
    return _footer(extra, lines)


def _render_iface_publish(data: dict, extra: dict) -> list[str]:
    if data.get("noop"):
        lines = ["interface publish: surface unchanged ⇒ no-op (no write, no version bump, exit 0; contract §6.5)"]
        return _footer(extra, lines)
    lines = [f"interface publish: {data.get('id', extra.get('id', '?'))}"]
    for key, label in (
        ("version", "version"), ("from", "old version"), ("to", "new version"),
        ("compat_gen", "COMPAT_GEN"), ("verdict", "change verdict"), ("published", "published"),
    ):
        if data.get(key) is not None:
            lines.append(f"  {label}: {_fmt(data[key])}")
    report = _as_dict(data.get("report"))
    if report:
        dependents = _as_dict(report.get("dependents"))
        for key, label in (("direct", "direct dependents"), ("transitive", "transitive dependents"), ("unsatisfied", "unsatisfied")):
            names = _as_list(dependents.get(key))
            if names:
                lines.append(f"  {label} ({len(names)}): {', '.join(str(n) for n in names)}")
    return _footer(extra, lines)


def _render_iface_state(data: dict, extra: dict, verb: str) -> list[str]:
    target = data.get("id") or extra.get("id") or "?"
    entry = data.get("entry") or extra.get("entry")
    where = f"{target}#{entry}" if entry else str(target)
    lines = [f"interface {verb}: {where}"]
    if data.get("proposal"):
        lines.append(f"  promotion proposal: {data['proposal']} (v0.1 only proposes; no frozen snapshot, no COMPAT_GEN bump -- A-26 exception)")
    if data.get("freeze_state"):
        lines.append(f"  freeze state: {data['freeze_state']}")
    if data.get("status"):
        lines.append(f"  status: {data['status']}")
    if data.get("compat_gen") is not None:
        lines.append(f"  COMPAT_GEN: {_fmt(data['compat_gen'])}")
    return _footer(extra, lines)


def _render_version(data: dict, extra: dict) -> list[str]:
    lines = [f"brickie {data.get('version', '-')}"]
    if data.get("protocol") is not None:
        lines.append(f"  protocol: {data['protocol']}")
    languages = [item for item in _as_list(data.get("languages")) if isinstance(item, dict)]
    if languages:
        lines.append("  languages:")
        for item in languages:
            lines.append(f"    {item.get('name', '?'):<6} {item.get('version', '-')}")
    deps = _as_list(data.get("deps"))
    if deps:
        lines.append("  dependency fingerprints:")
        for dep in deps:
            if isinstance(dep, dict):
                name = dep.get("name", dep.get("id", "?"))
                value = dep.get("version", dep.get("value", dep.get("hash", "-")))
                lines.append(f"    {name:<24} {_fmt(value)}")
            else:
                lines.append(f"    {_fmt(dep)}")
    elif extra.get("deps_requested"):
        lines.append("  dependency fingerprints: (core returned no deps)")
    return lines


def _render_build(data: dict, extra: dict) -> list[str]:
    """Human-readable ``brickie build``: target facts, units, step counts, outputs."""
    ok = bool(extra.get("ok", True))
    profile = _fmt(data.get("profile"))
    backend = _fmt(data.get("backend"))
    lines = [f"build {'done' if ok else 'failed'} (profile={profile}, backend={backend})"]

    target = _as_dict(data.get("target"))
    outputs = _as_dict(data.get("outputs"))
    lines.append(f"  target: {_fmt(target.get('arch'))} / cross prefix {_fmt(target.get('cross'))}"
                 f"  (from {_fmt(target.get('owner'))})")
    if target.get("linker_script"):
        lines.append(f"  linker script: {_fmt(target.get('linker_script'))}")
    lines.append(f"  outputs: {_fmt(outputs.get('elf'))} + {_fmt(outputs.get('bin'))}"
                 f"  (objects {_fmt(outputs.get('obj_dir'))})")

    units = [u for u in _as_list(data.get("units")) if isinstance(u, dict)]
    lines.append(f"  units: {len(units)} / sources: {_fmt(data.get('source_count'))}")
    for unit in units:
        lines.append(f"    {_fmt(unit.get('name'))}: {_fmt(unit.get('sources'))} sources"
                     + (f"  defines={unit.get('defines')}" if unit.get("defines") else ""))

    total = data.get("steps_total")
    todo = data.get("steps_todo")
    upto = data.get("up_to_date")
    lines.append(f"  steps: total {_fmt(total)} / to run {_fmt(todo)} / up to date {_fmt(upto)}")

    results = [r for r in _as_list(extra.get("results")) if isinstance(r, dict)]
    for res in results[:24]:
        state = "ok  " if res.get("ok") else "FAIL"
        lines.append(f"    {state} {_fmt(res.get('label'))}")
    if len(results) > 24:
        lines.append(f"    ...({len(results) - 24} more step results; all present in --json)")

    stale = _as_list(data.get("stale_outputs"))
    if stale:
        lines.append(f"  stale outputs: {len(stale)} (state-file entries no longer in the plan)")
    if extra.get("post"):
        lines.append(f"  post-build gates: {', '.join(str(p) for p in extra['post'])}")
    lines.append(f"  plan fingerprint: {_short_hash(_fmt(data.get('plan_hash')))}")
    return _footer(extra, lines)


def _render_clean(data: dict, extra: dict) -> list[str]:
    removed = _as_list(extra.get("removed"))
    lines = [f"clean done: removed {len(removed)} paths (image side; build/host tools untouched)"]
    for path in removed:
        lines.append(f"  remove {_fmt(path)}")
    if not removed:
        lines.append("  (nothing to remove)")
    for path in _as_list(data.get("paths")):
        if path not in removed:
            lines.append(f"  skip {_fmt(path)} (does not exist)")
    return _footer(extra, lines)


def _render_test(data: dict, extra: dict) -> list[str]:
    if extra.get("list"):
        lines = [f"gate declaration surface: {_fmt(data.get('file'))}"
                 + ("" if data.get("present") else "  (file missing)")]
        for key, label in (("hosttests", "host tests"), ("gates", "QEMU gates"), ("scripts", "script gates")):
            names = [str(x) for x in _as_list(data.get(key))]
            lines.append(f"  {label} ({len(names)}): {', '.join(names) if names else '(empty)'}")
        post = [str(x) for x in _as_list(data.get("build_post"))]
        lines.append(f"  run after build: {', '.join(post) if post else '(none)'}")
        return lines
    ok = bool(extra.get("ok", True))
    lines = [f"gates {'all green' if ok else 'red'}: {_fmt(data.get('name')) or '(all)'}"]
    gates = _as_dict(data.get("gates"))
    if gates:
        for key, label in (("hosttests", "host tests"), ("gates", "QEMU gates"), ("scripts", "script gates")):
            names = [str(x) for x in _as_list(gates.get(key))]
            if names:
                lines.append(f"  {label}: {', '.join(names)}")
    results = [r for r in _as_list(extra.get("results")) if isinstance(r, dict)]
    for res in results:
        state = "ok  " if res.get("ok") else "FAIL"
        judged = ""
        if res.get("judged"):
            judged = f"  [criteria: {res.get('judge_failed', 0)} unmet]" if res.get("judge_failed") else "  [criteria met]"
        lines.append(f"    {state} {_fmt(res.get('label'))}{judged}")
    if not results:
        lines.append("  (no steps -- is the gate list empty?)")
    return _footer(extra, lines)


def _render_size(data: dict, extra: dict) -> list[str]:
    # `size` prints its table to stdout from the executor; do not repeat it here.
    return []


def _render_disasm(data: dict, extra: dict) -> list[str]:
    return []


def _render_run(data: dict, extra: dict) -> list[str]:
    return []


def _render_generic(data: dict, extra: dict) -> list[str]:
    """Fallback for untyped ``data``: scalars line by line, compound structures as indented JSON.

    Commands with no dedicated renderer land here, so presentation never raises.
    """
    lines = [f"{extra.get('title', 'result')}:"]
    if not data:
        return _footer(extra, lines + ["  (data is empty)"])
    for key in data:
        value = data[key]
        if isinstance(value, (str, int, float, bool)) or value is None:
            lines.append(f"  {key}: {_fmt(value)}")
        else:
            lines.append(f"  {key}:")
            block = json.dumps(value, ensure_ascii=False, indent=2)
            lines.extend(f"    {line}" for line in block.splitlines())
    return _footer(extra, lines)


_RENDERERS = {
    "check": _render_check,
    "dep tree": _render_dep_tree,
    "dep graph": _render_dep_graph,
    "dep why": _render_dep_why,
    "dep index": _render_dep_index,
    "dep closure": _render_closure,
    "show bricks": _render_show_bricks,
    "show brick-init-flow": _render_show_init_flow,
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
    """Render one command's ``data`` as human-readable text (same source as ``--json``)."""
    extra = extra or {}
    if command == "new":
        args = _as_dict(extra.get("args"))
        title = (
            f"{args.get('name', '?')}  plugin skeleton generated"
            f" ({args.get('plugin_type', '?')} / {args.get('api_type', '?')} / {args.get('lang', '?')})"
        )
        failed = f"{args.get('name', '?')}  plugin skeleton plan (not generated)"
        return "\n".join(_render_plan(data, extra, title, failed))
    if command == "init":
        args = _as_dict(extra.get("args"))
        return "\n".join(_render_plan(
            data, extra,
            f"product {args.get('name', '?')} skeleton generated",
            f"product {args.get('name', '?')} skeleton plan (not generated)",
        ))
    if command == "gen":
        if extra.get("check"):
            stale = _as_list(extra.get("stale"))
            missing = _as_list(extra.get("missing"))
            orphan = _as_list(extra.get("orphan"))
            conflicts = stale + missing + orphan
            lines = [
                "gen --check: generated files byte-identical to recomputation"
                if not conflicts
                else f"gen --check: {len(conflicts)} generated files differ from recomputation (nothing written)"
            ]
            lines.extend(f"  stale {path}" for path in stale)
            lines.extend(f"  missing {path}" for path in missing)
            lines.extend(f"  orphan (belongs to no plugin) {path}" for path in orphan)
            return "\n".join(_footer(extra, lines))
        lines = _render_plan(
            data, extra,
            "gen: generated-file rebuild plan (written)",
            "gen: generated-file rebuild plan (not written)",
        )
        pruned = _as_list(extra.get("pruned"))
        if pruned:
            lines.append("")
            lines.append(f"pruned {len(pruned)} stale generated files (belonging to no plugin in the tree):")
            lines.extend(f"  stale {path}" for path in pruned)
        return "\n".join(lines)
    if command == "dep add":
        return "\n".join(_render_dep_snippet(data, extra, "add"))
    if command == "dep rm":
        return "\n".join(_render_dep_snippet(data, extra, "rm"))
    if command in ("iface freeze",):
        return "\n".join(_render_iface_state(data, extra, "freeze (promotion proposal pending)"))
    if command in ("iface deprecate",):
        return "\n".join(_render_iface_state(data, extra, "deprecate"))
    if command in ("iface undeprecate",):
        return "\n".join(_render_iface_state(data, extra, "undeprecate"))
    if command in ("iface unfreeze",):
        return "\n".join(_render_iface_state(data, extra, "unfreeze (enter window)"))
    if command in ("iface refreeze",):
        return "\n".join(_render_iface_state(data, extra, "refreeze (leave window)"))

    renderer = _RENDERERS.get(command)
    if renderer is not None:
        return "\n".join(renderer(data, extra))
    return "\n".join(_render_generic(data, {**extra, "title": f"{command} result"}))

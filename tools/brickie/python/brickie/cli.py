"""brickie CLI (L5 front-end).

Parses args (usage error ⇒ exit 2) and orchestrates core/gen; every decision and code comes
from core. Pipeline: shape check (BRV-MF-0001 / exit 2) → core → gen render → write
(idempotent; --check compares only ⇒ exit 1) → present; undelivered commands stay out of ``--help``.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import PROTOCOL, __version__
from . import native, present, runner, schema, writer
from .native import NativeError

_EXIT_OK = 0
_EXIT_RED = 1        # validation red (cycle / conflict / version / taxonomy / privilege / --check mismatch)
_EXIT_USAGE = 2      # usage or environment error (bad args / shape mismatch / missing path / missing native build)

_EPILOG = """\
v0.1 boundary: composition commands (check / dep / iface / gen / new) are **declaration-surface
completeness checkers** — they guarantee a composition is logically self-consistent, not that it
"compiles / runs right"; this family does **not** require cc / cargo / nm to be present
(brickie-v0.1 §0 boundary discipline, and the premise of CI jobs with zero build dependencies).

The build family (build / run / test / size / disasm / clean) is the **second family that grew
later** (ADR-0004): it composes images from the **same declaration surface** and drives the cross
toolchain — "requiring the compiler to be present" is its job, not an exception to the discipline.
Judgments and error codes always come from brickie-core; this front-end only parses args /
orchestrates / writes / executes / presents.
"""


class UsageError(Exception):
    """CLI usage error (invalid argument domain / wrong positional shape) — exit code 2."""


@dataclass
class Result:
    """The full result of one command run (sole source of presentation and exit code)."""

    command: str
    exit_code: int
    diagnostics: list[dict] = field(default_factory=list)
    data: dict = field(default_factory=dict)
    written: list[str] = field(default_factory=list)
    extra: dict = field(default_factory=dict)


# ---- parser ----


def _add_common(parser: argparse.ArgumentParser, *, with_defaults: bool) -> None:
    """``--json`` / ``--root`` on the root, groups and leaves (both spellings accepted).

    Subcommands need ``SUPPRESS``, else the leaf's ``False`` default swallows the global flag."""
    if with_defaults:
        parser.add_argument("--json", action="store_true", default=False,
                            help="machine-readable output (JSON to stdout; diagnostics in the same envelope)")
        parser.add_argument("--root", default=".", metavar="DIR",
                            help="repository root (default: current dir); plugin tree and state dir resolve against it")
    else:
        parser.add_argument("--json", action="store_true", default=argparse.SUPPRESS,
                            help="machine-readable output (JSON to stdout; diagnostics in the same envelope)")
        parser.add_argument("--root", default=argparse.SUPPRESS, metavar="DIR",
                            help="repository root (default: current dir); plugin tree and state dir resolve against it")


def _leaf(sub: argparse._SubParsersAction, name: str, help_: str, description: str | None = None):
    parser = sub.add_parser(name, help=help_, description=description or help_)
    _add_common(parser, with_defaults=False)
    return parser


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="brickie",
        description="brickie — brickOS composition-time tool (host side, zero build dependencies)",
        epilog=_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _add_common(parser, with_defaults=True)
    parser.add_argument("--version", action="store_true", help="print version and exit")
    parser.add_argument("--deps", dest="version_deps", action="store_true",
                        help="with --version: print language/dependency fingerprint (G-01)")

    sub = parser.add_subparsers(dest="command", metavar="<command>")

    new = _leaf(sub, "new", "generate a plugin skeleton (§8.4)",
                "Generate a plugin skeleton: brick.toml + impl/headers/tests/author notes + descriptor artifacts.")
    new.add_argument("plugin_type", metavar="<plugin_type>",
                     help="app | interface | ability | hal | platform | core")
    new.add_argument("name", metavar="<name>", help="plugin name, recommended <namespace>/<short> (§8.3)")
    new.add_argument("--api", default="native", metavar="<api_type>",
                     help="native (default) | runtime_adapter | third_party")
    new.add_argument("--lang", default="c", metavar="<lang>", help="c (v0.1 delivers c only)")
    new.add_argument("--subkind", default=None, metavar="<subkind>",
                     help="ability subkind; a bare name must be given explicitly")
    new.add_argument("--force", action="store_true", help="affects the generated-artifacts dir only (§8.4)")

    init = _leaf(sub, "init", "generate a product skeleton (§8.4)",
                 "Generate a product skeleton: product.toml + app/<name>/ + initial brickie.lock.")
    init.add_argument("product", metavar="<product>", help="product name")
    init.add_argument("--app", default=None, metavar="<app_name>",
                      help="app name (default = product name)")

    gen = _leaf(sub, "gen", "rebuild generated artifacts (--check compares only)",
                "Rebuild generated artifacts (descriptors + headers); --check compares byte-wise, writes nothing.")
    gen.add_argument("--check", action="store_true",
                     help="compare only, write nothing; mismatch with recomputation ⇒ exit code 1")

    build = _leaf(sub, "build", "build the image (declaration-surface driven; replaces make's composition part)",
                  "Build an aarch64 image: composition check → artifacts → compile/link → post-build gates.")
    build.add_argument("--profile", default=None, metavar="<profile>",
                       help="dev | release (default = product.toml's [product].stage)")
    build.add_argument("--jobs", "-j", type=int, default=0, metavar="N",
                       help="number of concurrent steps (0 = CPU count)")
    build.add_argument("--backend", default="direct", metavar="<backend>",
                       help="direct (default, brickie orchestrates) | make | ninja (BR-D4 A/B)")
    build.add_argument("--tag", default=None, metavar="<tag>",
                       help="artifact suffix (for backend-comparison gates: build/brick-<tag>.elf)")
    build.add_argument("--dry-run", action="store_true", help="print the plan only, do not execute")
    build.add_argument("--force", action="store_true", help="ignore incremental state, rebuild everything")
    build.add_argument("--emit-backends", action="store_true",
                       help="only emit build/gen/{build.mk,build.ninja}, do not execute")
    build.add_argument("--no-post", action="store_true",
                       help="skip the [build].post gates in tests/gates.toml")

    clean = _leaf(sub, "clean", "remove image artifacts (leaves build/host tools alone)")
    clean.add_argument("--all", action="store_true",
                       help="also clean build/gen and build/index")

    run = _leaf(sub, "run", "run the image on QEMU (Ctrl-A X to quit)")
    run.add_argument("--no-build", action="store_true", help="do not build first, run the existing image")
    run.add_argument("--timeout-s", type=int, default=0, metavar="N",
                     help="time-limited run (0 = unlimited, interactive)")

    size = _leaf(sub, "size", "section sizes (size -A -x)")
    size.add_argument("--no-build", action="store_true")

    disasm = _leaf(sub, "disasm", "disassemble the first N lines")
    disasm.add_argument("--head", type=int, default=120, metavar="N")
    disasm.add_argument("--no-build", action="store_true")

    test = _leaf(sub, "test", "run gates (host cases + QEMU log criteria + script gates)",
                 "Run the gates declared in tests/gates.toml; no name = run all.")
    test.add_argument("name", nargs="?", default=None, metavar="[name]")
    test.add_argument("--list", action="store_true", help="list all gate names")
    test.add_argument("--jobs", "-j", type=int, default=0, metavar="N")
    test.add_argument("--no-build", action="store_true", help="do not build first, run gates directly")

    dep = sub.add_parser("dep", help="dependency management and analysis (§7.7)",
                         description="Dependency management and analysis: snippet / tree / graph / path / index / closure.")
    _add_common(dep, with_defaults=False)
    dep_sub = dep.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    dep_add = _leaf(dep_sub, "add", "print a [[dep]] snippet (writes nothing — ruling R-2)")
    dep_add.add_argument("plugin", metavar="<plugin>", help="target plugin name")
    dep_add.add_argument("dep", metavar="<dep>", help="dependency plugin name, may carry an @<range> suffix")
    dep_add.add_argument("--kind", default=None, metavar="<kind>",
                         help="init | runtime | type (default decided by core)")
    dep_add.add_argument("--phase", default=None, metavar="<phase>",
                         help="early | core | late | app (phase assertion for init edges only)")

    dep_rm = _leaf(dep_sub, "rm", "print the block to delete (writes nothing — ruling R-2)")
    dep_rm.add_argument("plugin", metavar="<plugin>", help="target plugin name")
    dep_rm.add_argument("dep", metavar="<dep>", help="dependency plugin name")
    dep_rm.add_argument("--kind", default=None, metavar="<kind>", help="init | runtime | type")

    dep_tree = _leaf(dep_sub, "tree", "dependency tree (§7.7)")
    dep_tree.add_argument("--kind", default=None, metavar="<kind>", help="init | runtime | type | all")

    dep_graph = _leaf(dep_sub, "graph", "dependency graph export (§7.7)")
    dep_graph.add_argument("--format", default="dot", metavar="<format>",
                           help="dot (default) | mermaid | json")

    dep_why = _leaf(dep_sub, "why", "shortest dependency path explanation (§7.7)")
    dep_why.add_argument("a", metavar="<a>", help="source plugin")
    dep_why.add_argument("b", metavar="<b>", help="target plugin")

    _leaf(dep_sub, "index", "rebuild the reverse dependency index (§6.3)")

    _leaf(dep_sub, "closure", "product closure + topological order + budget totals (§7.6)")

    show = sub.add_parser("show", help="readable views: which bricks are in the image, and in what order they boot (ADR-0036)",
                          description="Presentation views over the composition: the product's closure grouped for reading, and the runtime init flow (the order the plugin manager actually walks).")
    _add_common(show, with_defaults=False)
    show_sub = show.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    _leaf(show_sub, "bricks", "the product's closure: bricks, origins, cost and budget (ADR-0036)")

    _leaf(show_sub, "brick-init-flow", "the runtime init order of every plugin (ADR-0036)")

    check = _leaf(sub, "check", "composition-time validation (§7.7)",
                  "Composition-time validation: dependency closure / interface / taxonomy / privilege and budget, incl. profile gates (§7.5).")
    check.add_argument("--deps", dest="scope_deps", action="store_true", help="check the dependency domain only")
    check.add_argument("--iface", dest="scope_iface", action="store_true", help="check the interface domain only")
    check.add_argument("--tax", dest="scope_tax", action="store_true", help="check the taxonomy domain only")
    check.add_argument("--priv", dest="scope_priv", action="store_true", help="check the privilege/resource domain only")
    check.add_argument("--src", dest="scope_src", action="store_true", help="check the source-text disciplines only (self-contained .c / globals stay inside the plugin)")
    check.add_argument("--all", dest="scope_all", action="store_true", help="all five domains (default)")
    check.add_argument("--profile", default="dev", metavar="<profile>", help="dev (default) | release")

    ver = sub.add_parser("ver", help="version management (§5 / §7.7)",
                         description="Version management: show the four-part version and COMPAT_GEN source / manual declaration advance.")
    _add_common(ver, with_defaults=False)
    ver_sub = ver.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    ver_show = _leaf(ver_sub, "show", "show the four-part version + COMPAT_GEN source (§7.7)")
    ver_show.add_argument("id", metavar="<id>", help="plugin name or <provider>#<unit>")

    ver_bump = _leaf(ver_sub, "bump", "manually declare MAJOR/MINOR/REVISE (§5.2)")
    ver_bump.add_argument("id", metavar="<id>",
                          help="plugin name (exactly one export) or <provider>#<unit>")
    ver_bump.add_argument("--rule", required=True, metavar="<rule>",
                          help="major | minor | revise (COMPAT_GEN is not among them)")

    iface = sub.add_parser("iface", help="interface publication and freeze state (§6.5)",
                           description="Interface units: inspect / change set / consistency / publish / freeze and unfreeze window.")
    _add_common(iface, with_defaults=False)
    iface_sub = iface.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    _leaf(iface_sub, "list", "list interface units and states (§6.5)")

    iface_show = _leaf(iface_sub, "show", "show the interface surface (entries + state + version + hash; §6.5)")
    iface_show.add_argument("id", metavar="<id>", help="<provider>#<unit>")

    iface_diff = _leaf(iface_sub, "diff", "old snapshot vs current declaration change set (§6.3)")
    iface_diff.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_diff.add_argument("--strict-params", action="store_true",
                            help="IFACE-IR rule 3: parameter names enter the hash too (default keeps types and order only)")

    iface_status = _leaf(iface_sub, "status", "independently recompute hash and compare with the snapshot (§6.5)")
    iface_status.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_status.add_argument("--check", action="store_true", help="compare only, write nothing")
    iface_status.add_argument("--strict-params", action="store_true",
                              help="use strict parameter names when recomputing (§6.2 rule 3)")

    iface_publish = _leaf(iface_sub, "publish", "publish the interface surface (§6.5 / §5.4)")
    iface_publish.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_publish.add_argument("--check", action="store_true", help="compare only, write nothing")
    iface_publish.add_argument("--note", default=None, metavar="<path>", help="decision-record path")
    iface_publish.add_argument("--set", default=None, metavar="<ver>",
                               help="override the unit version written this time (4 parts)")
    iface_publish.add_argument("--profile", default=None, metavar="<profile>",
                               help="dev | release (release runs the pre-publish gate hooks)")
    iface_publish.add_argument("--strict-params", action="store_true",
                               help="IFACE-IR rule 3: parameter names enter the hash too (stored in the snapshot)")

    for name, summary in (
        ("freeze", "freeze a commitment: emit the pending-promotion proposal (§6.4 A-26 exception)"),
        ("deprecate", "deprecate a unit or entry (§6.5)"),
        ("undeprecate", "cancel a deprecation (§6.5)"),
    ):
        leaf = _leaf(iface_sub, name, summary)
        leaf.add_argument("id_or_entry", metavar="<id>[#entry]",
                          help="<provider>#<unit> or <provider>#<unit>#<entry>")
        leaf.add_argument("--note", required=True, metavar="<path>", help="decision-record path (required)")

    iface_unfreeze = _leaf(iface_sub, "unfreeze", "enter the unfreeze window (§5.3.4)")
    iface_unfreeze.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_unfreeze.add_argument("--note", required=True, metavar="<path>", help="decision-record path (required)")

    iface_refreeze = _leaf(iface_sub, "refreeze", "exit the unfreeze window (§5.3.4)")
    iface_refreeze.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_refreeze.add_argument("--note", default=None, metavar="<path>", help="decision-record path (optional)")

    return parser


# ---- common orchestration ----


def _enum(value: str, allowed: tuple[str, ...], flag: str) -> str:
    """Flag value-domain check (the CLI's own flag domain, not a business judgment) — invalid ⇒ usage error 2."""
    if value not in allowed:
        raise UsageError(f"{flag} must be one of {' | '.join(allowed)} (got {value!r})")
    return value


def _split_entry(raw: str) -> tuple[str, str | None]:
    """``<provider>#<unit>[#<entry>]`` → ``(id, entry|None)``."""
    parts = raw.split("#")
    if len(parts) == 2 and all(parts):
        return f"{parts[0]}#{parts[1]}", None
    if len(parts) == 3 and all(parts):
        return f"{parts[0]}#{parts[1]}", parts[2]
    raise UsageError(f"`{raw}` is not of the form <provider>#<unit>[#<entry>]")


def _command_path(ns: argparse.Namespace) -> str:
    group = getattr(ns, "group_cmd", None)
    if ns.command in ("dep", "ver", "iface") and group:
        return f"{ns.command} {group}"
    return str(ns.command)


def _root_of(ns: argparse.Namespace) -> Path:
    root = Path(str(ns.root)).expanduser()
    if not root.is_dir():
        raise UsageError(f"--root points to a path that does not exist or is not a directory: {root}")
    return root.resolve()


def _call_core(command: str, root: Path, args: dict, context: dict | None = None) -> dict:
    request = native.make_request(
        command, root, args, context, templates=native.templates_root()
    )
    return native.call_core(request)


def _shape_gate(root: Path, kinds: tuple[str, ...]) -> list[dict]:
    """Optional single-record shape check (BRV-MF-0001)."""
    if not kinds:
        return []
    if not schema.any_schema_available():
        return [schema.missing_schema_diagnostic()]
    diagnostics: list[dict] = []
    for rel, kind in writer.iter_declaration_files(root, kinds):
        diagnostics.extend(schema.check_declaration(kind, rel, writer.read_text(root / rel)))
    return diagnostics


def _has_error(diagnostics: list[dict]) -> bool:
    return any(d.get("severity") == "error" for d in diagnostics)


def _combine(*codes: int) -> int:
    """Aggregate exit codes: 2 (usage/environment) > 1 (validation red) > 0."""
    return max(codes) if codes else _EXIT_OK


def _plan_artifacts(response: dict) -> list[dict]:
    data = response.get("data") or {}
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list):
        return []
    return [a for a in artifacts if isinstance(a, dict) and isinstance(a.get("path"), str)]


def _kinds_of(artifacts: list[dict]) -> dict[str, str]:
    """Index the plan's ``kind`` (human/generated) by path — the write layer uses it to protect human-written files."""
    return {
        a["path"]: str(a.get("kind") or "rendered")
        for a in artifacts
        if isinstance(a.get("path"), str)
    }


def _render(root: Path, artifacts: list[dict]) -> tuple[list[dict], list[dict], int]:
    if not artifacts:
        return [], [], _EXIT_OK
    response = native.render(artifacts, root=root)
    return response["files"], response["diagnostics"], response["exit_code"]


def _human_skip_diagnostics(skipped: list[str]) -> list[dict]:
    return [
        {
            "code": None,
            "severity": "warning",
            "target": "",
            "file": rel,
            "span": "",
            "message": "existing human-written file not overwritten (contract §4: never rewrite human-written files)",
            "hint": "edit this file by hand if you need changes",
        }
        for rel in skipped
    ]


def _check_mismatch_diagnostic(path: str, why: str) -> dict:
    return {
        "code": None,
        "severity": "error",
        "target": "",
        "file": path,
        "span": "",
        "message": f"--check mismatch: {path} {why}",
        "hint": "rerun `brickie gen` to write to disk; --check writes nothing",
    }


def _orphan_pruned_diagnostics(pruned: list[str]) -> list[dict]:
    """Info diagnostics tracing pruned stale artifacts (a fix, not a defect).

    Reported, not silent: ``[build].gen_sources`` globs would otherwise compile the leftovers."""
    return [
        {
            "code": None,
            "severity": "info",
            "target": "",
            "file": rel,
            "span": "",
            "message": "pruned stale artifact (belongs to no plugin in the plugin tree): otherwise it would be compiled into the image as a ghost plugin",
            "hint": "if this is a plugin you still want, restore its brick.toml and rerun `brickie gen`",
        }
        for rel in pruned
    ]


#: Machine-file bodies: core hands the body over in ``data.<key>`` (contract §5.2 gives the plan
#: only, ``template`` empty); L5 writes it to the planned path and never assembles content (§8).
_CORE_CONTENT_KEYS = {"brickie.lock": "lock_content"}


def _collect_files(
    root: Path, data: dict, artifacts: list[dict]
) -> tuple[list[dict], list[dict], int, dict[str, str]]:
    """Turn core's plan into ``files`` ready to be written.

    Templated artifacts go to ``brickie-gen render``; template-less machine files take the body
    from core (``artifact.content`` / ``data.<_CORE_CONTENT_KEYS>``), else one info diagnostic."""
    kinds = _kinds_of(artifacts)
    files: list[dict] = []
    diagnostics: list[dict] = []
    exit_code = _EXIT_OK

    templated = [a for a in artifacts if str(a.get("template") or "").strip()]
    if templated:
        rendered, gen_diagnostics, gen_exit = _render(root, templated)
        files += rendered
        diagnostics += gen_diagnostics
        exit_code = _combine(exit_code, gen_exit)

    for artifact in artifacts:
        if str(artifact.get("template") or "").strip():
            continue
        rel = artifact["path"]
        content = artifact.get("content")
        if not isinstance(content, str):
            content = data.get(_CORE_CONTENT_KEYS.get(rel, ""))
        if isinstance(content, str):
            entry = {"path": rel, "kind": artifact.get("kind") or "machine", "content": content}
            if isinstance(artifact.get("mode"), str):
                entry["mode"] = artifact["mode"]
            files.append(entry)
        else:
            diagnostics.append({
                "code": None,
                "severity": "info",
                "target": "",
                "file": rel,
                "span": "",
                "message": "this artifact in core's plan has neither a template nor a directly passed body; skipped writing",
                "hint": "machine-file bodies must come from core (contract §4); see the deviation table in rust/README",
            })
    return files, diagnostics, exit_code, kinds


def _readonly(
    ns: argparse.Namespace,
    command: str,
    core_command: str,
    args: dict,
    *,
    shape: tuple[str, ...] = (),
    extra: dict | None = None,
) -> Result:
    """Read-only command: shape check → core → present (**writes nothing**, even if core returned files)."""
    root = _root_of(ns)
    gate = _shape_gate(root, shape)
    extra = dict(extra or {})
    if _has_error(gate):
        return Result(command, _EXIT_USAGE, gate, {}, [], extra)
    response = _call_core(core_command, root, args)
    return Result(
        command,
        response["exit_code"],
        gate + response["diagnostics"],
        response["data"],
        [],
        extra,
    )


def _writing(
    ns: argparse.Namespace,
    command: str,
    core_command: str,
    args: dict,
    *,
    shape: tuple[str, ...] = (),
    write: bool = True,
    extra: dict | None = None,
) -> Result:
    """Writing command: shape check → core → write (idempotent; with ``write=False`` only present)."""
    root = _root_of(ns)
    gate = _shape_gate(root, shape)
    extra = dict(extra or {})
    if _has_error(gate):
        return Result(command, _EXIT_USAGE, gate, {}, [], extra)
    response = _call_core(core_command, root, args)
    diagnostics = gate + response["diagnostics"]
    exit_code = response["exit_code"]
    written: list[str] = []
    if exit_code == _EXIT_OK and write and response["files"]:
        outcome = writer.write_files(root, response["files"])
        written = outcome.written
        diagnostics += _human_skip_diagnostics(outcome.skipped_human)
        extra["unchanged"] = outcome.unchanged
        extra["skipped"] = outcome.skipped_human
    extra["written"] = written
    return Result(command, exit_code, diagnostics, response["data"], written, extra)


def _finish_plan(command: str, root: Path, response: dict, extra: dict) -> Result:
    exit_code = response["exit_code"]
    data = response["data"]
    diagnostics = list(response["diagnostics"])
    artifacts = _plan_artifacts(response)
    written: list[str] = []
    if exit_code == _EXIT_OK and artifacts:
        files, gen_diagnostics, gen_exit, kinds = _collect_files(root, data, artifacts)
        diagnostics += gen_diagnostics
        exit_code = _combine(exit_code, gen_exit)
        if files and exit_code == _EXIT_OK:
            outcome = writer.write_files(root, files, kinds=kinds)
            written = outcome.written
            diagnostics += _human_skip_diagnostics(outcome.skipped_human)
            extra.update(unchanged=outcome.unchanged, skipped=outcome.skipped_human)
    extra["written"] = written
    return Result(command, exit_code, diagnostics, data, written, extra)


# ---- command implementations ----


def _cmd_new(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    args = {
        "plugin_type": ns.plugin_type,
        "name": ns.name,
        "api_type": ns.api,
        "lang": ns.lang,
        "subkind": ns.subkind,
        "force": bool(ns.force),
    }
    extra = {"args": args}

    # Two-phase: probe which paths would be written (rules live in core), scan existing, re-request.
    probe = _call_core("plan-new", root, args)
    artifacts = _plan_artifacts(probe)
    paths = [a["path"] for a in artifacts]
    if probe["exit_code"] != _EXIT_OK or not paths:
        return Result("new", probe["exit_code"], probe["diagnostics"], probe["data"], [], extra)

    existing = writer.scan_existing(root, paths)
    response = _call_core("plan-new", root, args, {"existing": existing})
    return _finish_plan("new", root, response, extra)


def _cmd_init(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    args: dict = {"name": ns.product}
    if ns.app:
        args["app"] = ns.app
    extra = {"args": args}

    probe = _call_core("plan-init", root, args)
    artifacts = _plan_artifacts(probe)
    paths = [a["path"] for a in artifacts]
    if probe["exit_code"] != _EXIT_OK or not paths:
        return Result("init", probe["exit_code"], probe["diagnostics"], probe["data"], [], extra)

    existing = writer.scan_existing(root, paths)
    response = _call_core("plan-init", root, args, {"existing": existing})
    return _finish_plan("init", root, response, extra)


def _cmd_gen(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    check = bool(ns.check)
    gate = _shape_gate(root, ("plugin", "product"))
    extra = {"check": check}
    if _has_error(gate):
        return Result("gen", _EXIT_USAGE, gate, {}, [], extra)

    response = _call_core("gen-plan", root, {"check": check})
    diagnostics = gate + response["diagnostics"]
    exit_code = response["exit_code"]
    artifacts = _plan_artifacts(response)
    planned = {a["path"] for a in artifacts}
    written: list[str] = []

    if exit_code == _EXIT_OK and artifacts:
        files, gen_diagnostics, gen_exit, kinds = _collect_files(root, response["data"], artifacts)
        diagnostics += gen_diagnostics
        exit_code = _combine(exit_code, gen_exit)
        if files and exit_code == _EXIT_OK:
            outcome = writer.write_files(root, files, kinds=kinds, check=check)
            if check:
                extra["stale"] = outcome.stale
                extra["missing"] = outcome.missing
                for path in outcome.stale:
                    diagnostics.append(_check_mismatch_diagnostic(path, "content differs from recomputation"))
                for path in outcome.missing:
                    diagnostics.append(_check_mismatch_diagnostic(path, "does not exist"))
                if outcome.stale or outcome.missing:
                    exit_code = _combine(exit_code, _EXIT_RED)
            else:
                written = outcome.written
                diagnostics += _human_skip_diagnostics(outcome.skipped_human)
            extra["unchanged"] = outcome.unchanged
            extra["skipped"] = outcome.skipped_human

    # Stale descriptors (old plugin rename/delete) must be pruned too: leftovers under
    # `build/gen/` would be compiled into the image as ghost plugins.
    if exit_code == _EXIT_OK:
        if check:
            orphans = writer.prune_orphan_descriptors(root, planned, check=True)
            extra["orphan"] = orphans
            for path in orphans:
                diagnostics.append(
                    _check_mismatch_diagnostic(path, "belongs to no plugin in the plugin tree (stale artifact)")
                )
            if orphans:
                exit_code = _combine(exit_code, _EXIT_RED)
        else:
            pruned = writer.prune_orphan_descriptors(root, planned)
            extra["pruned"] = pruned
            diagnostics += _orphan_pruned_diagnostics(pruned)

    extra["written"] = written
    return Result("gen", exit_code, diagnostics, response["data"], written, extra)



# ---- build family ----


def _out(ns: argparse.Namespace):
    """Where human-readable progress goes: always stderr under ``--json`` (else stdout is not valid JSON)."""
    return sys.stderr if getattr(ns, "json", False) else sys.stdout


def _note(message: str, hint: str = "", *, severity: str = "error",
          code: str | None = None) -> dict:
    """L5-side orchestration-level diagnostic (not a business judgment): step failure / environment error."""
    return {
        "code": code, "severity": severity, "target": "", "file": "", "span": "",
        "message": message, "hint": hint,
    }


def _judge_callback(root: Path):
    """Wrap core's ``judge`` command as the callback the executor wants (criteria still live in core)."""
    def judge(name: str, text: str) -> dict:
        response = native.call_core(
            native.make_request("judge", root, {"name": name}, {"log": text})
        )
        return {
            "failed": (response.get("data") or {}).get("failed", 0),
            "diagnostics": response.get("diagnostics") or [],
            "exit_code": response.get("exit_code", 0),
        }
    return judge


def _compose(root: Path, profile: str | None) -> tuple[list[dict], int, dict]:
    """Three composition phases: ``check`` → ``gen-plan`` → render and write artifacts.

    First half of ``brickie build``. A non-zero exit code ⇒ the caller must stop: an image
    compiled from an inconsistent declaration surface is meaningless."""
    diagnostics: list[dict] = []
    check_args: dict = {}
    if profile:
        check_args["profile"] = profile
    check = _call_core("check", root, check_args)
    diagnostics += check["diagnostics"]
    if check["exit_code"] != _EXIT_OK:
        return diagnostics, check["exit_code"], check["data"]

    plan = _call_core("gen-plan", root, {})
    diagnostics += plan["diagnostics"]
    if plan["exit_code"] != _EXIT_OK:
        return diagnostics, plan["exit_code"], plan["data"]
    artifacts = _plan_artifacts(plan)
    if artifacts:
        files, gen_diagnostics, gen_exit, kinds = _collect_files(root, plan["data"], artifacts)
        diagnostics += gen_diagnostics
        if gen_exit != _EXIT_OK:
            return diagnostics, gen_exit, plan["data"]
        outcome = writer.write_files(root, files, kinds=kinds)
        diagnostics += _human_skip_diagnostics(outcome.skipped_human)
    # Prune stale artifacts here too: `build/gen/**` glob would compile leftovers as ghost plugins.
    planned = {a["path"] for a in artifacts}
    diagnostics += _orphan_pruned_diagnostics(writer.prune_orphan_descriptors(root, planned))
    return diagnostics, _EXIT_OK, check["data"]


def _persist(root: Path, files: list[dict]) -> tuple[list[str], list[dict]]:
    """Write the machine files core handed over (backend files / incremental state), idempotently."""
    if not files:
        return [], []
    kinds = {f["path"]: str(f.get("kind") or "machine") for f in files if isinstance(f.get("path"), str)}
    outcome = writer.write_files(root, files, kinds=kinds)
    return outcome.written, _human_skip_diagnostics(outcome.skipped_human)


def _post_gate_names(root: Path) -> list[str]:
    """``tests/gates.toml [build].post`` — gate names run automatically after a successful build."""
    response = _call_core("test", root, {"list": True})
    return list((response.get("data") or {}).get("build_post") or [])


def _cmd_build(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    gate = _shape_gate(root, ("plugin", "product"))
    if _has_error(gate):
        return Result("build", _EXIT_USAGE, gate, {}, [], {"args": {"build": True}})

    profile = _enum(ns.profile, ("dev", "release"), "--profile") if ns.profile else None
    diagnostics, exit_code, check_data = _compose(root, profile)
    if exit_code != _EXIT_OK:
        return Result("build", exit_code, gate + diagnostics, check_data, [])

    args: dict = {"backend": ns.backend, "jobs": int(ns.jobs or 0)}
    if profile:
        args["profile"] = profile
    if ns.tag:
        args["tag"] = ns.tag
    if ns.dry_run:
        args["dry_run"] = True
    if ns.force:
        args["force"] = True
    if ns.emit_backends:
        args["emit_backends"] = True

    response = _call_core("build", root, args)
    diagnostics += response["diagnostics"]
    data = response["data"]
    if response["exit_code"] != _EXIT_OK:
        return Result("build", response["exit_code"], gate + diagnostics, data, [])

    written, write_diagnostics = _persist(root, response["files"])
    diagnostics += write_diagnostics

    steps = list(data.get("steps") or [])
    if ns.dry_run or ns.emit_backends:
        results, ok = runner.run_steps(root, steps, jobs=ns.jobs, dry_run=True, out=_out(ns))
    else:
        results, ok = runner.run_steps(
            root, steps, jobs=ns.jobs, judge=_judge_callback(root), out=_out(ns)
        )

    post: list[str] = []
    # `--tag` builds are diagnostic builds (backend comparison / duplicate artifacts): the gate
    # argv names the default output path, which does not hold for tagged builds.
    if ok and not ns.dry_run and not ns.emit_backends and not ns.no_post and not ns.tag:
        post = _post_gate_names(root)
        for name in post:
            gate_result = _run_test(root, name, jobs=ns.jobs, diagnostics=diagnostics, out=_out(ns))
            diagnostics = gate_result["diagnostics"]
            if gate_result["exit_code"] != _EXIT_OK:
                ok = False
                break

    exit_code = _EXIT_OK if ok else _EXIT_RED
    extra = {
        "args": {"build": True, "backend": ns.backend, "profile": profile or data.get("profile")},
        "results": [_result_summary(r) for r in results],
        "post": post,
        "written": written,
    }
    if not ok:
        diagnostics.append(_note(
            "build failed: some steps did not pass (see step output above)",
            "look at the output of the first FAIL step; exit 1 = validation/build red, 2 = usage or environment error",
        ))
    return Result("build", exit_code, gate + diagnostics, data, written, extra)


def _result_summary(result) -> dict:
    return {
        "label": result.label,
        "kind": result.kind,
        "ok": result.ok,
        "rc": result.rc,
        "timed_out": result.timed_out,
        "judged": result.judged,
        "judge_failed": result.judge_failed,
        "log": result.log,
    }


def _run_test(root: Path, name: str | None, *, jobs: int, diagnostics: list[dict],
              out=None) -> dict:
    """Run a group (or a single) of gates; returns ``{diagnostics, exit_code, data, results}``."""
    args: dict = {"jobs": int(jobs or 0)}
    if name:
        args["name"] = name
    response = _call_core("test", root, args)
    diagnostics = diagnostics + response["diagnostics"]
    data = response["data"]
    if response["exit_code"] != _EXIT_OK:
        return {"diagnostics": diagnostics, "exit_code": response["exit_code"], "data": data, "results": []}
    steps = list(data.get("steps") or [])
    if not steps:
        return {"diagnostics": diagnostics, "exit_code": _EXIT_OK, "data": data, "results": []}
    results, ok = runner.run_steps(root, steps, jobs=jobs, judge=_judge_callback(root), out=out)
    for step, result in zip(steps, results):
        diagnostics += result.diagnostics
        if not result.ok and not result.judged:
            diagnostics.append(_note(
                f"gate step failed: {result.label}",
                f"exit code {result.rc}" + ("(timed out)" if result.timed_out else ""),
                code=step.get("fail_code"),
            ))
    return {
        "diagnostics": diagnostics,
        "exit_code": _EXIT_OK if ok else _EXIT_RED,
        "data": data,
        "results": results,
    }


def _cmd_test(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    if ns.list:
        response = _call_core("test", root, {"list": True})
        return Result("test", response["exit_code"], response["diagnostics"], response["data"], [],
                      {"list": True})

    diagnostics: list[dict] = []
    gate = _shape_gate(root, ("plugin", "product"))
    if _has_error(gate):
        return Result("test", _EXIT_USAGE, gate, {}, [])

    if not ns.no_build:
        compose_diagnostics, exit_code, _ = _compose(root, None)
        diagnostics += compose_diagnostics
        if exit_code != _EXIT_OK:
            return Result("test", exit_code, diagnostics, {}, [])
        build = _call_core("build", root, {"jobs": int(ns.jobs or 0)})
        diagnostics += build["diagnostics"]
        if build["exit_code"] != _EXIT_OK:
            return Result("test", build["exit_code"], diagnostics, build["data"], [])
        written, write_diagnostics = _persist(root, build["files"])
        diagnostics += write_diagnostics
        _, built = runner.run_steps(
            root, list(build["data"].get("steps") or []), jobs=ns.jobs,
            judge=_judge_callback(root), out=_out(ns),
        )
        if not built:
            diagnostics.append(_note("the build preceding the gates did not pass", "fix `brickie build` first"))
            return Result("test", _EXIT_RED, diagnostics, build["data"], [])

    outcome = _run_test(root, ns.name, jobs=ns.jobs, diagnostics=diagnostics, out=_out(ns))
    extra = {"results": [_result_summary(r) for r in outcome["results"]]}
    return Result("test", outcome["exit_code"], outcome["diagnostics"], outcome["data"], [], extra)


def _cmd_run(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    if not ns.no_build:
        built = _build_for_tools(ns)
        if built.exit_code != _EXIT_OK:
            built.command = "run"
            return built
    args: dict = {}
    if ns.timeout_s:
        args["timeout_s"] = int(ns.timeout_s)
    response = _call_core("run", root, args)
    data = response["data"]
    steps = list(data.get("steps") or [])
    if steps and not ns.json:
        print(f"QEMU: {' '.join(steps[0]['argv'])}")
        print("      (Ctrl-A X to quit)")
    results, ok = runner.run_steps(root, steps, jobs=1, out=_out(ns))
    return Result(
        "run", _EXIT_OK if ok else _EXIT_RED, response["diagnostics"], data, [],
        {"results": [_result_summary(r) for r in results]},
    )


def _cmd_size(ns: argparse.Namespace) -> Result:
    return _tool_command(ns, "size")


def _cmd_disasm(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    if not ns.no_build:
        built = _build_for_tools(ns)
        if built.exit_code != _EXIT_OK:
            built.command = "disasm"
            return built
    response = _call_core("disasm", root, {"head": int(ns.head or 120)})
    steps = list((response.get("data") or {}).get("steps") or [])
    results, ok = runner.run_steps(root, steps, jobs=1, out=_out(ns))
    return Result(
        "disasm", _EXIT_OK if ok else _EXIT_RED, response["diagnostics"], response["data"], [],
        {"results": [_result_summary(r) for r in results]},
    )


def _tool_command(ns: argparse.Namespace, which: str) -> Result:
    root = _root_of(ns)
    if not ns.no_build:
        built = _build_for_tools(ns)
        if built.exit_code != _EXIT_OK:
            built.command = which
            return built
    response = _call_core(which, root, {})
    steps = list((response.get("data") or {}).get("steps") or [])
    results, ok = runner.run_steps(root, steps, jobs=1, out=_out(ns))
    return Result(
        which, _EXIT_OK if ok else _EXIT_RED, response["diagnostics"], response["data"], [],
        {"results": [_result_summary(r) for r in results]},
    )


def _build_for_tools(ns: argparse.Namespace) -> Result:
    """The "build first" path of ``run`` / ``size`` / ``disasm`` reuses the ``build`` pipeline."""
    root = _root_of(ns)
    diagnostics, exit_code, _ = _compose(root, None)
    if exit_code != _EXIT_OK:
        return Result("build", exit_code, diagnostics, {}, [])
    response = _call_core("build", root, {"jobs": int(getattr(ns, "jobs", 0) or 0)})
    diagnostics += response["diagnostics"]
    if response["exit_code"] != _EXIT_OK:
        return Result("build", response["exit_code"], diagnostics, response["data"], [])
    written, write_diagnostics = _persist(root, response["files"])
    diagnostics += write_diagnostics
    results, ok = runner.run_steps(
        root, list((response.get("data") or {}).get("steps") or []),
        jobs=int(getattr(ns, "jobs", 0) or 0), judge=_judge_callback(root), out=_out(ns),
    )
    if not ok:
        diagnostics.append(_note("build did not pass: run/size/disasm is blocked", "fix `brickie build` first"))
    return Result(
        "build", _EXIT_OK if ok else _EXIT_RED, diagnostics, response["data"], written,
        {"args": {"build": True, "quiet": True}},
    )


def _cmd_clean(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    response = _call_core("clean", root, {"all": bool(ns.all)})
    diagnostics = list(response["diagnostics"])
    if response["exit_code"] != _EXIT_OK:
        return Result("clean", response["exit_code"], diagnostics, response["data"], [])

    removed: list[str] = []
    for rel in (response.get("data") or {}).get("paths") or []:
        target = (root / str(rel)).resolve()
        root_resolved = root.resolve()
        # Guard: delete only paths inside root, never root itself (declaration-surface input).
        if target == root_resolved or root_resolved not in target.parents:
            diagnostics.append(_note(
                f"refusing to delete a path outside root: {rel}",
                "`clean` only removes derived paths given by the declaration surface that fall inside the repo",
            ))
            continue
        if target.is_dir():
            shutil.rmtree(target, ignore_errors=True)
        elif target.exists():
            target.unlink()
        else:
            continue
        removed.append(str(rel))
    return Result("clean", _EXIT_OK, diagnostics, response["data"], removed,
                  {"removed": removed})


def _cmd_check(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    profile = _enum(ns.profile, ("dev", "release"), "--profile")
    scopes = [s for s in ("deps", "iface", "tax", "priv", "src") if getattr(ns, f"scope_{s}")]
    if ns.scope_all or not scopes:
        scopes = ["deps", "iface", "tax", "priv", "src"]

    extra = {"profile": profile}
    gate = _shape_gate(root, ("plugin", "product"))
    if _has_error(gate):
        return Result("check", _EXIT_USAGE, gate, {}, [], extra)

    response = _call_core("check", root, {"profile": profile, "scopes": scopes})
    return Result(
        "check",
        response["exit_code"],
        gate + response["diagnostics"],
        response["data"],
        [],
        extra,
    )


def _cmd_dep(ns: argparse.Namespace, group: str) -> Result:
    if group == "add":
        dep, _, rng = str(ns.dep).partition("@")
        args: dict = {"plugin": ns.plugin, "dep": dep}
        if rng:
            args["range"] = rng
        if ns.kind:
            args["kind"] = _enum(ns.kind, ("init", "runtime", "type"), "--kind")
        if ns.phase:
            args["phase"] = _enum(ns.phase, ("early", "core", "late", "app"), "--phase")
        return _readonly(ns, "dep add", "dep-add", args, shape=("plugin", "product"))
    if group == "rm":
        args = {"plugin": ns.plugin, "dep": ns.dep}
        if ns.kind:
            args["kind"] = _enum(ns.kind, ("init", "runtime", "type"), "--kind")
        return _readonly(ns, "dep rm", "dep-rm", args, shape=("plugin", "product"))
    if group == "tree":
        args = {}
        if ns.kind:
            args["kind"] = _enum(ns.kind, ("init", "runtime", "type", "all"), "--kind")
        return _readonly(ns, "dep tree", "dep-tree", args, shape=("plugin", "product"))
    if group == "graph":
        args = {"format": _enum(ns.format, ("dot", "mermaid", "json"), "--format")}
        return _readonly(ns, "dep graph", "dep-graph", args, shape=("plugin", "product"))
    if group == "why":
        return _readonly(
            ns, "dep why", "dep-why", {"from": ns.a, "to": ns.b},
            shape=("plugin", "product"), extra={"from": ns.a, "to": ns.b},
        )
    if group == "index":
        return _writing(ns, "dep index", "dep-index", {}, shape=("plugin", "product"))
    if group == "closure":
        return _readonly(ns, "dep closure", "closure", {}, shape=("plugin", "product"))
    raise UsageError(f"unimplemented command `dep {group}`")


def _cmd_show(ns: argparse.Namespace, group: str) -> Result:
    """``show bricks`` / ``show brick-init-flow``: read-only views (ADR-0036)."""
    if group == "bricks":
        return _readonly(ns, "show bricks", "show-bricks", {}, shape=("plugin", "product"))
    if group == "brick-init-flow":
        return _readonly(ns, "show brick-init-flow", "show-init-flow", {},
                         shape=("plugin", "product"))
    raise UsageError(f"unimplemented command `show {group}`")


def _cmd_ver(ns: argparse.Namespace, group: str) -> Result:
    if group == "show":
        return _readonly(ns, "ver show", "ver-show", {"id": ns.id},
                         shape=("plugin",), extra={"id": ns.id})
    if group == "bump":
        rule = _enum(ns.rule, ("major", "minor", "revise"), "--rule")
        return _writing(ns, "ver bump", "ver-bump", {"id": ns.id, "rule": rule},
                        shape=("plugin",), extra={"id": ns.id})
    raise UsageError(f"unimplemented command `ver {group}`")


def _cmd_iface(ns: argparse.Namespace, group: str) -> Result:
    if group == "list":
        return _readonly(ns, "iface list", "iface-list", {}, shape=("plugin",))
    if group == "show":
        return _readonly(ns, "iface show", "iface-show", {"id": ns.id},
                         shape=("plugin",), extra={"id": ns.id})
    if group == "diff":
        args = {"id": ns.id}
        if ns.strict_params:
            args["strict_params"] = True
        return _readonly(ns, "iface diff", "iface-diff", args,
                         shape=("plugin",), extra={"id": ns.id})
    if group == "status":
        args = {"id": ns.id, "check": bool(ns.check)}
        if ns.strict_params:
            args["strict_params"] = True
        return _writing(ns, "iface status", "iface-status", args, shape=("plugin",),
                        write=not ns.check, extra={"id": ns.id})
    if group == "publish":
        args = {"id": ns.id, "check": bool(ns.check)}
        if ns.note:
            args["note"] = ns.note
        if ns.set:
            args["set"] = ns.set
        if ns.profile:
            args["profile"] = _enum(ns.profile, ("dev", "release"), "--profile")
        if ns.strict_params:
            args["strict_params"] = True
        return _writing(ns, "iface publish", "iface-publish", args, shape=("plugin",),
                        write=not ns.check, extra={"id": ns.id})
    if group in ("freeze", "deprecate", "undeprecate"):
        unit_id, entry = _split_entry(ns.id_or_entry)
        args = {"id": unit_id, "note": ns.note}
        if entry:
            args["entry"] = entry
        return _writing(ns, f"iface {group}", f"iface-{group}", args, shape=("plugin",),
                        extra={"id": unit_id, "entry": entry})
    if group == "unfreeze":
        args = {"id": ns.id, "note": ns.note}
        return _writing(ns, "iface unfreeze", "iface-unfreeze", args, shape=("plugin",),
                        extra={"id": ns.id})
    if group == "refreeze":
        args = {"id": ns.id}
        if ns.note:
            args["note"] = ns.note
        return _writing(ns, "iface refreeze", "iface-refreeze", args, shape=("plugin",),
                        extra={"id": ns.id})
    raise UsageError(f"unimplemented command `iface {group}`")


def _dispatch(ns: argparse.Namespace) -> Result:
    if ns.command == "new":
        return _cmd_new(ns)
    if ns.command == "init":
        return _cmd_init(ns)
    if ns.command == "gen":
        return _cmd_gen(ns)
    if ns.command == "check":
        return _cmd_check(ns)
    if ns.command == "build":
        return _cmd_build(ns)
    if ns.command == "clean":
        return _cmd_clean(ns)
    if ns.command == "run":
        return _cmd_run(ns)
    if ns.command == "size":
        return _cmd_size(ns)
    if ns.command == "disasm":
        return _cmd_disasm(ns)
    if ns.command == "test":
        return _cmd_test(ns)
    group = getattr(ns, "group_cmd", None)
    if ns.command == "dep":
        return _cmd_dep(ns, group)
    if ns.command == "show":
        return _cmd_show(ns, group)
    if ns.command == "ver":
        return _cmd_ver(ns, group)
    if ns.command == "iface":
        return _cmd_iface(ns, group)
    raise UsageError(f"unimplemented command `{ns.command}`")


def _cmd_version(ns: argparse.Namespace) -> int:
    """``--version [--deps]`` (G-01): version/dependency fingerprint come from core's ``version``.

    Without core, degrade to L5's own version so ``--version`` never stalls make/CI self-checks;
    ``--deps`` without a fingerprint is an environment error (exit 2)."""
    root = Path(".").resolve()
    try:
        response = _call_core("version", root, {})
    except NativeError as exc:
        data = {"version": __version__, "protocol": PROTOCOL}
        diagnostic = {
            "code": exc.code, "severity": "info", "target": "", "file": "", "span": "",
            "message": f"degraded to the L5 front-end version: {exc}",
            "hint": "once brickie-core is built, `--version` carries the language/dependency fingerprint",
        }
        fallback_exit = _EXIT_OK if not ns.version_deps else _EXIT_USAGE
        if ns.json:
            print(present.emit_json(present.json_payload(
                command="version", exit_code=fallback_exit,
                diagnostics=[diagnostic], data=data)))
        else:
            print(f"brickie {__version__}")
            if ns.version_deps:
                print(present.emit_diagnostics_text([diagnostic]), file=sys.stderr)
        return fallback_exit

    data = response["data"]
    exit_code = response["exit_code"]
    if ns.json:
        print(present.emit_json(present.json_payload(
            command="version", exit_code=exit_code,
            diagnostics=response["diagnostics"], data=data)))
        return exit_code
    if not ns.version_deps:
        print(f"brickie {data.get('version', __version__)}")
        return exit_code
    print(present.render_text("version", data, {"deps_requested": True}))
    return exit_code


# ---- presentation/entry ----


def _error_diagnostic(message: str, code: str | None = None, severity: str = "error") -> dict:
    return {
        "code": code,
        "severity": severity,
        "target": "",
        "file": "",
        "span": "",
        "message": message,
        "hint": "",
    }


def _present(ns: argparse.Namespace, result: Result) -> None:
    result.diagnostics = present.sort_diagnostics(result.diagnostics)
    if ns.json:
        print(present.emit_json(present.json_payload(
            command=result.command,
            exit_code=result.exit_code,
            diagnostics=result.diagnostics,
            data=result.data,
            written=result.written,
        )))
        return
    text = present.emit_diagnostics_text(result.diagnostics)
    if text:
        print(text, file=sys.stderr)
    if result.data:
        result.extra["ok"] = result.exit_code == _EXIT_OK
        body = present.render_text(result.command, result.data, result.extra)
        if body:
            print(body)


def _report_failure(ns: argparse.Namespace, command: str, diagnostic: dict) -> int:
    """Single exit point for usage/environment errors (NativeError / WriteError): exit code 2."""
    if ns.json:
        print(present.emit_json(present.json_payload(
            command=command, exit_code=_EXIT_USAGE,
            diagnostics=[diagnostic], data={})))
    else:
        print(f"brickie: error: {diagnostic['message']}", file=sys.stderr)
    return _EXIT_USAGE


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    ns = parser.parse_args(argv)
    ns.json = bool(getattr(ns, "json", False))
    ns.root = getattr(ns, "root", ".") or "."

    if not getattr(ns, "version", False) and getattr(ns, "version_deps", False):
        parser.error("--deps can only be used together with --version")
    if getattr(ns, "version", False):
        return _cmd_version(ns)

    if not ns.command:
        parser.print_help()
        return _EXIT_USAGE

    try:
        result = _dispatch(ns)
    except UsageError as exc:
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc)))
    except runner.StepError as exc:
        # Step could not start (missing/too-old tool): report the message, not a traceback.
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc), exc.code))
    except NativeError as exc:
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc), exc.code))
    except writer.WriteError as exc:
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc)))
    except BrokenPipeError:  # `brickie ... | head`
        return _EXIT_OK

    _present(ns, result)
    return result.exit_code

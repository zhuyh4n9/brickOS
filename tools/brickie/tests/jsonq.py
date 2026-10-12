#!/usr/bin/env python3
"""**Structured** query / snapshot tool for `--json` output (the assertion backend for cases).

Why not textual grep / whole-text diff:
  contract §3 fixes a stable ordering of the diagnostics array by ``(code, file, span, target)``; §2
  fixes the envelope fields. Acceptance must lock **those structures** (key sets / diagnostic order /
  protocol version / exit codes), not text indented a few spaces on one machine — exactly four things:

  1. fetch values by key path (``get`` / ``count`` / ``has`` / ``keys``);
  2. compress the diagnostics array into the canonical tuple sequence (``diags``) of
     ``code|severity|file|span|target``, for "is the order stable" / "is the set unchanged after a stub swap";
  3. assert that the diagnostics array really is ordered by the contract keys (``sorted``);
  4. snapshots: project the whole response into stable JSON with **prose (message/hint) and dynamic
     write-back bodies removed**, compared byte-for-byte with ``tests/snap/<name>.json``; ``--emit`` regenerates.

Usage (all express assertion success or failure via exit code 0/1, so run.sh can check directly):
  jsonq.py envelope   <resp.json>
  jsonq.py get        <resp.json> <dotted.path>
  jsonq.py keys       <resp.json> <dotted.path>
  jsonq.py count      <resp.json> <dotted.path>
  jsonq.py has        <resp.json> <dotted.path>
  jsonq.py diags      <resp.json>
  jsonq.py diagcodes  <resp.json>
  jsonq.py sorted     <resp.json>          # yes/no
  jsonq.py snapshot   <name> <resp.json>   # compare with tests/snap/<name>.json
  jsonq.py snapshot   --emit <name> <resp.json>
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

SNAP_DIR = pathlib.Path(__file__).resolve().parent / "snap"

# Fields **deliberately dropped** from snapshots:
#   message / hint    — human prose, not protocol shape; locking it locks wording, not the contract
#   expected_snapshot — the "expected snapshot" body echoed when `iface status` mismatches (dynamic)
_DROP_KEYS = {"message", "hint", "expected_snapshot"}
# The `data.languages[].version` of `--version --json` is the **build-environment toolchain version**
# (e.g. rustc 1.93.1), not protocol shape; another toolchain reddens the snapshot ⇒ dropped (names kept).
_DROP_TOOLCHAIN = re.compile(r"languages(\[\d+\])?$")
_LONG_STRING = 200


def load(path: str) -> dict:
    return json.loads(pathlib.Path(path).read_text())


def walk(node: object, path: str) -> object:
    """Fetch by ``a.b.0.c``; an empty path ⇒ the root."""
    cur = node
    if not path:
        return cur
    for part in path.split("."):
        if isinstance(cur, dict):
            if part not in cur:
                raise KeyError(path)
            cur = cur[part]
        elif isinstance(cur, list):
            cur = cur[int(part)]
        else:
            raise KeyError(path)
    return cur


def flat(value: object) -> str:
    """Scalars pass through; containers emit compact JSON (so `check` can compare them)."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False, sort_keys=True)
    return str(value)


def diag_tuples(resp: dict) -> list[list[str]]:
    """Canonical tuple sequence of diagnostics (**array order kept**, so ordering asserts mean something)."""
    out = []
    for d in resp.get("diagnostics", []):
        out.append([
            str(d.get("code") or ""),
            str(d.get("severity") or ""),
            str(d.get("file") or ""),
            str(d.get("span") or ""),
            str(d.get("target") or ""),
        ])
    return out


def sort_key(t: list[str]) -> tuple:
    # Same keys as python/brickie/present.sort_diagnostics (contract §3);
    # diagnostics without a code sort first as "".
    return (t[0], t[2], t[3], t[4])


def project(node: object, path: str = "") -> object:
    """Stable projection: fixed key order, prose dropped, build-toolchain version dropped, long strings truncated."""
    if isinstance(node, dict):
        out = {}
        for k, v in sorted(node.items()):
            if k in _DROP_KEYS:
                continue
            if k == "version" and _DROP_TOOLCHAIN.search(path):
                continue
            child = f"{path}.{k}" if path else k
            out[k] = project(v, child)
        return out
    if isinstance(node, list):
        return [project(x, f"{path}[{i}]") for i, x in enumerate(node)]
    if isinstance(node, str) and len(node) > _LONG_STRING:
        return f"<str:{len(node)}>"
    return node


def render(value: object) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n"


def cmd_snapshot(args: list[str]) -> int:
    emit = False
    if args and args[0] == "--emit":
        emit = True
        args = args[1:]
    if len(args) != 2:
        print("usage: jsonq.py [--emit] snapshot <name> <resp.json>", file=sys.stderr)
        return 1
    name, path = args
    proj = render(project(load(path)))
    target = SNAP_DIR / f"{name}.json"
    if emit:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(proj)
        print(f"emit {target}")
        return 0
    if not target.exists():
        print(f"snapshot missing: {target} (generate with `jsonq.py --emit snapshot {name} <resp.json>`)")
        return 1
    want = target.read_text()
    if want == proj:
        return 0
    import difflib

    diff = list(difflib.unified_diff(
        want.splitlines(), proj.splitlines(),
        fromfile=f"snap/{name}.json (baseline)", tofile="current output", lineterm=""))
    print("\n".join(diff[:60]))
    return 1


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    op, rest = argv[0], argv[1:]

    if op == "snapshot":
        return cmd_snapshot(rest)

    if not rest:
        print(f"{op} requires <resp.json>", file=sys.stderr)
        return 2
    resp = load(rest[0])

    if op == "envelope":
        print(",".join(sorted(resp.keys())))
        return 0
    if op == "text":
        # text projection of diagnostics (so a case can use the same grep for both text/json modes)
        for d in resp.get("diagnostics", []):
            print(f'{d.get("severity", "?")}: {d.get("code") or "-"} '
                  f'[{d.get("file") or d.get("target") or ""}] ({d.get("span") or ""}) '
                  f'{d.get("message", "")} {d.get("hint", "")}')
        return 0
    if op == "diags":
        for t in diag_tuples(resp):
            print("|".join(t))
        return 0
    if op == "diagcodes":
        print(",".join(sorted({t[0] for t in diag_tuples(resp) if t[0]})))
        return 0
    if op == "sorted":
        tuples = diag_tuples(resp)
        print("yes" if tuples == sorted(tuples, key=sort_key) else "no")
        return 0
    if op == "proj":
        sys.stdout.write(render(project(resp)))
        return 0

    path = rest[1] if len(rest) > 1 else ""
    if op == "keys":
        node = walk(resp, path)
        if not isinstance(node, dict):
            print(f"{path} is not an object", file=sys.stderr)
            return 1
        print(",".join(sorted(node.keys())))
        return 0
    if op == "count":
        print(len(walk(resp, path)))
        return 0
    if op == "has":
        try:
            walk(resp, path)
            print("yes")
        except (KeyError, IndexError, ValueError):
            print("no")
        return 0
    if op == "get":
        print(flat(walk(resp, path)))
        return 0
    print(f"unknown operation: {op}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

"""Single-record shape validation of the declaration surface (BRV-D2): a bundled minimal JSON-Schema
subset, stdlib only. A shape mismatch produces ``BRV-MF-0001`` and exit code 2 (BRV-D9).

Cross-field / cross-file invariants (api_iface vs api_type, missing ``compat_gen``, version segment
count, ``range`` semantics, closures, topology) are always coded by brickie-core, so the schemas
deliberately omit version-segment-count/range patterns (they would mask core's code as MF-0001).
"""

from __future__ import annotations

import json
import os
import re
import tomllib
from functools import lru_cache
from pathlib import Path

from .native import repo_root, tool_root

#: Shape error for one declaration record (BRV-MF-0001).
MF_CODE = "BRV-MF-0001"

SCHEMA_FILES = {
    "plugin": "plugin.schema.json",   # <plugin>/brick.toml      (contract §7.1)
    "product": "product.schema.json", # <root>/product.toml       (contract §7.2)
    "lock": "lock.schema.json",       # <root>/brickie.lock       (contract §7.3)
    "index": "index.schema.json",     # build/index/dependents.json
}

_TOML_KINDS = frozenset({"plugin", "product"})


def schema_search_roots() -> list[Path]:
    """Schema directory search order, first hit wins:
    1. ``$BRICKIE_SCHEMA_ROOT`` -- test cases / installed layout override;
    2. ``<tool_root>/schema/`` -- canonical; source tree or entry-ELF payload root;
    3. ``python/brickie/schema/`` -- compatibility read path; a missing directory is skipped;
    4. ``<repo_root>/tools/brickie/schema/`` -- fallback from outside the working directory.
    """
    roots: list[Path] = []
    override = os.environ.get("BRICKIE_SCHEMA_ROOT")
    if override:
        roots.append(Path(override).expanduser())
    roots.append(tool_root() / "schema")
    roots.append(Path(__file__).resolve().parent / "schema")
    roots.append(repo_root() / "tools" / "brickie" / "schema")
    return roots


def schema_path(name: str) -> Path | None:
    """Return the schema file path for a record kind; ``None`` if not found in any of the four places (degraded)."""
    filename = SCHEMA_FILES.get(name)
    if filename is None:
        return None
    for root in schema_search_roots():
        candidate = root / filename
        if candidate.is_file():
            return candidate
    return None


@lru_cache(maxsize=None)
def _load_file(path: str) -> dict:
    with open(path, "rb") as handle:
        return json.loads(handle.read().decode("utf-8"))


def load_schema(name: str) -> dict | None:
    """Load and cache a schema; ``None`` if not found."""
    path = schema_path(name)
    if path is None:
        return None
    return _load_file(str(path))


def any_schema_available() -> bool:
    return any(schema_path(name) is not None for name in SCHEMA_FILES)


def _type_name(value: object) -> str:
    return {
        dict: "object",
        list: "array",
        str: "string",
        bool: "boolean",
        int: "integer",
        float: "number",
        type(None): "null",
    }.get(type(value), type(value).__name__)


def _type_ok(value: object, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return True  # unknown type keyword: do not false-positive (the validator is a "smaller allowed set")


def _span(pointer: str) -> str:
    """Turn an internal pointer ``/export[0]/entries[2]/sig`` into its TOML span form."""
    if not pointer:
        return ""
    text = pointer.lstrip("/").replace("/", ".")
    return text


def _walk(value: object, schema: object, pointer: str, out: list[tuple[str, str]]) -> None:
    """Append every mismatch of ``value`` against ``schema`` to ``out`` as (span, message)."""
    if not isinstance(schema, dict):
        return

    expected = schema.get("type")
    if isinstance(expected, str) and not _type_ok(value, expected):
        out.append((_span(pointer), f"type should be {expected}, got {_type_name(value)}"))
        return

    if "enum" in schema:
        allowed = schema["enum"]
        if isinstance(allowed, list) and value not in allowed:
            out.append((_span(pointer), f"value must be one of {allowed}, got {value!r}"))

    if "oneOf" in schema and isinstance(schema["oneOf"], list):
        matches = 0
        for branch in schema["oneOf"]:
            probe: list[tuple[str, str]] = []
            _walk(value, branch, pointer, probe)
            if not probe:
                matches += 1
        if matches != 1:
            out.append((_span(pointer), f"oneOf: exactly 1 branch must match, got {matches}"))

    if isinstance(value, dict):
        for key in schema.get("required", []) or []:
            if key not in value:
                out.append((_span(f"{pointer}/{key}"), f"missing required field `{key}`"))
        properties = schema.get("properties", {})
        if isinstance(properties, dict):
            for key, sub in properties.items():
                if key in value:
                    _walk(value[key], sub, f"{pointer}/{key}", out)
        if schema.get("additionalProperties") is False and isinstance(properties, dict):
            for key in value:
                if key not in properties:
                    out.append(
                        (_span(f"{pointer}/{key}"), f"unknown field `{key}` (additionalProperties=false)")
                    )
    elif isinstance(value, list):
        items = schema.get("items")
        if isinstance(items, dict):
            for index, item in enumerate(value):
                _walk(item, items, f"{pointer}[{index}]", out)
    elif isinstance(value, str) and isinstance(schema.get("pattern"), str):
        try:
            matched = re.search(schema["pattern"], value) is not None
        except re.error:
            matched = True
        if not matched:
            out.append((_span(pointer), f"does not match pattern {schema['pattern']!r}: {value!r}"))
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        minimum = schema.get("minimum")
        if isinstance(minimum, (int, float)) and value < minimum:
            out.append((_span(pointer), f"should be ≥ {minimum}, got {value}"))


def validate(record: object, schema: dict) -> list[tuple[str, str]]:
    """Validate a single record, returning ``[(span, message)]`` (empty = passed)."""
    out: list[tuple[str, str]] = []
    _walk(record, schema, "", out)
    return out


def _diag(rel: str, target: str, span: str, message: str) -> dict:
    return {
        "code": MF_CODE,
        "severity": "error",
        "target": target,
        "file": rel,
        "span": span,
        "message": message,
        "hint": "declaration-surface shape problem (BRV-D2); cross-field invariants are coded by brickie-core",
    }


def diagnostics_for(record: object, schema_name: str, *, file: str, target: str = "") -> list[dict]:
    """Run shape validation on a parsed record and produce ``BRV-MF-0001`` diagnostics."""
    schema = load_schema(schema_name)
    if schema is None:
        return []
    return [_diag(file, target, span, message) for span, message in validate(record, schema)]


def _record_target(kind: str, record: object) -> str:
    """Diagnostic ``target``: plugin/product name, so a shape error points at a person."""
    if not isinstance(record, dict):
        return ""
    section = record.get("plugin") if kind == "plugin" else record.get("product")
    if isinstance(section, dict):
        name = section.get("name")
        return name if isinstance(name, str) else ""
    return ""


def _parse_record(kind: str, text: str) -> object:
    """Parse by record kind: ``plugin``/``product`` use TOML, ``index`` uses JSON. ``lock`` is not
    pinned down in v0.1, so try TOML first, then JSON.
    """
    if kind in _TOML_KINDS:
        return tomllib.loads(text)
    if kind == "index":
        return json.loads(text)
    if kind == "lock":
        try:
            return tomllib.loads(text)
        except tomllib.TOMLDecodeError:
            return json.loads(text)
    return json.loads(text)


def check_declaration(kind: str, rel: str, text: str) -> list[dict]:
    """Parse + shape-validate a declaration file: a parse failure (TOML/JSON syntax) yields one
    ``BRV-MF-0001`` (BRV-D9: syntax maps to exit code 2); a missing schema file yields an empty list
    (degraded, no false positives; the orchestration layer adds an info note).
    """
    if schema_path(kind) is None:
        return []

    try:
        record = _parse_record(kind, text)
    except (tomllib.TOMLDecodeError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        return [_diag(rel, "", "", f"parse failed ({kind}): {exc}")]

    return diagnostics_for(record, kind, file=rel, target=_record_target(kind, record))


def missing_schema_diagnostic() -> dict:
    """Degraded notice (info; does not decide the exit code) when the schema directory is entirely
    missing.
    """
    searched = ", ".join(str(root) for root in schema_search_roots())
    return {
        "code": None,
        "severity": "info",
        "target": "",
        "file": "",
        "span": "",
        "message": "schema/ directory not found, single-record shape validation skipped (BRV-D2); "
                   f"set $BRICKIE_SCHEMA_ROOT to override; searched: {searched}",
        "hint": "shape validation is one of L5's allowed duties; decisions remain with brickie-core when it is missing",
    }

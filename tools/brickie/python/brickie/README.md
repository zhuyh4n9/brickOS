# `brickie` Python Frontend (L5) — Module Map and Command Status

> This file documents the **L5 (Python3 frontend)**: module layout, command status, call sites into `brickie-core` / `brickie-gen`, zero-dependency notes, remaining TODOs and contract gaps.
> Process-boundary contract: [`docs/contract.md`](../../docs/contract.md); whole-tool overview (C++/Rust/build) in [`../../README.md`](../../README.md).

---

## 1. Module Map

```
python/brickie/
├── __init__.py   version and PROTOCOL (envelope version = 1)
├── __main__.py   `python -m brickie` entry point
├── cli.py        argparse for 23 leaf commands + 2 global switches, and the **orchestration pipeline** (§3)
├── native.py     locate and call brickie-core / brickie-gen; envelope assembly and response checks (§2)
├── schema.py     built-in minimal JSON-Schema subset → BRV-MF-0001 / exit code 2 (§4)
├── writer.py     idempotent write / `--check` byte compare / declaration-surface scan (§4)
├── present.py    render data as human-readable text; `--json` and text are **same-source** (§5)
└── hostinfo.py   host triple (coordinates of build/host/<arch>/<os>; same convention as tools/host-detect.sh)

> schema (`../schema/*.schema.json`) is no longer mirrored in-package — `freeze.py`'s
> `PAYLOAD_ROOTS` collects it directly into the entry ELF payload, see §4.3.
```

**Hard rule**: this package has **no business rules**. Closure, version ranges, topology, cycles, phase monotonicity, taxonomy, privileges, budgets, four-segment version advancement, IFACE-IR canonicalization and hashing, and any "machine file" content assembly all live in `brickie-core`. Permitted here: arg parsing / locate binaries / spawn subprocesses / read-write files / JSON serialization / **single-record shape** validation per schema.

### Orchestration Pipeline (same for every command)

```
optional schema shape check (BRV-MF-0001 ⇒ exit code 2)
  → brickie-core <command>         (verdict + data + files; exit_code)
  → if data.artifacts: brickie-gen render   (render only, no verdict)
  → write to disk (idempotent; --check compare only ⇒ mismatch exit code 1; hand-written files never overwritten)
  → present (--json and text are two serializations of the same data)
exit code summary: 2 (usage/env) > 1 (validation red) > 0
```

---

## 2. Call Sites into `brickie-core` / `brickie-gen`

Unified envelope: one JSON line in on **stdin**, one JSON line out on **stdout**. Process exit code is always 0; the business exit code is in the response's `exit_code`; non-zero or protocol mismatch ⇒ `NativeError` (L5 exit code 2, protocol mismatch carries code `BRV-PROTO-0001`).

### 2.1 Binary Lookup Order (`native._find_tool`)

1. `$BRICKIE_CORE` / `$BRICKIE_GEN` (explicit override; entry ELF unpacked files use this)
2. `build/host/<host-arch>/<host-os>/bin/<name>` (local build output; **preferred over seed**)
3. `build/host/*/*/bin/<name>` (fallback if host triple conventions ever differ)
4. `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/<name>` (bootstrap seed)
5. `prebuilts/seed/brickie/*/*/bin/<name>`
6. `PATH`

### 2.2 CLI → core command → key `data` fields

| CLI | `brickie-core` command | request `args` | `data` / `files` used |
|---|---|---|---|
| `new <ptype> <name>` | `plan-new` (called twice, see §2.3) | `plugin_type,name,api_type,lang,subkind,force` + `context.existing` | `data.artifacts[]` → gen render; write to disk |
| `init <product> [--app]` | `plan-init` | `name,app` + `context.existing` | `data.artifacts[]` → gen render; `data.lock_content` → `brickie.lock` (§2.4); write to disk |
| `gen [--check]` | `gen-plan` | `check` (pass-through) | `data.artifacts[]` → gen render; write/compare |
| `dep add <p> <d>[@r]` | `dep-add` | `plugin,dep,range?,kind?,phase?` | `data.snippet` (**no disk write**, ruling R-2) |
| `dep rm <p> <d>` | `dep-rm` | `plugin,dep,kind?` | `data.snippet`, `data.locator` |
| `dep tree [--kind]` | `dep-tree` | `kind?` | `{roots,nodes,kind}` |
| `dep graph --format` | `dep-graph` | `format` | `{format,nodes,edges,text}` |
| `dep why <a> <b>` | `dep-why` | `from,to` | `{found,path,edges}` |
| `dep index` | `dep-index` | — | `data.dependents` + `files[build/index/dependents.json]` |
| `dep closure` | `closure` | — | `{plugins,topo_order,totals,cycles,profile,…}` |
| `check` | `check` | `profile,scopes` | `{summary,scope_results}` |
| `ver show <id>` | `ver-show` | `id` | `{segments,compat_gen_source,freeze_state,…}` |
| `ver bump <id> --rule` | `ver-bump` | `id,rule` | unit snapshot `files[]` + four version segments |
| `iface list` | `iface-list` | — | `{units[]}` |
| `iface show <id>` | `iface-show` | `id` | `{unit,entries,hash_scope,truth,not_abi,…}` |
| `iface diff <id>` | `iface-diff` | `id` | `{changes,verdict,version,dependents}` |
| `iface status <id> [--check]` | `iface-status` | `id,check` | `{consistent,recomputed_hash,snapshot_hash}` (+ possible write-back) |
| `iface publish <id> …` | `iface-publish` | `id,check,note?,set?,profile?` | snapshot + `CHANGELOG.md` + lock; surface unchanged ⇒ no-op |
| `iface freeze <id>[#e] --note` | `iface-freeze` | `id,entry?,note` | promotion proposal `build/gen/proposals/<unit>.toml` (A-26 exception) |
| `iface deprecate/undeprecate` | `iface-deprecate` / `iface-undeprecate` | `id,entry?,note` | snapshot |
| `iface unfreeze <id> --note` | `iface-unfreeze` | `id,note` | snapshot (records baseline) |
| `iface refreeze <id> [--note]` | `iface-refreeze` | `id,note?` | snapshot (may bump COMPAT_GEN) |
| `--version [--deps]` | `version` | — | `{version,protocol,languages[],deps[]}` |

`--check` appears in three places with two semantics: **`gen --check`** is a byte-for-byte compare by L5 (no write); **`iface status|publish --check`** passes through to core (core recomputes), L5 only skips writes.

### 2.3 Two-Phase `new` / `init`

`plan-new`'s `context.existing` requires L5 to know which paths will be written first, so:

1. probe `plan-new` once with empty `context` → obtain `data.artifacts[].path`;
2. `writer.scan_existing` reads the **first line** of those paths (generated marker is on line 1) → `context.existing`;
3. call `plan-new` again with `context.existing` (this run's diagnostics/exit code are authoritative);
4. `brickie-gen render` → write to disk.

`init` has the same shape (`plan-init`); contract gives no `context` for `plan-init`, so we add it under the same discipline as `new` (extra fields ignored, but core then also has input for "never overwrite existing hand-written files").

`gen render` args: `args = {templates_root, artifacts:[{path,template,vars}]}`; the request top level also carries a legacy first-cut `templates_root`.

### 2.4 core Passes "Machine File" Body Directly (lock)

`plan-init`'s plan includes `brickie.lock`, but it is **`kind = "machine"` and `template = ""`** — machine file bodies belong to core; L5 must not assemble them. The current core implementation puts the body in `data.lock_content`, so `cli._collect_files` uses a path→key map (`_CORE_CONTENT_KEYS = {"brickie.lock": "lock_content"}`) to place that body at the planned path:

* artifacts with a non-empty `template` → `brickie-gen render`;
* artifacts without `template` → only `artifact.content` or `data.<mapped key>` **direct body** accepted;
* neither present ⇒ one **info** diagnostic (never silently swallowed, and L5 never becomes a content producer).

Once core is finalized, if it uses templates or writes into `files`, just delete this mapping (single-point switch).

---

## 3. Command Status

**Delivered (full frontend)**: 23 leaf commands + 2 global switches, all visible in `--help` and exercising orchestration/write/presentation/exit codes; `checklist` §5.1 C-01…C-23 and G-01/G-02 land item by item on the **L5 side**.

| Group | Command | L5 frontend | Native side depended on |
|---|---|---|---|
| Skeleton | `new` | ✅ | `core plan-new` + `gen render` |
| Skeleton | `init` | ✅ | `core plan-init` + `gen render` |
| Skeleton | `gen [--check]` | ✅ | `core gen-plan` + `gen render` |
| Dependency | `dep add` / `dep rm` | ✅ (prints snippet only) | `core dep-add` / `dep-rm` |
| Dependency | `dep tree` / `dep graph` / `dep why` | ✅ | `core dep-tree` / `dep-graph` / `dep-why` |
| Dependency | `dep index` | ✅ | `core dep-index` |
| Dependency | `dep closure` | ✅ | `core closure` |
| Validation | `check [--deps\|--iface\|--tax\|--priv\|--all] [--profile]` | ✅ | `core check` |
| Version | `ver show` / `ver bump` | ✅ | `core ver-show` / `ver-bump` |
| Interface | `iface list/show/diff/status` | ✅ | `core iface-list/show/diff/status` |
| Interface | `iface publish` | ✅ | `core iface-publish` |
| Interface | `iface freeze/deprecate/undeprecate/unfreeze/refreeze` | ✅ | same-named core commands |
| Global | `--version [--deps]` / `--json` / `--root` | ✅ | `core version` (`--version` degrades when core absent) |

**Explicitly not done** (none appear in `--help`): `build` / `test` / `run` / `api-dump` / `verify` / `iface check` / `dbg` / `pack` / `add` / `show` / `env`.

**Exit codes**: `0` success / `1` validation red (including `gen --check` mismatch) / `2` usage or environment error (invalid args, schema shape mismatch, missing `--root`, missing native binaries or protocol mismatch).

---

## 4. schema Shape Validation

### 4.1 Allowed Subset and Zero Dependencies

`schema.py` **ships a minimal JSON-Schema subset**, standard library only (**no `jsonschema`**, honoring the "zero third-party dependencies" rule). Supported keywords:

```
type / properties / required / items / enum / oneOf /
additionalProperties / minimum / pattern
```

This is a **smaller allowed subset** — not a "complete JSON-Schema implementation", and **not meant** to become one: it only needs to cover the four files under `schema/`. To support `$ref`/`if-then`/`format` later, register it in contract §9 first rather than quietly widening L5's decision surface.

**Deliberately not in schema**: version-string segment count (VER-0005), `range` semantics (VER-0006), `api_iface == api_type` (TAX-0016), `compat_gen` required (VER-0003), closure and phases. These are **cross-field/cross-file invariants** reported by core; putting them in schema would mask core codes as MF-0001.

### 4.2 Record Types and Files

| Record | File | Format | Parsing |
|---|---|---|---|
| `plugin` | `<plugin>/brick.toml` | TOML | `tomllib` |
| `product` | `<root>/product.toml` | TOML | `tomllib` |
| `lock` | `<root>/brickie.lock` | **Not fixed** | try TOML then fall back to JSON (lenient) |
| `index` | `build/index/dependents.json` | JSON | `json` |

Shape failure ⇒ diagnostic `BRV-MF-0001` (error), exit code **2**; parse failure (TOML/JSON syntax) same code. span uses TOML path (e.g. `export[0].entries[2].sig`), `file` relative to `root`.

**Actual per-command gate scope**: `plugin` + `product`. `lock` / `index` are **core-produced machine files** whose field sets are not fixed in v0.1 (`W-17` "does lock include freeze generation" undecided); gating too early would misjudge core output as a shape error ⇒ these two schemas are exposed only to this module and tests via `schema.load_schema()` / `schema.validate_record()` / `schema.check_declaration()`. Once core is finalized, add `("lock","index")` to `cli._shape_gate`'s `kinds` (single switch).

### 4.3 schema Location (the payload carries the canonical copy)

`freeze.py`'s `PAYLOAD_ROOTS = ("python", "templates", "schema")` — `schema/**` travels with the entry ELF payload and unpacks to `<tool_root>/schema/`. So the single-file form's shape gate no longer needs the source tree; the old "in-package symlink mirror" (`python/brickie/schema/`) is deleted.

`schema.schema_search_roots()` still searches four tiers (first hit wins):

1. `$BRICKIE_SCHEMA_ROOT` — test / install-form override;
2. `python/brickie/schema/` — legacy slot of the old mirror (directory no longer exists, cannot hit);
3. `<tool_root>/schema/` — canonical location in source tree / payload (**the tier that actually applies**);
4. `<repo_root>/tools/brickie/schema/` — fallback from outside the working directory.

If none of the four is found, **degrade to skipping shape validation** with one info diagnostic (no false kill, no silence).

> `schema.py`'s docstring and the list above still describe tier 2 as "in-package mirror"; the directory is gone, so it is only a harmless early fallback and code behavior is unaffected. Changing `schema.py` is out of scope for this doc sync; recorded here to close later.

---

## 5. Presentation Discipline

* Text and `--json` are two serializations of the **same `data`**; the `--json` envelope is `{protocol,status,command,exit_code,diagnostics,data,written}` (`files` contents are transport detail, not in JSON output; write results shown via `written`).
* Diagnostics under `--json` are **stably sorted** by `(code, file, span, target)`; text diagnostics go to stderr, data to stdout.
* The artifact list for `new` / `init` / `gen` comes from `data.artifacts`; each artifact's status (written / up to date / skipped) is the same write result.
* Commands whose `data` shape is not fixed by contract (`ver bump` / `iface publish` / `iface freeze` family) use a **lenient renderer**: known fields printed line by line, unknown composite structures as indented JSON — no rework after integration, and no exceptions from missing fields.

---

## 6. Remaining TODOs and Contract Gaps Found

| # | Item | Status / action |
|---|---|---|
| **T-1** | ~~`freeze.py`'s `PAYLOAD_ROOTS` lacks `schema`~~ | **Resolved**: `PAYLOAD_ROOTS` now includes `"schema"`, in-package symlink mirror deleted (§4.3); only `schema.py`'s docstring/search-list wording is out of sync (see §4.3 note) |
| **T-2** | ~~core only landed part of it~~ | **Resolved**: all 15 `brickie-core` command families (`check`/`closure`/`dep-*`/`ver-*`/`iface-*`) are landed (Rust side `--selftest` = 432 cases); L5's orchestration/presentation/write/exit codes are hard-wired to contract §5 field names and verified field-by-field against core `data` (see `rust/README.md` §12). Authoritative end-to-end regression in `tools/brickie/tests/run.sh` |
| **T-3** | `brickie.lock` format and field set | Measured core output TOML `{schema, [lock]{profile, [lock.product]{name,version,stage}}}`; `schema/lock.schema.json` describes this + `plugins[]`/`units[]`, with **no required** (weak validation; `_parse_record` tries TOML then JSON) |
| **T-4** | Undecided whether `--note <path>` passes a **path** or a **body** | L5 passes the path as-is (matching CLI); if core needs the body, add `note_text` in `cli._cmd_iface` args (single-point change) |
| **T-5** | `plan-init` has no `context.existing` clause | L5 adds it using the `new` two-phase form (§2.3); harmless if core ignores it |
| **T-6** | `plan-init`'s `brickie.lock` body goes via `data.lock_content` (rust-side deviation) | L5 consumes it via `_CORE_CONTENT_KEYS` (§2.4); suggest contract §5.2 add a line on the "direct machine-file body channel" |
| **G-9** | **`gen --check` mismatch has no dedicated BRV code** | Diagnostic uses `code: null` (contract §2 allows code-less diagnostics) + file-precise message; *suggest* a new code (e.g. `BRV-GEN-0003` "generated output differs from recomputation") |
| **G-10** | `--check` only compares **planned** paths | Extra stale generated files on disk are not reported as "expired" (needs core's `gen-plan` to give a full list or another command) |
| **G-11** | `data` shape for `ver bump` / `iface publish` / `iface freeze` family not fixed in contract §5 | Presentation uses the lenient renderer (§5); suggest adding these `data` key fields to contract §5 |
| **G-12** | `dep index`'s index schema not listed in §9.2 (checklist C-09 logged) | L5 delivered `schema/index.schema.json` (shape aligned with `dep-index` data) |
| **G-13** | `--version` degrades to printing the L5 version when core is absent | Keeps `--version` (shell/CI self-check) from being blocked by "native binaries not built yet"; `--version --deps` still reports 2 as an environment error |

---

## 7. Self-Verification (stubs + real native binaries)

### 7.1 Stubs (reproduce the full L5 command surface end to end)

Before `brickie-core` is ready, two **temporary stubs** (fixed JSON replies only, **not committed**) verify L5: point `$BRICKIE_CORE` / `$BRICKIE_GEN` at the stub scripts, then run commands one by one. Stubs cover: 6 artifacts for four skeleton types, byte-idempotent repeated `gen`, `gen --check` consistent/inconsistent states, `check` 0/1 states, shape mismatch (exit 2), protocol mismatch (`BRV-PROTO-0001`), missing native binaries, `--json` global/per-command equivalence and snapshot stability, stable diagnostic ordering, `--help` negative acceptance, `init` writes, orchestration and presentation of `dep *` / `ver *` / `iface *`. At delivery this suite was **129 items all green**.

### 7.2 Real Native Binaries (`build/host/<triple>/bin/{brickie-core,brickie-gen}`)

At delivery one integration pass was run with real core/gen, **34 items all green**: `new` for four types × `c` (6 artifacts each, descriptor first-line marker), repeated `new` conflict red (`BRV-GEN-0002`), `--force` only on generated dirs, real core output passing the L5 shape gate, `gen` idempotence and `--check`, `init` writing 8 files (incl. `brickie.lock`), `dep add/rm` not writing, `--version --deps` fingerprint, `--json` equivalence and envelope shape. After the full core command surface landed, end-to-end regression moved to `tools/brickie/tests/run.sh` (see V-1…V-19 matrix in `tests/ACCEPTANCE.md`).

## 8. Running Locally

```sh
cd prototype/tools/brickie
PYTHONPATH=python python3 -m brickie --help
PYTHONPATH=python python3 -m brickie new ability service/crypto --subkind service --root /tmp/repo
PYTHONPATH=python python3 -m brickie check --root /tmp/repo
```

Dependencies: **Python ≥ 3.11** (`tomllib`) and two prebuilt native tools (core / gen). **Zero third-party Python packages**.

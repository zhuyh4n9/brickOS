# brickie v0.1 — Process Boundary Contract (L5 ↔ L0/L1/L2)

> **Status**: implementation contract, not a design document. The sole authority for design is
> `Design/docs/2-toolchain/brickie/brickie-v0.1.md` (hereafter `§x`); this file fixes only
> **the JSON shapes, file ownership and rulings between the three processes**, so that
> L5 (Python), L0/L1 (Rust) and L2 (C++) can be implemented separately without each growing a second source of truth.
> Where this file conflicts with the design, it is registered in §9 "Implementation-time Rulings", and is
> visible in the deviation table of `tools/brickie/README.md`.

## 1. The Three Host Executables

| Artifact | Language | Layer | Responsibility | Environment variables |
|---|---|---|---|---|
| `brickie` | C++ launcher + embedded payload | L5 entry | Unpack payload (containing the Python frontend, templates, `brickie-core`, `brickie-gen`) → run the frontend with the system `python3` | Set `BRICKIE_GEN` / `BRICKIE_CORE` to the unpacked native tools |
| `brickie-core` | Rust | L0 model + L1 solving/version/interface engine | All **decisions** and error codes; the content of every "machine-owned file" | — |
| `brickie-gen` | C++ | L2 generator | Only **template rendering** (skeletons / descriptors / headers), no decisions | — |

**Hard discipline** (§9.1 / V-18c): `python/brickie/**` contains **no** business rules. Any decision beyond argument parsing /
file orchestration / schema shape validation / JSON serialization / text rendering belongs in `brickie-core`.

## 2. Envelope and Protocol Version

Both native tools use the **same envelope**: one line of JSON read from stdin, one line of JSON written to stdout.

Request:

```json
{
  "protocol": 1,
  "command": "check",
  "root": "/abs/path/to/repo",
  "args": { },
  "context": { }
}
```

Response:

```json
{
  "protocol": 1,
  "status": "ok",
  "exit_code": 0,
  "diagnostics": [ ],
  "files": [ ],
  "data": { }
}
```

| Field | Rule |
|---|---|
| `protocol` | Constant `1`. If the request's or the peer's version differs from one's own ⇒ reply **`BRV-PROTO-0001`**, `exit_code = 2`, `status = "ok"` (protocol handled successfully; the business exit code is in the response) |
| `status` | `ok` = protocol handled successfully (even if `exit_code != 0`); `internal_error` = the tool itself failed (L5 raises an environment error) |
| `exit_code` | BRV-D9: `0` success / `1` validation red / `2` usage or environment error. **The process exit code is always 0** (except `internal_error` and crashes) — "tool failure" and "validation red" must be distinguishable |
| `diagnostics` | See §3; diagnostics without a code use `"code": null` |
| `files` | See §4; only `brickie-core` / `brickie-gen` produce content, Python only writes it to disk |
| `data` | Command-specific payload (used for rendering and `--json`); `{}` when empty |

**Protocol handshake**: L5 includes `protocol` on every call; the native side validates it after startup. If the
`protocol_version` in a snapshot/case differs from the tool's ⇒ `BRV-PROTO-0002` (no automatic migration).

## 3. Diagnostics

```json
{"code": "BRV-DEP-0009", "severity": "error", "target": "service/crypto",
 "file": "service/crypto/brick.toml", "span": "dep[0].phase",
 "message": "…", "hint": "…"}
```

* `severity ∈ {error, warning, info}`; **severity does not determine the exit code**; the exit code is determined by "whether the command completed" (§4 BRV-D8 note / G-1).
* The full set of numbers = the code table of design BRV-D8; **new defects with unassigned codes** are registered per §9 and must not be invented.
* Location: `file` is relative to `root`; `span` uses a TOML path (e.g. `export[0].entries[2].sig`).
* Under `--json` the diagnostics array is stably sorted by `(code, file, span, target)` ⇒ snapshot tests work.

**Build-family codes `BRV-BLD-0001..0014`** (a new family in this cut: `BRV-D8`'s code table has **no BUILD family**,
written per §9's "new defects with unassigned codes are registered in §9"; back-feed items in `rust/README.md` §10 S-B*):

| Code | Severity | Trigger |
|---|---|---|
| `BRV-BLD-0001` | error | `product.toml` missing, or it lacks `[build]` (building needs a product-level declaration) |
| `BRV-BLD-0002` | error | `[build].sources` expands to empty (declared, yet matched no file) |
| `BRV-BLD-0003` | error | No platform plugin declares `[build.target]` (no way to know arch / cross prefix / linker script) |
| `BRV-BLD-0004` | error | Multiple platform plugins declare `[build.target]` (which one is the truth is unclear) |
| `BRV-BLD-0005` | error | Tool resolution failed (cross compiler / objcopy / objdump / size / qemu absent) — **environment error**, exit code 2 |
| `BRV-BLD-0006` | error | A plugin in the closure has no `[build].sources` (it gets composed, yet does not say how to compile) |
| `BRV-BLD-0007` | error | A **literal** path in `[build].sources` does not exist (a glob matching nothing is **not** an error, see 0002) |
| `BRV-BLD-0008` | error | `tests/gates.toml` missing, or the name of `brickie test <name>` is not in the declaration surface |
| `BRV-BLD-0009` | error | Gate judged red (log missing `require` / hitting `forbid` / missing a PASS case tag) |
| `BRV-BLD-0010` | error | Shape problem in `[build]` (artifact locations / illegal flag table, e.g. empty `cflags`) |
| `BRV-BLD-0011` | error | An executed step failed (compile / link / script exited non-zero) — detected by L5, code assigned by core along with the step |
| `BRV-BLD-0012` | error | A regex in `tests/gates.toml` exceeds the **subset** core supports (reported, not silently unmatched) |
| `BRV-BLD-0013` | info | A generated artifact belongs to a plugin **not in the closure** ⇒ skipped (not a defect, but it must be visible; exit code unchanged) |
| `BRV-BLD-0015` | error | The symbol audit produced **no `nm` output**: the `.a` evidence is missing, so the disciplines cannot be proven (the audit fails closed) |
| `BRV-BLD-0014` | info | A generated artifact under `build/gen/` whose **owner cannot be identified** (stale leftovers after a plugin rename/removal) ⇒ skipped. Keeping it becomes a "ghost plugin": a plugin with the same symbol in the tree is registered twice, and `start` executes a second time ⇒ shutdown at boot (measured: `rc=-28` after renaming `third_party/uboot-mmc` → `driver/uboot-mmc`) |

**Source-discipline codes (ADR-0038)** — the `src` scope of `check`, plus the symbol audit of `build`:

| Code | Severity | Trigger |
|---|---|---|
| `BRV-SRC-0001` | error | A function prototype or an `extern` declaration in a `.c` has no definition in that same file (R1: a translation unit is self-contained; cross-TU declarations belong in a header) |
| `BRV-SRC-0002` | error | A file-scope object without `static` in a `.c`, an object exported through a public header, an `export.entries` entry with `kind = "var"`, or a **data symbol** of a `native` plugin referenced from another plugin (`.a` audit) -- R2: globals never leave their plugin |
| `BRV-DEP-0012` | error | A header of a plugin that is not in the consumer's **direct** dependency set (text: `#include`; symbol: strong undefined symbol resolved to that plugin) -- R3: dependencies are declared, transitive ones do not authorize |


## 4. File Ownership (§9.1.1 write-path discipline)

`files[i] = {"path": "<relative to root>", "kind": "machine"|"rendered"|"human", "content": "<UTF-8>", "mode": "0644"}`

| kind | Who produces | Who writes to disk | Example |
|---|---|---|---|
| `machine` | `brickie-core` | L5 | `api/iface/<provider>/<unit>.toml`, `api/iface/CHANGELOG.md`, `brickie.lock`, `build/index/dependents.json`, `build/gen/proposals/<unit>.toml` |
| `rendered` | `brickie-gen` | L5 | `build/gen/<plugin>/plugin_desc.c`, all skeleton files of `brickie new/init` |
| `human` | **not produced** | — | `brick.toml` / `product.toml` are **read-only inputs** (§9.1.1 rule 1) |

* **`brick.toml` / `product.toml` are never rewritten**; only `new` / `init` create them when the file does **not** exist.
* L5 writes to disk: only when the content differs **byte for byte** from the existing file (idempotent); `mkdir -p` before writing.
* **Reproducible**: two runs on the same input ⇒ `files` byte-identical. Machine files must **not contain timestamps / random numbers / host-specific information**. `CHANGELOG.md` is sectioned by "version section" rather than by date.
* `new` artifacts go through two stages: `brickie-core plan-new` gives the **plan** (path + template id + variables), `brickie-gen render` gives the content, L5 writes to disk. Existing human-written files are not overwritten ⇒ `BRV-GEN-0002`.

## 5. `brickie-core` Commands

Request `args` are optional defaults unless stated otherwise. `id` always means an **interface unit** `<provider>#<unit>`;
`plugin` means a plugin name.

### 5.1 Read-only

| `command` | `args` | Key `data` fields |
|---|---|---|
| `version` | `{}` | `{version, protocol, languages:[{name,version}], deps:[]}` (the fingerprint of G-01 `--version --deps`) |
| `model` | `{plugin?}` | `{plugins:[Plugin], count}` — normalized model |
| `check` | `{profile:"dev"\|"release", scopes:["deps","iface","tax","priv","src"], plugin?}` | `{summary:{errors,warnings,infos,profile,closure:{plugins,ram_kib,stack_kib}}, scope_results}` |
| `closure` | `{profile}` | `{plugins:[{name,version,compat_gen,api_type,plugin_type,phase,ram_kib,stack_kib,selected}], init_edges:[{from,to}], runtime_edges, type_edges, topo_order:[], totals:{ram_kib,stack_kib,platform_capacity:{ram_kib,stack_kib},budget:{ram_kib,stack_kib}}, profile, cycles:[]}` |
| `dep-tree` | `{kind:"init"\|"runtime"\|"type"\|"all", plugin?}` | `{roots:[], nodes:{name:{deps:[{name,kind,range,phase}]}}, kind}` |
| `dep-graph` | `{format:"dot"\|"mermaid"\|"json"}` | `{format, nodes:[], edges:[], text}` (for `json`, `text` = normalized JSON string) |
| `dep-why` | `{from, to}` | `{found, path:[name], edges:[{from,to,kind}]}` |
| `dep-index` | `{}` | `{dependents:{<plugin>:[{from,kind}]}}` + `files:[build/index/dependents.json]` |
| `show-bricks` | `{profile}` | `{product:{name,version,stage,profile}, has_product, bricks:[{name,namespace,plugin_type,api_type,subkind,phase,version,ram_kib,stack_kib,selftest,origin:"root"\|"dep",root_kind,root_alias}], totals:{plugins,roots,ram_kib,stack_kib,budget,platform_capacity,has_platform,by_type}, seeds:[]}` — the closure as a reading aid; re-decides nothing (ADR-0036) |
| `show-init-flow` | `{profile}` | `{product, steps:[{phase:"early"\|"core"\|"late"\|"app"\|"irq"\|"start"\|"selftest",plugin,plugin_type,subkind,hook,note}], counts:{<phase>:n}, order:[], order_basis, unsorted:null\|[name], selftest_on:bool}` — **the runtime boot chain** (ADR-0036). `order` is the runtime's `s_order`: section order + FIFO Kahn over `kind="init"` edges (a different tie-break from `closure.topo_order`); the `irq` step carries an empty `plugin` and follows **all** init (ADR-0037: the APP's init is the last init, in the `app` phase); `unsorted` is non-null only for an init cycle, which the runtime panics on |
| `ver-show` | `{id}` | `{id, plugin, version, compat_gen, segments:{compat_gen,major,minor,revise}, freeze_state, status, source:"snapshot"\|"declaration", compat_gen_source:{kind,note?}, published:bool}` |
| `iface-list` | `{}` | `{units:[{id,provider,unit,api_iface,form,version,compat_gen,freeze_state,status,published,hash}]}` |
| `iface-show` | `{id}` | `{unit:UnitSnapshot, entries:[Entry], source:"declaration"\|"snapshot", not_abi:true, truth, hash_scope, declaration_hash, snapshot_hash, published}` |
| `iface-diff` | `{id}` | `{id, changes:[Change], verdict, compat_gen_changed, version:{from,to,reasons:[]}, dependents:{direct:[],transitive:[],unsatisfied:[]}}` |
| `iface-status` | `{id, check:bool}` | `{id, recomputed_hash, snapshot_hash, declaration_hash, consistent:bool, files:[if write-back needed]}` |

### 5.2 Write surface (produces `files`)

| `command` | `args` | Files written | Key rules |
|---|---|---|---|
| `plan-new` | `{plugin_type,name,api_type,lang,subkind,force}` + `context.existing:[{path,first_line}]` | None (plan) | Name contract/subkind inference/phase inference/template availability; returns `data.artifacts:[{path,kind:"human"\|"generated",template,vars}]` |
| `plan-init` | `{name, app?}` | None (plan) | First version of `product.toml` + `app/<name>/` + `brickie.lock` |
| `gen-plan` | `{plugin?}` | None (plan) | Plan to rebuild `build/gen/**` (descriptors + headers) |
| `dep-add` | `{plugin, dep, range?, kind:"init"\|"runtime"\|"type", phase?}` | **None** (ruling R-2: prints a TOML snippet, does not modify `brick.toml`) | `data.snippet` = a `[[dep]]` block that can be pasted directly into `brick.toml` |
| `dep-rm` | `{plugin, dep, kind?}` | **None** | `data.snippet` = locator of the block to delete |
| `ver-bump` | `{id, rule:"major"\|"minor"\|"revise"}` | Unit snapshot | §5.2 advancement table; `compat_gen` is **not** in rule; monotonic, no rollback (`BRV-VER-0007`) |
| `iface-publish` | `{id, check:bool, note?, set?, profile}` | Snapshot + `CHANGELOG.md` + `brickie.lock` | Surface unchanged ⇒ **no-op** (no files, `exit_code` 0); `--profile release` runs the pre-release gate hooks |
| `iface-freeze` | `{id, entry?, note}` | `build/gen/proposals/<unit>.toml` | **A-26 exception**: v0.1 only emits a "promotion proposal", does not write a `frozen` snapshot, does not bump `compat_gen` |
| `iface-deprecate` | `{id, entry?, note}` | Snapshot | State machine `BRV-IFACE-*`; `--note` required |
| `iface-undeprecate` | `{id, entry?, note}` | Snapshot | Same as above |
| `iface-unfreeze` | `{id, note}` | Snapshot (records baseline) | No `--note` ⇒ red; not `frozen` ⇒ red |
| `iface-refreeze` | `{id, note?}` | Snapshot | Changed/deleted frozen entries ⇒ `COMPAT_GEN+1`; empty unfreeze ⇒ all four segments unchanged |

### 5.3 `selftest`

`brickie-core --selftest` (does not go through the stdin envelope) runs all domain cases
**without loading Python** (V-18a): model / version matrix / IFACE-IR / solving / taxonomy / privilege / budget / unfreeze window.
Exit code 0 all green / 1 any failure; failure details go to stdout.

### 5.4 Build-family Commands of `brickie-core`

> **This family is explicitly a second family** (ADR-0004 / S1–S4 of design ADR-0003). The envelope, diagnostic model and
> exit-code discipline are **exactly the same** as §5.1/5.2; the only difference is that it **requires a (cross) toolchain to be present** —
> §0's "must not require cc/cargo/nm to be present" only constrains **composition-time** commands, see §9 **R-13**.
> **All decisions are in core**: tool candidate order and resolution (R-14), incremental criteria (R-15), gate red/green (R-16).
> L5 does only four things: verify `argv[0]` is executable, run by `group` with concurrency/time limits, collect logs, render.

| `command` | `args` | Key `data` fields | `files` (machine files) |
|---|---|---|---|
| `build` | `{profile?, jobs, backend, tag?, dry_run, force, emit_backends}` | Common summary (see below) + `dry_run` / `emit_backends` | `build/gen/build.mk` (`backend=make` or `emit_backends`), `build/gen/build.ninja` (`backend=ninja` or `emit_backends`), `build/gen/build-state.json` (`direct` and not `dry_run`) |
| `clean` | `{all}` | `{all, paths:[<relative to root>], note}` — **paths is only a list; deletion is done by L5** (with a guard to "delete only inside root") | None |
| `run` | `{timeout_s?}` | Common summary + `mode:"run"`; `steps[0].kind="run"`, `expect_timeout=false` (interactive, inherits stdio) | None |
| `size` | `{}` | Common summary + `mode:"size"`; two `kind:"print"` steps (`size -A -x` and `size`) | None |
| `disasm` | `{head}` | Common summary + `mode:"disasm"`; one `kind:"print"` step (`objdump -d`), `stdout_lines=head` | None |
| `test` | `{name?, list, jobs}` | `list=true` ⇒ `{gates:[names], hosttests:[names], scripts:[names], build_post:[names], file, present}`; otherwise common summary + `mode:"test"` + `gates:{file,gates,hosttests,scripts,build_post}` | None |
| `judge` | `{name}` + `context.log` (the log **body**, not a path). Reserved `name = "symbol-audit"` judges `nm -A -g` output over `build/lib/*.a` (ADR-0038: R2 data symbols / R3 undeclared dependencies; non-gate, reads the model) | `{name, failed, checks:[{kind,pattern,ok}], log_lines, pass_lines, require, forbid, require_tags, file}`; all green ⇒ codeless **info**, `failed>0` ⇒ `BRV-BLD-0009` + exit code 1 | None |

**Common summary fields** (shared by `build`/`run`/`size`/`disasm`/`test`):

```json
{
  "profile": "dev",            "backend": "direct",   "tag": "",
  "target": {"owner": "platform/qemu-aarch64", "arch": "aarch64",
             "cross": "aarch64-linux-gnu-",
             "arch_flags": ["-march=armv8-a", "…"],
             "linker_script": "platform/qemu-aarch64/src/link.ld",
             "qemu": {"binary": "qemu-system-aarch64", "machine": "virt,gic-version=3",
                      "cpu": "cortex-a53", "memory": "128M", "extra": ["-nographic"]}},
  "outputs": {"obj_dir": "build/obj", "elf": "build/brick.elf",
              "bin": "build/brick.bin", "map": "build/brick.map"},
  "state_path": "build/gen/build-state.json", "gen_dir": "build/gen",
  "units": [{"name":"core/c_core","dir":"core/c_core","plugin":"core/c_core","sources":3,
             "includes":["core/c_core/include"],"defines":[],"sources_list":["core/c_core/src/…"]}],
  "unit_count": 8, "source_count": 30,
  "tools": [ … see below … ],
  "steps": [ … see below … ], "steps_total": 32, "steps_todo": 32, "up_to_date": 0,
  "stale_outputs": [], "plan_hash": "sha256:…",
  "flags": {"cflags": […], "asflags": […], "ldflags": […]}
}
```

**Fields of `data.steps[i]`** (L5 reads only these keys; all facts about a step are here):

| Field | Type | Semantics |
|---|---|---|
| `label` | string | Human-readable label (`cc <src>` / `ld <elf>` / `qemu <gate>` …) |
| `kind` | string | `compile` / `archive` / `audit` / `link` / `objcopy` / `run` / `gate` / `script` / `hosttest` / `hostrun` / `print` / `backend` |
| `argv` | [string] | **The concrete command**; `argv[0]` is already the tool resolved by core (a `@<name>` placeholder when core could not resolve it) |
| `cwd` | string? | Working directory relative to root. L5 defaults to `"."`; **core v0.1 does not emit this key** (all steps run under root); the key is reserved for the future |
| `group` | u32 | Scheduling: steps in the same group (and that are `compile`/`hosttest`) may run concurrently; groups run serially |
| `log` | string? | Log destination (relative to root); `null` = inherit stdio directly |
| `timeout_s` | u64? | Timeout limit; `null` = no limit |
| `expect_timeout` | bool | `true` = **expected to end by timeout** (QEMU gates: the image is meant to run until cut off) — a timeout does not count as failure |
| `judge` | string? | Name of a gate whose judgement needs core (the log body is judged back through the `judge` command). Reserved name **`symbol-audit`** = the `.a` symbol audit (ADR-0038); it is not a `tests/gates.toml` gate |
| `stdout_lines` | u64? | Max number of stdout lines when rendering (expands `--head`) |
| `outputs` | [string] | Files written by this step (used for incremental state / `clean`) |
| `consumes` | [string] | Files this step consumes that are **produced by other steps in this plan**. Sole purpose: when an upstream step fails, L5 uses this to **skip** downstream steps — otherwise `ld` would link successfully against the previous run's stale objects and print `ok ld`, and the log would lie (while humans read the log) |
| `fail_code` | string | **The code to use when the step fails**: L5 does not assign codes itself (always `BRV-BLD-0011`) |
| `tool_code` | string | Code for when `argv[0]` is not executable (always `BRV-BLD-0005`, environment error ⇒ exit code 2) |

> **Path convention (the easiest place to get criteria wrong)**: **all paths** in `data.*` and in
> `steps[i].{argv,outputs,log,state_path,elf/bin/map,obj_dir}` **are relative to root** (the repository root). Whereas paths in the declaration surface `brick.toml [build].sources/includes` and
> `[build.target].linker_script` are **relative to the plugin root** — core expands/joins them to be relative to root before
> they appear in `data`. So "the assembly source in the plan" is
> `platform/qemu-aarch64/src/start.S`, not `src/start.S`; the linker script is
> `platform/qemu-aarch64/src/link.ld`. These are two different things; write criteria according to the **shape of `data`**.

**Fields of `data.tools[i]` and "tool resolution is in core" (ruling R-14)**:

| Field | Type | Semantics |
|---|---|---|
| `name` | string | Logical name: `cc` / `objcopy` / `objdump` / `size` / `qemu` / `make` / `ninja` |
| `what` | string | What it is in this role (for rendering) |
| `candidates` | [string] | **Candidate order = policy**, tried in order. `cc` = `<cross>gcc` → `<cross>gcc-16…-13` → `cc`; binutils = cross version → host version |
| `resolved` | string? | **Resolution result** (i.e. `tools_resolved`): the repo's `prebuilts/toolchain/bin/<name>` takes precedence, then falls back to `context.path` (default = the process `PATH`); all fail ⇒ `null` |

> The reason `resolved` must come from core: the make / ninja backend files must **bake in the resolution result**
> (the generated file writes a concrete compiler path), and L5 and the generated file must not each resolve once — otherwise "who is compiling"
> would have two answers. So L5 only performs the **final** executability check (`runner.resolve_exe`) and does not re-select.

**Three backends and the BR-D4 A/B**: `--backend direct` (default) = L5 orchestrates directly from core's `steps`;
`make` = core renders `build/gen/build.mk` (**backend A** of BR-D4) and executes it; `ninja` = core renders
`build/gen/build.ninja` (**backend B**) and executes it. The ninja output has two key points:

* **One rule each for C sources and `.S` assembly** (`rule cc` / `rule asm`): the two source kinds have different flags
  (`cflags` vs `asflags`); rendering both with one template is compiling assembly as C — that it compiles is **luck**, not a criterion;
* Pin `builddir = build/gen/ninja` at the top of the file: ninja's `.ninja_log` / `.ninja_deps` land by default in the
  **invocation directory**; without pinning, two state files are dropped into the repository root (we have stepped on this).

The three backends run the **same argv batch** (only the scheduler differs) ⇒ the same input produces a **byte-identical** ELF/BIN
(the S2 criterion of BR-D4). `--emit-backends` produces both A/B in one go, without executing.

## 6. `brickie-gen` Commands

| `command` | `args` | `files` |
|---|---|---|
| `render` | `{templates_root, artifacts:[{path, template, vars}]}` | `[{path, kind:"rendered", content, mode}]` |
| `--selftest` (not through the envelope) | — | Self-checks of JSON round-trip / name contract / rendering / path layout |

* Template directory layout (§9.2 + ruling #5): `templates/<api_type>/<plugin_type>/<lang>/` (skeletons),
  `templates/descriptor/<api_type>/<lang>/` (descriptors), `templates/product/<lang>/` (product skeleton).
* The renderer has **no conditional syntax** (§9.2): differentiation is done by directories; `vars` only does placeholder substitution.
* Placeholder syntax: `{{name}}`; a missing variable ⇒ rendering fails (same-family usage error as `BRV-GEN-0002`).
* Generated output's first line is marked `brickie:generated` (identifies "is this a product of this tool").

## 7. Declaration Surface Schema (v0.1 authoritative field table)

Field shapes are described by `schema/*.schema.json` (BRV-D2: only expresses **single-record shape**);
**cross-field / cross-file invariants are reported with codes only by `brickie-core`**. TOML is the only expression format.

### 7.1 `brick.toml`

```toml
schema = 1
[plugin]
name        = "service/crypto"    # ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$  globally unique
plugin_type = "ability"           # app | interface | ability | platform | core
api_type    = "native"            # native | runtime_adapter | third_party
subkind     = "service"           # scheduler | framework | io | fs | service (ability only)
lang        = "c"                 # c | cxx | rust
phase       = "late"              # early | core | late | app (the ② completion point, §7.2)
sched_class = "SAFE_PREEMPT"      # SAFE_PREEMPT | COOP_ONLY | TT_SAFE (default SAFE_PREEMPT)
sched_kind  = "coop"              # only subkind=scheduler: coop | preempt | tt (ruling R-5)
version     = "0.1.0.0"           # COMPAT_GEN.MAJOR.MINOR.REVISE
summary     = "…"                 # optional
license     = "WTFPL"             # optional

[[res]]                            # may repeat
kind = "ram"                       # ram | stack
used_kib = 48
notes = "…"                        # optional

[sched.tt]                         # only sched_class = "TT_SAFE"
period_us = 1000
deadline_us = 800
wcet_us = 200

[compat]
core       = ">=1.0.0"             # range (3 segments)
api_rev    = 1                     # optional
hash_scope = "decl"                # v0.1 only allows decl
truth      = "decl"                # v0.1 only allows decl (paired with hash_scope)
abi_id     = ""                    # optional placeholder

[[compat.requires_iface]]          # v0.1 only validates schema, does not participate in solving (V-10)
id = "framework/vfs-core#file"
api_iface = "native"
compat_gen = 3
range = ">=1.2.0"
mode = "decl"

[iface.typedefs]                   # alias → canonical name (IFACE-IR rule 4)
u32 = "uint32_t"

[[dep]]
name  = "platform/qemu-aarch64"
range = ">=0.1.0"                  # init/runtime/type all **do not carry compat_gen** (F3)
kind  = "init"                     # init | runtime | type
phase = "early"                    # optional assertion (only kind=init)
symbol = "br_open"                 # optional (only kind=runtime)

[[export]]                         # interface unit = the basic granularity of freezing and versioning
name         = "crypto"            # unit name / registry name
api_iface    = "native"            # must == [plugin].api_type (TAX-0016)
form         = "api"               # api | skin | service
version      = "0.1.0.0"           # human-maintained declared intent; the snapshot is the publication record
compat_gen   = 0
freeze_state = "unfrozen"          # unfrozen | frozen | unfreezing
hash         = ""                  # written by the tool after publish; differs from the snapshot ⇒ IFACE-0012
status       = "experimental"
reexport_of  = []                  # only form=skin: ["<provider>#<unit>", …]
symbols      = []                  # only form=skin

[[export.entries]]                 # members of the surface
kind   = "func"                    # func | var | macro | type | enum | service | symbol-family
name   = "br_crypto_hash"
sig    = "int(const uint8_t*, size_t, uint8_t*)"   # func: types and order, **without parameter names**
layout = "# opaque"                # type
value  = "16"                      # macro/var/enum (enum uses a member list ["A","B"])
ops    = ["open","close"]          # service
status = "experimental"

[privileged]
level = "P2"
[[privileged.memory]]
granularity = "pool"
ops = ["alloc","free"]
regions = ["heap","contig","page"]
[[privileged.resources]]
irq = [32]
dma_channels = [3]
pins = []
device_names = ["hsm0"]

[build]
sources  = ["src/*.c"]             # glob relative to the plugin root (`*` does not cross `/`); a non-existent literal path ⇒ BRV-BLD-0007
includes = ["include"]             # include directories relative to the plugin root
defines  = ["BR_FOO=1"]            # optional: appends -D<name> (field added in this cut, ruling R-17)

[build.target]                     # ★ only plugin_type = "platform" may declare this (exactly one per image, §8.3 ④)
arch          = "aarch64"          # target architecture name (for rendering and artifact naming)
cross         = "aarch64-linux-gnu-"   # cross prefix (transitional form of WORKAROUND br-wa-toolchain-001)
arch_flags    = ["-march=armv8-a", "-mgeneral-regs-only", "-mstrict-align"]  # flags determined by the target
linker_script = "src/link.ld"      # linker script relative to **this plugin's root**

[build.target.qemu]                # how to run the target machine (used by QEMU gates of brickie run / test)
binary  = "qemu-system-aarch64"
machine = "virt,gic-version=3"
cpu     = "cortex-a53"
memory  = "128M"
extra   = ["-nographic"]
```

> `[build]` / `[build.target]` are "only recorded in v0.1, consumed from v0.3" in `brickie-v0.1.md` §7.1;
> this implementation **consumes them early** (S1–S3 of ADR-0004), and adds `defines` and the whole `[build.target]` table —
> gaps and rationale are registered in §9 **R-17**.

### 7.2 `product.toml`

```toml
schema = 1
[product]
name = "hsm"; version = "0.1.0.0"; stage = "dev"    # dev | release (profile default, overridden by CLI)
# Every root is a record (ADR-0035): `brick` = plugin name, `requires` = that brick's own version
# requirement (`{}` = none). app/platform/core are singletons; `[[product.ability]]` is the list
# (one `<alias> = { ... }` per plugin, the alias being a label only).
app      = { brick = "app/hsm", requires = {} }
platform = { brick = "platform/qemu-aarch64", requires = {} }
core     = { brick = "core/c_core", requires = { version = ">=1.0.0" } }
[[product.ability]]
crypto = { brick = "service/crypto", requires = {} }
coop   = { brick = "sched/coop", requires = {} }
# Removed by ADR-0035 (shape error, exit code 2): `[select].plugins` and the bare-string form
# (`app = "app/hsm"`, `core = ">=1.0.0"`).
[budget]
ram_kib = 512
stack_kib = 16
[lint]
frozen_deps = "inherit"            # inherit | allow | deny
allow_edges = [["framework/cdev-core","framework/dev-core"]]
[kernel]
hz = 200                           # tick frequency (Hz; cf. Linux CONFIG_HZ); default (absent) = 200
                                   # ⇒ 5 ms per tick. brickie build turns it into -DBR_CFG_TICK_HZ and passes it to all compilation units (ADR-0017)

[build]                            # ★ product-level build policy: flag tables + artifact locations + core-proper source set (S1 of ADR-0003)
selftest_sources = ["core/c_core/selftest/*.c"]   # appended to the core plugin's unit (product-level switch, ADR-0010/0031)
gen_sources   = ["build/gen/**/*.c"]  # generated-artifact source set (produced by brickie gen); v0.1 may be []
cflags        = ["-std=c11", "-O2"]   # required (empty ⇒ BRV-BLD-0010: the tool carries no default policy)
asflags       = ["-g3"]
ldflags       = ["-nostdlib", "-no-pie"]
obj_dir       = "build/obj"        # optional; default build/obj

[build.release]                    # `--profile release` **appends** after the dev flags
cflags_extra  = ["-DNDEBUG"]       # there are also asflags_extra / ldflags_extra
```

### 7.3 Machine Files

* `api/iface/<provider-short?>/…`: the directory uses the **path segments of the plugin name** (`service/crypto` ⇒ `api/iface/service/crypto/<unit>.toml`).
* The snapshot header must carry `hash_scope` / `truth` and the `NOT_ABI` notice (RV-3/V-11).
* Shapes of `brickie.lock` / `build/index/dependents.json` are in `schema/lock.schema.json` / `schema/index.schema.json`.
* `build/gen/build.mk` / `build/gen/build.ninja` (A/B backends of BR-D4) and `build/gen/build-state.json`
  (incremental state) are **derivatives**: first line marked `brickie:generated`, deletable and rebuildable, not committed to version control.
  Their shape is given by `brickie-core` (`files[]`); L5 only writes them to disk (§4).

### 7.4 Shape of `tests/gates.toml`

Gates of `brickie test` / `brickie build` are declared by this **human-written** table (shape gate = `schema/gates.schema.json`,
BRV-D2 convention: only expresses single-record shape; cross-field decisions are reported as `BRV-BLD-*` by `brickie-core`).

```toml
schema = 1

[run]
default_timeout_s = 5              # optional: default limit for QEMU gates without their own timeout_s

[host]                             # compilation facts for host cases (no cross toolchain/QEMU)
cc      = "cc"                     # optional; default cc
cflags  = ["-std=c11", "-O2"]      # optional
bin_dir = "build/hosttest"         # optional; default build/hosttest

[[hosttest]]                       # host executable case: compile → run → exit code is the criterion
name     = "string-test"
sources  = ["core/c_core/src/string.c", "tests/host/string_test.c"]
includes = ["core/c_core/include", "tests/host"]
defines  = ["BR_HOSTTEST=1"]       # optional
depends  = ["other-hosttest"]      # optional: run it before this case (topological expansion)
timeout_s = 30                     # optional

[[gate]]                           # QEMU gate: run the image with a time limit → judge the log by regex + require case tags
name         = "smoke"
timeout_s    = 3                   # optional (default from [run].default_timeout_s)
log          = "build/logs/smoke.log"  # optional (default build/logs/<name>.log)
require      = ["core MainLoop", "irq_ticks=[1-9]"]   # every one must appear in the log
forbid       = ["\\[PANIC\\]"]                        # hitting any one is red
require_tags = ["TC-IRQ-001"]      # every tag must have a "PASS <tag> " line (a trimmed case also counts as red)
expect       = "timeout"           # optional; default timeout (the image being cut off is the expected result)

[[script]]                         # script gate: run a script → exit code + optional stdout regex
name    = "check-string"
argv    = ["bash", "tools/check-string.sh", "build/obj/core/c_core/src/string.o"]
require = ["ok   compiler supports routines without self-recursion"]
forbid  = []

[[script]]                         # header self-sufficiency gate: each header alone/together/double-included
name    = "check-headers"          #   all must pass `-fsyntax-only`; the criteria are in the script
argv    = ["bash", "tools/check-headers.sh"]

[build]
post = ["check-string", "check-headers"]   # short list run automatically after a successful `brickie build`
```

> **Why `check-headers` deserves a gate**: a stray `*/` in a comment **closes the comment early**,
> and the most common source of that mistake is path globs — `core/c_core/src/sched/*.c`, `platform/*/src/link.ld`,
> `core/c_core/src/sync/**` all contain `*/` (this repo has stepped on it three times). Such an error "compiles" as an illusion:
> a header included by no `.c` file is never compiled at all, and the error only explodes when the first consumer appears ⇒ we must proactively run
> `-fsyntax-only` on every header (alone / all together / double-include).

**Regex subset (declared honestly, to avoid false greens that "never match")**: core **does not pull in a regex library** (§9.3 minimal dependency set),
and supports only `.` `*` `+` `?` `[...]` (with `^` negation and `a-z` ranges) and `\x` escapes; it **does not support**
groups / alternation / anchors `( ) | ^ $ { }` — writing them ⇒ **`BRV-BLD-0012`** (exit code 2), rather than
silently not matching. "forbid for three kinds of FAIL" must be written as three entries, not one alternation regex (see the self-note in `tests/gates.toml`).

## 8. Boundaries of L5 (Python)

**Allowed**: argparse / locating binaries / invoking subprocesses / reading and writing files / JSON serialization / text rendering /
**single-record shape** validation per `schema/*.json` (BRV-MF-0001, exit code 2) / exit-code aggregation.

**Forbidden** (the V-18c static check catches these):
* any decision about closure / range intersection / topology / cycles / phase monotonicity / taxonomy / privilege / budget;
* four-segment version advancement and `range` parsing;
* IFACE-IR normalization / hash / change set;
* content assembly of any "machine file" (including lock, snapshots, CHANGELOG, index).

**Rendering discipline**: text output and `--json` are two serializations of the **same `data`** (§BRV-D2 discipline 2).

## 9. Implementation-time Rulings (R-*: the spec has a gap or two readings; the implementation picks one first and leaves a switch point)

| # | Gap / ambiguity | Adopted | Rationale / switch cost |
|---|---|---|---|
| R-1 | What `brickie check` does when there is no `product.toml` | **Validate the whole plugin tree** (per plugin + in-tree edges); product-level quantity constraints are skipped | V-1 requires "generated plugins pass check immediately"; with a product, judge the full closure |
| R-2 | §7.7 says `dep add/rm` "writes `brick.toml`", §9.1.1 rule 1 says human-written files are **never rewritten** | Take **§9.1.1** (the later explicit decision): only **print** the snippet, do not write to disk | Writing the file would erase comments and violate the single source of truth; switch cost = adding `--write` |
| R-3 | `ver bump <plugin>` (§7.7) vs the version carrier being the **unit snapshot** (§9.1.1 rule 2) | The argument accepts **a plugin name or `<provider>#<unit>`**; a plugin name requires exactly one `[[export]]`; writes the snapshot and reports "what should be changed in `brick.toml`" as info | The semantics of human-written vs publication record do not mix |
| R-4 | The concrete shapes of §6.1 `value`/`ops` for `macro`/`var`/`enum`/`service` | `value` = string or string array; `ops` = string array | Both must enter the hash (rule 9) |
| R-5 | The scheduler policy class `sched_kind` has no field, while §2 item 3 requires judging `COOP_ONLY` conflicts | Add optional `[plugin].sched_kind` (only `subkind=scheduler`); default inferred from the name (`sched-coop`/`sched-preempt`/`sched-tt`) | Otherwise that item cannot be enforced; gap registered |
| R-6 | `platform provisioned amount` (§8.1 note) has no field | For a `plugin_type="platform"` plugin, its `[[res]]` is treated as **capacity** | No new field; gap registered |
| R-7 | Within the unfreeze window there is no baseline for "were frozen entries changed/deleted" | `unfreeze` writes a `baseline` (surface hash + entry table) into the snapshot; `refreeze` compares against it | Without a baseline that decision has no input (gap registered) |
| R-8 | Semantics of `publish`'s `--set` (in `§6.5` only for `ver`?) | `iface-publish --set <4 segments>` overrides **the unit version to be written this time** (still subject to monotonicity) | — |
| R-9 | Resource exclusivity conflicts / budget overruns / scheduling conflicts have no dedicated code | Use `BRV-MF-0001` (declaration-surface error; the message names **both conflicting parties / the delta**) | Code unassigned by BRV-D8; gap registered, code pending |
| R-10 | Default of `dep-add --range` | `*` (still: `compat_gen` constraints do not apply — structural dependencies do not pin a generation) | — |
| R-11 | Snapshot directory: is `<provider>` the full plugin name or a short name | Use the **path segments of the full plugin name** (`service/crypto` ⇒ `api/iface/service/crypto/`), unit file name = `<unit>.toml` | Consistent with `<provider>/<unit>` in §5.6 |
| R-12 | Timestamp-free `CHANGELOG.md` | Append by "version section", section header = `<unit> v<4 segments> (compat_gen=<N>)`, no date | Byte-for-byte reproducible (§4) |
| R-13 | `brickie-v0.1.md` §0 boundary discipline says "in v0.1 no command **may require cc/cargo/nm to be present**", while the build family **must** have a cross compiler | The accurate statement of the boundary discipline is: **composition-time** commands (check / dep / iface / gen / new) must not require a compiler; the build family (build / clean / run / size / disasm / test / judge) is an **explicit second family**, and it is the family that requires the toolchain | One sentence covering both families is self-contradictory; the **purpose** of the discipline (zero compilation dependency for CI composition-time jobs) is unaffected. See `tools/brickie/README.md` §1 and ADR-0004 |
| R-14 | Tool candidate order (`gcc` → `gcc-16` → …) and "which compiler to use" — who decides | **Both the candidate order and resolution are in core** (`data.tools[i].candidates/resolved`); L5 only does the final executability check and does not re-select | The make / ninja backend files are **generated artifacts** and must **bake in** the resolution result — if L5 or the generated artifact each resolved once, "who is compiling" would have two answers |
| R-15 | Incremental criteria: "content stamp of the input set" or "argv fingerprint + input mtime" | **argv fingerprint + input (size, mtime)**, two criteria (same family as make / ninja); an upstream needing rebuild ⇒ explicitly passes the rebuild downstream | `.d` dependency files are a **product of the build** (they exist only after the first compile) ⇒ stamping the "input set" would make header dependencies recompile one extra round forever (measured); the timestamp criterion has no such self-reference |
| R-16 | Who owns the red/green criteria of gate logs (require / forbid / PASS tag) | **The criteria are in core** (the `judge` command judges by the regexes in `tests/gates.toml`), L5 only collects the log body and executes | "Red/green" can have only one source of truth; otherwise text output, `--json` and the CI exit code would each judge once |
| R-17 | `brick.toml [build]` / `product.toml [build]` have only `sources`/`includes` in `§7.1`, which cannot express the full compilation facts | Add `[build].defines` (plugin level) and, for `product.toml [build]`, `gen_sources`/`selftest_sources`/`cflags`/`asflags`/`ldflags`/`obj_dir`/`[build.release].{cflags,asflags,ldflags}_extra` (`core_sources`/`core_includes` were retired by ADR-0031: the core is a plugin and declares its own sources), plus the whole platform `[build.target]`/`[build.target.qemu]` | "How to compile" must be writable in **one** declaration surface (otherwise we are back to Makefile literals); the shapes are added to `schema/*.schema.json` and §7.1/§7.2, pending back-feed into design §7.1/§7.2 |
| R-18 | How product-level **semantic configuration** (first item = tick HZ) enters compilation | `product.toml [kernel].hz` (integer, default 200) is turned by `brickie build` into `-DBR_CFG_TICK_HZ=<hz>` and passed to **all** compilation units (core/plugins/generated); this define is separate from generic `cflags` and uses the `config_defines` channel | Do not write bare `-D` into `cflags`: a semantic key can be validated by `schema/product.schema.json` (`integer` + `minimum: 1`), and "how many timer interrupts per second" has only one source of truth (core's jiffies and the platform's reload period read the same macro; ADR-0017) |
| R-19 | How declared dependencies become **compile-time facts** (previously `-I` was a global union, and undeclared cross-plugin `#include` silently compiled) | The include surface of each `build` unit = the selected core plugin's `[build].includes` + its own + the includes of every plugin in the **transitive closure of declared dependencies** (the core unit additionally gets platform headers; `build/gen` gets the headers of all selected plugins); plus a **declaration ↔ use** cross-check (scan source `#include`s, map to the provider, report if outside the declaration closure); the deps domain of `check` uses the same convention as `build` | Narrowing `-I` alone is not enough: the target toolchain's glibc headers would cover common names like `unistd.h` (not "header not found" but silently switching to the host libc declarations) ⇒ a criterion independent of the compiler search path is required; both places share `model::cross_plugin_includes()` to avoid divergence (ADR-0018) |

| R-20 | What "core is a dependency" means: does using `<br/core/**>` need a declaration? | **Yes.** Since ADR-0038 core is a first-class interface provider (`core/c_core` exports 20 units, one per header) and the outward door is the skin `runtime/core`. Every consumer declares `[[dep]] runtime/core` (platform declares `core/c_core` directly: the direction table forbids a platform from depending on an ability). `brickie build` no longer prepends core's `-I` unconditionally, so a missing declaration fails at compile time | Without it core is "dark matter": its surface is the only one that never has to be declared, and a future split/re-implementation of core has no boundary to move behind |
| R-21 | Dependency authorization: declared **transitive** closure or **direct** set? | **Direct only**, plus the units a directly declared skin re-exports (transitively: `api → iface/min → runtime/core → core/c_core`). Text (`BRV-DEP-0012` on `#include`) and symbols (`.a` audit) use the same set. `-I` keeps the **transitive** closure (compile surface), so the declaration is the compile prerequisite of *using* it, not of *seeing* it | Using C through B is an implicit dependency on C; supersedes the authorization half of R-19 (its other half — the declaration↔use cross-check shared by check/build — stands) |
| R-22 | The core plugin is platform-agnostic (`br/platform/**` contract) but the direction table forbids `core → platform` | The exception is explicit and one-sided: core may include `br/platform/**` and use the symbols declared there without naming a platform plugin; the platform side still declares `[[dep]] core/c_core`. Implemented identically in `check --deps`, `-I` surfaces and the symbol audit | The two call each other by design (`platform`'s entry is `bl br_core_main`, core reads `br_plat_ticks_*`); the cycle is broken by the two-step boot chain, not by the topological sort |

> **Supplementary rulings on the Rust side**: implementing the solving/validation/interface engine forced a batch of finer conventions
> (S-1…S-19: first publish = create the record without advancing segments, `closure` does not scan `requires_iface` while `check` does,
> platform's `[[res]]` as capacity does not enter the consumer Σ, the criteria of `DEP-0011`, the value convention of R2,
> the `app → third_party ability` exception, publish inside the window consuming the baseline to avoid a double bump, etc.),
> and the build family has **S-B1…S-B6** (tool resolution in core / incremental criteria / mapping of the three backends to BR-D4 /
> rejecting the regex subset rather than silently matching / ownership of gate criteria / ownership of `build/gen/**` artifacts).
> These are **engine-internal conventions**, registered one by one in `rust/README.md` §10; the R-* here are their parent list.
>
> ⚠ **Numbering note**: the history table in `rust/README.md` §10 uses its own R-1…R-16 numbering
> (a **different set** from the R-* here); R-13…R-16 here are the parent rulings of the build family. The two await unification when back-fed into the design.

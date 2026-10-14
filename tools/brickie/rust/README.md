# brickie-core — L0/L1 Rust core (`prototype/tools/brickie/rust/`)

> `brickie check` is a **declaration-surface completeness checker**: it guarantees a combination is **logically** self-consistent, not that it "compiles / runs correctly".
> This crate is the **single place of judgement**: no business rule for taxonomy / version / range / privilege / generation plan may stay in L5 (Python). Authoritative interface = [`../docs/contract.md`](../docs/contract.md).

**The v0.1 command surface is fully wired** (23 leaf commands + `version` / `model`): the first cut delivered `version` / `model` / `plan-new` / `plan-init` / `gen-plan` / `dep-add` / `dep-rm`; the second cut (agent-B) added `check` / `closure` / `dep-tree` / `dep-graph` / `dep-why` / `dep-index` / `ver-show` / `ver-bump` / `iface-list` / `iface-show` / `iface-diff` / `iface-status` / `iface-publish` / `iface-freeze` / `iface-deprecate` / `iface-undeprecate` / `iface-unfreeze` / `iface-refreeze`. Command-surface status is in §3, implementation-time rulings in §7/§10, field alignment with L5 in §12.

---

## 1. Build and self-verification

Toolchain threshold: `Cargo.lock` is **version 4** ⇒ requires `cargo ≥ 1.83` (older cargo only reports `lock file version 4 requires -Znext-lockfile-bump`). The repo entry point spares you the choice: the selection order in `../Makefile` is "explicit `CARGO=…` → the workspace's own `.toolchain` (`<repo>/.toolchain` or `<repo>/../.toolchain`) → PATH", and if the selected cargo is too old it **FAILs on the spot** and gives the fix.

```sh
cd prototype/tools/brickie
make core                      # = cargo build --release + copy to build/host/<triple>/bin/
```

When `$HOME` is read-only you **must** set a writable `CARGO_HOME` (`make core` already points both CARGO_* at the out-of-tree directory):

```sh
cd prototype/tools/brickie/rust
export CARGO_HOME=$PWD/../../../build/cargo-home   # same location as make core
mkdir -p "$CARGO_HOME"
cargo build --release          # artifact: rust/target/release/brickie-core
./target/release/brickie-core --selftest
```

- Dependencies (minimal dependency set): `toml = "0.8"`, `serde_json = "1"`, `sha2 = "0.10"`; clap / serde derive are **not introduced**. `Cargo.lock` is generated and **committed**.
- Recorded self-verification (passed on this machine, re-tested at the end of the second cut):
  - `CARGO_HOME=… cargo build --release --locked` ⇒ `Finished release ... in 3.9s`, **0 error / 0 warning** (full rebuild, not a cache hit).
  - `brickie-core --selftest` ⇒ `ok 432 cases`, exit code 0 (first cut 247 ⇒ second cut +185 assertions).
  - `echo '{"protocol":1,"command":"version","root":"."}' | brickie-core` ⇒ `exit_code:0`, `data.languages[0].version = "1.93.1"` (that field **follows the rustc that built it**, now determined by the workspace `.toolchain` 1.99.0, see `prebuilts/README.md` §5.1).
  - End to end (real L5 CLI, `BRICKIE_CORE` pointing at this cut's artifact): create a cycle ⇒ full path + exit 1; `iface publish` twice ⇒ second run `files=[]`; `iface status --check` consistent; `check --profile dev` green / `release` red; `unfreeze`/`refreeze` empty unfreeze ⇒ four segments unchanged.
- Re-tested 2026-10-12 (after switching to the workspace's own rustc 1.99.0): `make tools-prebuilt` green (host g++ 13.3 + cargo 1.99.0, auto-discovered by `make core`), the three seed files re-issued, and `make tools-prebuilt-check` all three ok; the entry ELF can still run standalone as a **single file** (`prebuilts/seed/brickie/x86-64/linux/bin/brickie --version` ⇒ `brickie 0.1.0`).
- **Drop point**: this cut's artifact is `rust/target/release/brickie-core` (`rust/.gitignore` already ignores `target/`); per G-6 it should go **out of tree** to `build/host/<host-arch>/<host-os>/bin/brickie-core`.

## 2. Module map

| File | Line scale | Responsibility |
|---|---|---|
| `main.rs` | 235 | envelope entry / command dispatch / `--version` / `--help`; process exit-code discipline |
| `proto.rs` | 182 | request↔response envelope, protocol validation (`BRV-PROTO-0001`), `internal_error` / `unimplemented` |
| `diag.rs` | 249 | diagnostics `{code,severity,target,file,span,message,hint}`, severity, exit-code aggregation, stable sort by `(code,file,span,target)` |
| `rules.rs` | 657 | name contract / namespace enum / `subkind` derivation / phase derivation and constraints (ADR-0037: app ⇒ `app`, i.e. last of all init) / §3.4 privilege levels and the `(ops×granularity×region)` orthogonal table / §7.3 dependency-direction table + `api_type` prohibitions + `allow_edges` / §3.5 export-surface invariants / template availability |
| `version.rs` | 413 | four-segment version parse/serialize/display/compare; `range` parsing + `^`/`~` expansion at parse time + intersection + satisfiability; `compat_gen` exact match; §5.2 event→segment advance; `BRV-VER-0007` |
| `model.rs` | 2674 | typed model for `brick.toml` / `product.toml` + loader (tree scan/skipped dirs/unique names); **selection records** `{ brick, requires }` and their shape gates (ADR-0035: `[product].app/platform/core`, `[[product.ability]]`, unknown-key rejection, the pre-0035 forms are shape errors); cross-field invariants within one record (TAX-0015..0019 / PRIV-0001/0002 / IFACE-0011); read-only `Snapshot` (`api/iface/**`, incl. `baseline_*`/`strict_params`) and `Lock` (`brickie.lock`); `DependentsIndex` |
| `emit.rs` | 329 | **deterministic** TOML/JSON emission primitives (`TDoc`/`TTable`/`TValue`) + `render_lock` / `render_dependents` / `render_snapshot` (incl. baseline / strict_params) |
| `plan.rs` | 627 | `plan-new` / `plan-init` / `gen-plan`; variable expansion (**both lower- and upper-case placeholder sets**); conflict decision (`BRV-GEN-0002`) |
| `dep.rs` | 203 | **snippet generation** for `dep-add` / `dep-rm` (ruling R-2, does not write `brick.toml`) |
| `solver.rs` | 1282 | ✅ `closure`/`dep-tree`/`dep-graph`/`dep-why`/`dep-index` + **pure-function closure solving** (`solve`: selection roots (`app`/`platform`/`core` records + `[[product.ability]]`) → per-record verdicts (`BRV-TAX-0024` kind vs `plugin_type`, `requires.version` vs the brick) → three edge classes closure → `compat_gen`/`range` → topology + cycles (structured paths) → phase monotonicity → direction prohibitions → budget total → profile verdict) |
| `check.rs` | 518 | ✅ `check`: aggregates the four domains (`deps`/`iface`/`tax`/`priv`) + `scope_results` + `summary`; reuses `solver::solve` |
| `iface.rs` | 2331 | ✅ `ver-show`/`ver-bump`/`iface-*`: **IFACE-IR canonicalization + sha256 hash**, change set (append vs modify), impact report, version advance, idempotence, unfreeze window + baseline, machine-file emission |
| `show.rs` | 442 | ADR-0036/0037: `show-bricks` (closure as a reading aid) + `show-init-flow` (**the runtime boot chain**: section-order FIFO Kahn, platform-first EARLY, category-decided CORE/LATE/**APP**, IRQ hand-off after *all* init, START with APP last, selftest pass) |
| `selftest.rs` | 3150 | `--selftest`: domain cases (no Python involved), 577 entries |

## 3. Command implementation status

| `command` | Status | Key `data` fields | Notes |
|---|---|---|---|
| `version` | ✅ | `{version,protocol,languages,deps}` | `languages=[{name:"rust",version:<rustc>}]`; `deps` = the three direct dependencies |
| `model` | ✅ | `{plugins:[Plugin],count,product,has_product,platform_capacity}` | `args.plugin?` filter; shape errors reported as a side effect |
| `plan-new` | ✅ | `{artifacts:[{path,kind,template,vars}],planned,count}` | two-phase: without `context.existing` = plan first; with it = conflict decision |
| `plan-init` | ✅ | same + `lock_content` | see deviation D-3 |
| `gen-plan` | ✅ | same | one `build/gen/<plugin>/plugin_desc.c` per plugin |
| `dep-add` | ✅ | `{snippet,path,span,found}` | ruling R-2: print only, do not write to disk |
| `dep-rm` | ✅ | `{snippet,path,span,found}` | same; `found=false` means not located |
| `check` | ✅ | `{summary:{errors,warnings,infos,profile,closure:{plugins,ram_kib,stack_kib}},scope_results,scopes}` | `args.profile` / `args.scopes[]` / `args.plugin?` (filter by diagnostic target, see S-7) |
| `closure` | ✅ | all contract §5.1 fields + `cycles:[{path,edges}]` + `selected`/`seeds`/`has_platform` | **does not scan `requires_iface`** (V-10② byte-reproducible, see S-1) |
| `show-bricks` | ✅ | `{product,has_product,bricks[{name,namespace,plugin_type,api_type,subkind,phase,version,ram_kib,stack_kib,selftest,origin,root_kind,root_alias}],totals{plugins,roots,ram_kib,stack_kib,budget,platform_capacity,has_platform,by_type},seeds}` | ADR-0036 §2.4: a reading view, no new verdicts; `origin` is the product record, not the edge graph |
| `show-init-flow` | ✅ | `{product,steps[{phase,plugin,plugin_type,subkind,hook,note}],counts,order,order_basis,unsorted,selftest_on}` | **the runtime boot chain** (ADR-0036 §2.1): `order` = section-order FIFO Kahn, **not** `closure.topo_order`; validated event-for-event against a real `build/logs/smoke.log` (88/88) |
| `dep-tree` | ✅ | `{roots,nodes,kind}` | `args.kind` = init/runtime/type/all; `args.plugin?` |
| `dep-graph` | ✅ | `{format,nodes,edges,text}` | dot/mermaid/json; with `json`, `text` is a canonical JSON string |
| `dep-why` | ✅ | `{found,path,edges}` | shortest consumer→provider path; not found ⇒ `found=false` and **exit 0** |
| `dep-index` | ✅ | `{dependents:{<plugin>:[{from,kind}]}}` + `files:[build/index/dependents.json]` | deterministic order; no timestamp |
| `ver-show` | ✅ | `{id,plugin,version,compat_gen,segments,freeze_state,status,source,compat_gen_source,published,not_abi,hash_scope,truth}` | RV-11's "generation does not participate in comparison" is reported as info |
| `ver-bump` | ✅ | `{id,plugin,unit,rule,from,to,version,segments,compat_gen,file}` | writes snapshot + CHANGELOG; R-3's "what `brick.toml` should become" is reported as info |
| `iface-list` | ✅ | `{units:[{id,provider,unit,api_iface,form,version,compat_gen,freeze_state,status,published,hash,hash_short,file}],count}` | declaration surface ∪ snapshots |
| `iface-show` | ✅ | `{id,unit,entries,source,not_abi:true,truth,hash_scope,declaration_hash,snapshot_hash,published}` | V-10③ / V-11①③ |
| `iface-diff` | ✅ | `{id,changes,verdict,compat_gen_changed,version:{from,to,reasons},dependents:{direct,transitive,unsatisfied},freeze_state,published}` | §6.3 |
| `iface-status` | ✅ | `{id,recomputed_hash,snapshot_hash,declaration_hash,plugin_toml_hash,consistent,check,expected_snapshot,expected_snapshot_path}` | V-11④'s `BRV-IFACE-0012`; the gate does not write to disk (S-12) |
| `iface-publish` | ✅ | `{id,published,noop,verdict,from,to,version,compat_gen,compat_gen_changed,hash,strict_params,changes,version_change,report:{dependents},files_planned}` | writes snapshot + `CHANGELOG.md` + `brickie.lock`; unchanged surface ⇒ `noop:true` + `files:[]` |
| `iface-freeze` | ✅ | `{id,entry,proposal,freeze_state,status,compat_gen}` | **A-26 exception**: writes only `build/gen/proposals/<unit>.toml` |
| `iface-deprecate` / `iface-undeprecate` | ✅ | `{id,entry,status,freeze_state,compat_gen,from,to,version,changes,files_planned}` | state machine + idempotence (S-9) |
| `iface-unfreeze` | ✅ | `{id,freeze_state:"unfreezing",status,compat_gen,version,baseline_hash,note,files_planned}` | requires `--note` + `frozen`; baseline recorded in the snapshot (R-7) |
| `iface-refreeze` | ✅ | `{id,freeze_state:"frozen",status,from,to,version,compat_gen,compat_gen_changed,empty_unfreeze,changes}` | empty unfreeze ⇒ all four segments unchanged (V-16) |
| unknown name | ✅ usage error | `{}` | `exit_code 2` + `code:null` |

## 4. Envelope, diagnostics and exit codes

- Request `{protocol, command, root, args, context}`; response `{protocol, status, exit_code, diagnostics, files, data}`. stdin is read **in full**, stdout gets **one line** of JSON.
- `protocol != 1` (including missing) ⇒ `BRV-PROTO-0001` + `exit_code:2` + `status:"ok"` + **process exit code 0**.
- **The process exit code is always 0** (except `status:"internal_error"` and crashes, both 2): business conclusions live only in `exit_code`.
- Exit-code aggregation: explicit override (2) > any `error` (1) > 0. Diagnostics of class `usage`/`env`/`shape` set the override to 2.
- The `--json` diagnostics array is **stably sorted** by `(code,file,span,target)`; code-less diagnostics come first.
- **JSON key order**: serde_json uses `BTreeMap` by default ⇒ object keys are emitted in lexicographic order. **Byte-exact snapshot tests must know this**; to be byte-identical to the key order of contract examples you need to reassemble on the L5 side.

## 5. Reusable interfaces (the first cut's entry points for the second cut, all now called)

### 5.1 Dispatch conventions

`main.rs`'s dispatch (fully wired by the second cut):

| Module | Commands |
|---|---|
| `check::run` | `check` |
| `solver::run` | `closure` / `dep-tree` / `dep-graph` / `dep-why` / `dep-index` |
| `iface::run` | `ver-show` / `ver-bump` / `iface-*` |

### 5.2 Ready-made primitives, **please reuse**

| Need | Primitive |
|---|---|
| range matching / intersection / conflict | `version::Range::{matches,intersect,is_satisfiable,canonical}` |
| `compat_gen` exact match | `version::compat_gen_matches` |
| §5.2 event→segment advance | `version::{Event,advance}` (right segments zeroed, `COMPAT_GEN` not zeroed) |
| `BRV-VER-0007` | `version::check_no_regress` |
| dependency direction / api_type prohibitions / `allow_edges` | `rules::dep_edge_violation` (+ `dep_direction_ok` / `api_type_edge_reason`) |
| phase monotonicity (R1) | `rules::phase_rank` + `Plugin.phase` |
| §3.4 orthogonal table | `rules::memory_combo_legal` / `min_level_for_memory_op` / `check_privileged` |
| export-surface invariants (single record) | `rules::check_export_invariants` |
| tree loading (incl. shape diagnostics) | `model::load_tree` → `TreeLoad{diags,plugins,product,has_product}` |
| **ruling R-6 platform capacity** | `model::TreeLoad::platform_capacity()` — `platform`'s `[[res]]` becomes `totals.platform_capacity` (see §10 S-14: it does **not** enter the consumer-side Σ) |
| **ruling R-1 no-product branch** | `model::TreeLoad::product_level_constraints_apply()` / `has_product` |
| §7.5 profile strictness | `model::Product::release_strict()` (`[lint].frozen_deps` overrides `stage`) |
| reverse dependency index | `model::DependentsIndex::build(&tree)` + `emit::render_dependents` |
| snapshot / lock read-only | `model::{load_snapshots,load_lock}` |
| deterministic TOML emission | `emit::{TDoc,TTable,TValue,toml_string,render_lock,render_snapshot}` |
| per-plugin placeholders | `plan::{PluginFacts,plugin_vars,facts_of,plugin_paths}` |
| segments / hash | `sha2` is already a dependency; IFACE-IR domain separation in `iface::HASH_REV` (rule 10) |
| **closure solving (pure function)** | `solver::solve(&tree, profile, scan_iface) -> SolveResult` (diagnostics carry domain labels) |
| **IFACE-IR / hash** | `iface::{canonical_surface_text,surface_hash,short_hash,Entry,Surface}` |
| **change set / impact report** | `iface::{change_set,classify_modify,Change,dependents_of,Dependents}` |

### 5.3 Codes **not to report twice**

`model::load_tree` already reports the following for **every record** at load time: `BRV-MF-0001` (shape), `BRV-VER-0005/0006` (version strings), `BRV-IFACE-0011` (truth/hash_scope), `BRV-TAX-0013` (plan-new only), `BRV-TAX-0015/0016/0017/0018/0019`, `BRV-PRIV-0001/0002`.

`check.rs` does **not** report the same plugin/export surface again; it only reports the **cross-record/cross-file** part: cycles and topology (`BRV-MF-0001` + full edge paths), phase monotonicity (`BRV-DEP-0009/0010`), single-version policy (`BRV-DEP-0011`), `compat_gen`/`range` (`BRV-VER-0001/0002/0004`), budget and exclusive-resource conflicts (`BRV-MF-0001` + both sides/difference), release gate (`BRV-IFACE-0009`), the **cross-file classification equality** of export invariant 3 (`BRV-TAX-0018`).

### 5.4 Rules implemented by the second cut (**deliberately not done** in the first)

- **Cross-plugin** enforcement of the dependency-direction table (call site of `dep_edge_violation`) + the `api_type` exemption for `skin` re-export edges.
- Cycle detection: init cycle ⇒ hard error + **structured full path**; runtime/type cycle ⇒ info only.
- Phase monotonicity R1/R2 (for the R2 convention ruling see §10 S-6).
- `sched_class` combination-time conflicts (`TT_SAFE` missing `[sched.tt]`; `COOP_ONLY` × non-coop scheduler; `sched_kind` see ruling R-5).
- Budget totals (product `[budget]` + platform capacity) and IRQ/DMA/pin/device-name exclusivity conflicts.
- The cross-file part of export invariant 3 (the unit named by `reexport_of` exists and has equal classification) + unit-name/symbol-family collisions.
- IFACE-IR canonicalization + `sha256` + change set + impact report + unfreeze window.

## 6. Machine file shapes

> The shapes are **authoritative in `schema/*.schema.json`** (verified with `jsonschema` that `plan-init`'s lock passes `lock.schema.json`). Generated files uniformly carry `# brickie:generated` on the first line (see `emit::GENERATED_MARKER`).

### 6.1 `brickie.lock` (TOML, `emit::render_lock`, shape = `schema/lock.schema.json`)

```toml
# brickie:generated
# brickie.lock — machine-owned file, do not edit by hand; two runs on the same input are byte-identical (contract §4)

schema = 1

[lock]
profile = "dev"

[lock.product]           # extension of this implementation (top level / lock both additionalProperties:true)
name = "hsm"
version = "0.1.0.0"
stage = "dev"

[[lock.plugins]]
name = "service/crypto"
version = "0.1.0.0"
compat_gen = 0

[[lock.units]]
id = "service/crypto#crypto"
provider = "service/crypto"
unit = "crypto"
api_iface = "native"
version = "0.1.0.0"
compat_gen = 0
hash = "sha256:…"
hash_scope = "decl"
truth = "decl"
```

`[[lock.plugins]]` is sorted by plugin name and `[[lock.units]]` by unit id (the caller guarantees this); no timestamp / randomness / host info ⇒ byte-reproducible. `model::load_lock` tolerates missing fields (v0.1 weak validation: no top-level required).

### 6.2 `build/index/dependents.json` (`emit::render_dependents`, shape = `schema/index.schema.json`)

```json
{"schema": 1, "dependents": {"service/crypto": [{"from": "app/hsm", "kind": "runtime"}]}}
```

keys are sorted with `BTreeMap`; edges are built only from **declared** `[[dep]]` (runtime registry lookups produce no edge).

### 6.3 Snapshot reading `api/iface/<provider path segments>/<unit>.toml`

`model::load_snapshots` reads **tolerantly**: fields may be flat or under `[unit_meta]`; entry table names accept `[[entry]]` / `[[entries]]` / `[[export.entries]]`; it also reads `strict_params` (rule 10), `baseline_hash` and `[[baseline_entry]]` (the unfreeze baseline of ruling R-7). `provider` is derived from the relative path (`api/iface/service/crypto/crypto.toml` ⇒ `service/crypto`), `id = <provider>#<unit>` (ruling R-11). `api/iface` missing ⇒ empty table + no diagnostics = "not published"; `brickie.lock` missing ⇒ `None`.

### 6.4 Machine files added by the second cut

| File | Written by | Shape |
|---|---|---|
| `api/iface/<provider path segments>/<unit>.toml` | `iface-publish`/`ver-bump`/`iface-{un,re}freeze`/`iface-{de,un}deprecate` | `emit::render_snapshot` (incl. `NOT_ABI` / `hash_scope` / `truth` / `strict_params`, optional baseline) |
| `api/iface/CHANGELOG.md` | same | **version-section append** (section header `<unit> v<4 segments> (compat_gen=<N>)` + hash + change summary), **no timestamp** (R-12) |
| `brickie.lock` | `iface-publish` (and writing surfaces other than `ver-bump`) | `emit::render_lock`; `units` = all published snapshots + this update (sorted by id) |
| `build/gen/proposals/<unit>.toml` | `iface-freeze` | pending promotion proposal (A-26), does **not** write a frozen snapshot and does **not** bump `COMPAT_GEN` |
| `build/index/dependents.json` | `dep-index` | `emit::render_dependents` |

## 7. Key implementation-time rulings / deviations from the cxx first cut

| # | Item | This implementation | Basis |
|---|---|---|---|
| D-1 | `[plugin].phase` allowed set | `interface` / `ability.service` ⇒ `{early,late}` (giving `core` ⇒ error); `platform` / `ability(other)` ⇒ `{early,core}`; `app` ⇒ `{early,core,app}`. **`early` is always legal** = no `init` hook | parent-agent clarification + §7.2; case in point: the init edge `platform/qemu-aarch64(phase=core)` × `sched-coop(phase=core)` does **not** report `BRV-DEP-0009` |
| D-2 | phase derivation defaults | `platform`/`ability(non-service)` ⇒ `core`; `ability.service`/`interface` ⇒ `late`; `app` ⇒ `app`; no `init` ⇒ `early` | same. **Note**: the cxx first cut derived `scheduler` as `early`; this implementation derives `core` per §7.2 (consistent with the parent-agent clarification) |
| D-3 | `plan-init`'s `brickie.lock` | the plan carries `{path:"brickie.lock", kind:"machine", template:""}`, with the text separately in `data.lock_content` | contract says plans produce no content, but lock is a **core-owned machine file** (§4); keeping both makes it easy for L5 to write it directly |
| D-4 | template placeholders | provide **both** lower-case (`{{name}}`) and upper-case (`{{PLUGIN_NAME}}`) variable sets | current `templates/` use lower case, the cxx first cut used upper case; extra variables have no side effect on rendering |
| D-5 | `descriptor_include` | `"<short>/<short>.h"` | the descriptor template `#include "{{descriptor_include}}"` has no spec definition; this is the only reasonable reading (the plugin's own public header) |
| D-6 | codes for unknown enums / name contract | always `BRV-MF-0001` + `exit_code 2` | BRV-D8's `MF-0001` explicitly covers "plugin name violates the name contract" (G-2 closure); R-9 catch-all |
| D-7 | `lang` not delivered | **code-less usage error** + `exit_code 2` (`cxx`/`rust`) | BRV-D8 has no corresponding code; registered as gap R-4 |
| D-8 | duplicate `[[dep]]` | **not reported** | no assigned code; borrowing `BRV-MF-0001` would raise a benign notice to exit code 2 (gap R-14) |
| D-9 | `gen-plan`'s "header files" | emits **descriptors** only | §8.4's `include/<short>/<short>.h` is a **hand-written** file in v0.1; generating headers is scheduled for v0.3 (gap R-13) |
| D-10 | missing `{{descriptor_include}}` etc. in the cxx first cut | this implementation fills in all current template placeholders | so that a `plan-new` plan can be rendered directly by `brickie-gen render` |
| D-11 | degree of loader alignment with the schema | **field-by-field alignment** with `schema/plugin.schema.json`: `[compat].core`, `export.version/compat_gen/freeze_state/status` are **required when present**; `[[privileged.resources]]` is read and accumulated as an **array table**; `value` accepts `string \| string[]` (arrays canonicalized with `,`, ruling R-4); `product.version` is **optional** per the schema (default `0.0.0.0`) | the schema is the shape authority; L5 rejects first, but core must also report independently (V-18a) |
| D-12 | whether machine TOML carries the `brickie:generated` marker | yes (`emit::machine_header`); the JSON index does not (JSON has no comments) | same convention as `brickie-gen` / existing `.scratch` machine files, so "is this our artifact" is easy to tell |
| D-13 | whether `dep closure` scans `requires_iface` | **no** (`scan_iface=false`); only `check` scans | §10 V-10② requires "adding/removing the field leaves the closure byte-identical"; V-7's `compat_gen` conflict is reported in `check --deps` (see §10 S-1) |
| D-14 | whether `closure`'s `plugins[]` contains closure members only | lists **all plugins of the tree** with a `selected` boolean (plus a `selected` name array) | contract §5.1 has a `selected` field ⇒ it must be able to express "excluded"; counting uses `summary.closure.plugins` |
| D-15 | whether `iface-status` writes the snapshot back | **produces no files** (gate); the expected snapshot goes in `data.expected_snapshot` | "status is a CI gate, not a repair command"; see §10 S-12 |
| D-16 | `dep-why` finds no path | `found:false` + `exit 0` | "not found" is a query result, not validation red |
| D-17 | do `publish` and `refreeze` inside a window both bump `COMPAT_GEN` | **bump once only**: `publish` records the change and **consumes the baseline** (the new surface becomes the new commitment surface), `refreeze` just closes the window ⇒ empty unfreeze | see §10 S-19; avoids "publish + refreeze double bump" |

## 8. `--selftest` coverage (V-18a, no Python involved)

`ok 432 cases` (first cut 247 + second cut 185), covering:

1. **Version matrix row by row** (V-4 a–f): `COMPAT_GEN+1` clears minor/revise only, `MAJOR+1` leaves `COMPAT_GEN` alone, add/enum append/status transition ⇒ `MINOR+1`, bug fix ⇒ `REVISE+1`, empty unfreeze ⇒ all four segments unchanged.
2. **Version strings**: 4-segment parse/serialize/`v` display, 3 segments ⇒ `0005`, `compat_gen` exact match, `0007` regression.
3. **range**: `=`,`>=`,`>`,`<=`,`<`,`~`,`^`,`*`, missing segments right-padded with 0, 4 segments ⇒ `0006`, intersection + satisfiability (open/closed at the same point).
4. **Name contract and subkind**: legal/illegal names, namespace extraction, recommended form, `derive_subkind`.
5. **Phase**: ranks, derivation defaults, allowed sets (incl. the `app+app` case and the `service+core` counterexample).
6. **Privilege**: per-level caps, orthogonal-table positives/negatives, three counterexample groups for `BRV-PRIV-0001/0002`.
7. **Dependency direction**: three hard prohibitions + `api_type` prohibitions + `allowedges` exempting only the direction table.
8. **Export-surface invariants**: `TAX-0016/0017/0018/0019` + the skin multi-provider positive case.
9. **TOML model loading**: positive cases (incl. `[[privileged.resources]]` array tables, `value` string arrays, R-6 platform capacity), missing name, 3-segment version, 4-segment range, incompatible phase, subkind conflict, duplicate names, invalid TOML, skipping `build/` and `.hidden/`, `[compat]` missing `core`, export missing `freeze_state`, `[privileged]` missing `level`.
10. **Snapshot / lock**: snapshot reading, "not published", lock round-trip byte-identical, dependents idempotent.
11. **`plan-new` four classes**: 6-item plans / phase / template paths / lower- and upper-case placeholders; bare name + `--subkind`; missing `--subkind`, `subkind` conflict, `runtime_adapter` (TAX-0014 + exit 2), name contract, hand-written conflict (`GEN-0002` exit 1), generated-artifact conflict + `--force`.
12. **`plan-init`**: `product.toml` + `app/<name>/` + `brickie.lock`.
13. **Emitter**: TOML escaping / arrays / idempotence / re-parsable by `toml`.
14. **Cycles and topology** (V-3): init cycle ⇒ `MF-0001` + full path + `data.cycles[].path/edges` + exit 1; runtime cycles report info only; `topo_order` is the **initialization order** (provider first).
15. **Phase monotonicity** (V-8): R1 counterexample (`BRV-DEP-0009`), R2 counterexample/positive (`BRV-DEP-0010`), `sched-coop(core) → platform(core)` **not falsely rejected**.
16. **Version conflicts** (V-7): `range` out of bounds `VER-0002`, `requires_iface`'s `compat_gen` conflict `VER-0001`, mutually exclusive ranges `DEP-0011`, missing dependency `MF-0001`, budget difference.
17. **profile gate** (V-14): same input dev green (info `VER-0004`) / release red (error `VER-0004`).
18. **`requires_iface` not part of the closure** (V-10②): adding/removing the field leaves `closure`'s `data` byte-identical; `iface show` reports info only for it (V-10③).
19. **Cross-file export invariants** (V-12): skin points at a nonexistent unit / unequal classification ⇒ `TAX-0018`; unit-name collision / symbol-family collision ⇒ `MF-0001`.
20. **Exclusive resources and scheduling** (V-19④): IRQ/DMA conflict **reports both sides**; `P2 + map` ⇒ `PRIV-0001`; illegal `(ops×granularity×region)` ⇒ `PRIV-0002`; `TT_SAFE` missing `[sched.tt]`; `COOP_ONLY` × `sched-preempt` red / × `sched-coop` green.
21. **IFACE-IR / hash input completeness** (V-17): `BR_MAX 16→4096`, service ops slot added, enum append, enum reorder **each produce different hashes**; `status` does not enter the hash; declaration order does not affect the hash; domain separation; parameter names/layout are non-strict and do not enter the hash, strict does; typedef expansion is equivalent.
22. **append vs modify** (V-15): enum tail ⇒ `EXTENDED`; enum reorder/struct field added/service ops slot added/func signature/macro value ⇒ `CHANGED`; `is_hard` is true only for already `frozen`/`deprecated`.
23. **publish idempotence** (V-5): first run writes 3 kinds of machine files (`kind=machine`) + snapshot header `NOT_ABI`/`hash_scope`/`truth`/`strict_params`; second run `files=[]` + `noop`; two runs byte-identical; `status --check` in both the consistent/inconsistent directions.
24. **Unfreeze window** (V-16): no `--note` red; `frozen→unfreezing` with baseline recorded in the snapshot; release inside the window ⇒ `IFACE-0009` (dev green); empty unfreeze + `refreeze` ⇒ all four segments unchanged; changing a frozen entry then `refreeze` ⇒ `COMPAT_GEN+1`.
25. **dep command surface**: graph in three formats (incl. parseable `text` for `json`), why positive/negative, index idempotent.
26. **Version-surface commands**: `ver show` four segments/source, `ver bump` three rules + illegal `compat_gen` ⇒ usage error, multiple export plugin names ⇒ usage error (R-3), `iface freeze`'s A-26 proposal (A-26).

## 9. TODO (explicit remaining items)

### 9.1 Command surface
- [x] `check.rs`: cycles + topology, cross-plugin dependency-direction enforcement, phase monotonicity, single version, budget/exclusivity, profile gate.
- [x] `solver.rs`: `closure` / `dep-tree` / `dep-graph` (dot/mermaid/json) / `dep-why` / `dep-index`.
- [x] `iface.rs`: `ver-show` / `ver-bump` / `iface-list/show/diff/status/publish/freeze/deprecate/undeprecate/unfreeze/refreeze`; IFACE-IR hash.
- [ ] `iface-publish --strict-params`: core already supports it (`args.strict_params`), but **L5's argparse has no such flag** ⇒ it can only be called directly on core; when needed, add one line in `cli.py` (not done under the no-touching-`python/` discipline).

### 9.2 Not done / not implementable (honest list, see §10 gaps)
- [ ] `SEMANTIC` change category: v0.1's declaration surface has **no input for a pure semantic change** (`sig/layout/value/ops` none of them moves ⇒ unobservable from TOML) ⇒ `iface-diff` never produces `SEMANTIC`, `BRV-IFACE-0010` has nothing to report (gap S-11).
- [ ] the "two minors with no use" count of the deprecation cycle: v0.1 only enforces the **zero use on the declaration surface** hard gate (`reexport_of`/`requires_iface` reference check); symbol-surface usage statistics need v0.2 (gap S-16).
- [ ] `check`'s quantity constraints (`app` exactly 1 / `platform` exactly 1 / `scheduler` exactly 1): with a product these are countable within the closure, but "exactly 1 per image" in **whole-tree mode without a product** cannot decide "which image" ⇒ not enforced (gap S-17).
- [ ] out-of-tree install to `build/host/<triple>/bin` (G-6, to be done by the repo Makefile owner).
- [ ] verify placeholders once `templates/` is finalized (especially `descriptor_include`; gap D-5).

## 10. Spec gap register (to be fed back into the design/BRV-D8)

> R-1…R-16 were registered by the **first cut**; S-1…S-17 by the **second cut** (solver/check/iface); **S-B1…S-B6** by the **third cut** (build family `build.rs`).
> ⚠ The `BRV-D8` code table has **no BUILD family** ⇒ this cut opens `BRV-BLD-0001..0014` (see `docs/contract.md` §3); the boundary discipline wording is in contract §9 R-13.
> 0013/0014 are the **two info entries added later**: "attribution clear but not selected" and "no plugin of this name is left in the tree" — stale leftovers get compiled into **ghost plugins** by the `build/gen/**` glob, which in measurement stopped `plugin_manager` at startup (`third_party/uboot-mmc` renamed to `driver/uboot-mmc`).

| # | Gap | This implementation | Suggestion |
|---|---|---|---|
| R-1 | `[plugin].phase` has a single value expressing two completion points, and the allowed set is inconsistent between documents | per the parent agent's 2026 clarification (§7 D-1) | write the allowed set into §7.2/§8.1 |
| R-2 | export invariant 3's "re-exported unit has equal classification" is a cross-file decision | ✅ enforced by the second cut (`check`'s iface domain, `BRV-TAX-0018`) | state the enforcement point in §3.5 |
| R-3 | **dependency-direction prohibitions have no assigned code** (three hard prohibitions) | `BRV-MF-0001` (R-9), message names both sides | add `BRV-DEP-0012/0013/0014` or `BRV-TAX-0001..` |
| R-4 | `lang` templates not delivered / unknown `lang` has no code | code-less usage error + exit 2 | add a code or explicitly assign it to `MF-0001` |
| R-5 | `[[dep]].phase` assertion semantics contradict (§7.2 writes `rank(assertion) ≤ rank(provider)`, §8.1 writes "provider no later than assertion") | ✅ ruled, see **S-6** | pick one and pin it |
| R-6 | `sched_class = COOP_ONLY` × `sched_kind = preempt` conflict has no code/no clause | ✅ enforced by the second cut (no code, uses `BRV-MF-0001`) | add a code in §2 item 3 |
| R-7 | the `lock` / `index` schemas were not landed | ✅ aligned (`render_lock`/`load_lock`/`render_dependents`) | ✅ closed |
| R-8 | descriptor `{{descriptor_include}}` has no semantics | use `<short>/<short>.h` | finalize in `3-05` §2 |
| R-9 | two case variants of `templates/` placeholders coexist | emit both | unify template variable naming |
| R-10 | `plan-init`'s lock content vs "plans produce no content" | plan + `data.lock_content` | add a sentence in contract §5.2 |
| R-11 | ownership of `gen-plan`'s "header files" | emits descriptors only | clarify in contract §5.2 |
| R-12 | quantity constraints (app exactly 1 / platform exactly 1 / scheduler exactly 1) and the R-1 branch | not enforced (gap S-17) | clarify what "per image" means without a product |
| R-13 | duplicate `[[dep]]` has no code | not reported (second cut unchanged) | optionally add an info-type code |
| R-14 | code obligation for "code-less usage errors" (G-5) | keep code-less; shape class always `MF-0001` | clarify in BRV-D8 |
| R-15 | response JSON key order unspecified | serde_json lexicographic order | if byte-exact snapshots are wanted, write it into the contract |
| R-16 | cross-agent fixture conflicts with the clarified convention (`app` + `phase="late"`) | `model` reports `BRV-MF-0001` (exit 2) | that fixture should be changed to `phase = "core"` (ADR-0036 §2.3: APP is a boot phase, not an init completion point, so `phase = "app"` is itself illegal now) |
| **S-1** | where `requires_iface` version validation lives: V-10② requires "closure byte-identical", V-7 requires "`compat_gen` conflict constructible" | `closure` **does not scan** (`scan_iface=false`); `check --deps` scans and reports `VER-0001/0002/0004` | add a sentence to contract §5.1 for `closure`: "does not evaluate interface dependencies" |
| **S-2** | structural deps (`init/runtime/type`) **do not pin a generation**, so what does V-7's `range` out-of-bounds compare against | compares `MAJOR.MINOR.REVISE` of the **provider plugin's four-segment version** ⇒ `VER-0002` | state in §7.4 that "structural dependency `range` applies to the provider plugin version" |
| **S-3** | "multiple versions of one plugin coexisting ⇒ `DEP-0011`" is not constructible under "one declaration per plugin" | the construction form is taken as **the intersection of several dependents' range constraints being empty** ⇒ `DEP-0011` | add the "range intersection empty" criterion to §7.4 |
| **S-4** | §7.1's table says `type` edges "participate in closure ✅ (but do not pull in runtime deps)" | a `type` edge only pulls the provider into the closure and does not expand along its `init`/`runtime` edges | state the closure expansion rules clearly in §7.1 |
| **S-5** | structured cycle shape: contract only writes `cycles:[]` | each element `{path:[...], edges:[{from,to}]}` (V-3 wants a "structured edge list", present also wants `path`) | give the shape in contract §5.1 |
| **S-6** | `[[dep]].phase` (R2) direction contradiction (§7.2 vs §8.1) | take the §8.1 convention: **red only when the provider's own phase is later than the assertion** (`rank(provider) > rank(assertion)`) | pin one |
| **S-7** | `check`'s `args.plugin?` semantics undefined | filter by the **diagnostic `target`** (i.e. "only what this plugin reports"), not "only compute its closure" | explain in contract §5.1 |
| **S-8** | does the first `publish` (no old snapshot) advance segments | **file creation**: records only the current declared version and hash, advances no segment; advancement by change set starts afterwards | add a "first publication" row in §6.5 |
| **S-9** | state-machine commands (`deprecate`/`undeprecate`) conflict with `brick.toml` being read-only | legality is judged by the **snapshot state**, the new surface is built from the declaration surface ⇒ repeated execution is a no-op; what `brick.toml` should become is reported as info | add a "hand-written surface sync" note in §6.5 |
| **S-10** | the "dependents still match" gate hook of `publish --profile release` makes **any** `COMPAT_GEN` jump rejected under release | follows the §6.5 table (`BRV-VER-0001`) and hints in the message to coordinate the dependents first | state in §6.5 that "a jump requires the dependents to change too" |
| **S-11** | `SEMANTIC` changes have no input in v0.1 (pure behavioural semantics are unobservable from TOML) | does not produce `SEMANTIC`; `BRV-IFACE-0010` has nothing to report | note in §6.3 that "v0.1's SEMANTIC is manually declared/unobservable" |
| **S-12** | semantics of `iface status`'s `files:[if write-back needed]` | the gate **does not write to disk**; when inconsistent, the expected snapshot goes in `data.expected_snapshot` | clarify in contract §5.1 |
| **S-13** | §7.3's app row "ability ✗" + §10 V-12② "`app → service/sqlite` must pass" | take V-12: `app` may depend on an ability with `api_type = third_party` (the precise form of "must not call **native** directly"); the app branch of `rules::dep_direction_ok` adds that exception | write the exception into the §7.3 table |
| **S-14** | R-6 "platform's `[[res]]` is also capacity" — if it **also enters Σ**, the physical cap is always red (§8.1 note ① loses meaning) | `platform`'s `[[res]]` becomes `platform_capacity` and does **not** enter the consumer-side Σ (`totals.ram_kib` = sum of non-platform) | clarify R-6: the platform declares **supply**, not its own consumption |
| **S-15** | does `unfreeze`'s baseline take "the last published frozen surface" or "the declaration surface at unfreeze time" | takes the **last published snapshot entries** (the truth of the commitment); unpublished changes inside the unfreeze window are counted at `refreeze` as "changed frozen entries" | add a baseline definition in §5.3.4 |
| **S-16** | the deprecation cycle's "two minors with no use" has no symbol-level input in v0.1 | only the **zero use on the declaration surface** hard gate is done; the ability to downgrade the symbol surface to a warning under release is **not implemented** (no symbol-level data) | clarify in §6.4 that v0.1 only handles the declaration surface |
| **S-17** | the "exactly 1 per image" quantity constraint for `app`/`platform`/`scheduler` cannot decide "which image" in whole-tree mode without `product.toml` | not enforced (countable within the closure when a product exists, but left off to avoid false positives) | clarify the quantity semantics without a product in §3.2/R-1 |
| **S-19** | inside an unfreeze window `publish` and `refreeze` overlap in responsibility: if both bump on "hard change relative to baseline" they double-bump; if `publish` rebuilds the snapshot from the declaration surface it wipes `unfreezing` into `frozen` (window lost) | `publish` **preserves the effective freeze state** (declaration OR snapshot) and advances this publish's surface as the **new baseline**; `refreeze` is then an "empty unfreeze" (four segments unchanged) | state clearly in §5.3.3/§6.5 "who records the change, who closes the window" |
| **S-18** | info-type diagnostics standing in for gaps (e.g. "generation does not participate in comparison"/"NOT_ABI"/"what it should become") borrow `BRV-MF-0001` (error domain) but write `info` | severity and code domain disagree, but machine consumers can still classify by severity | add a generic info-type code in BRV-D8 |
| **S-B1** | who provides the tool **candidate order** (`<cross>gcc` → `<cross>gcc-16…-13` → `cc`; cross binutils → host binutils) and the **resolution result** | **all in core**: `build.rs`'s `resolve_tool` (first `prebuilts/toolchain/bin/<name>`, then `context.path`/`PATH`), result in `data.tools[i].{candidates,resolved}`; L5 only does the final executability check (`runner.resolve_exe`, no re-selection) | the make / ninja backend files are **generated artifacts**, so the resolution result must be **baked in** — resolving in two places gives two answers to "who compiles" (contract §9 R-14) |
| **S-B2** | does incremental decision use "content stamps of the input set" or "argv fingerprint + input mtime" | two criteria: **argv fingerprint (`sha256(kind, argv)`) + input (size, mtime_ns)**, same family as make / ninja; when an upstream needs rebuilding, downstream is marked dirty **explicitly** (the plan is computed before the build, when objects are still old); missing `.d` ⇒ recompile once | `.d` is a **product of the build** (it exists only after the first compile) ⇒ a content-stamp convention is self-referential and header dependencies would cause one extra rebuild forever (hit in measurement). See contract §9 R-15 and the doc note on `make_plan` |
| **S-B3** | how `--backend direct|make|ninja` maps to BR-D4's "backend A / B + byte-identical" | `direct` = L5 orchestrates from core's `steps` (default); `make` / `ninja` = core produces `build/gen/build.{mk,ninja}` generated files + one `kind:"backend"` step handed over to it; in the ninja artifact **C sources and `.S` each get a rule** (`cc` / `asm`, different flags for the two source kinds) and `builddir = build/gen/ninja` is pinned at the top (otherwise `.ninja_log`/`.ninja_deps` land in the repo root); all three backends run the **same argv batch** and produce byte-identical artifacts (verified with `cmp`) | BR-D4's requirement "two backends produce the same image" is exactly the criterion; the generated files do not read each other and `direct` does not read them either — the three paths are independent |
| **S-B4** | how much regex syntax `tests/gates.toml` patterns may use | only `.` `*` `+` `?` `[...]` (`^` negation / `a-z` ranges) and `\x` escapes; if `( ) | ^ $ { }` appears ⇒ **`BRV-BLD-0012`** (exit code 2), **rejected rather than silently non-matching** | no regex library is pulled in (§9.3 minimal dependency set); a `require` that "never matches" turns into **false green**, far more dangerous than an error — hence a shape error is preferred (contract §7.4) |
| **S-B5** | do gate-log red/green criteria (require / forbid / `PASS <tag> `) belong to L5 or core | **core's `judge` command**: input = gate name + `context.log` (log body), output = `{failed, checks[], diagnostics}`; L5 only passes the step log body back and folds `failed>0` into red. All green ⇒ **code-less info** (not a defect) | red/green can only have one source of truth: text output, `--json` and the CI exit code must agree (contract §9 R-16) |
| **S-B6** | which files under `build/gen/**` are generated, and the semantics of `--emit-backends` | `build/gen/build.mk` (BR-D4 A) / `build/gen/build.ninja` (BR-D4 B) / `build/gen/build-state.json` (incremental state) all carry `brickie:generated` on the first line, core supplies `files[]` and L5 writes them to disk; `--emit-backends` = **produce only the two backend files, do not execute** (can be combined with `--dry-run` to run in a compiler-less environment); a build with `--tag` moves objects to `<obj_dir>-<tag>` and does **not** run `[build].post` | generated vs hand-written is distinguished by the first-line marker (§4); the state file is derived, and if corrupt it is treated as "no state" and rebuilt fully — not an error |

## 11. Change discipline

- **Judgement lives only in this crate**: do not move closure/range/topology/taxonomy/privilege/version advance into Python.
- **Codes come only from BRV-D8**: for defects with no assigned code use `BRV-MF-0001` (R-9) **and update the gap table in §10**.
- **Machine files have no timestamp/randomness/host info**; all new serialization goes through `emit.rs` primitives.
- **`brick.toml` / `product.toml` are read-only**: no command may rewrite them; `new`/`init` create files only when they do **not** exist.
- After changes run `cargo build --release --locked && ./target/release/brickie-core --selftest`.

## 12. Field alignment with L5 (Python)

`python/**` was not changed. Alignment is judged by end-to-end runs of the real CLI:

| Command | Fields read by L5 | Provided by core | Conclusion |
|---|---|---|---|
| `check` | `data.summary.{errors,warnings,infos,profile,closure{plugins,ram_kib,stack_kib}}`, `data.scope_results` | ✅ same-named fields | aligned |
| `dep closure` | `data.{plugins[].{name,version,compat_gen,plugin_type,phase,ram_kib,stack_kib,selected}, topo_order, totals{...}, cycles[].path, profile}` | ✅ | aligned |
| `show bricks` | `data.{bricks[], totals{...}, product}` | ✅ | aligned |
| `show brick-init-flow` | `data.{steps[], counts, order, order_basis, unsorted, selftest_on}` | ✅ | aligned; `irq` steps carry an empty `plugin` |
| `dep tree` | `data.{roots, nodes{<n>{deps[{name,kind,range,phase}]}}, kind}` | ✅ | aligned |
| `dep graph` | `data.{format, nodes, edges, text}` | ✅ | aligned |
| `dep why` | `data.{found, path, edges[{from,to,kind}]}` | ✅ | aligned |
| `dep index` | `data.dependents{<plugin>:[{from,kind}]}` | ✅ | aligned |
| `ver show` | `data.{id,version,segments,plugin,source,status,freeze_state,published,compat_gen_source{kind,note}}` | ✅ | aligned |
| `ver bump` | `data.{id,from,to,rule,segments,version}` | ✅ | aligned |
| `iface list` | `data.units[].{id,api_iface,form,version,compat_gen,freeze_state,status,published,hash}` | ✅ | aligned |
| `iface show` | `data.{unit{...}, entries[].{kind,name,status}, source, not_abi, truth, hash_scope}` | ✅ | aligned |
| `iface diff` | `data.{id,verdict,version{from,to,reasons},compat_gen_changed,changes[].{kind,name,entry,from,to},dependents{direct,transitive,unsatisfied}}` | ✅ | aligned |
| `iface status` | `data.{id,consistent,recomputed_hash,snapshot_hash,declaration_hash}` | ✅ | aligned |
| `iface publish` | `data.{id,noop,version,from,to,compat_gen,verdict,published,report.dependents}` | ✅ | aligned |
| `iface freeze/deprecate/undeprecate/unfreeze/refreeze` | `data.{id,entry,proposal,freeze_state,status,compat_gen}` | ✅ | aligned |

**Known differences (they do not affect L5 orchestration)**:

1. L5's `--json` outer envelope uses `{protocol,status,command,exit_code,diagnostics,data,written}`, while the core response uses `{protocol,status,exit_code,diagnostics,files,data}` — L5 is the **presentation layer** and replaces `files` with the "actually written list `written`". **The core side keeps the contract shape.**
2. in `dep tree` / `dep graph`'s `nodes`, core additionally provides `file`/`phase` (L5 ignores them) — purely additive.
3. `iface publish`'s `data.changes[].from/to` may be `null` for `ADDED`/`REMOVED` (L5 uses `is not None`, already compatible).
4. `check`'s `data.scope_results` is an **object** (keys = domains) and also contains the `manifest` pseudo-domain — L5's `present.py` supports both shapes (dict / list).
5. L5 is missing the `iface publish --strict-params` flag (see §9.1) — core already supports that `args` key.

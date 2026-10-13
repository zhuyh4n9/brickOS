# brickie — brickOS composition-time toolchain

> **`brickie check` is the "declaration-surface completeness checker"**: it guarantees the composition is **logically** self-consistent,
> but **not** that the composition **compiles / runs correctly**. The design doc explicitly requires this sentence in the CLI help and README — to prevent v0.x users from over-trusting it.
>
> Naming decision: this tool is `brickie`, not `br`; symbol prefixes `br_*` / `BR_*` / `.br_plugins` are **already-allocated identifiers that do not change with the tool name**.

**The single authority for the cross-language interface is this directory's [`docs/contract.md`](docs/contract.md)**: the three processes' JSON envelope, diagnostic model, file ownership, per-command `args` and `data`/`schema` field tables, and the implementation-time rulings **R-1…R-12** are all there. This file only describes **this repo's current state and division of labor**; the protocol itself is not copied here.

---

## 1. Current state (v0.1 command surface)

**L5 (Python frontend) is fully delivered**: `python/brickie/` has a complete argparse command surface of **29 leaf commands + 2 global switches**, with orchestration / disk writes / presentation / shape validation / exit-code aggregation all implemented. By purpose it splits into **two families + 2 global switches**:

| Family | Commands | Purpose |
|---|---|---|
| **Scaffold** | `new <ptype> <name>` · `init <product>` · `gen [--check]` | Generate plugin/product skeletons; rebuild `build/gen/**` artifacts |
| **Dependencies & validation** | `dep {add,rm,tree,graph,why,index,closure}` · `check [--deps\|--iface\|--tax\|--priv\|--all] [--profile dev\|release]` | Closure/tree/graph/path/reverse index + four-domain completeness |
| **Version & interface** | `ver {show,bump}` · `iface {list,show,diff,status,publish,freeze,deprecate,undeprecate,unfreeze,refreeze}` | Four-segment version + unit snapshot / freeze-window governance |
| **Build family (second family)** | `build [--profile\|--jobs\|--backend\|--tag\|--dry-run\|--force\|--emit-backends\|--no-post]` · `clean [--all]` · `run` · `size` · `disasm` · `test [<name>\|--list]` | Declaration-surface-driven build orchestration + execution + gates (build/clean/run/size/disasm/test; core also has `judge`, serving only internal L5 calls) |
| **Global switches** | `--version [--deps]` · `--json` (also `--root DIR`) | Fingerprint / machine-readable output / root override |

> **Scope of the v0.1 "boundary discipline" (ruling R-13)**: "no v0.1 command **may require cc/cargo/nm to be present**" constrains only **composition-period** commands (scaffold / dependencies & validation / version & interface), i.e. the "declaration-surface completeness checker" half. **The build family is an explicit second family**: it composes the image and drives the cross toolchain; "requiring the toolchain to be present" is its **job**. CI composition-period jobs still have zero compilation dependencies (`make tools` / `brickie check` do not touch the cross toolchain).

**Implementation status** (frontend = L5 argument parsing/orchestration/presentation; native side = the processes that actually decide):

| Command | L5 frontend | Native side | Notes |
|---|---|---|---|
| `new` / `init` / `gen` | ✅ | `core plan-new` / `plan-init` / `gen-plan` ✅ + `gen render` ✅ | Two-stage: core provides the plan (paths + templates + vars), gen produces content, L5 writes to disk |
| `dep add` / `dep rm` | ✅ | `core dep-add` / `dep-rm` ✅ | **Prints the snippet only, never writes to disk** (ruling R-2) |
| `check` | ✅ | `core check` ✅ | Four domains `deps/iface/tax/priv` + `summary`/`scope_results`; `--profile` gate |
| `dep tree/graph/why/index/closure` | ✅ | `core dep-*` / `closure` ✅ | Closure is a **pure function** (cycles report the full structured path; budget totals carry used/capacity/budget) |
| `ver show` / `ver bump` | ✅ | `core ver-show` / `ver-bump` ✅ | Version carrier = **unit snapshot**; the human-written `brick.toml` is read-only (ruling R-3) |
| `iface *` (10 commands) | ✅ | `core iface-*` ✅ | IFACE-IR + sha256 + change set + impact report + freeze window (incl. the `unfreezing` transient) |
| `--version [--deps]` | ✅ | `core version` ✅ | When core is absent, `--version` degrades to printing the frontend version |
| `build` / `clean` / `run` / `size` / `disasm` / `test` | ✅ | `core build/clean/run/size/disasm/test/judge` ✅ | **Second family** (ADR-0004 / design ADR-0003 S1–S4): declaration-surface-driven build + L5 executor (`runner.py`, concurrency/timeout/logging/progress); tool resolution / incremental / gate criteria all in core (rulings R-14/R-15/R-16) |

> **Which side decides**: all **decisions** and BRV codes live in `rust/` (L0/L1); L5 only does argument parsing, orchestration, disk writes, shape validation, presentation and exit-code aggregation (the V-18c static check catches overreach). The native side's domain self-check: `brickie-core --selftest` — **485 cases** (after `make core`, run `build/host/<triple>/bin/brickie-core --selftest`), it **does not need a Python interpreter** (V-18a). For end-to-end regression see `tests/run.sh`; the blueprint = the V-1…V-19 matrix + build-family §V-B in `tests/ACCEPTANCE.md`.

**Explicitly not done** (none of these names appear in `--help`; reverse acceptance will catch them): `api-dump` / `verify` / `iface check` / `dbg` / `pack`, plus the old names `add` / `show` / `env`. `build` / `test` / `run` were removed from the banned-name list when the second family delivered them, and reverse acceptance (`N-1`/`N-2`) was updated to the new list.

## 2. Directory structure

```
tools/brickie/
├── Makefile              build entry for the tool itself (delegates to cxx/ and cargo; does not duplicate compile rules)
├── pyproject.toml        Python package (brickie) and entry points
├── freeze.py             packer: python/** + templates/** + schema/** → uncompressed tar → C array
├── python/brickie/       L5 frontend (command surface / orchestration / disk writes / presentation / shape validation) + runner.py (build-family executor)
├── rust/                 brickie-core: L0 model + L1 solver/version/interface engine + build planner (the only place that decides)
├── cxx/                  L2 renderer (main/render) + L5 entry ELF (launcher)
├── templates/            skeleton/descriptor/product templates (data, not code)
│   ├── native/<plugin_type>/<lang>/     skeleton: brick.toml / src.c / include.h / smoke.toml / README.md
│   ├── descriptor/<api_type>/<lang>/    descriptor (orthogonal to plugin_type)
│   └── product/<lang>/                  product skeleton
├── schema/*.schema.json  machine-readable description of the declaration surface's **single-record shape** (plugin/product/lock/index/gates, 5 files)
├── docs/contract.md      ★ cross-language interface contract (single authority; see §3)
└── tests/                run.sh (end-to-end) + ACCEPTANCE.md (V-1…V-19 acceptance matrix + build-family §V-B)
```

**Out-of-tree artifacts** (cf. Android's `out/host/...`; decided by the repo root's `mk/host.mk` + `tools/host-detect.sh`). The source tree holds only sources; everything built goes under the repo-level `build/`:

```
build/host/<host-arch>/<host-os>/bin/brickie       ← L5 entry ELF (embedded payload)
                              …/bin/brickie-gen    ← L2 renderer
                              …/bin/brickie-core   ← L0/L1 Rust core
                              …/lib/libbrickie-gen.a
                              …/obj/cxx/*.o
                              …/share/brickie/payload.tar   ← payload embedded in brickie (for inspection)
```

`<host-arch>` = `x86-64` / `aarch64` / …, `<host-os>` = `linux` / `darwin` / `win`. The frontend `native.py` locates the two native tools by the same convention (`BRICKIE_CORE` / `BRICKIE_GEN` / `BRICKIE_HOST_ARCH` / `BRICKIE_HOST_OS` can override), and does not depend on old locations inside the source tree.

**Division of labor among the three executables**: `brickie` (C++ launcher + embedded payload), `brickie-core` (Rust), `brickie-gen` (C++ renderer). The entry ELF is **single-file self-contained**: `freeze.py` packs

- `python/brickie/**` (L5 frontend), `templates/**` (skeleton templates), `schema/**` (shape-gate input),
- **native tools** (`cxx/Makefile`'s `PAYLOAD_EMBEDS`: `brickie-gen` always embedded, `brickie-core` conditionally embedded when present),

into an uncompressed ustar tar embedded as data; at runtime it unpacks to a temporary directory, uses the system `python3` to interpret the embedded code, and points `BRICKIE_GEN` / `BRICKIE_CORE` at the **unpacked** native tools. Therefore:

- The artifact is a real ELF (magic `\x7fELF`) and enters the seed together with the standalone copies `brickie-gen` / `brickie-core`;
- **Copying only the single file `brickie`** is enough to run — no `PYTHONPATH`, no source tree, no native tools in the same directory, no `g++`/`cargo`;
- **Zero new third-party dependencies** (no PyInstaller/Nuitka); the payload is byte-for-byte reproducible.

**Bootstrap seed** (published by `make prebuilt`; isomorphic to `build/host/**` but **committed to version control**):

```
prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie        ← ★ single-file self-contained
                                           /brickie-gen    ← L2 renderer (redundant copy, for direct dev-mode use)
                                           /brickie-core   ← L0/L1 core (redundant copy)
```

This `brickie` "runs from a fresh checkout without `g++`/`cargo`". Dev-mode frontend lookup order (`native.py`'s `_find_tool`): `$BRICKIE_GEN`/`$BRICKIE_CORE` → `build/host/...` (just built locally) → `prebuilts/...` (seed) → `PATH` (each tier has a `*/*` fallback). See [prebuilts/README.md](../../prebuilts/README.md).

## 3. Three-language division of labor and protocol pointers

| Layer | Language / location | Responsibility | Must not do |
|---|---|---|---|
| **L0 + L1** | Rust · `rust/` (`brickie-core`) | model / solver / version / IFACE-IR / taxonomy / privilege / budget — **all decisions and error codes**, plus the **content** of "machine-owned files" | Writing to disk (content is handed to L5 to write) |
| **L5** | Python3 · `python/brickie/` (`brickie`) | argparse / locating binaries / spawning subprocesses / reading and writing files / JSON serialization / **single-record shape** validation against `schema/*.json` / text presentation / exit-code aggregation | Any business decision; concatenating the content of any machine file (contract §8) |
| **L2** | C++ · `cxx/` (`brickie-gen`) | **template rendering only**: `templates_root` + `artifacts[{path,template,vars}]` → `files[{path,kind:"rendered",content,mode}]` | Any decision (name contract/phase/path layout/version are all in Rust) |

**For protocol and rulings always consult [`docs/contract.md`](docs/contract.md)**; do not establish a second copy elsewhere:

- §1 the three host executables · §2 envelope and protocol version (`BRV-PROTO-0001/0002`) · §3 diagnostics (including build-family codes
  `BRV-BLD-0001..0014`) · §4 file ownership (§9.1.1 write-path discipline) · §5 `brickie-core` commands and
  the `args`/`data` field tables (**§5.4 = build-family seven** `data.steps[i]` / `data.tools[i]`) ·
  §6 `brickie-gen` commands · §7 declaration-surface schema (**§7.4 = `tests/gates.toml` shape**) ·
  §8 L5 boundary · §9 implementation-time rulings R-1…R-16.

**Three disciplines most easily gotten wrong**:

1. **Write paths**: `brick.toml` / `product.toml` are **read-only inputs**, never rewritten; the machine writes only `api/iface/**`, `brickie.lock`, `build/index/**`, `build/gen/**`.
2. **`dep add` / `dep rm` do not write to disk** (ruling **R-2**): design says "write `brick.toml`", but "never rewrite human-written files" wins. The command only **prints** a directly pasteable TOML snippet (`data.snippet`).
3. **In the build family L5 only executes, never decides** (rulings **R-14/R-15/R-16**): which compiler to use, whether to rebuild, whether the log counts as red or green, all come from core; `runner.py` only does "validate executable / concurrency / timeout / collect logs / present progress". Even the code used for a compile failure is given by core with the step (`steps[i].fail_code`).

## 4. Build and test

```sh
make                 # = make cxx: the two host ELFs (brickie-gen L2 + brickie L5 entry);
                     #   brickie-core is conditionally embedded if already at its out-of-tree location, otherwise continue after a notice
make core            # cargo-builds the L0/L1 brickie-core → build/host/<triple>/bin/brickie-core
make selftest        # brickie-gen --selftest: JSON round-trip / render / paths and modes / payload determinism
make launcher-smoke  # entry ELF self-check: can the embedded payload start up (brickie --version)
make test            # renderer selftest + tests/run.sh end-to-end (V-1…V-19 + §V-B build family / full command-surface coverage / purity / snapshot)
make prebuilt        # publish the **three-piece** seed {brickie,brickie-gen,brickie-core} (not written if cmp is identical)
make prebuilt-check  # whether the three pieces lag behind the sources (does not touch disk)
```

The image is built by brickie:

```sh
build/host/<triple>/bin/brickie build            # image entry: composition-period check → gen → compile → link → [build].post
build/host/<triple>/bin/brickie build --dry-run  # print the plan only (needs no compiler; use case V-B uses it)
build/host/<triple>/bin/brickie test             # run all gates in tests/gates.toml
build/host/<triple>/bin/brickie clean --all      # clean image-side derivatives (leaves build/host tools alone)
```

- **cargo version (≥ 1.83)**: `rust/Cargo.lock` is **version 4**; an old distro cargo (e.g. 1.75 on Ubuntu 24.04) cannot read it and only reports `lock file version 4 requires -Znext-lockfile-bump`. `make core`'s selection order is "explicit `CARGO=…` → the workspace's own `.toolchain` (`<repo>/.toolchain` or `<repo>/../.toolchain`) → PATH", and when too old it **FAILs immediately** with a fix.
- **cargo out-of-tree**: `CARGO_HOME` / `CARGO_TARGET_DIR` default to `prototype/build/{cargo-home,cargo-target}` (under managed environments `$HOME/.cargo` may be read-only), both can be overridden by environment variable; Rust sources are in `rust/`, artifacts are copied to the host bin location.
- **Core degrades, gates do not**: when Rust has not been built, `make` / `make cxx` / `make selftest` **succeed as usual** (the payload temporarily excludes core; only a notice is printed); but `make prebuilt` treats `brickie-core` as a **required piece** — missing core ⇒ FAIL (the bootstrap seed must have all three pieces); `prebuilt-check` also checks all three.
- **Repo-root delegation**: `make tools` (builds only L5/L2), `make tools-core` (builds only the Rust core), `make tools-test` (tool tests), `make brickie-check` / `brickie-check-release` / `make brickie-compose` (validate / rebuild artifacts using the entry ELF). **The image side is handed to brickie**: top-level `make` builds only the tool; `make all` / `run` / `smoke` / `irq-test` / `mem-test` / `string-test` / `dbg-test` / `check-string` / `size` / `disasm` / `clean-brickos` are all **thin delegations** (calling `$(BRICKIE) build|run|test …|size|disasm|clean`); the real thing lives in `brickie build` and `tests/gates.toml`. The image's source set / flags / rules **no longer** appear in the top-level Makefile — they are determined by `product.toml [build]` + each plugin's `[build]` + the platform's `[build.target]`, and the ② invariant in `make check-build` catches any regression. Declaration surface red ⇒ **`make` stops at ⓪** (exit code 1) and the image is not built; to build only the tool use `make tools` / `make tools-core`.

Two usages:

```sh
# (a) entry ELF: carries the Python code, templates, schema and both native tools
build/host/<host-arch>/<host-os>/bin/brickie check --root .
# (b) dev mode: run the Python sources directly; the frontend finds the native tools under build/host/ itself
python3 -m brickie check --root .   # needs PYTHONPATH=python (or the package installed)
# or specify explicitly
BRICKIE_CORE=/path/to/brickie-core BRICKIE_GEN=/path/to/brickie-gen python3 -m brickie gen
```

## 5. Diagnostics and exit codes

| Code | Meaning | v0.1 |
|---|---|---|
| 0 | Success | ✅ |
| 1 | Validation red (cycles / conflicts / budget / version / taxonomy / privilege); in the **build family** a failed step / red gate also folds into this bucket | ✅ |
| 2 | Usage or environment error (invalid arguments / schema mismatch / path does not exist / template unavailable / native artifact missing or protocol mismatch / command not yet implemented / build family cannot find compiler or QEMU) | ✅ |
| 3 | Compile or run failure | ✗ **still unused**: the build family folds "compile failure" into 1 (`BRV-BLD-0011`) and environment errors into 2 (`BRV-BLD-0005`) — consistent with BRV-D9's "three buckets", no fourth bucket added |

**Severity does not determine the exit code**: `severity ∈ {error,warning,info}` only describes "how heavy this message is"; the exit code is determined by "did the command complete". Two registered precedents: `BRV-TAX-0014` (the selected template is not delivered in v0.1) is `info`, but **not a single file was generated** ⇒ exit code 2; usage/environment errors (unknown `plugin_type`, missing `--subkind`, name-contract violation, schema shape mismatch…) carry a diagnostic but **`code: null`**.

A native tool's **process** exit code is always 0 (except `internal_error` and crashes): the business conclusion is only in the response's `exit_code` — so "tool failure" and "validation red" do not get conflated.

Diagnostics the delivered paths can emit (all codes taken from the design BRV-D8 **assigned code table**, none invented):

| Code | Severity | Trigger |
|---|---|---|
| `BRV-TAX-0013` | warning | plugin name does not match the recommended form `<namespace>/<short>` (§8.3; new plugins only) |
| `BRV-TAX-0014` | info | selected `api_type` template not delivered in v0.1 (`runtime_adapter` / `third_party`) |
| `BRV-TAX-0015` | error | `--subkind` conflicts with the namespace-derived value (§8.4) |
| `BRV-GEN-0002` | error | generation target already exists and is not an artifact directory, or the target is a **human-written file** (§8.4) |
| `BRV-MF-0001` | error | declaration-surface required field missing / field shape does not match schema (shape gate, exit code 2) |
| `BRV-BLD-0001..0014` | error (the all-green criterion is a **code-less info**) | Build family: missing `[build]` / empty sources / missing `[build.target]` / ambiguous target / tool missing (exit code 2) / unit missing `[build]` / literal source does not exist / unknown gate name / gate red / `[build]` shape / step failure / regex beyond subset / artifact belongs to a non-closure plugin (info) / artifact is a **stale leftover** (info, ghost plugin). See contract §3 for each |

## 6. Known deviations / gaps vs the design doc (need backporting)

| # | Design doc says | This implementation | Why |
|---|---|---|---|
| D-1 | §9.2: `cxx/ # CMake: libbrickie-gen + brickie-gen` | an equivalent minimal **Makefile** (the library/executable two-artifact split is unchanged) | cmake is not available locally; no build-system dependency introduced (same direction as §9.3's "minimal dependency set") |
| D-2 | §9.2: `templates/ # skeleton templates: <api_type>/<plugin_type>/<lang>/` | skeleton templates follow this; descriptors go in `templates/descriptor/<api_type>/<lang>/` | descriptors are **orthogonal** to `plugin_type` (they vary only with `api_type` and `lang`), and §9.2's path did not cover this dimension |
| D-3 | §13.2's A-3 / A-14 / BRV-Q7 (not backported); `1-01` §6.1 still says `ver[3]` | `brick.toml` and artifacts use a **four-segment version** `COMPAT_GEN.MAJOR.MINOR.REVISE` (e.g. `0.1.0.0`) | a three-segment descriptor **cannot express** the declaration surface; the artifact header already states this assumption |
| D-4 | the macro-form contradiction at `4-02:29` (static vs non-static) is not finalized | artifacts use **`static const` + `.br_plugins`** (`3-01` §13.3 / CA-10), and the header notes the alternative | section collection needs only `used`+`section`, not external linkage; CA-10 is an **adopted decision record** and explicitly says "the plugin export surface is near zero" |
| D-5 | `3-05` §2 does not finalize the descriptor header filename | artifacts `#include "br_plugin.h"`, and the header notes that it is not finalized | no document gives the filename; artifacts are **not compiled** (V-9), they are now a declaration-surface product |
| D-6 | §9.2 / BRV-D5 **do not specify where the tool's own native artifacts live** (only the source tree is given) | artifacts are **out of tree**: `build/host/<host-arch>/<host-os>/{bin,lib,obj}` (cf. Android's `out/host/...`) | hard requirement from the requester (tool bins must not land in the source tree); backported to the BRV-D5 table + §9.2 note + `2-02` BR-D5, registered in checklist §5.6 as **G-6** |
| D-7 | §9.2 / BRV-D5 **do not specify "prebuilt binaries committed with the sources"** (they only say "artifacts do not enter the repo") | **the bootstrap seed enters the repo**: `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}` (published by `make prebuilt`) | hard requirement from the requester (seeds "bootstrapping without a compiler" and future brickie self-hosting of its own compilation); ADR **0004** established and backported to the BRV-D5 table + §9.2 + `2-02` BR-D5, registered in checklist §5.6 as **G-7** |
| D-8 | §9.1/§9.2 define L5 as a **Python3** process (implying "needs an interpreter + `PYTHONPATH`") and treat the L2 generator as a **standalone executable** | **the Python frontend and the native tools are compiled into the same ELF**: a self-written `cxx/launcher.cpp` + `freeze.py` embed `python/brickie/**`, `templates/**`, `schema/**` and both native tools (payload `bin/brickie-gen`, `bin/brickie-core`) entirely into `brickie` (single-file self-contained; at runtime the system `python3` is still used, with environment variables pointing at the unpacked pieces) | hard requirement from the requester ("brickie's python code also needs to be compiled to an elf" + "tools such as brickie-gen all need to be compiled into python's brickie elf"); a zero-dependency entry was chosen over PyInstaller/Nuitka to keep §9.3's "Python zero third-party dependencies" discipline. Backported to ADR **0004 §7** and checklist §5.6 **G-8** |
| D-9 | §7.7 says `dep add` / `dep rm` "write `brick.toml`" | **print the TOML snippet only, do not write to disk** (ruling **R-2**); an overwrite would erase comments and violate §9.1.1 rule 1 | human-written files are the **single source of truth**; switch cost = adding `--write` later (contract §9 R-2) |
| D-10 | the interface state machine's `--note` **semantics are not ruled on** (path or body) | the frontend follows the CLI convention and passes `--note`'s value to core **verbatim as a path** (body reading and validation belong to core) | consistency with the "an argument is a path" CLI rule; if the core switches to taking the body, add `note_text` in one place in `cli.py`'s `iface` args (single-point switch) |
| D-11 | BRV-D8 **assigns no dedicated codes** to the following defects: artifact inconsistent with recomputation / render missing variable / hard dependency-direction prohibition / budget vs exclusive conflict / duplicate `[[dep]]` … | fall back "by proximity": shape and declaration-surface errors use `BRV-MF-0001` (naming both sides/the delta in the message), the rest use a **usage error with `code: null`** (`exit_code 2`); **no invented codes** | the code table is a design asset; inventing codes would distort the "code → spec" mapping; the gaps are registered separately on the L5 and Rust sides and will be closed one by one once codes are added |
| D-12 | the assertion semantics of `[[dep]].phase` are **self-contradictory** (§7.2 writes `rank(assertion) ≤ rank(provider)`, §8.1 writes "the provider is no later than the assertion") | v0.1 **does not enforce** it (only single-record shape and allowed-set validation), pending the spec choosing one of the two | the two readings give opposite conclusions, so picking one first would misjudge the other half of the cases; the gap is registered and visible in `rust/README.md`'s gap table |
| D-13 | §0: "no v0.1 **command** may require cc/cargo/nm to be present"; §7.1 note: `brick.toml [build]` "is only recorded in v0.1, consumed from v0.3" | **the build family is an explicit second family** that consumes `[build]`/`[build.target]` early (ADR-0004 / design ADR-0003 S1–S4); the boundary discipline is read as "constraining only **composition-period** commands" (ruling **R-13**) | the requester asked `brickie build` to take over the image entry; the two families have different responsibilities, and one sentence governing both would be self-contradictory. Backported to contract §9 R-13 + `rust/README` S-B* |
| D-14 | BRV-D8 **has no BUILD family** | opened `BRV-BLD-0001..0014` (see contract §3 for each), with no semantics invented beyond the shape | gate criteria / tool resolution / incremental all need distinguishable failure reasons; the code table awaits backporting (same direction as D-11) |
| D-15 | §9.2 mixes the tool's own build entry (`cxx/ Makefile`) and image building in the repo-root Makefile's "two-stage" narrative | the top-level `Makefile` **builds only the tool**; the image targets are downgraded to **thin delegation aliases** (`make all|run|smoke|…` → `$(BRICKIE) build|run|test|…`), with the real thing in `brickie build` + `tests/gates.toml`; the source set / flags / rules are removed from the Makefile and pinned by the ② gate in `make check-build` | ADR-0003 S1+S4 (transfer of build ownership + the reduction period of Makefile retirement); the aliases are kept during migration so as not to disrupt existing CI/habits |

## 7. Change discipline

- **Decisions belong only to L0/L1 (`rust/`), rendering only to L2 (`cxx/`), orchestration only to L5 (`python/`).** Do not add business rules in Python, and do not add conditional decisions in the C++ renderer; the "glue-layer purity check" and reverse acceptance cases enforce this.
- **Never hand-edit artifacts.** After changing `brick.toml`, rebuild (`brickie gen`); artifacts follow the same discipline as goldens.
- **Templates are data.** To differentiate the four skeleton kinds, change `templates/`, do not add conditional syntax to the renderer — the template language is replaced by "directories by `api_type`/`plugin_type`/`lang`".
- **After changing `python/**`, `templates/**`, `schema/**` or a native tool, remember to republish the seed.** They are all embedded into the entry ELF by `freeze.py` (`PAYLOAD_ROOTS = ("python","templates","schema")`), so `make prebuilt` is not just "recompiling C++" — if the payload changes, `brickie` changes; the "seed byte-for-byte identical" checks in `make prebuilt-check` and the end-to-end cases will catch an omission.
- **The launcher does not touch business logic.** `cxx/launcher.cpp` only does "locate/unpack/set up environment/forward"; not one line of Python semantics may be added; the code in the payload is still the same `python/brickie/`.
- **After a change first run `make selftest && make test`** (to verify together with the entry ELF, then run `make prebuilt`).

## 8. Authoritative description of the L5 (Python) frontend

The module map of `python/brickie/`, per-command status, call sites into the two native tools, the schema shape validation allowed set, remaining TODOs and contract gaps (T-*, G-*) are **all in [`python/brickie/README.md`](python/brickie/README.md)** — this section keeps only a pointer. The current-state table in §1 of this file is a **cross-layer** overview; if the two conflict, code and `docs/contract.md` win.

## 9. "To be confirmed" list from doc-sync time (now all closed)

> The ledger left from when doc sync and implementation ran in parallel; closed item by item as the implementation landed.

| # | State at writing time | Closing result |
|---|---|---|
| 1 | `rust/src/{solver,check,iface}.rs` were still `unimplemented` stubs and `brickie check` returned exit 2 | **Closed**: 15 command families landed, `brickie-core --selftest` = 432 cases; `check`/`dep *`/`ver *`/`iface *` measured 0 errors on the prototype tree |
| 2 | seed lagged behind the sources (`prebuilt-check` red) | **Closed**: the three seed pieces were republished from the final sources, `make tools-prebuilt-check` all ok |
| 3 | `schema.py`'s docstring/lookup list still said "in-package mirror `python/brickie/schema/`" | **Closed**: lookup order changed to `$BRICKIE_SCHEMA_ROOT` → `<tool_root>/schema` (canonical location) → in-package compatibility read (optional) → repo fallback |
| 4 | `cxx/Makefile`'s `PAYLOAD_SRCS` was narrower than `freeze.py`'s `PAYLOAD_ROOTS` ⇒ changing non-`.py`/schema files did not repack the payload (silent inconsistency) | **Closed**: `PAYLOAD_SRCS` now enumerates from the same source `PAYLOAD_ROOTS = python templates schema` (excluding `__pycache__`/`*.pyc`) |
| 5 | default `make` could not build the image because `brickie-check` was unimplemented | **Closed**: `brickie-check` is now a **real** composition-period validation (it stops only on a red declaration surface); default `make` passes on the prototype tree |

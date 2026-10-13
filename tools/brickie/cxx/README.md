# brickie-gen — L2 generator (C++) and L5 entry ELF

This directory holds **sources only**; everything built goes out of tree to `build/host/<host-arch>/<host-os>/` (see `../../mk/host.mk`). This file records the **C++ side of the process-boundary contract**, the **template variable contract** and **known gaps**.

## 1. Responsibilities and boundaries

| Artifact | Language | Responsibility |
|---|---|---|
| `brickie-core` | Rust (L0/L1, in `../rust/`) | name contract / subkind derivation / phase / path layout / version / IFACE-IR / solving — **all decisions**; and emits `plan-*` (paths + template ids + vars) |
| `brickie-gen` | C++ (this directory) | **template rendering only**: `templates_root` + `artifacts[{path,template,vars}]` → `files[{path,kind:"rendered",content,mode}]` |
| `brickie` | C++ launcher (`launcher.cpp`) + embedded payload | unpacks `python/**` + `templates/**` + the native tools, and runs the L5 frontend with the system `python3` |

**There is no taxonomy/solver/version/IFACE-IR/path-layout decision anywhere in C++**. The old `rules.*` (name contract / subkind / phase / symbol derivation / path layout) and `gen_new.*` (`new` / `plan-new`) have been deleted; the rules and plans moved to `brickie-core`.

## 2. Command surface

Requests / responses are **one line of JSON over stdio**; the response fields are fixed to `{protocol, status, exit_code, diagnostics, files, data}`.

| `command` | `args` | Response |
|---|---|---|
| `render` | `{templates_root, artifacts:[{path, template, vars, mode?}]}` | `files:[{path, kind:"rendered", content, mode}]` |
| `--selftest` (does not go through the envelope) | — | JSON round-trip / render / paths and modes / payload determinism self-check; exit code 0/1 |
| `--version` / `--help` | — | text |

* Process exit code: **0** = protocol handled successfully (even if the response's `exit_code != 0`); **2** = the tool's own failure (stdin is not JSON, etc.). Business exit codes are only in the response's `exit_code`.
* `status`: `ok` / `internal_error` (tool failure).
* `protocol != 1` or missing `protocol` ⇒ diagnostic `BRV-PROTO-0001` + `exit_code = 2` + `status = "ok"`.
* Usage errors (unknown command / missing `templates_root` / `artifacts` shape error / path escape / template unreadable / **template missing variable** / invalid mode / duplicate output path) ⇒ code-less diagnostic + `exit_code = 2` + `files = []`.
* Any artifact error ⇒ **no files are produced** (avoiding half-generation), and all diagnostics are reported.
* Entries in the plan with `kind = "machine"` (e.g. `plan-init`'s `brickie.lock`, whose content belongs to `brickie-core`) are **skipped**: no template is read and it is not a usage error — the renderer only handles `human` / `generated` / `rendered`.
* Rendering has **no conditional syntax**: differentiation comes from the `templates/<api_type>/<plugin_type>/<lang>/` directory split; `vars` only does `{{key}}` placeholder substitution (keys = `[A-Za-z0-9_]`, case-sensitive).

Example:

```sh
echo '{"protocol":1,"command":"render","args":{
  "templates_root":"'$PWD'/templates",
  "artifacts":[{"path":"service/crypto/brick.toml",
                "template":"native/ability/c/brick.toml.tmpl",
                "vars":{"name":"service/crypto","short":"crypto", "...":"..."}}]}}' \
| build/host/x86-64/linux/bin/brickie-gen
```

## 3. Template variable contract (agreement with `brickie-core`)

`artifacts[].vars` returned by `brickie-core`'s `plan-new` / `plan-init` **must** contain the keys in the table below; a missing key is a usage error in the renderer.

### 3.1 Skeleton templates (`templates/native/<plugin_type>/c/*`)

| Variable | Meaning | User |
|---|---|---|
| `name` | full plugin name (`service/crypto`) | all |
| `schema` | `brick.toml`'s `schema` value (`1`) | brick.toml |
| `plugin_type` | `app`/`interface`/`ability`/`platform`/`core` (template directories are already split; current templates hardcode it literally per directory) | (reserved) |
| `api_type` | `native`/`runtime_adapter`/`third_party` | brick.toml / README |
| `subkind` | ability subdivision (`""` for non-ability) | ability brick.toml |
| `lang` | `c` | brick.toml / README |
| `phase` | `early`/`core`/`late`/`app` | brick.toml / README |
| `sched_class` | `SAFE_PREEMPT`/`COOP_ONLY`/`TT_SAFE` | ability brick.toml |
| `version` | four-segment version (skeleton initial value `0.1.0.0`) | brick.toml / README / descriptor |
| `summary` / `license` | human-readable fields | brick.toml |
| `export_name` | `[[export]].name` (currently = `short`) | brick.toml |
| `export_api_iface` | `[[export]].api_iface` (must = `api_type`) | brick.toml |
| `export_form` | `[[export]].form` (skeleton uses `api`) | brick.toml |
| `short` | last name segment (`crypto` / `qemu-aarch64`); used for **paths** and header inclusion | src / include |
| `symbol_prefix` | C identifier prefix (non-alphanumeric → `_`, trailing `_` appended: `qemu_aarch64_`) | src / include / smoke / descriptor |
| `header_guard` | header guard (`PLATFORM_QEMU_AARCH64_H`) | include |
| `descriptor_symbol` | descriptor `static const` instance name (`_br_plugin_crypto`) | descriptor |

### 3.2 Descriptor templates (`templates/descriptor/<api_type>/<lang>/plugin_desc.c.tmpl`)

| Variable | Meaning |
|---|---|
| `descriptor_include` | the described plugin's own public header (`crypto/crypto.h`), used for `#include "{{descriptor_include}}"` |

### 3.3 Product templates (`templates/product/<lang>/product.toml.tmpl`, `plan-init`)

| Variable | Meaning |
|---|---|
| `schema` | `1` |
| `name` | product name (`hsm`) |
| `version` | four-segment version |
| `app` | the single APP directory (`app/hsm`) |

### 3.4 Naming notes and differences

* The canonical name given by the task/contract is `namespace`; `brickie-core` actually emits **`ns`** (while also keeping the uppercase `NS` and similar first-cut variable names). Current templates use **neither**, so there is no gap; if templates later need namespace semantics, either add a `namespace` key in core or switch templates to `ns` (registered as a cross-agent interface item).
* `plugin_type` is currently hardcoded literally per directory in templates (`"app"` written in `native/app/c/`), because the contract convention is **directory split**; core still passes `plugin_type`, for future deduplication.
* The table above is the **authoritative list**; any new template variable must be registered here first, then added by core.

## 4. Template directory layout

```
templates/<api_type>/<plugin_type>/<lang>/   # skeleton: brick.toml / src.c / include.h / smoke.toml / README.md
templates/descriptor/<api_type>/<lang>/      # descriptor (orthogonal to plugin_type)
templates/product/<lang>/                    # product skeleton
```

The first line of an artifact is marked `brickie:generated` (already present in descriptor templates).

## 5. Build, bootstrap seed and degradation behavior

```sh
cd prototype/tools/brickie
make            # = make cxx: brickie-gen + brickie (conditionally embedding brickie-core)
make core       # cargo build --release -p brickie-core → build/host/<triple>/bin/brickie-core
make selftest   # brickie-gen --selftest
make launcher-smoke
make prebuilt       # publish the **three-piece** seed {brickie,brickie-gen,brickie-core}
make prebuilt-check # whether the three pieces lag behind (does not touch disk)
```

* `CARGO_HOME` / `CARGO_TARGET_DIR` default to `prototype/build/{cargo-home,cargo-target}` (under managed environments `$HOME/.cargo` may be read-only); both can be overridden by environment variable.
* **Degradation**: `cxx/Makefile` uses `CORE_EMBED := $(if $(wildcard $(CORE_BIN)),...)` to conditionally embed `brickie-core`. When the Rust side has not been built, **`make` / `make cxx` / `make selftest` succeed as usual**, printing only a notice; the payload and entry ELF exclude core (in that case the launcher does not set `BRICKIE_CORE`).
* **Gates do not degrade**: `make prebuilt` depends on `core` and treats `brickie-core` as a required piece — missing core ⇒ FAIL (the bootstrap seed must have all three pieces). `make prebuilt-check` also checks all three.
* After the launcher unpacks: if the payload contains `bin/brickie-core` it sets `BRICKIE_CORE` (like `BRICKIE_GEN`, it **does not override** an explicit environment variable); otherwise it leaves it empty.

## 6. Payload (`freeze.py`)

`freeze.py` packs `python/**` + `templates/**` + the native tools given by `--embed` into a **normalized uncompressed ustar tar** (paths sorted, mode bits explicit, mtime/uid/gid zeroed) ⇒ byte-for-byte reproducible. `--embed` may be repeated (currently `brickie-gen` is always embedded, `brickie-core` conditionally), and embedding the same location twice errors out before packing. `payload.tar` lands in `build/host/<triple>/share/brickie/` for inspection.

## 7. Known gaps / TODO

1. **The Rust core is a moving target**: if `rust/` fails to build, `make` / `make cxx` still succeed (payload excludes core) while `make prebuilt` FAILs — this is exactly the intended degradation convention. **A red gate ≠ a regression**: as long as the payload sources (`python/**`, `templates/**`, `schema/**`) or a native tool changed after the last `make prebuilt`, `prebuilt-check` and the "seed byte-for-byte identical" cases report red until the seed is republished.
2. **Deleting core does not automatically repack the payload**: make cannot notice "an embedded piece was deleted"; adding core back (the new file is newer than `PAYLOAD_C`) repacks normally. `rm build/.../obj/cxx/brickie_payload.cpp` forces a repack.
3. **The missing-variable diagnostic has no dedicated code**: contract §6 only says "a usage error of the `BRV-GEN-0002` family"; BRV-D8's code table has no matching code, so currently a **code-less usage error** is used (`code: null`, `exit_code = 2`), in the same family as G-2/G-5. Suggested: add `BRV-GEN-0003` (render missing variable) and backport it to the code table.
4. **The descriptor's four-segment `.ver` is hardcoded `{0,1,0,0}` in the template**: the skeleton initial value = the declaration-surface initial value, so the template literal is not a decision; if `new` later supports an initial value other than `0.1.0.0`, core must additionally emit the four variables `compat_gen/major/minor/revise` (or make the descriptor read the snapshot instead).
5. **`templates/app/c/**` was not created**: `plan-init`'s app skeleton reuses `templates/native/app/c/` (core's `plan.rs` already does this), so no separate directory is needed.
6. **`-j` concurrency in `build/`**: `payload` / `prebuilt` depend on the core location; the top-level `prebuilt: core` builds core serially first, then enters the cxx sub-make, ensuring core already exists when the sub-make parses.

## 8. Measurements (local x86-64/linux)

* `cd prototype && make tools` — g++ 15.2 all green, zero warnings; `make selftest` — all 30 items `ok`; `make launcher-smoke` — `brickie 0.1.0`.
* `render` — 4 plugin kinds × 5 templates + descriptors + product (31 files total) rendered with the variable set that `brickie-core` actually emits: `exit_code = 0`, `files` complete, no diagnostics; the 9 error paths — missing variable / out-of-bounds path / invalid mode / protocol mismatch, etc. — all return `exit_code = 2`.
* End-to-end (via the single-file `brickie` ELF): `brickie new` for each of the four plugin kinds all `exit 0` with all 6 artifacts present (`platform/qemu-aarch64`'s symbol prefix measured as `qemu_aarch64_`); `brickie init hsm` `exit 0`, with product.toml + app skeleton + `brickie.lock` present (the machine file `brickie.lock` is correctly skipped by render, and its content is written by L5).
* Seed: the **three pieces** were published via the regular `core → cxx` path; when this file was written, the seed's `brickie-core --selftest` = `ok 247 cases`. Byte-for-byte consistency holds only when "after the last publication the payload sources did not change again"; if they did, `make prebuilt` must republish. Copying only the single file `brickie` into an empty directory, under `env -i PATH=/usr/bin:/bin`, `new platform/qemu-aarch64` produced all 6 artifacts (BRICKIE_GEN / BRICKIE_CORE both point at the unpacked embedded pieces).
* Running `make cxx` again is a no-op (0 compilations) ⇒ the payload/ELF is reproducible; `prebuilt-check` is all green only when the seed keeps up with the **last publication**.
* Payload: embeds `bin/brickie-gen` and `bin/brickie-core` (both 0755); the launcher was measured setting `BRICKIE_GEN` / `BRICKIE_CORE`, forwarding the exit code, and not overriding explicit environment variables; conditional embedding and the three-piece `prebuilt` / `prebuilt-check` were verified with a "fake core" (cleaned up after verification).

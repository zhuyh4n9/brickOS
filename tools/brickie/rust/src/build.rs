//! `brickie build` — declaration-driven build orchestration (core side: plan + verdict).
//! Sources come from the `product.toml [select]` closure + each plugin's `[build].sources`; compile
//! flags from `product.toml [build]` + platform `[build.target]`; gates from `tests/gates.toml`.
//! L5 only executes `data.steps[i].argv` and writes files; log criteria (`judge`) are decided here,
//! so red/green has one source of truth. Core owns tool resolution and the incremental decision
//! (`stamp` = argv + inputs (size, mtime), injectable via `context.files`); L5 owns process
//! orchestration (concurrency, timeouts, log capture).
//! This command family requires cc/objcopy (composition-time commands do not).

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::diag::{Diags, Severity, EXIT_OK, EXIT_RED};
use crate::model::{self, Plugin, ProductBuild, TreeLoad};
use crate::proto::{Request, Response};
use crate::solver;

/// `product.toml` lacks `[build]` (or the whole product is absent).
pub const CODE_NO_BUILD: &str = "BRV-BLD-0001";
/// `[build].sources` expands to nothing (declared but matching no file).
pub const CODE_EMPTY_SOURCES: &str = "BRV-BLD-0002";
/// No platform plugin declares `[build.target]` (no way to know arch/linker script).
pub const CODE_NO_TARGET: &str = "BRV-BLD-0003";
/// Several platform plugins declare `[build.target]`: which one is authoritative is unclear.
pub const CODE_TARGET_AMBIGUOUS: &str = "BRV-BLD-0004";
/// Tool resolution failed (compiler / objcopy / qemu absent); environment error (exit code 2).
pub const CODE_TOOL_MISSING: &str = "BRV-BLD-0005";
/// A plugin in the closure has no `[build]` (it gets composed but says nothing about compiling).
pub const CODE_UNIT_NO_BUILD: &str = "BRV-BLD-0006";
/// A literal path in `[build].sources` does not exist; a non-matching glob is not an error.
pub const CODE_MISSING_SOURCE: &str = "BRV-BLD-0007";
/// No gate with this name in `tests/gates.toml` (or the file is absent).
pub const CODE_GATE_UNKNOWN: &str = "BRV-BLD-0008";
/// Gate judged red (log missing a require / hitting a forbid / missing a PASS case tag).
pub const CODE_GATE_FAILED: &str = "BRV-BLD-0009";
/// Shape problem in `[build]` (illegal artifact location / flag table).
pub const CODE_BUILD_SHAPE: &str = "BRV-BLD-0010";
/// An executed step failed (compile/link/script non-zero exit) — detected by L5, **code supplied by core** (see `Step::to_json`).
pub const CODE_STEP_FAILED: &str = "BRV-BLD-0011";
/// A regex in `tests/gates.toml` exceeds the **subset** core supports (always reported, never silently unmatched).
pub const CODE_GATE_SHAPE: &str = "BRV-BLD-0012";
/// Generated artifact belongs to a plugin **outside the closure** ⇒ skipped (info: not a defect, but must be visible).
pub const CODE_GEN_NOT_SELECTED: &str = "BRV-BLD-0013";
/// Generated artifact under `build/gen/` with no identifiable owner (stale leftover) ⇒ skipped.
/// 0013 = owner known but unselected; this = no plugin of that name, hence separate codes.
pub const CODE_GEN_ORPHAN: &str = "BRV-BLD-0014";

/// Dispatch for the build command family (forwarded from `main.rs`).
pub fn run(req: &Request) -> Response {
    match req.command.as_str() {
        "build" => cmd_build(req),
        "clean" => cmd_clean(req),
        "run" => cmd_run(req),
        "size" => cmd_size(req),
        "disasm" => cmd_disasm(req),
        "test" => cmd_test(req),
        "judge" => cmd_judge(req),
        other => {
            let mut d = Diags::new();
            d.usage(
                format!("no command `{other}` in the build family"),
                "see the build command family in docs/contract.md §5.4",
            );
            Response::ok(d, json!({}))
        }
    }
}

/// One compilation unit (core body / plugin / generated artifacts); paths are relative to root.
#[derive(Clone, Debug, Default)]
struct Unit {
    /// Plugin name (`service/trace`), or `core` / `build/gen (generated)`.
    name: String,
    /// Unit dir (`""` = repo root).
    dir: String,
    /// Sources (globs expanded, lexicographic).
    sources: Vec<String>,
    includes: Vec<String>,
    defines: Vec<String>,
    /// Unit cflags (`[build].cflags`), applied after product cflags so they can disable a warning.
    cflags: Vec<String>,
    plugin: Option<String>,
}

#[derive(Clone, Debug, Default)]
struct Target {
    arch: String,
    cross: String,
    arch_flags: Vec<String>,
    linker_script: Option<String>,
    qemu_binary: String,
    qemu_machine: String,
    qemu_cpu: String,
    qemu_memory: String,
    qemu_extra: Vec<String>,
    owner: String,
}

#[derive(Clone, Debug, Default)]
struct Step {
    label: String,
    /// `compile` / `link` / `objcopy` / `run` / `script` / `hosttest` / `hostrun` / `backend`
    kind: String,
    argv: Vec<String>,
    /// Steps sharing a `group` may run concurrently (L5's orchestration contract).
    group: u32,
    /// Log destination relative to root (`None` = inherit stdio).
    log: Option<String>,
    timeout_s: Option<u64>,
    /// Expected to end in a timeout (QEMU gate: the image runs until killed).
    expect_timeout: bool,
    /// Gate name requiring a core verdict (see `cmd_judge`).
    judge: Option<String>,
    stdout_lines: Option<u64>,
    /// Files this step writes (incremental state + cleanup).
    outputs: Vec<String>,
    /// Files produced by other steps of this plan. When an upstream step fails, L5 must skip
    /// downstream instead of rerunning on stale outputs (stale objects still link).
    consumes: Vec<String>,
}

impl Step {
    fn to_json(&self) -> Value {
        json!({
            "label": self.label,
            "kind": self.kind,
            "argv": self.argv,
            "group": self.group,
            "log": self.log,
            "timeout_s": self.timeout_s,
            "expect_timeout": self.expect_timeout,
            "judge": self.judge,
            "stdout_lines": self.stdout_lines,
            "outputs": self.outputs,
            "consumes": self.consumes,
            // Failure codes ship with the step, so codes have one source of truth; L5 invents none.
            "fail_code": CODE_STEP_FAILED,
            "tool_code": CODE_TOOL_MISSING,
        })
    }
}

struct Ctx {
    root: PathBuf,
    profile: String,
    backend: String,
    tag: String,
    units: Vec<Unit>,
    target: Target,
    pb: ProductBuild,
    obj_dir: String,
    elf: String,
    bin: String,
    map: String,
    state_path: String,
    gen_dir: String,
    cflags: Vec<String>,
    asflags: Vec<String>,
    ldflags: Vec<String>,
    /// Product config as defines (`[kernel].hz` ⇒ `BR_CFG_TICK_HZ=<hz>`), pushed to all C units
    /// so "timer interrupts per second" has one source of truth. Distinct from `[build].cflags`.
    config_defines: Vec<String>,
    /// Unit name → include dirs (root-relative, sorted, deduped). No global union: core + self +
    /// the declared dep closure, so an undeclared dep fails at compile time (header not found).
    unit_includes: BTreeMap<String, Vec<String>>,
    tools: BTreeMap<String, Vec<String>>,
    resolved: BTreeMap<String, String>,
    tool_what: BTreeMap<String, String>,
    /// `context.files[path] = (size, mtime_ns)`; when `Some`, no disk access (self-test).
    stat_override: Option<BTreeMap<String, (u64, i64)>>,
    force: bool,
}

fn hex_of(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// `sha256:<64 hex>`.
fn sha256_tag(parts: &[String]) -> String {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p.as_bytes());
        h.update([0u8]);
    }
    format!("sha256:{}", hex_of(&h.finalize()))
}

/// Simple glob match: `*` (does not cross `/`) and `?`.
fn glob_match(pat: &str, name: &str) -> bool {
    let p: Vec<char> = pat.chars().collect();
    let n: Vec<char> = name.chars().collect();
    // Iterative two-pointer with a backtracking point, avoiding recursion stack blowup.
    let (mut pi, mut ni) = (0usize, 0usize);
    let (mut star, mut mark) = (usize::MAX, 0usize);
    while ni < n.len() {
        if pi < p.len() && (p[pi] == '?' || p[pi] == n[ni]) {
            pi += 1;
            ni += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = pi;
            mark = ni;
            pi += 1;
        } else if star != usize::MAX {
            pi = star + 1;
            mark += 1;
            ni = mark;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

/// Whether a wildcard is present (decides if "no match" is an error (literal) or empty (glob)).
fn has_wildcard(pat: &str) -> bool {
    pat.contains('*') || pat.contains('?')
}

/// Expand a glob relative to `base_dir`, returning root-relative paths (lexicographic). Supports
/// `*`/`?` (not crossing `/`) and `**` (any number of dirs, including zero): artifact depth follows
/// plugin-name namespace depth, so a fixed `*/*/` pattern would not be a contract.
fn expand_glob(root: &Path, base_dir: &str, pattern: &str) -> Vec<String> {
    let base = if base_dir.is_empty() {
        root.to_path_buf()
    } else {
        root.join(base_dir)
    };
    let comps: Vec<&str> = pattern.split('/').filter(|c| !c.is_empty()).collect();
    let mut abs: Vec<PathBuf> = Vec::new();
    glob_walk(&base, &comps, &mut abs);
    abs.sort();
    abs.dedup();
    let mut out: Vec<String> = Vec::new();
    for p in abs {
        if let Ok(rel) = p.strip_prefix(root) {
            out.push(rel.to_string_lossy().replace('\\', "/"));
        }
    }
    out.sort();
    out.dedup();
    out
}

fn glob_walk(dir: &Path, comps: &[&str], out: &mut Vec<PathBuf>) {
    let Some(comp) = comps.first().copied() else {
        return;
    };
    let rest = &comps[1..];
    if comp == "**" {
        // `**` matches zero levels ⇒ continue in place with rest; also matches one or more ⇒ descend keeping `**`.
        glob_walk(dir, rest, out);
        let mut subs: Vec<PathBuf> = match fs::read_dir(dir) {
            Ok(entries) => entries
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect(),
            Err(_) => Vec::new(),
        };
        subs.sort();
        for sub in subs {
            glob_walk(&sub, comps, out);
        }
        return;
    }
    let last = rest.is_empty();
    let mut hits: Vec<PathBuf> = Vec::new();
    if has_wildcard(comp) {
        if let Ok(entries) = fs::read_dir(dir) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if glob_match(comp, &name) {
                    hits.push(entry.path());
                }
            }
        }
    } else {
        let path = dir.join(comp);
        if path.exists() {
            hits.push(path);
        }
    }
    hits.sort();
    for path in hits {
        if last {
            if path.is_file() {
                out.push(path);
            }
        } else if path.is_dir() {
            glob_walk(&path, rest, out);
        }
    }
}

fn plugin_dir_of(name: &str) -> String {
    name.to_string()
}

/// A plugin's include dirs (relative to repo root): `[build].includes` expand relative to the **plugin root**.
fn plugin_incs(p: &model::Plugin) -> Vec<String> {
    let dir = plugin_dir_of(&p.name);
    p.build
        .as_ref()
        .map(|b| b.includes.iter().map(|i| format!("{dir}/{i}")).collect())
        .unwrap_or_default()
}

/// Parse a `.d` file (make syntax: `target: dep dep …`, supporting `\` line continuations).
fn parse_depfile(path: &Path, root: &Path) -> Vec<String> {
    let text = match fs::read_to_string(path) {
        Ok(t) => t,
        Err(_) => return Vec::new(),
    };
    let joined = text.replace("\\\n", " ");
    let mut out: Vec<String> = Vec::new();
    for line in joined.lines() {
        let Some((_, rhs)) = line.split_once(':') else {
            continue;
        };
        for tok in rhs.split_whitespace() {
            if tok.is_empty() {
                continue;
            }
            // `.d` paths are relative to the compile-time cwd (= repo root).
            let rel = tok.replace('\\', "/");
            if rel.starts_with('/') {
                if let Ok(p) = Path::new(&rel).strip_prefix(root) {
                    out.push(p.to_string_lossy().replace('\\', "/"));
                }
            } else {
                out.push(rel);
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

fn default_obj_dir() -> String {
    "build/obj".to_string()
}

fn load_ctx(req: &Request, tree: &TreeLoad) -> Result<(Ctx, Diags), Diags> {
    let mut d = Diags::new();
    let root = PathBuf::from(&req.root);

    let Some(prod) = tree.product.clone() else {
        d.shape(
            CODE_NO_BUILD,
            "",
            "product.toml",
            "",
            "no `product.toml`: building needs a product-level declaration (which plugins + how to compile)",
            "run `brickie init <product>` or add product.toml; contract §7.2",
        );
        return Err(d);
    };
    let Some(pb) = prod.build.clone() else {
        d.shape(
            CODE_NO_BUILD,
            &prod.name,
            "product.toml",
            "build",
            "`product.toml` lacks `[build]` (flag table and artifact locations)",
            "add `[build]`: cflags/asflags/ldflags + artifact locations; see contract §7.2",
        );
        return Err(d);
    };

    // `[kernel].hz`, default 200 Hz (5 ms/tick): becomes `BR_CFG_TICK_HZ` for all units, so the
    // platform timer reload and core jiffies agree. Re-checked although the schema sets minimum: 1,
    // because composition-time validation and build may be called separately.
    const DEFAULT_TICK_HZ: i64 = 200;
    let tick_hz = prod.kernel_hz.unwrap_or(DEFAULT_TICK_HZ);
    if tick_hz < 1 {
        d.shape(
            CODE_BUILD_SHAPE,
            &prod.name,
            "product.toml",
            "kernel.hz",
            format!("`[kernel].hz` must be ≥ 1 (got {tick_hz})"),
            "ADR-0017: HZ = timer interrupts per second; 0/negative is meaningless; default (omitted) = 200",
        );
    }
    let config_defines = vec![format!("BR_CFG_TICK_HZ={tick_hz}")];

    // `--profile` overrides `[product].stage`.
    let profile = req
        .arg_str("profile")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| prod.stage.clone());
    if !matches!(profile.as_str(), "dev" | "release") {
        d.usage(
            format!("`--profile` must be dev | release (got {profile})"),
            "BRV-D9 exit code 2",
        );
        return Err(d);
    }

    let backend = req
        .arg_str("backend")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "direct".to_string());
    if !matches!(backend.as_str(), "direct" | "make" | "ninja") {
        d.usage(
            format!("`--backend` must be direct | make | ninja (got {backend})"),
            "BRV-D9 exit code 2",
        );
        return Err(d);
    }

    let tag = req.arg_str("tag").unwrap_or_default();
    let tag_suffix = if tag.is_empty() {
        String::new()
    } else {
        format!("-{tag}")
    };

    let owners: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| p.build_target.is_some())
        .collect();
    let mut target = Target {
        arch: String::new(),
        cross: "aarch64-linux-gnu-".to_string(),
        arch_flags: Vec::new(),
        linker_script: None,
        qemu_binary: "qemu-system-aarch64".to_string(),
        qemu_machine: "virt".to_string(),
        qemu_cpu: String::new(),
        qemu_memory: "128M".to_string(),
        qemu_extra: Vec::new(),
        owner: String::new(),
    };
    match owners.len() {
        0 => {
            d.shape(
                CODE_NO_TARGET,
                "",
                "product.toml",
                "",
                "no plugin declares `[build.target]`: arch / cross prefix / linker script unknown",
                "platform plugin should add `[build.target]` (exactly one platform per image, §8.3 ④)",
            );
            return Err(d);
        }
        1 => {
            let p = owners[0];
            let t = p.build_target.clone().unwrap_or_default();
            target.arch = t.arch.clone();
            if !t.cross.is_empty() {
                target.cross = t.cross.clone();
            }
            target.arch_flags = t.arch_flags.clone();
            target.linker_script = if t.linker_script.is_empty() {
                None
            } else {
                Some(format!("{}/{}", plugin_dir_of(&p.name), t.linker_script))
            };
            if !t.qemu_binary.is_empty() {
                target.qemu_binary = t.qemu_binary.clone();
            }
            if !t.qemu_machine.is_empty() {
                target.qemu_machine = t.qemu_machine.clone();
            }
            target.qemu_cpu = t.qemu_cpu.clone();
            if !t.qemu_memory.is_empty() {
                target.qemu_memory = t.qemu_memory.clone();
            }
            target.qemu_extra = t.qemu_extra.clone();
            target.owner = p.name.clone();
        }
        _ => {
            d.shape(
                CODE_TARGET_AMBIGUOUS,
                "",
                "product.toml",
                "",
                format!(
                    "{} plugins declare `[build.target]`: {}",
                    owners.len(),
                    owners
                        .iter()
                        .map(|p| p.name.clone())
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
                "exactly one platform per image (§8.3 ④); other plugins' build facts go in `[build]`",
            );
            return Err(d);
        }
    }

    let sol = solver::solve(tree, &profile, false);
    let closure = sol.closure.clone();
    let mut diags = sol.into_diags();
    if diags.has_error() {
        diags.merge(d);
        return Err(diags);
    }
    let mut units: Vec<Unit> = Vec::new();

    // Self-test master switch: decides both core selftest sources and the generated selftest hook.
    let selftest_on = tree
        .product
        .as_ref()
        .map(|p| p.selftest_on())
        .unwrap_or(false);

    // Plugins in the closure, in topological order (makes link order reproducible too). The core
    // plugin is one of them: it is composed and compiled like any other plugin, from its own
    // `brick.toml [build]`, and only its selftest sources are gated by the product switch below.
    let mut ordered: Vec<String> = closure.topo_order.clone();
    for name in &closure.selected {
        if !ordered.contains(name) {
            ordered.push(name.clone());
        }
    }
    ordered.retain(|n| closure.selected.contains(n));
    for name in &ordered {
        let Some(p) = tree.by_name(name) else { continue };
        let dir = plugin_dir_of(&p.name);
        let b = p.build.clone().unwrap_or_default();
        if b.sources.is_empty() {
            diags.shape(
                CODE_UNIT_NO_BUILD,
                &p.name,
                format!("{dir}/brick.toml"),
                "build.sources",
                format!("plugin `{}` in the closure has no `[build].sources`: it gets composed but says nothing about compiling", p.name),
                "add `[build] sources/includes`; contract §7.1",
            );
            continue;
        }
        let mut sources = expand_many(&root, &dir, &b.sources, &p.name, &format!("{dir}/brick.toml"), "build.sources", &mut diags);
        // Selftest master switch: off ⇒ the core's suite is not compiled at all. The descriptor
        // writes NO_HOOK for the same switch, so nothing references these symbols either way.
        if selftest_on && p.plugin_type == "core" {
            sources.extend(expand_many(
                &root,
                "",
                &pb.selftest_sources,
                &p.name,
                "product.toml",
                "build.selftest_sources",
                &mut diags,
            ));
        }
        units.push(Unit {
            name: p.name.clone(),
            dir: dir.clone(),
            sources,
            includes: b
                .includes
                .iter()
                .map(|i| format!("{dir}/{i}"))
                .collect(),
            defines: b.defines.clone(),
            cflags: b.cflags.clone(),
            plugin: Some(p.name.clone()),
        });
    }

    // (c) Generated artifacts (`build/gen/**`), produced by `brickie gen`.
    if !pb.gen_sources.is_empty() {
        let sources = expand_many(&root, "", &pb.gen_sources, "build/gen", "product.toml", "build.gen_sources", &mut diags);
        // Keep only artifacts of selected plugins: a descriptor compiled without its .c links
        // as an undefined symbol; artifacts naming no plugin in the tree are skipped as stale.
        let (sources, skipped, orphans) = filter_gen_sources(&sources, tree, &closure.selected);
        for path in &skipped {
            diags.push(
                crate::diag::Diag::new(
                    Severity::Info,
                    format!("generated artifact `{path}` belongs to a plugin outside the closure, skipped (not in image)"),
                )
                .code(CODE_GEN_NOT_SELECTED)
                .file("product.toml")
                .span("build.gen_sources")
                .hint("add the plugin to [select].plugins, or exclude it from the artifact glob"),
            );
        }
        for path in &orphans {
            diags.push(
                crate::diag::Diag::new(
                    Severity::Info,
                    format!(
                        "generated artifact `{path}` belongs to no plugin in the tree (stale leftover), skipped (not in image)"
                    ),
                )
                .code(CODE_GEN_ORPHAN)
                .file("product.toml")
                .span("build.gen_sources")
                .hint("run `brickie gen` (auto-clears stale descriptors) or `brickie clean --all`"),
            );
        }
        if !sources.is_empty() {
            units.push(Unit {
                name: "build/gen".to_string(),
                dir: String::new(),
                sources,
                includes: tree
                    .plugins
                    .iter()
                    .filter(|p| closure.selected.contains(&p.name))
                    .map(|p| format!("{}/{}", plugin_dir_of(&p.name), "include"))
                    .collect(),
                defines: Vec::new(),
                cflags: Vec::new(),
                plugin: None,
            });
        }
    }

    // Per-unit include surface = core + self + declared dep closure; no global union, so an
    // undeclared dep is a hard compile error. The core plugin's headers are the **ambient base** of
    // every unit (nothing has to declare a dep on core to include `<br/core/**>`); the core unit
    // additionally gets the platform headers, because its clock reads the platform timer.
    let platform_incs: Vec<String> = tree
        .plugins
        .iter()
        .filter(|p| p.plugin_type == "platform" && closure.selected.contains(&p.name))
        .flat_map(plugin_incs)
        .collect();
    let core_incs: Vec<String> = tree
        .plugins
        .iter()
        .filter(|p| p.plugin_type == "core" && closure.selected.contains(&p.name))
        .flat_map(plugin_incs)
        .collect();
    let is_core_unit = |u: &Unit| {
        u.plugin
            .as_deref()
            .and_then(|n| tree.by_name(n))
            .map(|p| p.plugin_type == "core")
            .unwrap_or(false)
    };
    let mut unit_includes: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for u in &units {
        // Order is semantic, not cosmetic: `-I` is searched left to right, and two ports can each
        // ship a forwarding shim for the same u-boot header (`<linux/types.h>`, `<asm/io.h>`, ...).
        // The unit's **own** shims must therefore win over a dependency's. Sorting this list (which
        // is what used to happen) made the winner an accident of directory naming: it worked only
        // while every consumer's dir sorted before its provider's (ADR-0033 moved `uboot-mmc` to
        // `driver/`, which sorts before `fs/`, and u-boot's `__u64`/`le16_to_cpu` got redefined).
        let mut incs: Vec<String> = core_incs.clone();
        if is_core_unit(u) {
            incs.extend(platform_incs.clone());
        } else if u.name == "build/gen" {
            incs.extend(u.includes.clone());
        } else {
            incs.extend(u.includes.clone());
            for d in model::dep_closure(&tree, &u.name) {
                if let Some(dp) = tree.by_name(&d) {
                    incs.extend(plugin_incs(dp));
                }
            }
        }
        // Dedup keeping the **first** occurrence (order-preserving), and drop empties.
        let mut seen: std::collections::HashSet<String> = std::collections::HashSet::new();
        incs.retain(|i| !i.is_empty() && seen.insert(i.clone()));
        unit_includes.insert(u.name.clone(), incs);
    }

    // An undeclared cross-plugin `#include` is a build error. Narrowing `-I` alone is not enough:
    // glibc headers mask common names such as `unistd.h`. `check` calls the same function.
    for u in model::cross_plugin_includes(&root, &tree, &closure.selected) {
        // The core plugin is the base and is platform-agnostic: it includes the platform *contract*
        // (`br/platform/**`, provided by whichever platform plugin is selected) without naming a
        // platform plugin, so that one include family is not dep-closure-checked.
        if u.header.starts_with("br/platform/")
            && tree
                .plugins
                .iter()
                .any(|p| p.name == u.consumer && p.plugin_type == "core")
        {
            continue;
        }
        // A-2: an APP directly including a core header is a build error; others are unrestricted.
        if u.provider == "core" {
            let is_app = tree
                .plugins
                .iter()
                .any(|p| p.name == u.consumer && p.plugin_type == "app");
            if !is_app {
                continue;
            }
            d.shape(
                CODE_BUILD_SHAPE,
                &u.consumer,
                &u.file,
                "include",
                format!(
                    "`{}` (APP) directly includes core header `{}`: APPs must not depend on interfaces below the iface layer",
                    u.consumer, u.header
                ),
                "ADR-0020 / A-2: use the Interface skin (e.g. `<iface/min/min.h>`); add entries to the skin when more of core is needed",
            );
            continue;
        }
        if model::dep_closure(&tree, &u.consumer).contains(&u.provider) {
            continue;
        }
        d.shape(
            CODE_BUILD_SHAPE,
            &u.consumer,
            &u.file,
            "include",
            format!(
                "`{}` includes `{}`'s header `{}`, but `brick.toml` declares no dependency (direct or transitive)",
                u.consumer, u.provider, u.header
            ),
            "ADR-0018: compile-time include surface = declared dep closure; add `[[dep]]` or drop this include",
        );
    }

    let cflags = if profile == "release" {
        let mut v = pb.cflags.clone();
        v.extend(pb.release_cflags_extra.clone());
        v
    } else {
        pb.cflags.clone()
    };
    let asflags = if profile == "release" {
        let mut v = pb.asflags.clone();
        v.extend(pb.release_asflags_extra.clone());
        v
    } else {
        pb.asflags.clone()
    };
    let ldflags = if profile == "release" {
        let mut v = pb.ldflags.clone();
        v.extend(pb.release_ldflags_extra.clone());
        v
    } else {
        pb.ldflags.clone()
    };
    if cflags.is_empty() {
        diags.shape(
            CODE_BUILD_SHAPE,
            &prod.name,
            "product.toml",
            "build.cflags",
            "`[build].cflags` is empty: compile options must be declared by the product (no built-in defaults)",
            "write `-std=c11`/`-O2`/`-ffreestanding` etc. explicitly into `[build].cflags`",
        );
    }

    let obj_dir = pb.obj_dir.clone().unwrap_or_else(default_obj_dir);
    let elf = format!("build/brick{tag_suffix}.elf");
    let bin = format!("build/brick{tag_suffix}.bin");
    let map = format!("build/brick{tag_suffix}.map");
    let state_path = if tag.is_empty() {
        "build/gen/build-state.json".to_string()
    } else {
        format!("build/gen/build-state{tag_suffix}.json")
    };
    let gen_dir = "build/gen".to_string();
    // Tagged builds relocate objects too, so the two backends do not collide (needed by the backend-compare gate).
    let obj_dir = if tag.is_empty() {
        obj_dir
    } else {
        format!("{obj_dir}{tag_suffix}")
    };

    // Tool candidates, in resolution order.
    let cross = &target.cross;
    let mut tools: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut cc_cands: Vec<String> = vec![format!("{cross}gcc")];
    for v in ["16", "15", "14", "13"] {
        cc_cands.push(format!("{cross}gcc-{v}"));
    }
    cc_cands.push("cc".to_string());
    tools.insert("cc".to_string(), cc_cands);
    tools.insert(
        "objcopy".to_string(),
        vec![format!("{cross}objcopy"), "objcopy".to_string()],
    );
    tools.insert(
        "objdump".to_string(),
        vec![format!("{cross}objdump"), "objdump".to_string()],
    );
    tools.insert(
        "size".to_string(),
        vec![format!("{cross}size"), "size".to_string()],
    );
    tools.insert("qemu".to_string(), vec![target.qemu_binary.clone()]);
    tools.insert("make".to_string(), vec!["make".to_string()]);
    tools.insert("ninja".to_string(), vec!["ninja".to_string()]);

    let mut tool_what: BTreeMap<String, String> = BTreeMap::new();
    tool_what.insert("cc".into(), "cross C compiler (br-wa-toolchain-001 interim)".into());
    tool_what.insert("objcopy".into(), "object file to binary".into());
    tool_what.insert("objdump".into(), "disassembly (for gates)".into());
    tool_what.insert("size".into(), "section sizes".into());
    tool_what.insert("qemu".into(), "target machine emulator".into());
    tool_what.insert("make".into(), "BR-D4 backend A".into());
    tool_what.insert("ninja".into(), "BR-D4 backend B".into());

    // Tool resolution happens here: generated backend files must bake in the result.
    let path_env = req
        .context("path")
        .and_then(Value::as_str)
        .map(str::to_string)
        .or_else(|| std::env::var("PATH").ok())
        .unwrap_or_default();
    let mut resolved: BTreeMap<String, String> = BTreeMap::new();
    for (name, cands) in &tools {
        if let Some(found) = resolve_tool(&root, cands, &path_env) {
            resolved.insert(name.clone(), found);
        }
    }

    // `context.files`: deterministic stat for self-test (when given, no disk access).
    let stat_override = req.context("files").and_then(Value::as_object).map(|m| {
        let mut out: BTreeMap<String, (u64, i64)> = BTreeMap::new();
        for (k, v) in m {
            let size = v.get("size").and_then(Value::as_u64).unwrap_or(0);
            let mtime = v.get("mtime_ns").and_then(Value::as_i64).unwrap_or(0);
            out.insert(k.clone(), (size, mtime));
        }
        out
    });

    diags.merge(d);
    if diags.has_error() {
        return Err(diags);
    }

    // Do not drop `diags`: loading also emits info/warning diagnostics that callers must present.
    Ok((Ctx {
        root,
        profile,
        backend,
        tag,
        units,
        target,
        pb,
        obj_dir,
        elf,
        bin,
        map,
        state_path,
        gen_dir,
        cflags,
        asflags,
        ldflags,
        config_defines,
        unit_includes,
        tools,
        resolved,
        tool_what,
        stat_override,
        force: req.arg_bool("force").unwrap_or(false),
    }, diags))
}

/// Expand a group of globs; a literal path that does not exist ⇒ `BRV-BLD-0007`.
fn expand_many(
    root: &Path,
    base_dir: &str,
    patterns: &[String],
    unit: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for pat in patterns {
        let hits = expand_glob(root, base_dir, pat);
        if hits.is_empty() {
            if has_wildcard(pat) {
                continue;
            }
            d.shape(
                CODE_MISSING_SOURCE,
                unit,
                file,
                span,
                format!("`{pat}` does not exist (literal path, not a glob)"),
                "check the path spelling and working directory; a non-matching glob is **not** an error (see BRV-BLD-0002)",
            );
            continue;
        }
        out.extend(hits);
    }
    out.sort();
    out.dedup();
    out
}

/// Tool resolution: `<root>/prebuilts/toolchain/bin/` first (reproducible), then PATH.
fn resolve_tool(root: &Path, candidates: &[String], path_env: &str) -> Option<String> {
    let dirs: Vec<PathBuf> = path_env
        .split(':')
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .collect();
    for cand in candidates {
        if cand.contains('/') {
            let p = root.join(cand);
            if p.is_file() {
                return Some(cand.clone());
            }
            continue;
        }
        let local = root.join("prebuilts/toolchain/bin").join(cand);
        if local.is_file() {
            return Some(format!("prebuilts/toolchain/bin/{cand}"));
        }
        for dir in &dirs {
            if dir.join(cand).is_file() {
                return Some(cand.clone());
            }
        }
    }
    None
}

impl Ctx {
    fn stat(&self, rel: &str) -> Option<(u64, i64)> {
        if let Some(map) = &self.stat_override {
            return map.get(rel).copied();
        }
        let meta = fs::metadata(self.root.join(rel)).ok()?;
        let size = meta.len();
        let mtime = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_nanos() as i64)
            .unwrap_or(0);
        Some((size, mtime))
    }

    fn obj_of(&self, src: &str) -> String {
        let stem = src.rsplit_once('.').map(|(a, _)| a).unwrap_or(src);
        format!("{}/{}.o", self.obj_dir, stem)
    }

    fn dep_of(&self, src: &str) -> String {
        format!("{}.d", self.obj_of(src))
    }

    fn tool(&self, name: &str) -> String {
        self.resolved
            .get(name)
            .cloned()
            .unwrap_or_else(|| format!("@{name}"))
    }

    fn unit_includes_argv(&self, unit: &str) -> Vec<String> {
        self.unit_includes
            .get(unit)
            .map(|v| v.iter().map(|i| format!("-I{i}")).collect())
            .unwrap_or_default()
    }
}

fn all_steps(ctx: &Ctx) -> Vec<Step> {
    let mut steps: Vec<Step> = Vec::new();
    let cc = ctx.tool("cc");
    let objcopy = ctx.tool("objcopy");
    let arch_flags = ctx.target.arch_flags.clone();
    let arch_in_cflags = !arch_flags.is_empty();
    let arch_in_asflags = !arch_flags.is_empty();
    // Product config defines go to every C unit, after the unit's own defines.
    let config_defines: Vec<String> = ctx
        .config_defines
        .iter()
        .map(|d| format!("-D{d}"))
        .collect();

    let mut srcs: Vec<(String, String)> = Vec::new(); // (src, unit)
    for u in &ctx.units {
        for s in &u.sources {
            srcs.push((s.clone(), u.name.clone()));
        }
    }
    srcs.sort();

    for (src, unit) in &srcs {
        let obj = ctx.obj_of(src);
        let dep = ctx.dep_of(src);
        let is_asm = src.ends_with(".S") || src.ends_with(".s");
        // This unit's include surface (declared dep closure, not a global union).
        let includes = ctx.unit_includes_argv(unit);
        let mut argv: Vec<String> = vec![cc.clone()];
        if is_asm {
            if arch_in_asflags {
                argv.extend(arch_flags.clone());
            }
            argv.extend(ctx.asflags.clone());
        } else {
            argv.extend(ctx.cflags.clone());
            if arch_in_cflags {
                argv.extend(arch_flags.clone());
            }
            // Unit cflags follow product cflags: a later `-Wno-*` can undo an earlier `-Werror`.
            {
                let unit_cflags: Vec<String> = ctx
                    .units
                    .iter()
                    .find(|u| &u.name == unit)
                    .map(|u| u.cflags.clone())
                    .unwrap_or_default();
                argv.extend(unit_cflags);
            }
            let unit_defines: Vec<String> = ctx
                .units
                .iter()
                .find(|u| &u.name == unit)
                .map(|u| u.defines.iter().map(|d| format!("-D{d}")).collect())
                .unwrap_or_default();
            argv.extend(includes.clone());
            argv.extend(unit_defines);
            argv.extend(config_defines.clone());
        }
        if is_asm {
            argv.extend(includes.clone());
        }
        argv.push("-MMD".to_string());
        argv.push("-MF".to_string());
        argv.push(dep.clone());
        argv.push("-c".to_string());
        argv.push(src.clone());
        argv.push("-o".to_string());
        argv.push(obj.clone());
        steps.push(Step {
            label: format!("cc {src}"),
            kind: "compile".to_string(),
            argv,
            group: 0,
            log: None,
            timeout_s: None,
            expect_timeout: false,
            judge: None,
            stdout_lines: None,
            outputs: vec![obj, dep],
            consumes: Vec::new(),
        });
    }

    let objs: Vec<String> = srcs.iter().map(|(s, _)| ctx.obj_of(s)).collect();
    let mut link_argv: Vec<String> = vec![cc.clone()];
    link_argv.extend(ctx.ldflags.clone());
    if let Some(ld) = &ctx.target.linker_script {
        link_argv.push(format!("-Wl,-T,{ld}"));
    }
    link_argv.push(format!("-Wl,-Map,{}", ctx.map));
    link_argv.extend(objs.iter().cloned());
    link_argv.push("-o".to_string());
    link_argv.push(ctx.elf.clone());
    steps.push(Step {
        label: format!("ld {}", ctx.elf),
        kind: "link".to_string(),
        argv: link_argv,
        group: 1,
        log: None,
        timeout_s: None,
        expect_timeout: false,
        judge: None,
        stdout_lines: None,
        outputs: vec![ctx.elf.clone(), ctx.map.clone()],
        consumes: objs.clone(),
    });

    steps.push(Step {
        label: format!("objcopy {}", ctx.bin),
        kind: "objcopy".to_string(),
        argv: vec![
            objcopy,
            "-O".to_string(),
            "binary".to_string(),
            ctx.elf.clone(),
            ctx.bin.clone(),
        ],
        group: 2,
        log: None,
        timeout_s: None,
        expect_timeout: false,
        judge: None,
        stdout_lines: None,
        outputs: vec![ctx.bin.clone()],
        consumes: vec![ctx.elf.clone()],
    });

    steps
}

/// A step's input set (source + dependencies listed in `.d`).
fn step_inputs(ctx: &Ctx, step: &Step) -> Vec<String> {
    let mut inputs: Vec<String> = Vec::new();
    match step.kind.as_str() {
        "compile" => {
            // argv tail shape: … -c <src> -o <obj>
            let mut it = step.argv.iter().rev();
            let _obj = it.next();
            let _o = it.next();
            if let Some(src) = it.next() {
                inputs.push(src.clone());
                let dep = format!("{}.d", _obj.cloned().unwrap_or_default());
                if ctx.stat_override.is_some() {
                    // In self-test mode `.d` comes from context (`context.files` key `deps:<obj>`)
                } else {
                    let deps = parse_depfile(&ctx.root.join(&dep), &ctx.root);
                    inputs.extend(deps);
                }
            }
        }
        "link" => {
            for a in &step.argv {
                if a.starts_with("-Wl,-T,") {
                    inputs.push(a.trim_start_matches("-Wl,-T,").to_string());
                } else if a.ends_with(".o") {
                    inputs.push(a.clone());
                }
            }
        }
        "objcopy" => {
            if step.argv.len() >= 4 {
                inputs.push(step.argv[3].clone());
            }
        }
        _ => {}
    }
    inputs.sort();
    inputs.dedup();
    inputs
}

/// Filter paths from `build/gen/**` by ownership (owner = plugin that prefixes the path).
/// Selected owner ⇒ keep; owner in tree but unselected ⇒ skip (would link as an undefined
/// symbol); no owner under `build/gen/` ⇒ skip as stale (a ghost descriptor panics the boot).
fn filter_gen_sources(
    sources: &[String],
    tree: &TreeLoad,
    selected: &BTreeSet<String>,
) -> (Vec<String>, Vec<String>, Vec<String>) {
    let mut kept: Vec<String> = Vec::new();
    let mut skipped: Vec<String> = Vec::new();
    let mut orphans: Vec<String> = Vec::new();
    for path in sources {
        let owner = tree.plugins.iter().find(|p| {
            path.starts_with(&format!("build/gen/{}/", p.name))
        });
        match owner {
            Some(p) if !selected.contains(&p.name) => skipped.push(path.clone()),
            Some(_) => kept.push(path.clone()),
            None if path.starts_with("build/gen/") => orphans.push(path.clone()),
            None => kept.push(path.clone()),
        }
    }
    (kept, skipped, orphans)
}

/// argv fingerprint: the **command itself** changed (flags / compiler / output path) ⇒ must rerun.
fn argv_hash_of(step: &Step) -> String {
    sha256_tag(&[step.kind.clone(), step.argv.join("\u{1}")])
}

/// Previous state (machine file, produced by core / written by L5; absent = full rebuild).
#[derive(Clone, Debug, Default)]
struct StateEntry {
    argv_hash: String,
    inputs: Vec<String>,
}

fn load_state(ctx: &Ctx) -> BTreeMap<String, StateEntry> {
    let mut out: BTreeMap<String, StateEntry> = BTreeMap::new();
    let Ok(text) = fs::read_to_string(ctx.root.join(&ctx.state_path)) else {
        return out;
    };
    let Ok(v) = text.parse::<toml::Value>() else {
        // Corrupt state file = treat as "no state" (full rebuild), not an error — it is a derived artifact.
        return out;
    };
    if let Some(jobs) = v.get("jobs").and_then(|j| j.as_array()) {
        for j in jobs {
            let Some(target) = j.get("target").and_then(|t| t.as_str()) else {
                continue;
            };
            out.insert(
                target.to_string(),
                StateEntry {
                    argv_hash: j
                        .get("argv_hash")
                        .and_then(|t| t.as_str())
                        .unwrap_or_default()
                        .to_string(),
                    inputs: j
                        .get("inputs")
                        .and_then(|t| t.as_array())
                        .map(|a| {
                            a.iter()
                                .filter_map(|x| x.as_str())
                                .map(str::to_string)
                                .collect()
                        })
                        .unwrap_or_default(),
                },
            );
        }
    }
    out
}

struct BuildPlan {
    todo: Vec<Step>,
    stale: Vec<String>,
    state_content: String,
    up_to_date: usize,
    total: usize,
    plan_hash: String,
}

/// Incremental policy, two criteria (like make/ninja): rerun when the argv fingerprint changed,
/// or the target is missing / an input is newer or gone. Not "input set + content hash": `.d` is
/// itself a build product, so the hash would disagree with itself on every run after the first.
fn make_plan(ctx: &Ctx) -> BuildPlan {
    let steps = all_steps(ctx);
    let old = load_state(ctx);
    let mut todo: Vec<Step> = Vec::new();
    let mut jobs: Vec<Value> = Vec::new();
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut up_to_date = 0usize;
    let mut plan_parts: Vec<String> = Vec::new();

    // Propagate upstream rebuilds explicitly: the plan is computed before the build, when
    // objects are still old, so timestamps alone would miss a needed relink.
    let mut dirty: BTreeSet<String> = BTreeSet::new();
    for step in &steps {
        let target = step.outputs.first().cloned().unwrap_or_default();
        seen.insert(target.clone());
        let inputs = step_inputs(ctx, step);
        let argv_hash = argv_hash_of(step);
        plan_parts.push(format!("{target}\u{1}{}", step.argv.join("\u{1}")));

        let mut needs = ctx.force;
        let target_stat = ctx.stat(&target);
        if !needs && target_stat.is_none() {
            needs = true;
        }
        if !needs {
            if old.get(&target).map(|e| e.argv_hash.as_str()) != Some(argv_hash.as_str()) {
                needs = true;
            }
        }
        if !needs && step.kind == "compile" {
            // Missing `.d` ⇒ header deps unknown ⇒ recompile once (a bad link is worse).
            if ctx.stat(&format!("{target}.d")).is_none() {
                needs = true;
            }
        }
        if !needs {
            let target_mtime = target_stat.map(|(_, m)| m).unwrap_or(i64::MIN);
            for inp in &inputs {
                match ctx.stat(inp) {
                    // Input newer than target, or input gone ⇒ rerun (the latter surfaces as a compile error)
                    Some((_, m)) if m <= target_mtime => {}
                    _ => {
                        needs = true;
                        break;
                    }
                }
            }
        }
        if !needs {
            for inp in &inputs {
                if dirty.contains(inp) {
                    needs = true;
                    break;
                }
            }
        }
        if needs {
            dirty.insert(target.clone());
            todo.push(step.clone());
        } else {
            up_to_date += 1;
        }
        jobs.push(json!({
            "target": target,
            "argv_hash": argv_hash,
            "inputs": inputs,
            "kind": step.kind,
        }));
    }

    let stale: Vec<String> = old
        .keys()
        .filter(|k| !seen.contains(*k) && k.starts_with(&ctx.obj_dir))
        .cloned()
        .collect();

    let state_content = format!(
        "# brickie:generated — incremental state (derived, safe to delete)\n\
         schema = 1\n\
         profile = \"{}\"\n\
         plan = \"{}\"\n\
         \n{}\n",
        ctx.profile,
        sha256_tag(&plan_parts),
        jobs.iter()
            .map(|j| {
                let inputs = j["inputs"]
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .filter_map(|x| x.as_str())
                            .map(|x| format!("\"{x}\""))
                            .collect::<Vec<_>>()
                            .join(", ")
                    })
                    .unwrap_or_default();
                format!(
                    "[[jobs]]\ntarget = \"{}\"\nargv_hash = \"{}\"\nkind = \"{}\"\ninputs = [{}]\n",
                    j["target"].as_str().unwrap_or_default(),
                    j["argv_hash"].as_str().unwrap_or_default(),
                    j["kind"].as_str().unwrap_or_default(),
                    inputs,
                )
            })
            .collect::<Vec<_>>()
            .join("\n")
    );

    BuildPlan {
        total: steps.len(),
        todo,
        stale,
        state_content,
        up_to_date,
        plan_hash: sha256_tag(&plan_parts),
    }
}

/// Render the make backend (BR-D4 A): generated, do not edit by hand.
fn render_make(ctx: &Ctx, steps: &[Step]) -> String {
    let mut s = String::new();
    s.push_str("# brickie:generated — generated file, do not edit by hand.\n");
    s.push_str("# regenerate: brickie build --backend make\n");
    s.push_str("# source: product.toml [build] + each plugin [build] + platform [build.target] (single source of truth)\n");
    s.push_str("# This file is BR-D4's **backend A** (make does the work); the `brickie build` direct backend does not read it.\n\n");
    s.push_str(".DEFAULT_GOAL := all\n.PHONY: all clean\n\n");
    s.push_str(&format!("OBJ_DIR := {}\nELF := {}\nBIN := {}\nMAP := {}\n\n", ctx.obj_dir, ctx.elf, ctx.bin, ctx.map));
    let compiles: Vec<&Step> = steps.iter().filter(|s| s.kind == "compile").collect();
    let objs: Vec<String> = compiles
        .iter()
        .map(|s| s.outputs.first().cloned().unwrap_or_default())
        .collect();
    s.push_str(&format!("OBJS := {}\n", objs.join(" ")));
    s.push_str("DEPS := $(OBJS:.o=.d)\n\n");
    s.push_str("all: $(ELF) $(BIN)\n\n");
    for step in compiles {
        let obj = step.outputs.first().cloned().unwrap_or_default();
        let src = step
            .argv
            .iter()
            .position(|a| a == "-c")
            .and_then(|i| step.argv.get(i + 1))
            .cloned()
            .unwrap_or_default();
        s.push_str(&format!("{obj}: {src}\n"));
        s.push_str(&format!("\t@mkdir -p $(dir $@)\n\t{}\n\n", shell_join(&step.argv)));
    }
    if let Some(link) = steps.iter().find(|s| s.kind == "link") {
        s.push_str(&format!("$(ELF): $(OBJS)\n\t@mkdir -p $(dir $@)\n\t{}\n\n", shell_join(&link.argv)));
    }
    if let Some(cp) = steps.iter().find(|s| s.kind == "objcopy") {
        s.push_str(&format!("$(BIN): $(ELF)\n\t{}\n\n", shell_join(&cp.argv)));
    }
    s.push_str("clean:\n\trm -rf $(OBJ_DIR) $(ELF) $(BIN) $(MAP)\n\n");
    s.push_str("-include $(DEPS)\n");
    s
}

/// Render the ninja backend (BR-D4 B).
/// `cc` and `asm` must be separate rules: C and `.S` have different argv, so one template
/// would compile assembly as C. Each template is the first step of its kind.
fn render_ninja(ctx: &Ctx, steps: &[Step]) -> String {
    let mut s = String::new();
    s.push_str("# brickie:generated — generated file, do not edit by hand.\n");
    s.push_str("# regenerate: brickie build --backend ninja\n");
    s.push_str("# This file is BR-D4's **backend B** (ninja does the work); it produces a **byte-identical** image to backend A.\n\n");
    s.push_str(&format!("cc = {}\n\n", ctx.tool("cc")));
    // Pin ninja's `.ninja_log`/`.ninja_deps` under build/ instead of the invocation directory.
    s.push_str(&format!("builddir = {}/ninja\n\n", ctx.gen_dir));

    let compiles: Vec<&Step> = steps.iter().filter(|s| s.kind == "compile").collect();
    let src_of = |st: &Step| -> String {
        st.argv
            .iter()
            .position(|a| a == "-c")
            .and_then(|i| st.argv.get(i + 1))
            .cloned()
            .unwrap_or_default()
    };
    let is_asm = |st: &Step| src_of(st).ends_with(".S") || src_of(st).ends_with(".s");

    // One rule per kind (template = that kind's first step argv, with concrete in/out replaced by $in/$out)
    for (tag, want_asm) in [("cc", false), ("asm", true)] {
        let Some(tmpl_step) = compiles.iter().find(|st| is_asm(st) == want_asm) else {
            continue;
        };
        let obj = tmpl_step.outputs.first().cloned().unwrap_or_default();
        let src = src_of(tmpl_step);
        let line = tmpl_step
            .argv
            .iter()
            .map(|a| {
                if a == &ctx.tool("cc") {
                    "$cc".to_string()
                } else if a == &obj {
                    "$out".to_string()
                } else if a == &src {
                    "$in".to_string()
                } else {
                    a.replace('$', "$$")
                }
            })
            .collect::<Vec<_>>()
            .join(" ");
        s.push_str(&format!("rule {tag}\n  command = {line}\n"));
        s.push_str("  depfile = $out.d\n  deps = gcc\n");
        s.push_str(&format!("  description = {tag} $in\n\n"));
    }
    for step in &compiles {
        let obj = step.outputs.first().cloned().unwrap_or_default();
        let src = src_of(step);
        let rule = if is_asm(step) { "asm" } else { "cc" };
        s.push_str(&format!("build {obj}: {rule} {src}\n"));
    }
    s.push('\n');
    if let Some(link) = steps.iter().find(|s| s.kind == "link") {
        s.push_str(&format!(
            "rule link\n  command = {}\n  description = link $out\n\n",
            shell_join(&link.argv)
        ));
        let objs: Vec<String> = compiles
            .iter()
            .map(|st| st.outputs.first().cloned().unwrap_or_default())
            .collect();
        s.push_str(&format!("build {}: link {}\n\n", ctx.elf, objs.join(" ")));
    }
    if let Some(cp) = steps.iter().find(|s| s.kind == "objcopy") {
        s.push_str(&format!(
            "rule objcopy\n  command = {}\n  description = objcopy $out\n\n",
            shell_join(&cp.argv)
        ));
        s.push_str(&format!("build {}: objcopy {}\n\n", ctx.bin, ctx.elf));
    }
    s.push_str(&format!("default {}\n", ctx.bin));
    s
}

/// Minimal shell quoting (commands in generated files must run verbatim under make).
fn shell_join(argv: &[String]) -> String {
    argv.iter()
        .map(|a| {
            if a.is_empty()
                || a.contains(|c: char| c.is_whitespace() || "\"'$`\\*?()[]{}|&;<>#~!".contains(c))
            {
                format!("'{}'", a.replace('\'', "'\\''"))
            } else {
                a.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

fn summary_json(ctx: &Ctx, plan: &BuildPlan, steps: &[Step], extra: Value) -> Value {
    let mut units: Vec<Value> = ctx
        .units
        .iter()
        .map(|u| {
            json!({
                "name": u.name,
                "dir": u.dir,
                "plugin": u.plugin,
                "sources": u.sources.len(),
                "includes": u.includes,
                "defines": u.defines,
                "sources_list": u.sources,
            })
        })
        .collect();
    units.sort_by_key(|u| u["name"].as_str().unwrap_or_default().to_string());
    let tools: Vec<Value> = ctx
        .tools
        .iter()
        .map(|(k, cands)| {
            json!({
                "name": k,
                "what": ctx.tool_what.get(k).cloned().unwrap_or_default(),
                "candidates": cands,
                "resolved": ctx.resolved.get(k),
            })
        })
        .collect();
    let mut data = json!({
        "profile": ctx.profile,
        "backend": ctx.backend,
        "tag": ctx.tag,
        "target": {
            "owner": ctx.target.owner,
            "arch": ctx.target.arch,
            "cross": ctx.target.cross,
            "arch_flags": ctx.target.arch_flags,
            "linker_script": ctx.target.linker_script,
            "qemu": {
                "binary": ctx.target.qemu_binary,
                "machine": ctx.target.qemu_machine,
                "cpu": ctx.target.qemu_cpu,
                "memory": ctx.target.qemu_memory,
                "extra": ctx.target.qemu_extra,
            },
        },
        "outputs": {"obj_dir": ctx.obj_dir, "elf": ctx.elf, "bin": ctx.bin, "map": ctx.map},
        "state_path": ctx.state_path,
        "gen_dir": ctx.gen_dir,
        "units": units,
        "unit_count": ctx.units.len(),
        "source_count": ctx.units.iter().map(|u| u.sources.len()).sum::<usize>(),
        "tools": tools,
        "steps": steps.iter().map(Step::to_json).collect::<Vec<_>>(),
        "steps_total": plan.total,
        "steps_todo": plan.todo.len(),
        "up_to_date": plan.up_to_date,
        "stale_outputs": plan.stale,
        "plan_hash": plan.plan_hash,
        "flags": {"cflags": ctx.cflags, "asflags": ctx.asflags, "ldflags": ctx.ldflags},
    });
    if let (Some(obj), Some(extra)) = (data.as_object_mut(), extra.as_object()) {
        for (k, v) in extra {
            obj.insert(k.clone(), v.clone());
        }
    }
    data
}

fn cmd_build(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (ctx, mut diags) = match load_ctx(req, &tree) {
        Ok(c) => c,
        Err(mut d) => {
            d.merge(tree.diags);
            return Response::ok(d, json!({}));
        }
    };
    diags.merge(tree.diags.clone());
    let plan = make_plan(&ctx);
    let steps = all_steps(&ctx);

    let dry_run = req.arg_bool("dry_run").unwrap_or(false);
    let emit_backends = req.arg_bool("emit_backends").unwrap_or(false);

    let mut files: Vec<Value> = Vec::new();
    let want_make = emit_backends || ctx.backend == "make";
    let want_ninja = emit_backends || ctx.backend == "ninja";
    if want_make {
        files.push(json!({
            "path": format!("{}/build.mk", ctx.gen_dir),
            "kind": "machine",
            "content": render_make(&ctx, &steps),
            "mode": "0644",
        }));
    }
    if want_ninja {
        files.push(json!({
            "path": format!("{}/build.ninja", ctx.gen_dir),
            "kind": "machine",
            "content": render_ninja(&ctx, &steps),
            "mode": "0644",
        }));
    }

    let exec_steps: Vec<Step> = match ctx.backend.as_str() {
        "make" => {
            let jobs = req.arg_u64("jobs").unwrap_or(0);
            let mut argv = vec![ctx.tool("make"), "-f".to_string(), format!("{}/build.mk", ctx.gen_dir)];
            if jobs > 0 {
                argv.push(format!("-j{jobs}"));
            }
            vec![Step {
                label: "make -f build/gen/build.mk".to_string(),
                kind: "backend".to_string(),
                argv,
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: vec![ctx.elf.clone(), ctx.bin.clone()],
                consumes: Vec::new(),
            }]
        }
        "ninja" => {
            let jobs = req.arg_u64("jobs").unwrap_or(0);
            let mut argv = vec![
                ctx.tool("ninja"),
                "-f".to_string(),
                format!("{}/build.ninja", ctx.gen_dir),
            ];
            if jobs > 0 {
                argv.push(format!("-j{jobs}"));
            }
            vec![Step {
                label: "ninja -f build/gen/build.ninja".to_string(),
                kind: "backend".to_string(),
                argv,
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: vec![ctx.elf.clone(), ctx.bin.clone()],
                consumes: Vec::new(),
            }]
        }
        _ => plan.todo.clone(),
    };

    // Tool resolution failed ⇒ environment error (exit code 2).
    if !dry_run && ctx.backend == "direct" && !plan.todo.is_empty() {
        for name in ["cc", "objcopy"] {
            if !ctx.resolved.contains_key(name) {
                diags.shape(
                    CODE_TOOL_MISSING,
                    "",
                    "product.toml",
                    "build.target",
                    format!(
                        "cannot find tool `{name}` (candidates: {})",
                        ctx.tools.get(name).cloned().unwrap_or_default().join(", ")
                    ),
                    "install a cross toolchain or fetch prebuilt: make prebuilt; or point `[build.target].cross` elsewhere",
                );
                return Response::ok(diags, json!({}));
            }
        }
    }

    let state_file = json!({
        "path": ctx.state_path.clone(),
        "kind": "machine",
        "content": plan.state_content.clone(),
        "mode": "0644",
    });
    // The state file only matters for the direct backend (make/ninja track timestamps themselves).
    if ctx.backend == "direct" && !dry_run {
        files.push(state_file);
    }

    if ctx.profile == "release" && plan.total == 0 {
        diags.coded(
            CODE_BUILD_SHAPE,
            Severity::Info,
            "",
            "product.toml",
            "build.release",
            "release and dev use identical compile flags (only composition-time check strictness differs)",
            "the in-image assertion axis (debug=panic / release=trace, 1-01 §13) is not implemented yet; logged for v0.2",
        );
    }

    let show_steps: Vec<Step> = if dry_run { steps.clone() } else { exec_steps.clone() };
    let data = summary_json(
        &ctx,
        &plan,
        &show_steps,
        json!({
            "dry_run": dry_run,
            "emit_backends": emit_backends,
            "steps": show_steps.iter().map(Step::to_json).collect::<Vec<_>>(),
        }),
    );
    Response::ok_with_files(diags, files, data)
}

fn cmd_clean(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let all = req.arg_bool("all").unwrap_or(false);
    let ctx = load_ctx(req, &tree).ok().map(|(c, _)| c);
    let mut d = tree.diags.clone();
    let mut paths: Vec<String> = Vec::new();
    if let Some(ctx) = &ctx {
        paths.push(ctx.obj_dir.clone());
        paths.push(ctx.elf.clone());
        paths.push(ctx.bin.clone());
        paths.push(ctx.map.clone());
        paths.push(ctx.state_path.clone());
    } else {
        // Cleaning must work before the declaration surface exists (e.g. no [build]): fall back to historical paths.
        d = Diags::new();
        paths.push(default_obj_dir());
        for f in ["build/brick.elf", "build/brick.bin", "build/brick.map"] {
            paths.push(f.to_string());
        }
    }
    for log in ["smoke", "irq", "dbg"] {
        paths.push(format!("build/logs/{log}.log"));
    }
    paths.push("build/hosttest".to_string());
    if all {
        paths.push("build/gen".to_string());
        paths.push("build/index".to_string());
        paths.push("build/logs".to_string());
        paths.push("build/toolchain".to_string());
    }
    paths.sort();
    paths.dedup();
    let data = json!({
        "all": all,
        "paths": paths,
        "note": "L5 deletes each path in turn (every path must lie inside root; excludes build/host tool artifacts)",
    });
    Response::ok(d, data)
}

fn cmd_run(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (ctx, mut diags) = match load_ctx(req, &tree) {
        Ok(c) => c,
        Err(mut d) => {
            d.merge(tree.diags);
            return Response::ok(d, json!({}));
        }
    };
    diags.merge(tree.diags.clone());
    let argv = qemu_argv(&ctx);
    if !ctx.resolved.contains_key("qemu") {
        diags.shape(
            CODE_TOOL_MISSING,
            "",
            "product.toml",
            "build.target.qemu",
            format!("cannot find QEMU (`{}`)", ctx.target.qemu_binary),
            "install qemu-system-arm; or point `[build.target.qemu].binary` elsewhere",
        );
        return Response::ok(diags, json!({}));
    }
    let steps = vec![Step {
        label: "qemu run".to_string(),
        kind: "run".to_string(),
        argv,
        group: 9,
        log: None,
        timeout_s: req.arg_u64("timeout_s"),
        expect_timeout: false,
        judge: None,
        stdout_lines: None,
        outputs: Vec::new(),
        consumes: Vec::new(),
    }];
    let plan = BuildPlan {
        todo: Vec::new(),
        stale: Vec::new(),
        state_content: String::new(),
        up_to_date: 0,
        total: 0,
        plan_hash: String::new(),
    };
    let data = summary_json(&ctx, &plan, &steps, json!({"mode": "run"}));
    Response::ok(diags, data)
}

fn qemu_argv(ctx: &Ctx) -> Vec<String> {
    let mut argv = vec![ctx.tool("qemu")];
    if !ctx.target.qemu_machine.is_empty() {
        argv.push("-M".to_string());
        argv.push(ctx.target.qemu_machine.clone());
    }
    if !ctx.target.qemu_cpu.is_empty() {
        argv.push("-cpu".to_string());
        argv.push(ctx.target.qemu_cpu.clone());
    }
    if !ctx.target.qemu_memory.is_empty() {
        argv.push("-m".to_string());
        argv.push(ctx.target.qemu_memory.clone());
    }
    argv.extend(ctx.target.qemu_extra.clone());
    argv.push("-kernel".to_string());
    argv.push(ctx.elf.clone());
    argv
}

fn cmd_size(req: &Request) -> Response {
    binutils_cmd(req, "size")
}

fn cmd_disasm(req: &Request) -> Response {
    binutils_cmd(req, "objdump")
}

fn binutils_cmd(req: &Request, which: &str) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (ctx, mut diags) = match load_ctx(req, &tree) {
        Ok(c) => c,
        Err(mut d) => {
            d.merge(tree.diags);
            return Response::ok(d, json!({}));
        }
    };
    diags.merge(tree.diags.clone());
    let tool = ctx.tool(which);
    if !ctx.resolved.contains_key(which) {
        diags.shape(
            CODE_TOOL_MISSING,
            "",
            "product.toml",
            "build.target",
            format!("cannot find tool `{which}` (candidates: {})", ctx.tools.get(which).cloned().unwrap_or_default().join(", ")),
            "install aarch64 binutils or fetch prebuilt: make prebuilt",
        );
        return Response::ok(diags, json!({}));
    }
    let steps = if which == "size" {
        vec![
            Step {
                label: "size -A -x".to_string(),
                kind: "print".to_string(),
                argv: vec![tool.clone(), "-A".to_string(), "-x".to_string(), ctx.elf.clone()],
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: Vec::new(),
                consumes: Vec::new(),
            },
            Step {
                label: "size".to_string(),
                kind: "print".to_string(),
                argv: vec![tool, ctx.elf.clone()],
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: Vec::new(),
                consumes: Vec::new(),
            },
        ]
    } else {
        vec![Step {
            label: "objdump -d".to_string(),
            kind: "print".to_string(),
            argv: vec![tool, "-d".to_string(), ctx.elf.clone()],
            group: 9,
            log: None,
            timeout_s: None,
            expect_timeout: false,
            judge: None,
            stdout_lines: Some(req.arg_u64("head").unwrap_or(120)),
            outputs: Vec::new(),
            consumes: Vec::new(),
        }]
    };
    let plan = BuildPlan {
        todo: Vec::new(),
        stale: Vec::new(),
        state_content: String::new(),
        up_to_date: 0,
        total: 0,
        plan_hash: String::new(),
    };
    let data = summary_json(&ctx, &plan, &steps, json!({"mode": which}));
    Response::ok(diags, data)
}

#[derive(Clone, Debug, Default)]
struct GateSpec {
    name: String,
    timeout_s: Option<u64>,
    log: Option<String>,
    require: Vec<String>,
    forbid: Vec<String>,
    require_tags: Vec<String>,
    expect_timeout: bool,
}

#[derive(Clone, Debug, Default)]
struct HostTestSpec {
    name: String,
    sources: Vec<String>,
    includes: Vec<String>,
    defines: Vec<String>,
    depends: Vec<String>,
    timeout_s: Option<u64>,
}

#[derive(Clone, Debug, Default)]
struct ScriptSpec {
    name: String,
    argv: Vec<String>,
    require: Vec<String>,
    forbid: Vec<String>,
}

#[derive(Clone, Debug, Default)]
struct GatesFile {
    run_timeout_s: Option<u64>,
    host_cc: String,
    host_cflags: Vec<String>,
    host_bin_dir: String,
    gates: Vec<GateSpec>,
    hosttests: Vec<HostTestSpec>,
    scripts: Vec<ScriptSpec>,
    build_post: Vec<String>,
    file: String,
    present: bool,
}

fn str_list(t: &toml::Table, key: &str) -> Vec<String> {
    t.get(key)
        .and_then(|v| v.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn opt_str(t: &toml::Table, key: &str) -> Option<String> {
    t.get(key).and_then(|v| v.as_str()).map(str::to_string)
}

fn load_gates(root: &Path, d: &mut Diags) -> GatesFile {
    let rel = "tests/gates.toml".to_string();
    let mut g = GatesFile {
        host_cc: "cc".to_string(),
        host_bin_dir: "build/hosttest".to_string(),
        file: rel.clone(),
        ..Default::default()
    };
    let abs = root.join(&rel);
    if !abs.is_file() {
        g.run_timeout_s = Some(5);
        return g;
    }
    g.present = true;
    let text = match fs::read_to_string(&abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("cannot read `{rel}`: {e}"), "check the path and permissions");
            return g;
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(format!("`{rel}` is not valid TOML: {e}"), "contract §7.4");
            return g;
        }
    };
    let Some(top) = value.as_table() else {
        d.env(format!("`{rel}` top level must be a table"), "contract §7.4");
        return g;
    };
    if let Some(run) = top.get("run").and_then(|v| v.as_table()) {
        g.run_timeout_s = run.get("default_timeout_s").and_then(|v| v.as_integer()).map(|v| v as u64);
    }
    if let Some(h) = top.get("host").and_then(|v| v.as_table()) {
        if let Some(cc) = opt_str(h, "cc") {
            g.host_cc = cc;
        }
        g.host_cflags = str_list(h, "cflags");
        if let Some(dir) = opt_str(h, "bin_dir") {
            g.host_bin_dir = dir;
        }
    }
    if let Some(arr) = top.get("gate").and_then(|v| v.as_array()) {
        for item in arr {
            let Some(t) = item.as_table() else { continue };
            g.gates.push(GateSpec {
                name: opt_str(t, "name").unwrap_or_default(),
                timeout_s: t.get("timeout_s").and_then(|v| v.as_integer()).map(|v| v as u64),
                log: opt_str(t, "log"),
                require: str_list(t, "require"),
                forbid: str_list(t, "forbid"),
                require_tags: str_list(t, "require_tags"),
                expect_timeout: t
                    .get("expect")
                    .and_then(|v| v.as_str())
                    .map(|s| s == "timeout")
                    .unwrap_or(true),
            });
        }
    }
    if let Some(arr) = top.get("hosttest").and_then(|v| v.as_array()) {
        for item in arr {
            let Some(t) = item.as_table() else { continue };
            g.hosttests.push(HostTestSpec {
                name: opt_str(t, "name").unwrap_or_default(),
                sources: str_list(t, "sources"),
                includes: str_list(t, "includes"),
                defines: str_list(t, "defines"),
                depends: str_list(t, "depends"),
                timeout_s: t.get("timeout_s").and_then(|v| v.as_integer()).map(|v| v as u64),
            });
        }
    }
    if let Some(arr) = top.get("script").and_then(|v| v.as_array()) {
        for item in arr {
            let Some(t) = item.as_table() else { continue };
            g.scripts.push(ScriptSpec {
                name: opt_str(t, "name").unwrap_or_default(),
                argv: str_list(t, "argv"),
                require: str_list(t, "require"),
                forbid: str_list(t, "forbid"),
            });
        }
    }
    if let Some(b) = top.get("build").and_then(|v| v.as_table()) {
        g.build_post = str_list(b, "post");
    }

    // Prefer a shape error over a never-matching require turning falsely green.
    let mut check_patterns: Vec<(String, String, String)> = Vec::new();
    for gate in &g.gates {
        for (i, pat) in gate.require.iter().enumerate() {
            check_patterns.push((gate.name.clone(), format!("gate.require[{i}]"), pat.clone()));
        }
        for (i, pat) in gate.forbid.iter().enumerate() {
            check_patterns.push((gate.name.clone(), format!("gate.forbid[{i}]"), pat.clone()));
        }
    }
    for s in &g.scripts {
        for (i, pat) in s.require.iter().enumerate() {
            check_patterns.push((s.name.clone(), format!("script.require[{i}]"), pat.clone()));
        }
        for (i, pat) in s.forbid.iter().enumerate() {
            check_patterns.push((s.name.clone(), format!("script.forbid[{i}]"), pat.clone()));
        }
    }
    for (name, span, pat) in check_patterns {
        if let Some(ch) = regex_unsupported(&pat) {
            d.shape(
                CODE_GATE_SHAPE,
                name,
                rel.clone(),
                span,
                format!("regex `{pat}` uses `{ch}`, unsupported by core's regex subset"),
                "supports `.` `*` `+` `?` `[...]` and `\\x` escapes; groups/alternation/anchors unsupported — split into several patterns",
            );
        }
    }
    g
}

/// First unsupported metacharacter in the pattern (`None` = inside the subset). No regex crate:
/// only ask "does the log contain this shape", so out-of-subset syntax must be rejected truthfully.
fn regex_unsupported(pat: &str) -> Option<char> {
    let mut escaped = false;
    for ch in pat.chars() {
        if escaped {
            escaped = false;
            continue;
        }
        match ch {
            '\\' => escaped = true,
            '(' | ')' | '|' | '{' | '}' | '^' | '$' => return Some(ch),
            _ => {}
        }
    }
    None
}

fn cmd_test(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let mut diags = tree.diags.clone();
    let g = load_gates(&root, &mut diags);
    let want = req.arg_str("name").unwrap_or_default();
    let (ctx, ctx_diags) = match load_ctx(req, &tree) {
        Ok((c, d)) => (Some(c), Some(d)),
        Err(_) => (None, None),
    };
    if let Some(d) = ctx_diags {
        diags.merge(d);
    }

    if req.arg_bool("list").unwrap_or(false) {
        let data = json!({
            "gates": g.gates.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
            "hosttests": g.hosttests.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
            "scripts": g.scripts.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
            "build_post": g.build_post,
            "file": g.file,
            "present": g.present,
        });
        return Response::ok(diags, data);
    }

    if !g.present {
        diags.shape(
            CODE_GATE_UNKNOWN,
            "",
            "tests/gates.toml",
            "",
            "no `tests/gates.toml`: gates have no declaration surface, cannot be orchestrated",
            "write `[[gate]]` / `[[hosttest]]` / `[[script]]`; contract §7.4",
        );
        return Response::ok(diags, json!({}));
    }
    let known: Vec<String> = g
        .gates
        .iter()
        .map(|x| x.name.clone())
        .chain(g.hosttests.iter().map(|x| x.name.clone()))
        .chain(g.scripts.iter().map(|x| x.name.clone()))
        .collect();
    if !want.is_empty() && !known.contains(&want) {
        diags.shape(
            CODE_GATE_UNKNOWN,
            want.clone(),
            &g.file.clone(),
            "",
            format!("no gate named `{want}` (known: {})", known.join(", ")),
            "`brickie test --list` lists them all",
        );
        return Response::ok(diags, json!({}));
    }

    // Selected set: a single name = it + its depends (topologically expanded); none given = run all.
    let selected: BTreeSet<String> = if want.is_empty() {
        g.hosttests
            .iter()
            .map(|x| x.name.clone())
            .chain(g.gates.iter().map(|x| x.name.clone()))
            .chain(g.scripts.iter().map(|x| x.name.clone()))
            .collect()
    } else {
        let mut sel: BTreeSet<String> = BTreeSet::new();
        fn add(sel: &mut BTreeSet<String>, g: &GatesFile, name: &str) {
            if !sel.insert(name.to_string()) {
                return;
            }
            for h in &g.hosttests {
                if h.name == name {
                    for dep in &h.depends {
                        add(sel, g, dep);
                    }
                }
            }
        }
        add(&mut sel, &g, &want);
        sel
    };

    let mut steps: Vec<Step> = Vec::new();
    let mut group: u32 = 0;

    for h in &g.hosttests {
        if !selected.contains(&h.name) {
            continue;
        }
        let mut argv = vec![g.host_cc.clone()];
        argv.extend(g.host_cflags.clone());
        argv.extend(h.defines.iter().map(|d| format!("-D{d}")));
        argv.extend(h.includes.iter().map(|i| format!("-I{i}")));
        argv.extend(h.sources.iter().cloned());
        argv.push("-o".to_string());
        let bin = format!("{}/{}", g.host_bin_dir, h.name);
        argv.push(bin.clone());
        steps.push(Step {
            label: format!("hostcc {}", h.name),
            kind: "hosttest".to_string(),
            argv,
            group,
            log: Some(format!("build/logs/{}.compile.log", h.name)),
            timeout_s: None,
            expect_timeout: false,
            judge: None,
            stdout_lines: None,
            outputs: vec![bin.clone()],
            consumes: Vec::new(),
        });
        group += 1;
        steps.push(Step {
            label: format!("run {}", h.name),
            kind: "hostrun".to_string(),
            argv: vec![format!("./{bin}")],
            group,
            log: Some(format!("build/logs/{}.log", h.name)),
            timeout_s: h.timeout_s,
            expect_timeout: false,
            judge: None,
            stdout_lines: None,
            outputs: Vec::new(),
            consumes: vec![bin.clone()],
        });
        group += 1;
    }

    // ---- QEMU gates ----
    for gate in &g.gates {
        if !selected.contains(&gate.name) {
            continue;
        }
        let Some(ctx) = &ctx else { continue };
        let argv = qemu_argv(ctx);
        steps.push(Step {
            label: format!("qemu {}", gate.name),
            kind: "gate".to_string(),
            argv,
            group,
            log: Some(
                gate
                    .log
                    .clone()
                    .unwrap_or_else(|| format!("build/logs/{}.log", gate.name)),
            ),
            timeout_s: Some(
                gate
                    .timeout_s
                    .or(g.run_timeout_s)
                    .unwrap_or(5),
            ),
            expect_timeout: gate.expect_timeout,
            judge: Some(gate.name.clone()),
            stdout_lines: None,
            outputs: Vec::new(),
            consumes: Vec::new(),
        });
        group += 1;
    }

    // ---- Script gates ----
    for s in &g.scripts {
        if !selected.contains(&s.name) {
            continue;
        }
        let judge = if s.require.is_empty() && s.forbid.is_empty() {
            None
        } else {
            Some(s.name.clone())
        };
        steps.push(Step {
            label: format!("script {}", s.name),
            kind: "script".to_string(),
            argv: s.argv.clone(),
            group,
            log: Some(format!("build/logs/{}.log", s.name)),
            timeout_s: None,
            expect_timeout: false,
            judge,
            stdout_lines: None,
            outputs: Vec::new(),
            consumes: Vec::new(),
        });
        group += 1;
    }

    let plan = BuildPlan {
        todo: Vec::new(),
        stale: Vec::new(),
        state_content: String::new(),
        up_to_date: 0,
        total: steps.len(),
        plan_hash: String::new(),
    };
    let mut data = if let Some(ctx) = &ctx {
        summary_json(ctx, &plan, &steps, json!({"mode": "test", "name": want}))
    } else {
        json!({"mode": "test", "name": want, "steps": steps.iter().map(Step::to_json).collect::<Vec<_>>()})
    };
    if let Some(obj) = data.as_object_mut() {
        obj.insert(
            "gates".into(),
            json!({
                "file": g.file,
                "gates": g.gates.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
                "hosttests": g.hosttests.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
                "scripts": g.scripts.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
                "build_post": g.build_post,
            }),
        );
    }
    Response::ok(diags, data)
}

fn cmd_judge(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let mut diags = Diags::new();
    let g = load_gates(&root, &mut diags);
    let name = req.arg_str("name").unwrap_or_default();
    let log = req
        .context("log")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let lines: Vec<&str> = log.lines().collect();

    let gate = g.gates.iter().find(|x| x.name == name);
    let script = g.scripts.iter().find(|x| x.name == name);
    if gate.is_none() && script.is_none() {
        diags.shape(
            CODE_GATE_UNKNOWN,
            name.clone(),
            &g.file.clone(),
            "",
            format!("no gate named `{name}` to judge"),
            "`brickie test --list` lists them all",
        );
        return Response::ok(diags, json!({}));
    }

    let (require, forbid, tags) = match (gate, script) {
        (Some(x), _) => (x.require.clone(), x.forbid.clone(), x.require_tags.clone()),
        (None, Some(s)) => (s.require.clone(), s.forbid.clone(), Vec::new()),
        _ => unreachable!(),
    };

    let mut checks: Vec<Value> = Vec::new();
    let mut failed = 0usize;
    for (i, pat) in require.iter().enumerate() {
        let hit = lines.iter().any(|l| regex_lite_match(pat, l));
        checks.push(json!({"kind": "require", "pattern": pat, "ok": hit}));
        if !hit {
            failed += 1;
            diags.coded(
                CODE_GATE_FAILED,
                Severity::Error,
                name.clone(),
                g.file.clone(),
                format!("gate[{i}].require"),
                format!("gate `{name}`: no log line matches `{pat}`"),
                "criteria live in tests/gates.toml; the log is at the path declared by the step",
            );
        }
    }
    for (i, pat) in forbid.iter().enumerate() {
        let hit = lines.iter().any(|l| regex_lite_match(pat, l));
        checks.push(json!({"kind": "forbid", "pattern": pat, "ok": !hit}));
        if hit {
            failed += 1;
            let sample = lines
                .iter()
                .find(|l| regex_lite_match(pat, l))
                .copied()
                .unwrap_or_default();
            diags.coded(
                CODE_GATE_FAILED,
                Severity::Error,
                name.clone(),
                g.file.clone(),
                format!("gate[{i}].forbid"),
                format!("gate `{name}`: log hit forbidden pattern `{pat}`: {sample}"),
                "this is \"something that must not appear appeared\" — look at that log line first",
            );
        }
    }
    for (i, tag) in tags.iter().enumerate() {
        let pat = format!("PASS {tag} ");
        let hit = log.contains(&pat);
        checks.push(json!({"kind": "require_tag", "pattern": pat, "ok": hit}));
        if !hit {
            failed += 1;
            diags.coded(
                CODE_GATE_FAILED,
                Severity::Error,
                name.clone(),
                g.file.clone(),
                format!("gate[{i}].require_tags"),
                format!("gate `{name}`: missing PASS for case `{tag}` (a dropped case also goes red)"),
                "a missing case is as red as a failing one — same discipline as irq-test",
            );
        }
    }

    let pass_lines = lines.iter().filter(|l| l.contains("PASS ")).count();
    if failed == 0 {
        // All green is info with no code: not a defect, and a failure code would mislead readers.
        diags.push(
            crate::diag::Diag::new(
                Severity::Info,
                format!(
                    "gate `{name}` all green: {} require / {} forbid / {} case tags / {} log lines ({pass_lines} PASS lines)",
                    require.len(),
                    forbid.len(),
                    tags.len(),
                    lines.len()
                ),
            )
            .target(name.clone())
            .file(g.file.clone()),
        );
        diags.set_exit_override(EXIT_OK);
    }

    let data = json!({
        "name": name,
        "failed": failed,
        "checks": checks,
        "log_lines": lines.len(),
        "pass_lines": pass_lines,
        "require": require.len(),
        "forbid": forbid.len(),
        "require_tags": tags.len(),
        "file": g.file,
    });
    let mut resp = Response::ok(diags, data);
    if failed > 0 {
        resp.exit_code = EXIT_RED;
    }
    resp
}

/// Minimal regex: subset matching for `.` `*` `+` `?` `[...]` and `\x` escapes (no engine).
fn regex_lite_match(pat: &str, text: &str) -> bool {
    match regex_lite_at(pat, text) {
        Some(_) => true,
        None => false,
    }
}

/// Try to match `pat` anywhere in `text` (backtracking implementation; patterns are short).
fn regex_lite_at(pat: &str, text: &str) -> Option<usize> {
    let p: Vec<char> = pat.chars().collect();
    let t: Vec<char> = text.chars().collect();
    for start in 0..=t.len() {
        if let Some(end) = regex_lite_here(&p, &t, 0, start) {
            return Some(end);
        }
    }
    None
}

/// Try to match from `(pi, ti)` to the end of the pattern, returning the **end position** (for `+`/`*` backtracking).
fn regex_lite_here(p: &[char], t: &[char], pi: usize, ti: usize) -> Option<usize> {
    if pi >= p.len() {
        return Some(ti);
    }
    let (atom_len, atom) = match p[pi] {
        '\\' if pi + 1 < p.len() => (2usize, Some(p[pi + 1])),
        '[' => {
            // Character class: scan to `]`, supporting `^` negation and `a-z`
            let mut j = pi + 1;
            while j < p.len() && p[j] != ']' {
                j += 1;
            }
            if j >= p.len() {
                (1, Some('['))
            } else {
                (j + 1 - pi, None)
            }
        }
        c => (1, Some(c)),
    };
    let class: Option<Vec<char>> = if p[pi] == '[' {
        let mut j = pi + 1;
        while j < p.len() && p[j] != ']' {
            j += 1;
        }
        if j >= p.len() {
            None
        } else {
            Some(p[pi + 1..j].to_vec())
        }
    } else {
        None
    };
    let quant = p.get(pi + atom_len).copied();
    let (min, max): (usize, Option<usize>) = match quant {
        Some('*') => (0, None),
        Some('+') => (1, None),
        Some('?') => (0, Some(1)),
        _ => (1, Some(1)),
    };
    let next_pi = if matches!(quant, Some('*') | Some('+') | Some('?')) {
        pi + atom_len + 1
    } else {
        pi + atom_len
    };
    let matches_atom = |c: char| -> bool {
        if p[pi] == '.' {
            return true;
        }
        if let Some(cls) = &class {
            let negate = cls.first() == Some(&'^');
            let body = if negate { &cls[1..] } else { &cls[..] };
            let mut i = 0usize;
            let mut hit = false;
            while i < body.len() {
                if i + 2 < body.len() && body[i + 1] == '-' {
                    if c >= body[i] && c <= body[i + 2] {
                        hit = true;
                    }
                    i += 3;
                } else {
                    if c == body[i] {
                        hit = true;
                    }
                    i += 1;
                }
            }
            return hit != negate;
        }
        match atom {
            Some(a) => a == c,
            None => false,
        }
    };

    // Greedily consume the atom, then back off step by step (patterns are short; no smarter engine needed)
    let mut count = 0usize;
    while ti + count < t.len() && matches_atom(t[ti + count]) {
        count += 1;
        if let Some(m) = max {
            if count >= m {
                break;
            }
        }
    }
    if count < min {
        return None;
    }
    let mut k = count;
    loop {
        if let Some(end) = regex_lite_here(p, t, next_pi, ti + k) {
            return Some(end);
        }
        if k == min {
            return None;
        }
        k -= 1;
    }
}

/// Pure-function entry for `brickie-core --selftest` (no disk access: stat injected via `context.files`).
pub fn selftest_plan(root: &Path, req: &Request) -> (Diags, Value) {
    let tree = model::load_tree(root);
    let mut diags = tree.diags.clone();
    match load_ctx(req, &tree) {
        Ok((ctx, mut load_diags)) => {
            let plan = make_plan(&ctx);
            let steps = all_steps(&ctx);
            load_diags.merge(diags);
            (
                load_diags,
                summary_json(
                    &ctx,
                    &plan,
                    &steps,
                    json!({"selftest": true, "state_content": plan.state_content}),
                ),
            )
        }
        Err(d) => {
            diags.merge(d);
            (diags, json!({}))
        }
    }
}

/// Let `selftest` unit-test the regex subset too.
pub fn selftest_regex(pat: &str, text: &str) -> bool {
    regex_lite_match(pat, text)
}

pub fn selftest_regex_unsupported(pat: &str) -> Option<char> {
    regex_unsupported(pat)
}

pub fn selftest_glob(pat: &str, name: &str) -> bool {
    glob_match(pat, name)
}

/// `selftest`: render one backend file directly (without writing it), to pin key lines of generated output.
pub fn selftest_render_backend(root: &Path, req: &Request, which: &str) -> String {
    let tree = model::load_tree(root);
    match load_ctx(req, &tree) {
        Ok((ctx, _)) => {
            let steps = all_steps(&ctx);
            if which == "make" {
                render_make(&ctx, &steps)
            } else {
                render_ninja(&ctx, &steps)
            }
        }
        Err(_) => String::new(),
    }
}

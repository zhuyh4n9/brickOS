//! Plan generation: `plan-new` / `plan-init` / `gen-plan`.
//! Plan only, no content: returns `data.artifacts: [{path, kind, template, vars}]`; rendering is
//! L2's job (two-phase: plan → scan → render). Never overwrite human-written files: an existing
//! `human` path ⇒ `BRV-GEN-0002` (error, exit 1). `--force` applies only to generated directories;
//! generated files carry the first-line marker `brickie:generated`. Both placeholder sets are
//! provided (lowercase `{{name}}`, uppercase `{{PLUGIN_NAME}}`).

use std::collections::BTreeMap;

use serde_json::{json, Value};

use crate::diag::Diags;
use crate::model::Plugin;
use crate::rules;
use crate::version;

/// Generated-file first-line marker; single source is [`crate::emit::GENERATED_MARKER`].
pub use crate::emit::GENERATED_MARKER;

#[derive(Clone, Debug)]
pub struct Artifact {
    /// Relative to `root`.
    pub path: String,
    /// `human` | `generated` | `machine`.
    pub kind: String,
    /// Template id (relative to `templates/`); empty for `machine` files written by core.
    pub template: String,
    pub vars: BTreeMap<String, String>,
}

impl Artifact {
    pub fn to_json(&self) -> Value {
        json!({
            "path": self.path,
            "kind": self.kind,
            "template": self.template,
            "vars": self.vars,
        })
    }
}

/// Expansion of one `[[dep]]` (one `br_dep_t` in the descriptor).
#[derive(Clone, Debug)]
pub struct DepFact {
    pub name: String,
    pub kind: String,
    pub phase: Option<String>,
}

/// Facts of one plugin for variable expansion; decoupled from model::Plugin so plan-new runs first.
#[derive(Clone, Debug)]
pub struct PluginFacts<'a> {
    pub name: &'a str,
    pub plugin_type: &'a str,
    pub api_type: &'a str,
    pub subkind: Option<&'a str>,
    pub lang: &'a str,
    pub phase: &'a str,
    pub sched_class: &'a str,
    /// Hook/descriptor symbol prefix override (see `model::Plugin::symbol_prefix`); `None` = derive from short.
    pub symbol_prefix: Option<String>,
    pub version: &'a str,
    /// `[compat].api_rev` (default 1).
    pub api_rev: i64,
    /// `[compat].abi_id` (default empty).
    pub abi_id: String,
    /// Sum over `[[res]]`: RAM / stack (KiB).
    pub res_ram_kib: i64,
    pub res_stack_kib: i64,
    /// One entry per structural dependency (the descriptor's `.deps` array).
    pub deps: Vec<DepFact>,
    /// Number of generated artifacts (value of weak symbol `br_plugin_gen_total`).
    pub gen_total: u32,
    /// Declaration-surface anchor header of the descriptor (path relative to the plugin's `include/` root).
    pub decl_include: String,
    /// Has an `init` hook (derived from `phase != "early"`, see rules::derive_phase).
    pub has_init: bool,
    /// Whether this generation emits the `selftest` hook: declares `[selftest]` and the product
    /// switch (incl. allowlist) permits it; false ⇒ `BR_PLUGIN_NO_HOOK` + `--gc-sections` drop.
    pub emit_selftest: bool,
    /// One entry per `[[mount]]` (this APP's mount plan); the table is executed by its start phase.
    pub mounts: Vec<MountFact>,
}

/// One `[[mount]]` entry (generation-time fact; see `model::MountDecl`).
#[derive(Clone, Debug)]
pub struct MountFact {
    pub source: Option<String>,
    pub target: String,
    pub fstype: String,
    pub order: i64,
}

/// Fallback value for `descriptor_include`: the meta-contract header (present in any composition).
pub const FALLBACK_DECL_INCLUDE: &str = "br/core/br_plugin.h";

fn plugin_type_const(t: &str) -> &'static str {
    match t {
        "platform" => "BR_PLUGIN_TYPE_PLATFORM",
        "app" => "BR_PLUGIN_TYPE_APP",
        "interface" => "BR_PLUGIN_TYPE_INTERFACE",
        "core" => "BR_PLUGIN_TYPE_CORE",
        _ => "BR_PLUGIN_TYPE_ABILITY",
    }
}

fn subkind_const(s: Option<&str>) -> &'static str {
    match s.unwrap_or("") {
        "scheduler" => "BR_SUBKIND_SCHEDULER",
        "framework" => "BR_SUBKIND_FRAMEWORK",
        "io" => "BR_SUBKIND_IO",
        "fs" => "BR_SUBKIND_FS",
        "service" => "BR_SUBKIND_SERVICE",
        _ => "BR_SUBKIND_NONE",
    }
}

fn sched_class_const(c: &str) -> &'static str {
    match c {
        "COOP_ONLY" => "BR_SCHED_CLASS_COOP_ONLY",
        "TT_SAFE" => "BR_SCHED_CLASS_TT_SAFE",
        _ => "BR_SCHED_CLASS_SAFE_PREEMPT",
    }
}

fn dep_kind_const(kind: &str) -> &'static str {
    match kind {
        "init" => "BR_DEP_INIT",
        "type" => "BR_DEP_TYPE",
        _ => "BR_DEP_RUNTIME",
    }
}

/// `br_dep_t.phase`: only for `kind = "init"`; otherwise `BR_PHASE_NONE` (unconstrained).
fn dep_phase_const(kind: &str, phase: Option<&str>) -> &'static str {
    if kind != "init" {
        return "BR_PHASE_NONE";
    }
    match phase {
        Some("early") => "BR_PHASE_EARLY",
        Some("core") => "BR_PHASE_CORE",
        Some("late") => "BR_PHASE_LATE",
        Some("app") => "BR_PHASE_APP",
        _ => "BR_PHASE_NONE",
    }
}

/// C text of the dependency array + the `.deps` value expression (no edges ⇒ `BR_NULL`), and the
/// mount plan table `<symbol_prefix>mount_plan`; empty `source` ⇒ `BR_NULL`, empty plan ⇒ two
/// empty strings so the template emits nothing.
fn mount_plan_decl(prefix: &str, mounts: &[MountFact]) -> (String, String) {
    if mounts.is_empty() {
        return (String::new(), String::new());
    }
    let mut sorted: Vec<&MountFact> = mounts.iter().collect();
    sorted.sort_by_key(|m| m.order); // stable sort: same order keeps declaration order
    let mut out = String::new();
    out.push_str(
        "/*\n * Mount plan (from brick.toml [[mount]]; ADR-0030) -- **executed by this APP**.\n"
    );
    out.push_str(
        " *\n * Why emit a \"table + apply function\" instead of letting the APP iterate itself:\n"
    );
    out.push_str(
        " *   the APP may not include core headers (ADR-0020: APP depends on Interface only) ⇒ `br_mount_plan_t`\n"
    );
    out.push_str(
        " *   cannot appear in APP source. So both the table and the iteration live in this generated file,\n"
    );
    out.push_str(
        " *   and the APP calls just `int <prefix>mount_plan_apply(void)` (prototype in the APP's own header).\n"
    );
    out.push_str(
        " * `order` is used only for generation-time sorting (\"parent before sub-mount points\"), not in mount() calls. */\n"
    );
    out.push_str("#include <sys/mount.h>   /* POSIX mount() */\n");
    out.push_str("#include <errno.h>\n");
    out.push_str("#include <stddef.h>   /* NULL */\n");
    out.push_str("#include <br/core/br_log.h>\n\n");
    out.push_str(&format!(
        "static const br_mount_plan_t {prefix}mount_plan[] = {{\n"
    ));
    for m in &sorted {
        let src = match &m.source {
            Some(s) => format!("\"{s}\""),
            None => "BR_NULL".to_string(),
        };
        out.push_str(&format!(
            "    {{ .source = {}, .target = \"{}\", .fstype = \"{}\", .order = {}u }},\n",
            src, m.target, m.fstype, m.order
        ));
    }
    out.push_str("};\n\n");
    out.push_str(
        "/* Execute the mount plan entry by entry. Returns 0 or -1 (errno set); stopping at the first\n * failure is deliberate: running the APP in a half-broken namespace with a missing volume only turns\n * \"mount failed\" into \"file not found\" elsewhere. */\n",
    );
    out.push_str(&format!("int {prefix}mount_plan_apply(void)\n{{\n"));
    out.push_str(&format!(
        "    for (br_u32 i = 0u; i < {}u; i++) {{\n", sorted.len()
    ));
    out.push_str("        const br_mount_plan_t *m = &");
    out.push_str(prefix);
    out.push_str("mount_plan[i];\n");
    out.push_str("        if (mount(m->source, m->target, m->fstype, 0ul, NULL) != 0) {\n");
    out.push_str("            br_log_error(\"mount(%s, %s, %s) failed errno=%d (halt on first failure)\",\n");
    out.push_str("                         (m->source != NULL) ? m->source : \"-\", m->target,\n");
    out.push_str("                         m->fstype, errno);\n");
    out.push_str("            return -1;\n");
    out.push_str("        }\n");
    out.push_str("        br_log_info(\"mounted %s (fstype=%s)\", m->target, m->fstype);\n");
    out.push_str("    }\n    return 0;\n}\n");
    let n = format!("const br_u32 {prefix}mount_plan_n = {}u;\n", sorted.len());
    out.push_str(&n);
    (out, format!("{prefix}mount_plan_n"))
}

fn deps_decl(tag: &str, deps: &[DepFact]) -> (String, String) {
    if deps.is_empty() {
        return (String::new(), "BR_NULL".to_string());
    }
    let sym = format!("_br_deps_{tag}");
    let mut out = String::new();
    out.push_str(&format!(
        "static const br_dep_t {sym}[] __attribute__((used)) = {{\n"
    ));
    for d in deps {
        out.push_str(&format!(
            "    {{ .name = \"{}\", .kind = {}, .phase = {}, .compat_gen = 0u }},\n",
            d.name,
            dep_kind_const(&d.kind),
            dep_phase_const(&d.kind, d.phase.as_deref())
        ));
    }
    out.push_str("    { .name = BR_NULL, .kind = 0u, .phase = BR_PHASE_NONE, .compat_gen = 0u },\n};\n");
    (out, sym)
}

/// Pick the declaration-surface anchor header: the `include/` header declaring the most
/// `[[export.entries]]` symbols (ties ⇒ lexicographically first), else the meta-contract header.
fn scan_decl_include(root: &std::path::Path, plugin: &str, symbols: &[String]) -> Option<String> {
    let base = root.join(plugin).join("include");
    let mut found: Vec<String> = Vec::new();
    collect_headers(&base, &base, &mut found);
    found.sort();
    if found.is_empty() {
        return None;
    }
    let mut best: Option<(usize, String)> = None;
    for rel in &found {
        let text = std::fs::read_to_string(base.join(rel)).unwrap_or_default();
        let hits = symbols.iter().filter(|s| !s.is_empty() && text.contains(s.as_str())).count();
        match &best {
            Some((n, _)) if *n >= hits => {}
            _ => best = Some((hits, rel.clone())),
        }
    }
    best.map(|(_, rel)| rel)
}

fn collect_headers(base: &std::path::Path, dir: &std::path::Path, out: &mut Vec<String>) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect_headers(base, &p, out);
        } else if p.extension().map(|x| x == "h").unwrap_or(false) {
            if let Ok(rel) = p.strip_prefix(base) {
                out.push(rel.to_string_lossy().replace('\\', "/"));
            }
        }
    }
}

/// Symbol prefix: non-`[a-z0-9]` → `_`, trailing `_` appended (same convention as cxx `rules.cpp`).
pub fn symbol_prefix(short: &str) -> String {
    let mut out = String::with_capacity(short.len() + 1);
    for c in short.chars() {
        if c.is_ascii_lowercase() || c.is_ascii_digit() {
            out.push(c);
        } else if c.is_ascii_uppercase() {
            out.push(c.to_ascii_lowercase());
        } else {
            out.push('_');
        }
    }
    out.push('_');
    out
}

pub fn include_guard(ns: &str, short: &str) -> String {
    let base = if ns.is_empty() {
        short.to_string()
    } else {
        format!("{ns}_{short}")
    };
    let mut up = String::new();
    for c in base.chars() {
        if c.is_ascii_lowercase() {
            up.push(c.to_ascii_uppercase());
        } else if c.is_ascii_uppercase() || c.is_ascii_digit() {
            up.push(c);
        } else {
            up.push('_');
        }
    }
    format!("{up}_H")
}

/// Descriptor macro argument (symbol prefix without the trailing underscore).
pub fn descriptor_tag(short: &str) -> String {
    let mut p = symbol_prefix(short);
    p.pop();
    p
}

pub fn descriptor_symbol(short: &str) -> String {
    format!("_br_plugin_{}", descriptor_tag(short))
}

/// Expand a plugin's placeholder table (both lowercase and uppercase sets).
pub fn plugin_vars(f: &PluginFacts<'_>) -> BTreeMap<String, String> {
    let ns = rules::name_namespace(f.name).to_string();
    let short = rules::name_short(f.name).to_string();
    let sub = f.subkind.unwrap_or("").to_string();
    let sym = f
        .symbol_prefix
        .clone()
        .unwrap_or_else(|| symbol_prefix(&short));
    let guard = include_guard(&ns, &short);
    let dtag = descriptor_tag(&short);
    let dsym = descriptor_symbol(&short);
    let parsed = version::parse_version(f.version).ok();

    let mut v: BTreeMap<String, String> = BTreeMap::new();
    v.insert("name".into(), f.name.to_string());
    v.insert("schema".into(), "1".into());
    v.insert("plugin_type".into(), f.plugin_type.to_string());
    v.insert("api_type".into(), f.api_type.to_string());
    v.insert("subkind".into(), sub.clone());
    v.insert("lang".into(), f.lang.to_string());
    v.insert("phase".into(), f.phase.to_string());
    v.insert("version".into(), f.version.to_string());
    v.insert("sched_class".into(), f.sched_class.to_string());
    v.insert("summary".into(), "TODO: one-line description of this plugin's capability surface (see §8.1)".into());
    v.insert("license".into(), "WTFPL".into());
    v.insert("short".into(), short.clone());
    v.insert("ns".into(), ns.clone());
    v.insert("header_guard".into(), guard.clone());
    v.insert("symbol_prefix".into(), sym.clone());
    v.insert("descriptor_symbol".into(), dsym.clone());
    // `#include "{{descriptor_include}}"` is the declaration-surface anchor header: exactly one
    // public header under `include/` ⇒ use it, else the meta-contract header.
    v.insert("descriptor_include".into(), f.decl_include.clone());
    v.insert("plugin_type_id".into(), plugin_type_const(f.plugin_type).into());
    v.insert("subkind_id".into(), subkind_const(f.subkind).into());
    v.insert("sched_class_id".into(), sched_class_const(f.sched_class).into());
    v.insert("api_rev".into(), f.api_rev.to_string());
    v.insert("abi_id".into(), f.abi_id.clone());
    v.insert("res_ram_kib".into(), f.res_ram_kib.max(0).to_string());
    v.insert("res_stack_kib".into(), f.res_stack_kib.max(0).to_string());
    v.insert("gen_total".into(), f.gen_total.to_string());
    // early_init/start for all plugins; init only when phase != "early", else BR_PLUGIN_NO_HOOK.
    // Two exceptions: `core` drives the phase machine instead of taking part in it, so it defines no
    // lifecycle at all (its `[selftest]` hook stays -- self-test is verification, not a phase); and the
    // APP emits no `early_init` (ADR-0037: it registers through the section scan, so the hook has
    // nothing to do -- the runtime re-proves that on the descriptor).
    let has_lifecycle = f.plugin_type != "core";
    let has_early = has_lifecycle && f.plugin_type != "app";
    v.insert(
        "early_hook".into(),
        if has_early {
            format!("{sym}early_init")
        } else {
            "BR_PLUGIN_NO_HOOK".into()
        },
    );
    v.insert(
        "init_hook".into(),
        if has_lifecycle && f.has_init {
            format!("{sym}init")
        } else {
            "BR_PLUGIN_NO_HOOK".into()
        },
    );
    v.insert(
        "start_hook".into(),
        if has_lifecycle {
            format!("{sym}start")
        } else {
            "BR_PLUGIN_NO_HOOK".into()
        },
    );
    v.insert(
        "early_proto".into(),
        if has_early {
            format!("int {sym}early_init(void);")
        } else {
            String::new()
        },
    );
    v.insert(
        "init_proto".into(),
        if has_lifecycle && f.has_init {
            format!("int {sym}init(void);")
        } else {
            String::new()
        },
    );
    v.insert(
        "start_proto".into(),
        if has_lifecycle {
            format!("int {sym}start(void);")
        } else {
            String::new()
        },
    );
    // selftest prototype only when the hook is emitted (else -Wmissing-prototypes/link errors).
    v.insert(
        "selftest_proto".into(),
        if f.emit_selftest {
            format!("int {sym}selftest(void);")
        } else {
            String::new()
        },
    );
    v.insert(
        "selftest_hook".into(),
        if f.emit_selftest {
            format!("{sym}selftest")
        } else {
            "BR_PLUGIN_NO_HOOK".into()
        },
    );
    let (deps_array, deps_expr) = deps_decl(&dtag, &f.deps);
    v.insert("deps_array".into(), deps_array);
    v.insert("deps_expr".into(), deps_expr);
    // Mount plan: table + length; stable sort by `order` so the result is reproducible.
    let (mount_plan, mount_plan_n) = mount_plan_decl(&sym, &f.mounts);
    v.insert("mount_plan".into(), mount_plan);
    v.insert("mount_plan_n".into(), mount_plan_n);
    v.insert("export_api_iface".into(), f.api_type.to_string());
    v.insert("export_form".into(), "api".into()); // §8.4 / ruling G-3
    v.insert("export_name".into(), short.clone());
    v.insert("app".into(), f.name.to_string());

    v.insert("PLUGIN_NAME".into(), f.name.to_string());
    v.insert("NS".into(), ns);
    v.insert("SHORT".into(), short.clone());
    v.insert("PLUGIN_TYPE".into(), f.plugin_type.to_string());
    v.insert("API_TYPE".into(), f.api_type.to_string());
    v.insert("SUBKIND".into(), sub);
    v.insert("LANG".into(), f.lang.to_string());
    v.insert("PHASE".into(), f.phase.to_string());
    v.insert("VERSION".into(), f.version.to_string());
    v.insert(
        "COMPAT_GEN".into(),
        parsed.map(|p| p.compat_gen.to_string()).unwrap_or_else(|| "0".into()),
    );
    v.insert(
        "MAJOR".into(),
        parsed.map(|p| p.major.to_string()).unwrap_or_else(|| "1".into()),
    );
    v.insert(
        "MINOR".into(),
        parsed.map(|p| p.minor.to_string()).unwrap_or_else(|| "0".into()),
    );
    v.insert(
        "REVISE".into(),
        parsed.map(|p| p.revise.to_string()).unwrap_or_else(|| "0".into()),
    );
    v.insert("SUMMARY".into(), v["summary"].clone());
    v.insert("LICENSE".into(), v["license"].clone());
    v.insert("CORE_RANGE".into(), ">=1.0.0".into());
    v.insert("API_REV".into(), "1".into());
    v.insert("SYMPREFIX".into(), sym);
    v.insert("GUARD".into(), guard);
    v.insert("DESC_TAG".into(), dtag);
    v.insert("DESC_SYMBOL".into(), dsym);
    v.insert("DESC_INCLUDE".into(), f.decl_include.clone());
    v.insert("EXPORT_UNIT".into(), short);
    v
}

/// Path layout: the six `(path, kind, template)` triples.
pub fn plugin_paths(f: &PluginFacts<'_>) -> Vec<(String, String, String)> {
    let short = rules::name_short(f.name);
    let skel = format!("{}/{}/{}/", f.api_type, f.plugin_type, f.lang);
    let desc = format!("descriptor/{}/{}/", f.api_type, f.lang);
    vec![
        (
            format!("{}/brick.toml", f.name),
            "human".into(),
            format!("{skel}brick.toml.tmpl"),
        ),
        (
            format!("{}/src/{}.c", f.name, short),
            "human".into(),
            format!("{skel}src.c.tmpl"),
        ),
        (
            format!("{}/include/{}/{}.h", f.name, short, short),
            "human".into(),
            format!("{skel}include.h.tmpl"),
        ),
        (
            format!("{}/tests/smoke.toml", f.name),
            "human".into(),
            format!("{skel}smoke.toml.tmpl"),
        ),
        (
            format!("{}/README.md", f.name),
            "human".into(),
            format!("{skel}README.md.tmpl"),
        ),
        (
            format!("build/gen/{}/plugin_desc.c", f.name),
            "generated".into(),
            format!("{desc}plugin_desc.c.tmpl"),
        ),
    ]
}

fn existing_index(context: &serde_json::Map<String, Value>) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    if let Some(arr) = context.get("existing").and_then(|v| v.as_array()) {
        for e in arr {
            if let (Some(p), h) = (
                e.get("path").and_then(|v| v.as_str()),
                e.get("first_line").and_then(|v| v.as_str()),
            ) {
                out.insert(p.to_string(), h.unwrap_or("").to_string());
            }
        }
    }
    out
}

fn looks_generated(first_line: &str) -> bool {
    first_line.contains(GENERATED_MARKER)
}

/// Conflict detection: never overwrite human files; foreign generated files need `--force`.
fn apply_conflicts(
    artifacts: &mut Vec<Artifact>,
    existing: &BTreeMap<String, String>,
    force: bool,
    target: &str,
    d: &mut Diags,
) {
    let mut keep = Vec::with_capacity(artifacts.len());
    for a in artifacts.drain(..) {
        match existing.get(&a.path) {
            None => keep.push(a),
            Some(first) => {
                if a.kind == "human" {
                    d.coded(
                        "BRV-GEN-0002",
                        crate::diag::Severity::Error,
                        target,
                        &a.path,
                        "",
                        format!("generation target `{}` already exists and is a human-written file, not overwriting", a.path),
                        "human-written files are not rewritten by the tool; rename the plugin or move the file away (§8.4)",
                    );
                    continue;
                }
                if looks_generated(first) {
                    keep.push(a); // already a generated file ⇒ normal rebuild
                } else if force {
                    keep.push(a); // --force applies only to generated directories
                } else {
                    d.coded(
                        "BRV-GEN-0002",
                        crate::diag::Severity::Error,
                        target,
                        &a.path,
                        "",
                        format!("generation target `{}` already exists and is not this tool's artifact", a.path),
                        "add --force once overwriting is confirmed (--force applies only to generated directories) (§8.4)",
                    );
                }
            }
        }
    }
    *artifacts = keep;
}

/// `plan-new`: validate args → derive → conflict detection → plan.
pub fn plan_new(args: &serde_json::Map<String, Value>, context: &serde_json::Map<String, Value>) -> (Diags, Vec<Artifact>) {
    let mut d = Diags::new();
    let get = |k: &str| args.get(k).and_then(|v| v.as_str()).map(str::to_string);

    let plugin_type = get("plugin_type").unwrap_or_default();
    let name = get("name").unwrap_or_default();
    let api_type = get("api_type").unwrap_or_else(|| "native".to_string());
    let lang = get("lang").unwrap_or_else(|| "c".to_string());
    let explicit_subkind = get("subkind").filter(|s| !s.is_empty());
    let force = args.get("force").and_then(|v| v.as_bool()).unwrap_or(false);

    if plugin_type.is_empty() {
        d.shape("BRV-MF-0001", &name, "", "args.plugin_type", "missing required argument `plugin_type`", &format!("valid values: {}", rules::list_of(rules::PLUGIN_TYPES)));
        return (d, Vec::new());
    }
    if !rules::in_list(rules::PLUGIN_TYPES, &plugin_type) {
        d.shape("BRV-MF-0001", &name, "", "args.plugin_type", format!("unknown plugin type `{plugin_type}`"), &format!("valid values: {} (§8.1)", rules::list_of(rules::PLUGIN_TYPES)));
        return (d, Vec::new());
    }
    if !rules::in_list(rules::API_TYPES, &api_type) {
        d.shape("BRV-MF-0001", &name, "", "args.api_type", format!("unknown api_type `{api_type}`"), &format!("valid values: {} (§3.1)", rules::list_of(rules::API_TYPES)));
        return (d, Vec::new());
    }
    if name.is_empty() {
        d.shape("BRV-MF-0001", "", "", "args.name", "missing required argument `name`", "recommended `<namespace>/<short>` (§8.3)");
        return (d, Vec::new());
    }
    if !rules::valid_plugin_name(&name) {
        d.shape("BRV-MF-0001", &name, "", "args.name", format!("plugin name `{name}` violates the name contract"), "contract: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return (d, Vec::new());
    }
    if !rules::in_list(rules::LANGS, &lang) {
        d.shape("BRV-MF-0001", &name, "", "args.lang", format!("unknown lang `{lang}`"), &format!("valid values: {} (§8.1)", rules::list_of(rules::LANGS)));
        return (d, Vec::new());
    }
    if !rules::in_list(rules::TEMPLATE_LANGS, &lang) {
        d.usage(
            format!("v0.1 delivers only the `c` template, got `{lang}`"),
            "`rust` is scheduled for v2.0 (1-03); `cxx` pending BRV-Q8",
        );
        return (d, Vec::new());
    }

    // BRV-TAX-0014 is info, but the command cannot complete ⇒ exit code 2 (G-1).
    if !rules::template_available(&api_type, &lang) {
        d.coded(
            "BRV-TAX-0014",
            crate::diag::Severity::Info,
            &name,
            "",
            "args.api_type",
            format!("`{api_type}` template not delivered in v0.1 (directory reserved)"),
            rules::template_unavailable_reason(&api_type, &lang),
        );
        d.set_exit_override(crate::diag::EXIT_USAGE);
        return (d, Vec::new());
    }

    let mut subkind = String::new();
    if plugin_type == "ability" {
        match (rules::derive_subkind(&name), &explicit_subkind) {
            (Some(der), Some(explicit)) if der != explicit => {
                d.coded(
                    "BRV-TAX-0015",
                    crate::diag::Severity::Error,
                    &name,
                    "",
                    "args.subkind",
                    format!("derived and explicit subkind conflict: derived from name = `{der}`, explicit = `{explicit}`"),
                    "drop `--subkind`, or use a name consistent with the namespace (§8.4)",
                );
                return (d, Vec::new());
            }
            (Some(der), _) => subkind = der.to_string(),
            (None, None) => {
                d.shape(
                    "BRV-MF-0001",
                    &name,
                    "",
                    "args.subkind",
                    format!("ability plugin `{name}` must give `subkind` explicitly (a bare name gives nothing to derive from)"),
                    &format!("valid values: {}; or use a derivable name (service/ sched/ framework/ io/ fs/) (§8.4)", rules::list_of(rules::SUBKINDS)),
                );
                return (d, Vec::new());
            }
            (None, Some(explicit)) => {
                if !rules::in_list(rules::SUBKINDS, explicit) {
                    d.shape("BRV-MF-0001", &name, "", "args.subkind", format!("unknown subkind `{explicit}`"), &format!("valid values: {}", rules::list_of(rules::SUBKINDS)));
                    return (d, Vec::new());
                }
                subkind = explicit.clone();
            }
        }
    } else if explicit_subkind.is_some() {
        d.shape(
            "BRV-MF-0001",
            &name,
            "",
            "args.subkind",
            format!("`subkind` is meaningful only for ability plugins, `{plugin_type}` does not accept it"),
            "§8.1: subkind subdivides ability only",
        );
        return (d, Vec::new());
    }

    if !rules::is_recommended_form(&name) {
        d.coded(
            "BRV-TAX-0013",
            crate::diag::Severity::Warning,
            &name,
            "",
            "args.name",
            "plugin name does not match the recommended form `<namespace>/<short>`",
            format!("recommended namespaces: {} (§8.3)", rules::list_of(rules::NAMESPACES)),
        );
    }

    // The new skeleton includes an init stub (early_init/init/start) ⇒ has_init = true, except for
    // `core`: it defines no lifecycle at all (it drives the phase machine), so it stays at `early`.
    let sub_for_phase = if plugin_type == "ability" {
        Some(subkind.as_str())
    } else {
        None
    };
    let has_init = plugin_type != "core";
    let phase = rules::derive_phase(&plugin_type, sub_for_phase, has_init);
    let facts = PluginFacts {
        name: &name,
        plugin_type: &plugin_type,
        api_type: &api_type,
        subkind: Some(&subkind),
        lang: &lang,
        phase,
        sched_class: "SAFE_PREEMPT",
        symbol_prefix: None,
        version: "0.1.0.0",
        api_rev: 1,
        abi_id: String::new(),
        res_ram_kib: 0,
        res_stack_kib: 0,
        deps: Vec::new(),
        // Skeleton phase: one artifact; `brickie gen` recomputes it from the plugin tree.
        gen_total: 1,
        // Skeleton phase: the plan creates `include/<short>/<short>.h` itself.
        decl_include: format!(
            "{short}/{short}.h",
            short = rules::name_short(&name)
        ),
        has_init,
        emit_selftest: false,
        mounts: Vec::new(),
    };
    let vars = plugin_vars(&facts);
    let mut artifacts: Vec<Artifact> = plugin_paths(&facts)
        .into_iter()
        .map(|(path, kind, template)| Artifact {
            path,
            kind,
            template,
            vars: vars.clone(),
        })
        .collect();

    let existing = existing_index(context);
    if context.contains_key("existing") {
        apply_conflicts(&mut artifacts, &existing, force, &name, &mut d);
    }
    if d.has_error() {
        return (d, Vec::new());
    }
    (d, artifacts)
}

/// `plan-init`: `product.toml` + `app/<name>/` skeleton + initial `brickie.lock`.
///
/// The lock is a machine file (core owns the content): `kind = "machine"`, `template = ""`, and the
/// text goes into `data.lock_content` from [`crate::emit::render_lock`].
pub fn plan_init(
    args: &serde_json::Map<String, Value>,
    context: &serde_json::Map<String, Value>,
) -> (Diags, Vec<Artifact>, Option<String>) {
    let mut d = Diags::new();
    let name = args.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string();
    if name.is_empty() {
        d.shape("BRV-MF-0001", "", "", "args.name", "missing required argument `name`", "product name (§8.2)");
        return (d, Vec::new(), None);
    }
    if !rules::valid_plugin_name(&name) {
        d.shape("BRV-MF-0001", &name, "", "args.name", format!("product name `{name}` violates the name contract"), "contract: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return (d, Vec::new(), None);
    }
    let app = args
        .get("app")
        .and_then(|v| v.as_str())
        .map(str::to_string)
        .unwrap_or_else(|| format!("app/{name}"));
    if !rules::valid_plugin_name(&app) {
        d.shape("BRV-MF-0001", &name, "", "args.app", format!("app name `{app}` violates the name contract"), "§8.3");
        return (d, Vec::new(), None);
    }
    let lang = args.get("lang").and_then(|v| v.as_str()).unwrap_or("c").to_string();
    if !rules::in_list(rules::TEMPLATE_LANGS, &lang) {
        d.usage(format!("v0.1 delivers only the `c` template, got `{lang}`"), "product templates live in `templates/product/<lang>/`");
        return (d, Vec::new(), None);
    }

    let mut pvars: BTreeMap<String, String> = BTreeMap::new();
    pvars.insert("name".into(), name.clone());
    pvars.insert("schema".into(), "1".into());
    pvars.insert("version".into(), "0.1.0.0".into());
    pvars.insert("app".into(), app.clone());
    pvars.insert("PRODUCT_NAME".into(), name.clone());
    pvars.insert("PRODUCT_VERSION".into(), "0.1.0.0".into());
    pvars.insert("APP".into(), app.clone());
    pvars.insert("CORE_RANGE".into(), ">=1.0.0".into());
    pvars.insert("STAGE".into(), "dev".into());
    let mut artifacts = vec![Artifact {
        path: "product.toml".into(),
        kind: "human".into(),
        template: format!("product/{lang}/product.toml.tmpl"),
        vars: pvars,
    }];

    let app_facts = PluginFacts {
        name: &app,
        plugin_type: "app",
        api_type: "native",
        subkind: None,
        lang: &lang,
        phase: "app",
        sched_class: "SAFE_PREEMPT",
        symbol_prefix: None,
        version: "0.1.0.0",
        api_rev: 1,
        abi_id: String::new(),
        res_ram_kib: 0,
        res_stack_kib: 0,
        deps: Vec::new(),
        gen_total: 1,
        decl_include: format!("{short}/{short}.h", short = rules::name_short(&app)),
        has_init: true,
        emit_selftest: false,
        mounts: Vec::new(),
    };
    let app_vars = plugin_vars(&app_facts);
    for (path, kind, template) in plugin_paths(&app_facts) {
        artifacts.push(Artifact {
            path,
            kind,
            template,
            vars: app_vars.clone(),
        });
    }

    let lock = crate::model::Lock {
        schema: 1,
        profile: "dev".into(),
        product: Some(crate::model::LockProduct {
            name: name.clone(),
            version: "0.1.0.0".into(),
            stage: "dev".into(),
        }),
        plugins: Vec::new(),
        units: Vec::new(),
    };
    let lock_content = crate::emit::render_lock(&lock);
    let mut lvars: BTreeMap<String, String> = BTreeMap::new();
    lvars.insert("product".into(), name.clone());
    lvars.insert("version".into(), "0.1.0.0".into());
    artifacts.push(Artifact {
        path: "brickie.lock".into(),
        kind: "machine".into(),
        template: String::new(),
        vars: lvars,
    });

    let existing = existing_index(context);
    if context.contains_key("existing") {
        apply_conflicts(&mut artifacts, &existing, false, &name, &mut d);
    }
    if d.has_error() {
        return (d, Vec::new(), None);
    }
    (d, artifacts, Some(lock_content))
}

/// `gen-plan`: plan for rebuilding `build/gen/**` (descriptors only — in v0.1 the plugin header is
/// human-written). Generating the header too is left to v0.3.
pub fn gen_plan(
    root: &std::path::Path,
    args: &serde_json::Map<String, Value>,
) -> (Diags, Vec<Artifact>) {
    let only = args.get("plugin").and_then(|v| v.as_str()).map(str::to_string);
    let tree = crate::model::load_tree(root);
    let mut d = tree.diags.clone();
    let mut out = Vec::new();
    // Generated-artifact count = descriptors compiled into the image (same in every artifact, so
    // the weak symbol `br_plugin_gen_total` agrees). Closure size, not tree size: the build
    // filters generated files by closure and core's TC-PLUG-001 checks the `.br_plugins` count.
    let gen_total = match &tree.product {
        Some(prod) => {
            let sol = crate::solver::solve(&tree, &prod.stage, false);
            sol.closure.selected.len() as u32
        }
        None => tree.plugins.len() as u32,
    };
    for p in &tree.plugins {
        if let Some(name) = &only {
            if &p.name != name {
                continue;
            }
        }
        let mut facts = facts_of(p);
        facts.gen_total = gen_total;
        // Selftest off ⇒ BR_PLUGIN_NO_HOOK (test code not in the image); no product.toml ⇒ off.
        facts.emit_selftest = p.has_selftest
            && tree
                .product
                .as_ref()
                .map(|prod| prod.selftest_for(&p.name))
                .unwrap_or(false);
        let symbols: Vec<String> = p
            .exports
            .iter()
            .flat_map(|e| e.entries.iter().map(|x| x.name.clone()))
            .collect();
        facts.decl_include = scan_decl_include(root, &p.name, &symbols)
            .unwrap_or_else(|| FALLBACK_DECL_INCLUDE.to_string());
        out.push(Artifact {
            path: format!("build/gen/{}/plugin_desc.c", p.name),
            kind: "generated".into(),
            template: format!("descriptor/{}/{}/plugin_desc.c.tmpl", p.api_type, p.lang),
            vars: plugin_vars(&facts),
        });
    }
    if let Some(name) = &only {
        if !tree.plugins.iter().any(|p| &p.name == name) {
            d.usage(format!("no plugin `{name}` found in the plugin tree"), "`gen-plan`'s `plugin` argument is optional");
        }
    }
    (d, out)
}

/// Build [`PluginFacts`] from a model plugin. `gen_total` / `decl_include` are plan-level facts
/// (disk scans / tree counts) filled by the caller: real in `gen_plan`, skeletons predict.
pub fn facts_of(p: &Plugin) -> PluginFacts<'_> {
    let (ram, stack) = p.res_totals();
    PluginFacts {
        name: &p.name,
        plugin_type: &p.plugin_type,
        api_type: &p.api_type,
        subkind: p.subkind.as_deref(),
        lang: &p.lang,
        phase: &p.phase,
        sched_class: &p.sched_class,
        symbol_prefix: p.symbol_prefix.clone(),
        version: &p.version_raw,
        api_rev: p.compat.api_rev.unwrap_or(1),
        abi_id: p.compat.abi_id.clone().unwrap_or_default(),
        res_ram_kib: ram,
        res_stack_kib: stack,
        deps: p
            .deps
            .iter()
            .map(|d| DepFact {
                name: d.name.clone(),
                kind: d.kind.clone(),
                phase: d.phase.clone(),
            })
            .collect(),
        gen_total: 0,
        decl_include: String::new(),
        has_init: p.phase != "early",
        // Default no emission: only gen_plan knows the product switch; skeletons have no selftest.
        emit_selftest: false,
        // Carried over entry by entry from brick.toml's [[mount]].
        mounts: p
            .mounts
            .iter()
            .map(|m| MountFact {
                source: m.source.clone(),
                target: m.target.clone(),
                fstype: m.fstype.clone(),
                order: m.order,
            })
            .collect(),
    }
}

pub fn artifacts_data(artifacts: &[Artifact]) -> Value {
    let arr: Vec<Value> = artifacts.iter().map(|a| a.to_json()).collect();
    let planned: Vec<Value> = artifacts
        .iter()
        .map(|a| json!({"path": a.path, "kind": a.kind}))
        .collect();
    json!({"artifacts": arr, "planned": planned, "count": artifacts.len()})
}

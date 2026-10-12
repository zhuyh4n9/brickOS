//! Taxonomy, name contract, phase, privilege and dependency-direction rules (§3 / §7.2 / §7.3 / §8.3 / §8.4).
//!
//! This module is a **pure rule table** (no IO, no diagnostic-set state), reused by model.rs
//! and check.rs (agent-B). All "rules" live here; do not scatter decisions into L5.
//!
//! Encoding discipline: use only codes allocated by BRV-D8; for unallocated-code defects use
//! `BRV-MF-0001` per ruling R-9 and register the gap in `rust/README.md`.

use crate::diag::{Diag, Severity};

// ------------------------------------------------------------------ enum universe

pub const PLUGIN_TYPES: &[&str] = &["app", "interface", "ability", "platform"];
pub const API_TYPES: &[&str] = &["native", "runtime_adapter", "third_party"];
pub const SUBKINDS: &[&str] = &["scheduler", "framework", "io", "fs", "service"];
pub const NAMESPACES: &[&str] = &["app", "iface", "platform", "sched", "framework", "io", "fs", "service", "runtime"];
pub const PHASES: &[&str] = &["early", "core", "late", "app"];
pub const SCHED_CLASSES: &[&str] = &["SAFE_PREEMPT", "COOP_ONLY", "TT_SAFE"];
pub const SCHED_KINDS: &[&str] = &["coop", "preempt", "tt"];
pub const LANGS: &[&str] = &["c", "cxx", "rust"];
pub const PRIV_LEVELS: &[&str] = &["P0", "P1", "P2", "P3", "P4"];
pub const GRANULARITIES: &[&str] = &["byte", "page", "region", "pool"];
pub const MEM_OPS: &[&str] = &["alloc", "free", "map", "protect", "flush", "invalidate", "unmap"];
pub const REGIONS: &[&str] = &["heap", "dma", "contig", "page", "mmio", "reserved"];
pub const ENTRY_KINDS: &[&str] = &["func", "var", "macro", "type", "enum", "service", "symbol-family"];
pub const EXPORT_FORMS: &[&str] = &["api", "skin", "service"];
pub const DEP_KINDS: &[&str] = &["init", "runtime", "type"];
pub const FREEZE_STATES: &[&str] = &["unfrozen", "frozen", "unfreezing"];
pub const ENTRY_STATUSES: &[&str] = &["experimental", "frozen", "deprecated"];
pub const RES_KINDS: &[&str] = &["ram", "stack"];

/// Template dimensions delivered in v0.1 (criterion for `BRV-TAX-0014`, §8.4).
pub const TEMPLATE_API_TYPES: &[&str] = &["native"];
pub const TEMPLATE_LANGS: &[&str] = &["c"];

pub fn in_list(list: &[&str], v: &str) -> bool {
    list.contains(&v)
}

/// Render a list as `a | b | c` for diagnostic hints.
pub fn list_of(list: &[&str]) -> String {
    list.join(" | ")
}

// ------------------------------------------------------------------ name contract (§8.3)

fn is_alnum_lower(c: u8) -> bool {
    c.is_ascii_lowercase() || c.is_ascii_digit()
}

fn is_separator(c: u8) -> bool {
    c == b'-' || c == b'_' || c == b'/'
}

/// `^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$`
pub fn valid_plugin_name(name: &str) -> bool {
    let b = name.as_bytes();
    if b.is_empty() || !b[0].is_ascii_lowercase() {
        return false;
    }
    let mut i = 1;
    while i < b.len() && is_alnum_lower(b[i]) {
        i += 1;
    }
    while i < b.len() {
        if !is_separator(b[i]) {
            return false;
        }
        i += 1;
        let seg = i;
        while i < b.len() && is_alnum_lower(b[i]) {
            i += 1;
        }
        if i == seg {
            return false; // at least one [a-z0-9] required after a separator
        }
    }
    true
}

/// Name namespace (before the first `/`); no `/` ⇒ empty string (bare name).
pub fn name_namespace(name: &str) -> &str {
    match name.find('/') {
        Some(p) => &name[..p],
        None => "",
    }
}

/// Name short (after the last `/`).
pub fn name_short(name: &str) -> &str {
    match name.rfind('/') {
        Some(p) => &name[p + 1..],
        None => name,
    }
}

/// Whether the name uses the recommended `<namespace>/<short>` form (§8.3 ①).
pub fn is_recommended_form(name: &str) -> bool {
    let ns = name_namespace(name);
    !ns.is_empty() && in_list(NAMESPACES, ns)
}

/// namespace → `plugin_type` (§8.3 ①).
pub fn namespace_plugin_type(ns: &str) -> Option<&'static str> {
    match ns {
        "app" => Some("app"),
        "iface" => Some("interface"),
        "platform" => Some("platform"),
        "sched" | "framework" | "io" | "fs" | "service" => Some("ability"),
        // `runtime` (ADR-0015): POSIX runtime, a "whole-runtime" plugin class. plugin_type is still
        // `ability` (it is an ability plugin); it differs from `service` in **size and role**, not taxonomy.
        "runtime" => Some("ability"),
        _ => None,
    }
}

/// namespace → `subkind` (§8.3 ①; ability family only).
pub fn namespace_subkind(ns: &str) -> Option<&'static str> {
    match ns {
        "sched" => Some("scheduler"),
        "framework" => Some("framework"),
        "io" => Some("io"),
        "fs" => Some("fs"),
        "service" => Some("service"),
        // `runtime/posix` subkind is still `service`: phase derivation (ability + service ⇒ LATE)
        // and D18's "POSIX runtime **service**" positioning both rely on it. See ADR-0015 §2.2.
        "runtime" => Some("service"),
        _ => None,
    }
}

/// Derive `subkind` from the name namespace (§8.4). Bare name / non-ability namespace ⇒ `None`.
pub fn derive_subkind(name: &str) -> Option<&'static str> {
    let ns = name_namespace(name);
    if ns.is_empty() {
        return None;
    }
    namespace_subkind(ns)
}

// ------------------------------------------------------------------ phase (§7.2 + parent agent clarification)

/// Phase rank: `EARLY=0 < CORE=1 < LATE=2 < APP=3`.
pub fn phase_rank(phase: &str) -> Option<u8> {
    match phase {
        "early" => Some(0),
        "core" => Some(1),
        "late" => Some(2),
        "app" => Some(3),
        _ => None,
    }
}

/// "service-like" ⇒ completion point ② is LATE: `plugin_type = interface`
/// or `plugin_type = ability` with `subkind = service`.
pub fn is_service_like(plugin_type: &str, subkind: Option<&str>) -> bool {
    plugin_type == "interface" || (plugin_type == "ability" && subkind == Some("service"))
}

/// Derive `[plugin].phase` (completion point ②).
///
/// - No `init` hook ⇒ `early` (special case where ② coincides with ①).
/// - service-like (interface / ability.service) ⇒ `late`.
/// - `app` ⇒ `app`.
/// - Otherwise (platform / other ability subkinds) ⇒ `core`.
pub fn derive_phase(plugin_type: &str, subkind: Option<&str>, has_init: bool) -> &'static str {
    if !has_init {
        return "early";
    }
    if is_service_like(plugin_type, subkind) {
        "late"
    } else if plugin_type == "app" {
        "app"
    } else {
        "core"
    }
}

/// Whether `phase` is legal for the given `plugin_type`/`subkind`. `None` = legal.
///
/// Allowed sets (**parent agent 2026 clarification**):
/// - `interface` or `ability.service` ⇒ `{early, late}` (`core` ⇒ error).
/// - `platform` / `ability` (other subkinds) ⇒ `{early, core}`.
/// - `app` ⇒ `{early, core, app}` (M0's `app/hello` has no `init`, only a `start`
///   main loop, so `phase = "app"` must pass).
///
/// `early` is always legal = "this plugin has no `init` hook".
pub fn phase_reason(plugin_type: &str, subkind: Option<&str>, phase: &str) -> Option<String> {
    if !in_list(PHASES, phase) {
        return Some(format!(
            "unknown phase `{phase}`; legal values: {}(§7.2)",
            list_of(PHASES)
        ));
    }
    if phase == "early" {
        return None;
    }
    let allowed: &[&str] = if is_service_like(plugin_type, subkind) {
        &["late"]
    } else if plugin_type == "app" {
        &["core", "app"]
    } else {
        &["core"]
    };
    if allowed.contains(&phase) {
        return None;
    }
    Some(format!(
        "phase `{phase}` is incompatible with `plugin_type = {plugin_type}`{}{}; this combination only allows init completion point {}(`early` always works, for plugins without an `init` hook) (§7.2)",
        subkind.map(|s| format!(" / `subkind = {s}`")).unwrap_or_default(),
        "",
        list_of(allowed)
    ))
}

// ------------------------------------------------------------------ privilege (§3.4)

/// Level → rank (`P0`=0 … `P4`=4).
pub fn level_rank(level: &str) -> Option<u8> {
    PRIV_LEVELS
        .iter()
        .position(|l| *l == level)
        .map(|i| i as u8)
}

pub fn level_name(rank: u8) -> &'static str {
    PRIV_LEVELS[(rank as usize).min(4)]
}

/// Highest privilege level allowed for `plugin_type`/`subkind` (§3.2 + §3.4).
pub fn max_privilege_level(plugin_type: &str, subkind: Option<&str>) -> &'static str {
    match plugin_type {
        "app" | "interface" => "P0",
        "platform" => "P4",
        "ability" => {
            if subkind == Some("scheduler") {
                "P3"
            } else {
                "P2"
            }
        }
        _ => "P0",
    }
}

/// Minimum level required by a memory op (§3.4 orthogonal table).
pub fn min_level_for_memory_op(op: &str) -> Option<u8> {
    match op {
        "alloc" | "free" | "flush" | "invalidate" => Some(2), // P2
        "map" | "protect" | "unmap" => Some(4),               // P4(machine)
        _ => None,
    }
}

/// Whether an `(ops × granularity × region)` combination is legal (§3.4 orthogonal table).
///
/// Illegal ⇒ `BRV-PRIV-0002`; legal but required level exceeds declared level ⇒ `BRV-PRIV-0001`.
pub fn memory_combo_legal(op: &str, granularity: &str, region: &str) -> bool {
    if !in_list(GRANULARITIES, granularity) || !in_list(REGIONS, region) {
        return false;
    }
    match op {
        // alloc/free: pool lifetime (pool) only for the two dedicated pools contig/page;
        // other granularities allow heap (byte/page/region only)/contig/page/dma.
        "alloc" | "free" => {
            if granularity == "pool" {
                matches!(region, "contig" | "page")
            } else {
                matches!(region, "heap" | "contig" | "page" | "dma")
            }
        }
        // map/protect: MMU/page tables ⇒ P4; regions = mmio / reserved / heap (identity region attributes only).
        "map" | "protect" | "unmap" => {
            matches!(granularity, "page" | "region") && matches!(region, "mmio" | "reserved" | "heap")
        }
        // flush/invalidate: cache maintenance.
        "flush" | "invalidate" => {
            matches!(granularity, "byte" | "page" | "region")
                && matches!(region, "dma" | "heap" | "contig")
        }
        _ => false,
    }
}

/// One `[[privileged.memory]]` declaration (input shape decoupled from model).
#[derive(Clone, Debug, Default)]
pub struct MemoryEntry {
    pub granularity: String,
    pub ops: Vec<String>,
    pub regions: Vec<String>,
}

/// Input for privilege-declaration checking (decoupled from model).
#[derive(Clone, Debug, Default)]
pub struct PrivInput<'a> {
    pub plugin_type: &'a str,
    pub subkind: Option<&'a str>,
    pub target: &'a str,
    pub file: &'a str,
    /// `[privileged].level`; required when `[privileged]` is present.
    pub level: Option<&'a str>,
    pub memory: &'a [MemoryEntry],
    /// Whether `[privileged.resources]` has content (IRQ/DMA/pin/device name ⇒ P1+).
    pub has_resources: bool,
}

/// Privilege-declaration check (§3.4 / §10 V-19). Returns diagnostics (codes fixed to `BRV-PRIV-0001/0002`).
pub fn check_privileged(input: &PrivInput<'_>) -> Vec<Diag> {
    let mut out = Vec::new();
    if input.level.is_none() && input.memory.is_empty() && !input.has_resources {
        return out; // no privilege surface declared
    }

    let declared = match input.level {
        Some(l) => {
            if level_rank(l).is_none() {
                out.push(
                    Diag::new(Severity::Error, format!(
                        "unknown privilege level `{l}`; legal values: {}(§3.4)",
                        list_of(PRIV_LEVELS)
                    ))
                    .code("BRV-PRIV-0001")
                    .target(input.target)
                    .file(input.file)
                    .span("privileged.level")
                    .hint("P0 none / P1 resource / P2 memory / P3 core / P4 machine"),
                );
                return out;
            }
            level_rank(l).unwrap()
        }
        None => {
            out.push(
                Diag::new(Severity::Error, "privilege surface declared but `[privileged].level` is missing")
                    .code("BRV-PRIV-0001")
                    .target(input.target)
                    .file(input.file)
                    .span("privileged.level")
                    .hint(format!("legal values: {}", list_of(PRIV_LEVELS))),
            );
            return out;
        }
    };

    // ① The declared level must not exceed the maximum allowed by plugin_type.
    let cap_name = max_privilege_level(input.plugin_type, input.subkind);
    let cap = level_rank(cap_name).unwrap_or(0);
    if declared > cap {
        out.push(
            Diag::new(Severity::Error, format!(
                "`plugin_type = {}`{} allows at most privilege level {cap_name}, but {} was declared",
                input.plugin_type,
                input
                    .subkind
                    .map(|s| format!(" / `subkind = {s}`"))
                    .unwrap_or_default(),
                level_name(declared)
            ))
            .code("BRV-PRIV-0001")
            .target(input.target)
            .file(input.file)
            .span("privileged.level")
            .hint("§3.2: app/interface ≤ P0; ability ≤ P2(scheduler ≤ P3); platform ≤ P4"),
        );
    }

    // ② `[privileged.resources]` (IRQ/DMA/pin/device name) is P1.
    if input.has_resources && declared < 1 {
        out.push(
            Diag::new(
                Severity::Error,
                "`[privileged.resources]` (IRQ/DMA/pin/device name) declared but level is P0; exclusive resources are P1",
            )
            .code("BRV-PRIV-0001")
            .target(input.target)
            .file(input.file)
            .span("privileged.resources")
            .hint("§3.4: only P1 resource may declare exclusive resources"),
        );
    }

    // ③ Per memory entry: combination legality + minimum level.
    for (mi, m) in input.memory.iter().enumerate() {
        if m.ops.is_empty() {
            out.push(
                Diag::new(Severity::Error, "`[[privileged.memory]]` is missing `ops`")
                    .code("BRV-PRIV-0002")
                    .target(input.target)
                    .file(input.file)
                    .span(format!("privileged.memory[{mi}].ops"))
                    .hint(format!("legal op: {}", list_of(MEM_OPS))),
            );
        }
        if m.regions.is_empty() {
            out.push(
                Diag::new(Severity::Error, "`[[privileged.memory]]` is missing `regions`")
                    .code("BRV-PRIV-0002")
                    .target(input.target)
                    .file(input.file)
                    .span(format!("privileged.memory[{mi}].regions"))
                    .hint(format!("legal region: {}", list_of(REGIONS))),
            );
        }
        for (oi, op) in m.ops.iter().enumerate() {
            if !in_list(MEM_OPS, op) {
                out.push(
                    Diag::new(Severity::Error, format!(
                        "unknown memory op `{op}`; legal values: {}",
                        list_of(MEM_OPS)
                    ))
                    .code("BRV-PRIV-0002")
                    .target(input.target)
                    .file(input.file)
                    .span(format!("privileged.memory[{mi}].ops[{oi}]"))
                    .hint("§3.4 orthogonal table"),
                );
                continue;
            }
            for (ri, region) in m.regions.iter().enumerate() {
                if !memory_combo_legal(op, &m.granularity, region) {
                    out.push(
                        Diag::new(Severity::Error, format!(
                            "illegal (op × granularity × region) combination: `{op}` × `{}` × `{region}`",
                            m.granularity
                        ))
                        .code("BRV-PRIV-0002")
                        .target(input.target)
                        .file(input.file)
                        .span(format!("privileged.memory[{mi}].regions[{ri}]"))
                        .hint("§3.4 orthogonal table: alloc/free→P2 (pool only for contig/page); map/protect/unmap→P4 (page/region × mmio/reserved/heap); flush/invalidate→P2"),
                    );
                }
            }
            if let Some(need) = min_level_for_memory_op(op) {
                if declared < need {
                    out.push(
                        Diag::new(Severity::Error, format!(
                            "op `{op}` requires at least {}, but declared level is {}",
                            level_name(need),
                            level_name(declared)
                        ))
                        .code("BRV-PRIV-0001")
                        .target(input.target)
                        .file(input.file)
                        .span(format!("privileged.memory[{mi}]"))
                        .hint("§3.4: map/protect/unmap are P4 (machine); alloc/free/flush/invalidate are P2"),
                    );
                }
            }
        }
    }
    out
}

// ------------------------------------------------------------------ dependency direction (§7.3)

/// `plugin_type` direction table (§7.3): whether consumer → provider is permitted.
///
/// The `—` (platform → platform) entry is treated as "not applicable/not permitted": `platform`
/// is exactly one per image, so self-dependency is impossible under single-version-per-generation.
pub fn dep_direction_ok(consumer_pt: &str, provider_pt: &str, provider_is_third_party: bool) -> bool {
    match consumer_pt {
        // platform: all ✗ (platform is the lowest SoC level and depends on no plugin)
        "platform" => false,
        // ability: platform ✅ / ability ✅ (one-way) / third_party ✅ / interface ✗
        "ability" => {
            provider_is_third_party
                || matches!(provider_pt, "platform" | "ability")
        }
        // interface: platform/ability/interface ✅ (re-export only) / third_party ✗
        "interface" => {
            !provider_is_third_party && matches!(provider_pt, "platform" | "ability" | "interface")
        }
        // app: interface only ✅ (must not call native ability / platform directly);
        // **exception**: ability with `api_type = third_party` (e.g. `service/sqlite`) —
        // the app row in §7.3 says "must not call **native** directly", while §10 V-12② requires
        // `app → service/sqlite` to pass (see rust/README §10 S-13).
        "app" => provider_pt == "interface" || provider_is_third_party,
        _ => false,
    }
}

/// `api_type` prohibitions (§3.1; **only for providers with a surface** — `app`/`interface` are pure consumers).
///
/// Returns the rule explanation on violation; `None` when legal.
pub fn api_type_edge_reason(
    consumer_pt: &str,
    consumer_api: &str,
    provider_api: &str,
    kind: &str,
) -> Option<String> {
    // Pure consumers (app/interface) are exempt from api_type prohibitions (§3.1 narrowed scope).
    if !matches!(consumer_pt, "ability" | "platform") {
        return None;
    }
    if consumer_api == "native" && provider_api == "runtime_adapter" {
        return Some(
            "prohibition (2): `native` must not depend on the ability surface of `runtime_adapter` (§3.1/§7.3)".to_string(),
        );
    }
    if consumer_api == "native" && provider_api == "third_party" {
        return Some(
            "a `native` plugin must not depend on a plugin with `api_type = \"third_party\"` (§3.1 table / §7.3 clause 4)".to_string(),
        );
    }
    if consumer_api == "runtime_adapter" && provider_api == "third_party" && kind != "type" {
        return Some(format!(
            "`runtime_adapter` may only declare a compile-time type edge with `kind = \"type\"` to `third_party`, got `kind = \"{kind}\"` (§3.1 explicit exception)"
        ));
    }
    None
}

/// Full dependency-edge decision (direction table + api_type prohibitions + `allow_edges` whitelist).
///
/// `allow_edges` is the `(consumer, provider)` list from `product.toml [lint].allow_edges`;
/// it exempts **only** the `plugin_type` direction table (framework special case), **not** the hard `api_type` prohibitions.
///
/// `Some(message)` = violation. Callers report code `BRV-MF-0001` per ruling R-9
/// (BRV-D8 allocated no code for dependency-direction prohibitions; gap registered in README).
pub fn dep_edge_violation(
    consumer_name: &str,
    consumer_pt: &str,
    consumer_api: &str,
    provider_name: &str,
    provider_pt: &str,
    provider_api: &str,
    kind: &str,
    allow_edges: &[(String, String)],
) -> Option<String> {
    let provider_is_tp = provider_api == "third_party";
    let whitelisted = allow_edges
        .iter()
        .any(|(c, p)| c == consumer_name && p == provider_name);

    if !whitelisted && !dep_direction_ok(consumer_pt, provider_pt, provider_is_tp) {
        return Some(format!(
            "dependency-direction prohibition (§7.3): `{consumer_name}` with `plugin_type = {consumer_pt}` must not depend on `{provider_name}` with `plugin_type = {provider_pt}`{}",
            if provider_is_tp { "(`api_type = third_party`)" } else { "" }
        ));
    }
    api_type_edge_reason(consumer_pt, consumer_api, provider_api, kind)
}

// ------------------------------------------------------------------ export-surface classification invariants (§3.5)

/// Read-only view of one `[[export]]` (for [`check_export_invariants`]).
#[derive(Clone, Debug)]
pub struct ExportView<'a> {
    pub api_iface: &'a str,
    pub form: &'a str,
    pub reexport_of: &'a [String],
    pub symbols: &'a [String],
}

/// Check the shape of a `reexport_of` element: `<provider>#<unit>` (§6.1).
pub fn valid_unit_ref(s: &str) -> bool {
    match s.split_once('#') {
        Some((provider, unit)) => {
            !provider.is_empty() && !unit.is_empty() && !unit.contains('#') && valid_plugin_name(provider)
        }
        None => false,
    }
}

/// Export-surface classification invariants (§3.5 invariants 1–4). Returns `(export index, code, message)`.
///
/// Split of duties: invariant 3's "every re-exported unit has an `api_iface` **equal to its own**"
/// requires reading **other plugins**' export surfaces ⇒ a cross-file decision owned by agent-B
/// (`iface`/`check`); this function only checks non-empty + element shape + inverse invariant 4.
pub fn check_export_invariants(
    plugin_api_type: &str,
    exports: &[ExportView<'_>],
) -> Vec<(usize, &'static str, String)> {
    let mut out = Vec::new();
    for (i, e) in exports.iter().enumerate() {
        // Invariants 1 + 2: api_iface must equal plugin.api_type; third_party must not export a foreign surface.
        // The two **partition** the `third_party` case to avoid reporting one problem with two codes.
        if plugin_api_type == "third_party" {
            if e.api_iface != "third_party" {
                out.push((
                    i,
                    "BRV-TAX-0017",
                    format!(
                        "a plugin with `api_type = third_party` must not export an interface surface classified as `{}` (invariant 2)",
                        e.api_iface
                    ),
                ));
            }
        } else if e.api_iface != plugin_api_type {
            out.push((
                i,
                "BRV-TAX-0016",
                format!(
                    "`export.api_iface = {}` must equal `[plugin].api_type = {}` (invariant 1)",
                    e.api_iface, plugin_api_type
                ),
            ));
        }

        // Invariant 3: form = skin ⇒ reexport_of non-empty and each element well-shaped.
        if e.form == "skin" {
            if e.reexport_of.is_empty() {
                out.push((
                    i,
                    "BRV-TAX-0018",
                    "`form = \"skin\"` must declare a non-empty `reexport_of` (invariant 3)".to_string(),
                ));
            }
            for r in e.reexport_of {
                if !valid_unit_ref(r) {
                    out.push((
                        i,
                        "BRV-TAX-0018",
                        format!("`reexport_of` element `{r}` has a bad shape: expected `<provider>#<unit>`"),
                    ));
                }
            }
        } else {
            // Invariant 4: form != skin ⇒ must not declare reexport_of / symbols.
            if !e.reexport_of.is_empty() {
                out.push((
                    i,
                    "BRV-TAX-0019",
                    format!(
                        "`form = \"{}\"` declares `reexport_of` (only skin allows it; invariant 4)",
                        e.form
                    ),
                ));
            }
            if !e.symbols.is_empty() {
                out.push((
                    i,
                    "BRV-TAX-0019",
                    format!(
                        "`form = \"{}\"` declares `symbols` (only skin allows it; invariant 4)",
                        e.form
                    ),
                ));
            }
        }
    }
    out
}

// ------------------------------------------------------------------ template availability (§8.4)

/// Whether the v0.1 template is delivered. Not delivered ⇒ `BRV-TAX-0014` (severity=info, but command incomplete ⇒ exit code 2, G-1).
pub fn template_available(api_type: &str, lang: &str) -> bool {
    in_list(TEMPLATE_API_TYPES, api_type) && in_list(TEMPLATE_LANGS, lang)
}

/// Explanation for an undelivered template (for hints).
pub fn template_unavailable_reason(api_type: &str, lang: &str) -> String {
    format!(
        "v0.1 delivers only the `native` × `c` template; requested `{api_type}` × `{lang}` (unavailable = delivered in v0.x)"
    )
}

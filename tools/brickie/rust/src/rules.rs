//! Taxonomy, name contract, phase, privilege and dependency-direction rules. Pure rule table reused
//! by model.rs and check.rs; unallocated-code defects use `BRV-MF-0001` (ruling R-9).

use crate::diag::{Diag, Severity};

pub const PLUGIN_TYPES: &[&str] = &["app", "interface", "ability", "hal", "runtime", "core"];
pub const API_TYPES: &[&str] = &["native", "runtime_adapter", "third_party"];
pub const SUBKINDS: &[&str] = &["scheduler", "io", "fs", "service"];
pub const NAMESPACES: &[&str] = &["app", "iface", "runtime", "core", "hal", "provider", "sched", "driver", "fs"];
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

pub const TEMPLATE_API_TYPES: &[&str] = &["native"];
pub const TEMPLATE_LANGS: &[&str] = &["c"];

pub fn in_list(list: &[&str], v: &str) -> bool {
    list.contains(&v)
}

/// Render a list as `a | b | c` for diagnostic hints.
pub fn list_of(list: &[&str]) -> String {
    list.join(" | ")
}

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

pub fn name_short(name: &str) -> &str {
    match name.rfind('/') {
        Some(p) => &name[p + 1..],
        None => name,
    }
}

/// Whether the name uses the recommended `<namespace>/<short>` form.
pub fn is_recommended_form(name: &str) -> bool {
    let ns = name_namespace(name);
    !ns.is_empty() && in_list(NAMESPACES, ns)
}

pub fn namespace_plugin_type(ns: &str) -> Option<&'static str> {
    match ns {
        "app" => Some("app"),
        "iface" => Some("interface"),
        // `runtime/` is the target runtime environment (the SoC/board port). Renamed from
        // `platform/` by ADR-0040; the word "runtime" was freed when `runtime/posix` moved to
        // `provider/` (the namespace of concrete implementations).
        "runtime" => Some("runtime"),
        // `core` is a namespace of implementations (`core/c_core`, future `core/r_core`), not a
        // language marker: the type is the same and exactly one of them is in an image.
        "core" => Some("core"),
        // `hal/` is the infrastructure layer: a **peer of `ability`**, not a subkind of it. An
        // ability is a concrete implementation and selects the HAL bricks it runs on (ADR-0039).
        "hal" => Some("hal"),
        "provider" | "sched" | "driver" | "fs" => Some("ability"),
        _ => None,
    }
}

pub fn namespace_subkind(ns: &str) -> Option<&'static str> {
    match ns {
        "sched" => Some("scheduler"),
        // `driver/` = where this repo's **own** (native) device drivers live; the capability they
        // provide is still `subkind = "io"` -- namespace says where it lives, subkind what it is.
        // A ported driver keeps `subkind = "io"` too: it *is* an I/O driver; only its provenance
        // and its `api_type` (surface, ADR-0033) differ from a self-written one.
        "driver" => Some("io"),
        "fs" => Some("fs"),
        // `provider/` is the generic namespace of concrete implementations (ADR-0040: it absorbed
        // the old `provider/` and `runtime/`): service-shaped, so `ability + service ⇒ LATE` holds
        // for `provider/posix`, `provider/core`, `provider/trace` …
        "provider" => Some("service"),
        _ => None,
    }
}

/// Whether the name lives in the `driver/` namespace (own drivers: `api_type` must be `native`).
pub fn is_driver_namespace(name: &str) -> bool {
    name_namespace(name) == "driver"
}

/// Derive `subkind` from the name namespace. Bare name / non-ability namespace ⇒ `None`.
pub fn derive_subkind(name: &str) -> Option<&'static str> {
    let ns = name_namespace(name);
    if ns.is_empty() {
        return None;
    }
    namespace_subkind(ns)
}

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

/// "service-like" (interface, or ability with `subkind = service`) completes LATE.
pub fn is_service_like(plugin_type: &str, subkind: Option<&str>) -> bool {
    plugin_type == "interface" || (plugin_type == "ability" && subkind == Some("service"))
}

/// Derive `[plugin].phase` = the phase the `init` hook actually runs in, decided by the
/// **category** (never by the declaration): service-like ⇒ `late`, **app ⇒ `app`** (last of all
/// init, ADR-0037), everything else -- runtime, ability -- ⇒ `core`; no `init` ⇒ `early`.
/// Mirrors the runtime's `init_phase_of()` (`core/c_core/src/plugin/plugin_mgr.c`), which
/// `show brick-init-flow` prints.
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

/// Whether `phase` is legal for `plugin_type`/`subkind`; `None` = legal. The declaration is the
/// `init` **completion point**, which the category decides: interface or ability.service ⇒ {early,
/// late}; runtime / other ability subkinds ⇒ {early, core}; **app ⇒ {early, app}** (its init runs
/// last, in the APP phase -- ADR-0037). `early` is always legal (it means "no `init` hook").
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
    // `core` has no lifecycle hooks at all (it drives the phase machine instead of taking part in
    // it), so no init completion point other than "early" (= no `init` hook) can be asserted.
    if plugin_type == "core" {
        return Some(format!(
            "`plugin_type = \"core\"` defines no lifecycle: its `early_init`/`init`/`start` hooks are never emitted, so it has no init completion point and `phase = {phase}` cannot be asserted (only `early` = \"no init hook\" applies) (§7.2)"
        ));
    }
    let allowed: &[&str] = if is_service_like(plugin_type, subkind) {
        &["late"]
    } else if plugin_type == "app" {
        &["app"]
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

pub fn level_rank(level: &str) -> Option<u8> {
    PRIV_LEVELS
        .iter()
        .position(|l| *l == level)
        .map(|i| i as u8)
}

pub fn level_name(rank: u8) -> &'static str {
    PRIV_LEVELS[(rank as usize).min(4)]
}

/// Highest privilege level allowed for `plugin_type`/`subkind`.
pub fn max_privilege_level(plugin_type: &str, subkind: Option<&str>) -> &'static str {
    match plugin_type {
        "app" | "interface" => "P0",
        "runtime" => "P4",
        // core owns the boot chain: page tables + MMU activation are machine-level (§3.4 P4).
        "core" => "P4",
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

/// Minimum level required by a memory op.
pub fn min_level_for_memory_op(op: &str) -> Option<u8> {
    match op {
        "alloc" | "free" | "flush" | "invalidate" => Some(2), // P2
        "map" | "protect" | "unmap" => Some(4),               // P4(machine)
        _ => None,
    }
}

/// Whether an `(ops × granularity × region)` combination is legal (illegal ⇒ `BRV-PRIV-0002`;
/// legal but level too low ⇒ `BRV-PRIV-0001`).
pub fn memory_combo_legal(op: &str, granularity: &str, region: &str) -> bool {
    if !in_list(GRANULARITIES, granularity) || !in_list(REGIONS, region) {
        return false;
    }
    match op {
        // alloc/free: pool only for the contig/page pools, otherwise heap/contig/page/dma.
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
        "flush" | "invalidate" => {
            matches!(granularity, "byte" | "page" | "region")
                && matches!(region, "dma" | "heap" | "contig")
        }
        _ => false,
    }
}

#[derive(Clone, Debug, Default)]
pub struct MemoryEntry {
    pub granularity: String,
    pub ops: Vec<String>,
    pub regions: Vec<String>,
}

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

/// Privilege-declaration check; diagnostics use `BRV-PRIV-0001`/`BRV-PRIV-0002`.
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

/// `plugin_type` direction table: whether consumer → provider is permitted. The platform → platform
/// entry is not permitted (`platform` is exactly one per image, so self-dependency is impossible).
/// `core` is the base every other plugin is written against, so depending on it is always permitted.
/// `core` → `core` stays forbidden (exactly one core, same reason as platform). Core reaches the
/// platform through the platform *contract* (`br/runtime/**`) and registered ops, so a
/// `core → platform` edge is legal but neither side may turn it into an `init` edge: the two call
/// each other (platform's entry `bl br_core_main`, core's `br_plat_ticks_*`), and that cycle is
/// broken by the hard two-step boot chain, not by the topological sort.
pub fn dep_direction_ok(consumer_pt: &str, provider_pt: &str, provider_is_third_party: bool) -> bool {
    if provider_pt == "core" && consumer_pt != "core" {
        return true;
    }
    match consumer_pt {
        // runtime: core (above) + **hal** -- the HAL is below the target-runtime layer: the runtime
        // plugin implements the HAL contracts it declares (the mmu/pic ops tables) and calls HAL
        // APIs (`br_irq_enable`, `br_malloc`, `br_sched_on_tick`). Before ADR-0039 the HAL lived
        // inside `core`, where this row never had to name it.
        "runtime" => !provider_is_third_party && matches!(provider_pt, "hal"),
        // hal: below `ability` -- it may use core (base) and platform (implements its contracts)
        // and lower HAL bricks (`bdev` -> `dev`, `cdev` -> `dev`), never an ability/interface.
        "hal" => !provider_is_third_party && matches!(provider_pt, "runtime" | "hal"),
        // core: the runtime contract (`br/runtime/**`) + the HAL it boots (`br_mem_init`,
        // `br_mm_activate`, `br_sched_run`). Boot order is hard-coded in `br_core_main`, not
        // derived from init edges.
        "core" => !provider_is_third_party && matches!(provider_pt, "runtime" | "hal" | "ability"),
        // ability: a concrete implementation selects HAL (+ runtime, peer ability, third_party)
        "ability" => {
            provider_is_third_party
                || matches!(provider_pt, "runtime" | "hal" | "ability")
        }
        // interface: runtime/hal/ability/interface allowed (re-export only) / third_party forbidden
        "interface" => {
            !provider_is_third_party
                && matches!(provider_pt, "runtime" | "hal" | "ability" | "interface")
        }
        // app: interface only; exception: ability with `api_type = third_party`.
        "app" => provider_pt == "interface" || provider_is_third_party,
        _ => false,
    }
}

/// `api_type` prohibitions, for providers with a surface only (app/interface are pure consumers);
/// returns the reason on violation, `None` when legal.
pub fn api_type_edge_reason(
    consumer_pt: &str,
    consumer_api: &str,
    provider_api: &str,
    kind: &str,
) -> Option<String> {
    // Pure consumers (app/interface) are exempt from api_type prohibitions. `core` is a provider
    // with a surface, so a `native` core must not reach into `runtime_adapter`/`third_party`.
    if !matches!(consumer_pt, "ability" | "hal" | "runtime" | "core") {
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
/// `Some(message)` = violation, reported as `BRV-MF-0001` (ruling R-9). `allow_edges` (from
/// `product.toml [lint].allow_edges`) exempts the direction table only, not api_type rules.
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

/// Provenance prohibition (ADR-0033): `third_party/` is a copyleft-isolated vendor tree, so a plugin
/// that does **not** carry vendor code must not depend on one that does -- the vendor include
/// directories (reached through the plugin's `../../third_party/**` forwarding entries) would enter
/// a native plugin's compile closure. Replaces the former `native → third_party` prohibition, which
/// keyed provenance on the *surface* axis (`api_type`); this one keys on the declared build surface,
/// so it holds regardless of which namespace the adapter lives in.
pub fn vendor_edge_reason(
    consumer_carries_vendor: bool,
    provider_carries_vendor: bool,
) -> Option<String> {
    if provider_carries_vendor && !consumer_carries_vendor {
        return Some(
            "provenance prohibition: this plugin declares no vendor code (`third_party/`) in its `[build]`, so it must not depend on a plugin that does -- the vendor include directories would enter its compile closure (ADR-0033)".to_string(),
        );
    }
    None
}

/// Read-only view of one `[[export]]` (for [`check_export_invariants`]).
#[derive(Clone, Debug)]
pub struct ExportView<'a> {
    pub api_iface: &'a str,
    pub form: &'a str,
    pub reexport_of: &'a [String],
    pub symbols: &'a [String],
}

/// Check the shape of a `reexport_of` element: `<provider>#<unit>`.
pub fn valid_unit_ref(s: &str) -> bool {
    match s.split_once('#') {
        Some((provider, unit)) => {
            !provider.is_empty() && !unit.is_empty() && !unit.contains('#') && valid_plugin_name(provider)
        }
        None => false,
    }
}

/// Export-surface classification invariants 1–4. Returns `(export index, code, message)`.
/// Invariant 3's "re-exported unit's `api_iface` equals its own" needs other plugins' surfaces,
/// is owned by the cross-file caller; this function checks non-empty + element shape + invariant 4.
pub fn check_export_invariants(
    plugin_api_type: &str,
    exports: &[ExportView<'_>],
) -> Vec<(usize, &'static str, String)> {
    let mut out = Vec::new();
    for (i, e) in exports.iter().enumerate() {
        // Invariants 1 + 2 partition the `third_party` case to avoid two codes for one problem.
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

/// Whether the v0.1 template is delivered. Not delivered ⇒ `BRV-TAX-0014` (severity=info, but command incomplete ⇒ exit code 2, G-1).
pub fn template_available(api_type: &str, lang: &str) -> bool {
    in_list(TEMPLATE_API_TYPES, api_type) && in_list(TEMPLATE_LANGS, lang)
}

pub fn template_unavailable_reason(api_type: &str, lang: &str) -> String {
    format!(
        "v0.1 delivers only the `native` × `c` template; requested `{api_type}` × `{lang}` (unavailable = delivered in v0.x)"
    )
}

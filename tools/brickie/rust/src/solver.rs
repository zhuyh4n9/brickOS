//! L1 closure solving + the `dep` command family (`closure` / `dep-tree` / `dep-graph` /
//! `dep-why` / `dep-index`; `dep-graph --format json` puts a canonical JSON string in `data.text`).
//! `solve` is pure (no IO beyond `tree`): product selection → init/runtime/type edge closure →
//! `compat_gen` match → `range` intersection (single version) → topo + init cycle detection →
//! phase monotonicity → taxonomy prohibitions → budget totals → profile decision.
//! Diagnostics: `BRV-DEP-0009`, `BRV-DEP-0010`, `BRV-DEP-0011`, `BRV-VER-0001`, `BRV-VER-0002`,
//! `BRV-VER-0004`, `BRV-MF-0001` (gap substitute); messages must name the conflicting
//! sides / the delta / the full cycle path. `dep-why` not found ⇒ `data.found = false`, exit 0.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::PathBuf;

use serde_json::{json, Value};

use crate::diag::{Diag, Diags, Severity};
use crate::emit;
use crate::model::{self, Plugin, TreeLoad};
use crate::proto::{Request, Response};
use crate::rules;
use crate::version::{self, Range};

pub const SCOPE_DEPS: &str = "deps";
pub const SCOPE_IFACE: &str = "iface";
pub const SCOPE_TAX: &str = "tax";
pub const SCOPE_PRIV: &str = "priv";

/// One declared structural dependency edge (consumer → provider).
#[derive(Clone, Debug, Default)]
pub struct Edge {
    pub from: String,
    pub to: String,
    /// `init` | `runtime` | `type`.
    pub kind: String,
    pub range: String,
    /// `[[dep]].phase` assertion (init edges only).
    pub phase: Option<String>,
    pub symbol: Option<String>,
    /// `[[dep]]` index within the plugin declaration (diagnostic spans).
    pub index: usize,
}

#[derive(Clone, Debug, Default)]
pub struct Cycle {
    pub path: Vec<String>,
    pub edges: Vec<Edge>,
}

#[derive(Clone, Debug, Default)]
pub struct Closure {
    pub selected: BTreeSet<String>,
    /// Selection set (roots of the product manifest; no product ⇒ whole tree).
    pub seeds: Vec<String>,
    pub init_edges: Vec<Edge>,
    pub runtime_edges: Vec<Edge>,
    pub type_edges: Vec<Edge>,
    pub topo_order: Vec<String>,
    /// Init cycles (structured; V-3).
    pub cycles: Vec<Cycle>,
    pub totals_ram: i64,
    pub totals_stack: i64,
    pub capacity_ram: i64,
    pub capacity_stack: i64,
    /// Tree has `plugin_type = "platform"` ⇒ capacity is meaningful.
    pub has_platform: bool,
}

impl Closure {
    pub fn all_edges(&self) -> Vec<&Edge> {
        self.init_edges
            .iter()
            .chain(self.runtime_edges.iter())
            .chain(self.type_edges.iter())
            .collect()
    }
}

/// Result of `solve`: closure + **domain-tagged** diagnostics.
pub struct SolveResult {
    pub closure: Closure,
    pub diags: Vec<(&'static str, Diag)>,
}

impl SolveResult {
    pub fn into_diags(self) -> Diags {
        let mut d = Diags::new();
        for (_, diag) in self.diags {
            d.push(diag);
        }
        d
    }
}

fn err(
    code: &str,
    target: impl Into<String>,
    file: impl Into<String>,
    span: impl Into<String>,
    message: impl Into<String>,
    hint: impl Into<String>,
) -> Diag {
    Diag::new(Severity::Error, message)
        .code(code)
        .target(target)
        .file(file)
        .span(span)
        .hint(hint)
}

fn info(
    code: &str,
    target: impl Into<String>,
    file: impl Into<String>,
    span: impl Into<String>,
    message: impl Into<String>,
    hint: impl Into<String>,
) -> Diag {
    Diag::new(Severity::Info, message)
        .code(code)
        .target(target)
        .file(file)
        .span(span)
        .hint(hint)
}

/// Profile-dependent severity (`BRV-VER-0004`: dev ⇒ info / release ⇒ error).
fn sev_for_profile(profile: &str) -> Severity {
    if profile == "release" {
        Severity::Error
    } else {
        Severity::Info
    }
}

const REACH_TYPE_ONLY: u8 = 1;
const REACH_FULL: u8 = 2;

pub fn solve(tree: &TreeLoad, profile: &str, scan_iface: bool) -> SolveResult {
    let by_name: BTreeMap<&str, &Plugin> =
        tree.plugins.iter().map(|p| (p.name.as_str(), p)).collect();
    let mut diags: Vec<(&'static str, Diag)> = Vec::new();
    let mut closure = Closure::default();

    let mut seeds: Vec<String> = Vec::new();
    if let Some(prod) = &tree.product {
        if !prod.app.is_empty() {
            seeds.push(prod.app.clone());
        }
        for s in &prod.select {
            if !seeds.contains(s) {
                seeds.push(s.clone());
            }
        }
        if seeds.is_empty() {
            for p in &tree.plugins {
                seeds.push(p.name.clone());
            }
        }
    } else {
        for p in &tree.plugins {
            seeds.push(p.name.clone());
        }
    }
    seeds.sort();
    seeds.dedup();
    closure.seeds = seeds.clone();

    for s in &seeds {
        if !by_name.contains_key(s.as_str()) {
            let file = tree
                .product
                .as_ref()
                .map(|p| p.file.clone())
                .unwrap_or_else(|| "product.toml".to_string());
            diags.push((
                SCOPE_DEPS,
                err(
                    "BRV-MF-0001",
                    s,
                    &file,
                    "select.plugins",
                    format!("plugin `{s}` in the selection set is not in the plugin tree (missing `{s}/brick.toml`)"),
                    "gap substitute (R-9): selection/dependency reference integrity has no dedicated code; add the plugin or drop it from the selection set",
                ),
            ));
        }
    }

    // Edge closure; type edges do not pull in runtime deps.
    let mut reach: BTreeMap<String, u8> = BTreeMap::new();
    let mut queue: VecDeque<String> = VecDeque::new();
    for s in &seeds {
        if !by_name.contains_key(s.as_str()) {
            continue;
        }
        let slot = reach.entry(s.clone()).or_insert(0);
        if *slot < REACH_FULL {
            *slot = REACH_FULL;
            queue.push_back(s.clone());
        }
    }
    while let Some(name) = queue.pop_front() {
        let mode = *reach.get(&name).unwrap_or(&REACH_FULL);
        let Some(p) = by_name.get(name.as_str()) else {
            continue;
        };
        for d in &p.deps {
            if mode == REACH_TYPE_ONLY && d.kind != "type" {
                continue;
            }
            if !by_name.contains_key(d.name.as_str()) {
                continue;
            }
            let child = if d.kind == "type" {
                REACH_TYPE_ONLY
            } else {
                mode
            };
            let slot = reach.entry(d.name.clone()).or_insert(0);
            if *slot < child {
                *slot = child;
                queue.push_back(d.name.clone());
            }
        }
    }
    closure.selected = reach.keys().cloned().collect();

    let mut all_edges: Vec<Edge> = Vec::new();
    for name in &closure.selected {
        let p = by_name[name.as_str()];
        for (i, d) in p.deps.iter().enumerate() {
            if !closure.selected.contains(&d.name) {
                if !by_name.contains_key(d.name.as_str()) {
                    diags.push((
                        SCOPE_DEPS,
                        err(
                            "BRV-MF-0001",
                            &p.name,
                            &p.file,
                            format!("dep[{i}].name"),
                            format!(
                                "`{}` depends on `{}` ({}) but it is not in the plugin tree (missing `{}/brick.toml`)",
                                p.name, d.name, d.kind, d.name
                            ),
                            "§7.6: missing deps report 'dependency name + which edge pulled it in'; gap substitute (R-9)",
                        ),
                    ));
                }
                continue;
            }
            let e = Edge {
                from: p.name.clone(),
                to: d.name.clone(),
                kind: d.kind.clone(),
                range: d.range.canonical(),
                phase: d.phase.clone(),
                symbol: d.symbol.clone(),
                index: i,
            };
            match d.kind.as_str() {
                "init" => closure.init_edges.push(e.clone()),
                "runtime" => closure.runtime_edges.push(e.clone()),
                "type" => closure.type_edges.push(e.clone()),
                _ => {}
            }
            all_edges.push(e);
        }
    }

    // `range` out of bounds (BRV-VER-0002) + single-version intersection (BRV-DEP-0011).
    let mut constraints: BTreeMap<String, (Range, Vec<String>)> = BTreeMap::new();
    for e in &all_edges {
        let prov = by_name[e.to.as_str()];
        let r = Range::parse(&e.range).unwrap_or_else(|_| Range::any());
        if !r.matches(prov.version.triple()) {
            diags.push((
                SCOPE_DEPS,
                err(
                    "BRV-VER-0002",
                    &e.from,
                    &by_name[e.from.as_str()].file,
                    format!("dep[{}].range", e.index),
                    format!(
                        "range out of bounds within the same generation: `{}` requires `{}` to satisfy `{}`, but the provider's current version is {}",
                        e.from, e.to, e.range, prov.version
                    ),
                    "§7.4: range compares only MAJOR.MINOR.REVISE (missing segments right-padded with 0); fix the range or advance the provider",
                ),
            ));
        }
        let slot = constraints
            .entry(e.to.clone())
            .or_insert_with(|| (Range::any(), Vec::new()));
        slot.0 = slot.0.intersect(&r);
        slot.1.push(e.from.clone());
    }

    // `requires_iface` checks run only when `scan_iface` is set.
    if scan_iface {
        for name in &closure.selected {
            let p = by_name[name.as_str()];
            for (ri, r) in p.compat.requires_iface.iter().enumerate() {
                let span = format!("compat.requires_iface[{ri}]");
                let Some((prov_name, unit)) = r.id.split_once('#') else {
                    continue; // shape errors are already reported at load time
                };
                let Some(prov) = by_name.get(prov_name) else {
                    diags.push((
                        SCOPE_IFACE,
                        err(
                            "BRV-MF-0001",
                            &p.name,
                            &p.file,
                            format!("{span}.id"),
                            format!(
                                "the provider `{prov_name}` referenced by `requires_iface` is not in the plugin tree (unit `{}`)",
                                r.id
                            ),
                            "gap substitute (R-9): interface reference integrity has no dedicated code",
                        ),
                    ));
                    continue;
                };
                let Some(exp) = prov.find_export(unit) else {
                    diags.push((
                        SCOPE_IFACE,
                        err(
                            "BRV-MF-0001",
                            &p.name,
                            &p.file,
                            format!("{span}.id"),
                            format!("the interface unit `{}` referenced by `requires_iface` does not exist", r.id),
                            "gap substitute (R-9): interface reference integrity has no dedicated code",
                        ),
                    ));
                    continue;
                };
                let prov_cg = exp.compat_gen.unwrap_or(0);
                let prov_ver = exp.version.unwrap_or_default();
                if !version::compat_gen_matches(r.compat_gen, prov_cg) {
                    diags.push((
                        SCOPE_DEPS,
                        err(
                            "BRV-VER-0001",
                            &p.name,
                            &p.file,
                            format!("{span}.compat_gen"),
                            format!(
                                "compat_gen mismatch: `{}` pins `{}`'s compat_gen={}, but the provider is currently at {}",
                                p.name, r.id, r.compat_gen, prov_cg
                            ),
                            "§5.5/§7.4: compat_gen matches exactly; cross-generation comparison is meaningless",
                        ),
                    ));
                }
                if !r.range.matches(prov_ver.triple()) {
                    diags.push((
                        SCOPE_DEPS,
                        err(
                            "BRV-VER-0002",
                            &p.name,
                            &p.file,
                            format!("{span}.range"),
                            format!(
                                "range out of bounds within the same generation: `{}` requires `{}` to satisfy `{}`, but the provider version is {}",
                                p.name, r.id, r.range_raw, prov_ver
                            ),
                            "§7.4: range compares only MAJOR.MINOR.REVISE",
                        ),
                    ));
                }
                let slot = constraints
                    .entry(prov.name.clone())
                    .or_insert_with(|| (Range::any(), Vec::new()));
                slot.0 = slot.0.intersect(&r.range);
                slot.1.push(p.name.clone());
                if prov_cg == 0 {
                    diags.push((
                        SCOPE_DEPS,
                        Diag::new(
                            sev_for_profile(profile),
                            format!(
                                "interface consumption `{}`: unit `{}` is not yet frozen (compat_gen = 0), unprotected (profile={profile})",
                                p.name, r.id
                            ),
                        )
                        .code("BRV-VER-0004")
                        .target(&p.name)
                        .file(&p.file)
                        .span(span.clone())
                        .hint("§7.5: dev allows (info) / release forbids (error)"),
                    ));
                }
            }
        }
    }

    for (prov, (r, consumers)) in &constraints {
        if !r.is_satisfiable() {
            diags.push((
                SCOPE_DEPS,
                err(
                    "BRV-DEP-0011",
                    prov,
                    by_name
                        .get(prov.as_str())
                        .map(|p| p.file.clone())
                        .unwrap_or_default(),
                    "dep[].range",
                    format!(
                        "empty range intersection under the single-version policy: plugin `{prov}` is required with disjoint ranges by {} (`{}`)",
                        consumers.join(", "),
                        r.canonical()
                    ),
                    "§7.4: a plugin name may have only one version in the closure ⇒ BRV-DEP-0011",
                ),
            ));
        }
    }

    // Profile gating of skin re-export edges (V-14); ignores `requires_iface`.
    for name in &closure.selected {
        let p = by_name[name.as_str()];
        for (ei, exp) in p.exports.iter().enumerate() {
            if exp.form != "skin" {
                continue;
            }
            for r in &exp.reexport_of {
                let Some((pn, un)) = r.split_once('#') else {
                    continue;
                };
                let Some(prov) = by_name.get(pn) else { continue };
                let Some(pe) = prov.find_export(un) else {
                    continue;
                };
                if pe.compat_gen.unwrap_or(0) == 0 {
                    diags.push((
                        SCOPE_DEPS,
                        Diag::new(
                            sev_for_profile(profile),
                            format!(
                                "skin `{}` (export[{ei}]) re-exports unit `{r}`, which is not yet frozen (compat_gen = 0) (profile={profile})",
                                p.name
                            ),
                        )
                        .code("BRV-VER-0004")
                        .target(&p.name)
                        .file(&p.file)
                        .span(format!("export[{ei}].reexport_of"))
                        .hint("§7.5: interface consumption must not use unfrozen interfaces under release"),
                    ));
                }
            }
        }
    }

    // Phase monotonicity (R1/R2), declared init edges only.
    for e in &closure.init_edges {
        let a = by_name[e.from.as_str()];
        let b = by_name[e.to.as_str()];
        let (ra, rb) = (rules::phase_rank(&a.phase), rules::phase_rank(&b.phase));
        if let (Some(ra), Some(rb)) = (ra, rb) {
            if rb > ra {
                diags.push((
                    SCOPE_DEPS,
                    err(
                        "BRV-DEP-0009",
                        &a.name,
                        &a.file,
                        format!("dep[{}].phase", e.index),
                        format!(
                            "phase monotonicity violation (R1): `{}` completes in `{}` (rank {}), later than consumer `{}`'s `{}` (rank {}), but `{}` depends on `{}`",
                            b.name, b.phase, rb, a.name, a.phase, ra, a.name, b.name
                        ),
                        "§7.2: rank(complete(B)) ≤ rank(complete(A)); covers declared init edges only",
                    ),
                ));
            }
        }
        if let Some(assert) = &e.phase {
            let rank_assert = rules::phase_rank(assert);
            if let (Some(rank_assert), Some(rb)) = (rank_assert, rb) {
                // Provider self-declares later than the consumer's assertion ⇒ red.
                if rb > rank_assert {
                    diags.push((
                        SCOPE_DEPS,
                        err(
                            "BRV-DEP-0010",
                            &a.name,
                            &a.file,
                            format!("dep[{}].phase", e.index),
                            format!(
                                "`[[dep]].phase` assertion conflicts with the provider's self-declaration: `{}` asserts `{}` completes in `{assert}`, but the provider self-declares `{}`",
                                a.name, b.name, b.phase
                            ),
                            "§7.2 R2 / §8.1: rank(complete(B)) ≤ rank(assertion)",
                        ),
                    ));
                }
            }
        }
    }

    // Taxonomy prohibitions (dependency direction ⇒ BRV-MF-0001).
    let allow = tree.allow_edges();
    for e in &all_edges {
        let a = by_name[e.from.as_str()];
        let b = by_name[e.to.as_str()];
        let skin_exempt = a.exports.iter().any(|x| {
            x.form == "skin"
                && x.reexport_of
                    .iter()
                    .any(|r| r.starts_with(&format!("{}#", b.name)))
        });
        let violation = if skin_exempt {
            // `form = skin` re-export is exempt from the api_type prohibition (direction table).
            let prov_is_tp = b.api_type == "third_party";
            let whitelisted = allow.iter().any(|(c, p)| c == &a.name && p == &b.name);
            if !whitelisted && !rules::dep_direction_ok(&a.plugin_type, &b.plugin_type, prov_is_tp) {
                Some(format!(
                    "dependency-direction prohibition (§7.3): `{}` with `plugin_type = {}` must not depend on `{}` with `plugin_type = {}`",
                    a.plugin_type, a.name, b.plugin_type, b.name
                ))
            } else {
                None
            }
        } else {
            rules::dep_edge_violation(
                &a.name,
                &a.plugin_type,
                &a.api_type,
                &b.name,
                &b.plugin_type,
                &b.api_type,
                &e.kind,
                &allow,
            )
        };
        if let Some(msg) = violation {
            diags.push((
                SCOPE_TAX,
                err(
                    "BRV-MF-0001",
                    &a.name,
                    &a.file,
                    format!("dep[{}]", e.index),
                    format!("consumer `{}` → provider `{}`: {msg}", a.name, b.name),
                    "gap substitute (R-9): dependency-direction prohibitions have no dedicated code; allow_edges exempts only the direction table",
                ),
            ));
        }
    }

    // Budget totals: platform `[[res]]` is capacity, not counted into the consumer-side Σ.
    let mut used_ram = 0i64;
    let mut used_stack = 0i64;
    for name in &closure.selected {
        let p = by_name[name.as_str()];
        if p.plugin_type == "platform" {
            continue;
        }
        let (r, s) = p.res_totals();
        used_ram += r;
        used_stack += s;
    }
    closure.totals_ram = used_ram;
    closure.totals_stack = used_stack;
    let (cap_ram, cap_stack) = tree.platform_capacity();
    closure.capacity_ram = cap_ram;
    closure.capacity_stack = cap_stack;
    closure.has_platform = tree.plugins.iter().any(|p| p.plugin_type == "platform");

    // Exactly one `core` per image, and a product must select one: core owns the boot chain and
    // drives every other plugin, so an image without it cannot start and an image with two cannot
    // decide which one does. Product-level count (like app/platform), so per ruling R-1 it is only
    // enforced when a `product.toml` exists — a bare plugin tree is validated on its own.
    if let Some(prod) = &tree.product {
        let cores: Vec<String> = closure
            .selected
            .iter()
            .filter(|n| {
                by_name
                    .get(n.as_str())
                    .map(|p| p.plugin_type == "core")
                    .unwrap_or(false)
            })
            .cloned()
            .collect();
        if cores.len() != 1 {
            diags.push((
                SCOPE_DEPS,
                err(
                    "BRV-TAX-0020",
                    "product",
                    &prod.file,
                    "select.plugins",
                    format!(
                        "an image must contain exactly one `plugin_type = \"core\"` plugin, found {}: {}",
                        cores.len(),
                        if cores.is_empty() {
                            "none selected".to_string()
                        } else {
                            cores.join(", ")
                        }
                    ),
                    "§8.3 ④ / §7.3: `core` is mandatory and singular (it is the plugin that drives the other plugins); add it to `[select].plugins` (e.g. `core/c_core`) or drop the extra one",
                ),
            ));
        }
    }

    let platform_file = tree
        .plugins
        .iter()
        .find(|p| p.plugin_type == "platform")
        .map(|p| p.file.clone())
        .unwrap_or_else(|| "product.toml".to_string());

    if closure.has_platform {
        if used_ram > cap_ram {
            diags.push((
                SCOPE_PRIV,
                err(
                    "BRV-MF-0001",
                    "product",
                    &platform_file,
                    "res",
                    format!(
                        "Σ ram_kib = {used_ram} exceeds the platform capacity {cap_ram}, delta {} KiB (physical limit, no exemption)",
                        used_ram - cap_ram
                    ),
                    "§8.1: Σ [[res]] ≤ what platform provides; gap substitute (R-9)",
                ),
            ));
        }
        if used_stack > cap_stack {
            diags.push((
                SCOPE_PRIV,
                err(
                    "BRV-MF-0001",
                    "product",
                    &platform_file,
                    "res",
                    format!(
                        "Σ stack_kib = {used_stack} exceeds the platform capacity {cap_stack}, delta {} KiB",
                        used_stack - cap_stack
                    ),
                    "§8.1: Σ [[res]] ≤ what platform provides; gap substitute (R-9)",
                ),
            ));
        }
    }
    if let Some(prod) = &tree.product {
        if let Some(b) = prod.budget_ram_kib {
            if used_ram > b {
                diags.push((
                    SCOPE_PRIV,
                    err(
                        "BRV-MF-0001",
                        "product",
                        &prod.file,
                        "budget.ram_kib",
                        format!(
                            "Σ ram_kib = {used_ram} exceeds the product budget {b}, delta {} KiB",
                            used_ram - b
                        ),
                        "§8.1: Σ [[res]] ≤ [budget]; [budget] may be relaxed reviewably; gap substitute (R-9)",
                    ),
                ));
            }
        }
        if let Some(b) = prod.budget_stack_kib {
            if used_stack > b {
                diags.push((
                    SCOPE_PRIV,
                    err(
                        "BRV-MF-0001",
                        "product",
                        &prod.file,
                        "budget.stack_kib",
                        format!(
                            "Σ stack_kib = {used_stack} exceeds the product budget {b}, delta {} KiB",
                            used_stack - b
                        ),
                        "§8.1: Σ [[res]] ≤ [budget]; gap substitute (R-9)",
                    ),
                ));
            }
        }
    }

    // Cycle detection: init ⇒ hard error with full path; runtime/type ⇒ info only.
    closure.topo_order = topo_order(&closure.selected, &closure.init_edges);
    closure.cycles = find_cycles(&closure.selected, &closure.init_edges);
    for cyc in &closure.cycles {
        let head = cyc.path.first().cloned().unwrap_or_default();
        let file = by_name
            .get(head.as_str())
            .map(|p| p.file.clone())
            .unwrap_or_default();
        diags.push((
            SCOPE_DEPS,
            err(
                "BRV-MF-0001",
                &head,
                &file,
                "dep",
                format!("init dependency cycle: {}", cyc.path.join(" → ")),
                "§7.1: init cycles are forbidden (topological hard error); gap substitute (R-9), V-3 requires the full path",
            ),
        ));
    }
    for (kind, edges) in [
        ("runtime", &closure.runtime_edges),
        ("type", &closure.type_edges),
    ] {
        for cyc in find_cycles(&closure.selected, edges) {
            let head = cyc.path.first().cloned().unwrap_or_default();
            let file = by_name
                .get(head.as_str())
                .map(|p| p.file.clone())
                .unwrap_or_default();
            diags.push((
                SCOPE_DEPS,
                info(
                    "BRV-MF-0001",
                    &head,
                    &file,
                    "dep",
                    format!(
                        "`{kind}` dependency cycle (allowed, recorded only): {}",
                        cyc.path.join(" → ")
                    ),
                    "§7.1: runtime/type cycles report info only",
                ),
            ));
        }
    }

    SolveResult { closure, diags }
}

fn adjacency(nodes: &BTreeSet<String>, edges: &[Edge]) -> BTreeMap<String, Vec<String>> {
    let mut adj: BTreeMap<String, Vec<String>> =
        nodes.iter().map(|n| (n.clone(), Vec::new())).collect();
    for e in edges {
        adj.entry(e.from.clone()).or_default().push(e.to.clone());
    }
    for v in adj.values_mut() {
        v.sort();
        v.dedup();
    }
    adj
}

/// All elementary cycles; dedup by rotated minimal lexicographic form.
pub fn find_cycles(nodes: &BTreeSet<String>, edges: &[Edge]) -> Vec<Cycle> {
    let adj = adjacency(nodes, edges);
    let mut out: Vec<Cycle> = Vec::new();
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut budget: usize = 50_000;
    for start in nodes {
        let mut stack: Vec<String> = Vec::new();
        dfs_cycles(start, &adj, edges, &mut stack, &mut seen, &mut out, &mut budget);
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    out
}

#[allow(clippy::too_many_arguments)]
fn dfs_cycles(
    node: &str,
    adj: &BTreeMap<String, Vec<String>>,
    edges: &[Edge],
    stack: &mut Vec<String>,
    seen: &mut BTreeSet<String>,
    out: &mut Vec<Cycle>,
    budget: &mut usize,
) {
    if *budget == 0 {
        return;
    }
    *budget -= 1;
    stack.push(node.to_string());
    if let Some(children) = adj.get(node) {
        for c in children {
            if let Some(pos) = stack.iter().position(|x| x == c) {
                let mut ring: Vec<String> = stack[pos..].to_vec();
                let minpos = ring
                    .iter()
                    .enumerate()
                    .min_by_key(|(_, v)| v.as_str())
                    .map(|(i, _)| i)
                    .unwrap_or(0);
                ring.rotate_left(minpos);
                let key = ring.join("\u{1}");
                if seen.insert(key) {
                    let mut path = ring.clone();
                    path.push(ring[0].clone());
                    let mut cyc_edges = Vec::new();
                    for i in 0..path.len().saturating_sub(1) {
                        if let Some(e) = edges
                            .iter()
                            .find(|e| e.from == path[i] && e.to == path[i + 1])
                        {
                            cyc_edges.push(e.clone());
                        }
                    }
                    out.push(Cycle {
                        path,
                        edges: cyc_edges,
                    });
                }
            } else {
                dfs_cycles(c, adj, edges, stack, seen, out, budget);
            }
        }
    }
    stack.pop();
}

/// Init topo order: provider before consumer (Kahn; lexicographic ready set ⇒ deterministic).
/// Edges point consumer → provider, so Kahn uses the reverse adjacency; cycle nodes come last.
pub fn topo_order(nodes: &BTreeSet<String>, edges: &[Edge]) -> Vec<String> {
    let mut pairs: BTreeSet<(String, String)> = BTreeSet::new();
    for e in edges {
        if nodes.contains(&e.from) && nodes.contains(&e.to) {
            pairs.insert((e.from.clone(), e.to.clone()));
        }
    }
    let mut indeg: BTreeMap<String, usize> = nodes.iter().map(|n| (n.clone(), 0)).collect();
    let mut adj: BTreeMap<String, Vec<String>> =
        nodes.iter().map(|n| (n.clone(), Vec::new())).collect();
    for (from, to) in &pairs {
        // Init-order edges: provider (`to`) → consumer (`from`).
        adj.get_mut(to).unwrap().push(from.clone());
        *indeg.get_mut(from).unwrap() += 1;
    }
    for v in adj.values_mut() {
        v.sort();
    }
    let mut ready: Vec<String> = indeg
        .iter()
        .filter(|(_, d)| **d == 0)
        .map(|(n, _)| n.clone())
        .collect();
    ready.sort();
    let mut order: Vec<String> = Vec::new();
    while !ready.is_empty() {
        let n = ready.remove(0);
        order.push(n.clone());
        for m in adj.get(&n).cloned().unwrap_or_default() {
            if let Some(d) = indeg.get_mut(&m) {
                *d = d.saturating_sub(1);
                if *d == 0 {
                    ready.push(m);
                    ready.sort();
                }
            }
        }
    }
    if order.len() < nodes.len() {
        let done: BTreeSet<String> = order.iter().cloned().collect();
        for n in nodes {
            if !done.contains(n) {
                order.push(n.clone());
            }
        }
    }
    order
}

fn edges_json(edges: &[Edge]) -> Value {
    Value::Array(
        edges
            .iter()
            .map(|e| json!({"from": e.from, "to": e.to}))
            .collect(),
    )
}

pub fn closure_json(tree: &TreeLoad, c: &Closure, profile: &str) -> Value {
    let plugins: Vec<Value> = tree
        .plugins
        .iter()
        .map(|p| {
            let (r, s) = p.res_totals();
            json!({
                "name": p.name,
                "version": p.version.to_string(),
                "compat_gen": p.version.compat_gen,
                "api_type": p.api_type,
                "plugin_type": p.plugin_type,
                "phase": p.phase,
                "ram_kib": r,
                "stack_kib": s,
                "selected": c.selected.contains(&p.name),
            })
        })
        .collect();
    let cycles: Vec<Value> = c
        .cycles
        .iter()
        .map(|cy| {
            json!({
                "path": cy.path,
                "edges": cy.edges.iter().map(|e| json!({"from": e.from, "to": e.to})).collect::<Vec<_>>(),
            })
        })
        .collect();
    json!({
        "plugins": plugins,
        "init_edges": edges_json(&c.init_edges),
        "runtime_edges": edges_json(&c.runtime_edges),
        "type_edges": edges_json(&c.type_edges),
        "topo_order": c.topo_order,
        "totals": {
            "ram_kib": c.totals_ram,
            "stack_kib": c.totals_stack,
            "platform_capacity": {"ram_kib": c.capacity_ram, "stack_kib": c.capacity_stack},
            "budget": {
                "ram_kib": tree.product.as_ref().and_then(|p| p.budget_ram_kib),
                "stack_kib": tree.product.as_ref().and_then(|p| p.budget_stack_kib),
            },
        },
        "profile": profile,
        "cycles": cycles,
        "selected": c.selected.iter().cloned().collect::<Vec<_>>(),
        "seeds": c.seeds,
        "has_platform": c.has_platform,
    })
}

/// Read `args.profile`; default = product.stage, then dev.
pub fn profile_of(req: &Request, tree: &TreeLoad) -> String {
    req.arg_str("profile")
        .filter(|s| !s.is_empty())
        .or_else(|| tree.product.as_ref().map(|p| p.stage.clone()))
        .unwrap_or_else(|| "dev".to_string())
}

pub fn run(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    match req.command.as_str() {
        "closure" => cmd_closure(req, &tree),
        "dep-tree" => cmd_tree(req, &tree),
        "dep-graph" => cmd_graph(req, &tree),
        "dep-why" => cmd_why(req, &tree),
        "dep-index" => cmd_index(&tree),
        other => {
            let mut d = tree.diags.clone();
            d.usage(
                format!("solver does not recognize command `{other}`"),
                "see the command table in contract §5.1",
            );
            Response::ok(d, json!({}))
        }
    }
}

fn cmd_closure(req: &Request, tree: &TreeLoad) -> Response {
    let profile = profile_of(req, tree);
    // `closure` does not scan `requires_iface` ⇒ `--json` is byte-identical either way.
    let sol = solve(tree, &profile, false);
    let mut d = tree.diags.clone();
    for (_, diag) in sol.diags {
        d.push(diag);
    }
    Response::ok(d, closure_json(tree, &sol.closure, &profile))
}

fn cmd_tree(req: &Request, tree: &TreeLoad) -> Response {
    let profile = profile_of(req, tree);
    let sol = solve(tree, &profile, false);
    let kind = req.arg_str("kind").unwrap_or_else(|| "all".to_string());
    if !rules::in_list(&["init", "runtime", "type", "all"], &kind) {
        let mut d = tree.diags.clone();
        d.usage(
            format!("`--kind {kind}` out of range"),
            "valid values: init | runtime | type | all (§7.7)",
        );
        return Response::ok(d, json!({}));
    }
    let only = req.arg_str("plugin").filter(|s| !s.is_empty());
    let selected = &sol.closure.selected;

    let mut nodes = serde_json::Map::new();
    for name in selected {
        let p = tree.by_name(name).expect("plugins in the closure are always in the tree");
        let deps: Vec<Value> = p
            .deps
            .iter()
            .filter(|d| kind == "all" || d.kind == kind)
            .map(|d| {
                json!({
                    "name": d.name,
                    "kind": d.kind,
                    "range": d.range.canonical(),
                    "phase": d.phase,
                })
            })
            .collect();
        nodes.insert(
            name.clone(),
            json!({"deps": deps, "file": p.file, "phase": p.phase}),
        );
    }
    let roots: Vec<String> = match &only {
        Some(n) => vec![n.clone()],
        None => sol.closure.seeds.clone(),
    };
    let mut d = tree.diags.clone();
    for (_, diag) in sol.diags {
        d.push(diag);
    }
    Response::ok(
        d,
        json!({"roots": roots, "nodes": Value::Object(nodes), "kind": kind}),
    )
}

pub fn sorted_edges(c: &Closure, kind: &str) -> Vec<Edge> {
    let mut out: Vec<Edge> = match kind {
        "init" => c.init_edges.clone(),
        "runtime" => c.runtime_edges.clone(),
        "type" => c.type_edges.clone(),
        _ => c.all_edges().into_iter().cloned().collect(),
    };
    out.sort_by(|a, b| (&a.from, &a.to, &a.kind).cmp(&(&b.from, &b.to, &b.kind)));
    out.dedup_by(|a, b| a.from == b.from && a.to == b.to && a.kind == b.kind);
    out
}

fn cmd_graph(req: &Request, tree: &TreeLoad) -> Response {
    let profile = profile_of(req, tree);
    let sol = solve(tree, &profile, false);
    let format = req.arg_str("format").unwrap_or_else(|| "dot".to_string());
    if !rules::in_list(&["dot", "mermaid", "json"], &format) {
        let mut d = tree.diags.clone();
        d.usage(
            format!("`--format {format}` out of range"),
            "valid values: dot | mermaid | json (§7.7)",
        );
        return Response::ok(d, json!({}));
    }
    let nodes: Vec<String> = sol.closure.selected.iter().cloned().collect();
    let edges = sorted_edges(&sol.closure, "all");
    let edges_json_v: Vec<Value> = edges
        .iter()
        .map(|e| json!({"from": e.from, "to": e.to, "kind": e.kind}))
        .collect();
    let text = match format.as_str() {
        "dot" => {
            let mut s = String::from("digraph deps {\n  rankdir=LR;\n");
            for n in &nodes {
                s.push_str(&format!("  \"{n}\";\n"));
            }
            for e in &edges {
                s.push_str(&format!(
                    "  \"{}\" -> \"{}\" [label=\"{}\"];\n",
                    e.from, e.to, e.kind
                ));
            }
            s.push_str("}\n");
            s
        }
        "mermaid" => {
            let mut s = String::from("graph LR\n");
            for n in &nodes {
                s.push_str(&format!("  {}[\"{}\"]\n", safe_id(n), n));
            }
            for e in &edges {
                s.push_str(&format!(
                    "  {} -->|{}| {}\n",
                    safe_id(&e.from),
                    e.kind,
                    safe_id(&e.to)
                ));
            }
            s
        }
        _ => serde_json::to_string(&json!({"nodes": nodes, "edges": edges_json_v}))
            .expect("JSON is always serializable"),
    };
    let mut d = tree.diags.clone();
    for (_, diag) in sol.diags {
        d.push(diag);
    }
    Response::ok(
        d,
        json!({"format": format, "nodes": nodes, "edges": edges_json_v, "text": text}),
    )
}

fn safe_id(name: &str) -> String {
    name.chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect()
}

fn cmd_why(req: &Request, tree: &TreeLoad) -> Response {
    let profile = profile_of(req, tree);
    let sol = solve(tree, &profile, false);
    let from = req.arg_str("from").unwrap_or_default();
    let to = req.arg_str("to").unwrap_or_default();
    let mut d = tree.diags.clone();

    // BFS in consumer → provider direction (shortest path).
    let mut adj: BTreeMap<String, Vec<(String, String)>> = BTreeMap::new();
    for e in sol.closure.all_edges() {
        adj.entry(e.from.clone())
            .or_default()
            .push((e.to.clone(), e.kind.clone()));
    }
    for v in adj.values_mut() {
        v.sort();
    }

    if from.is_empty() || to.is_empty() {
        d.usage("`dep why` requires `from` and `to`", "contract §5.1: {from,to}");
        return Response::ok(d, json!({"found": false, "path": [], "edges": []}));
    }

    let mut path: Vec<String> = Vec::new();
    let mut path_edges: Vec<Value> = Vec::new();
    if from == to {
        path.push(from.clone());
    } else {
        let mut prev: BTreeMap<String, (String, String)> = BTreeMap::new();
        let mut seen: BTreeSet<String> = BTreeSet::new();
        let mut q: VecDeque<String> = VecDeque::new();
        seen.insert(from.clone());
        q.push_back(from.clone());
        let mut reached = false;
        while let Some(n) = q.pop_front() {
            for (m, k) in adj.get(&n).cloned().unwrap_or_default() {
                if seen.contains(&m) {
                    continue;
                }
                seen.insert(m.clone());
                prev.insert(m.clone(), (n.clone(), k));
                if m == to {
                    reached = true;
                    break;
                }
                q.push_back(m);
            }
            if reached {
                break;
            }
        }
        if reached {
            let mut cur = to.clone();
            let mut chain = vec![cur.clone()];
            while let Some((p, k)) = prev.get(&cur).cloned() {
                path_edges.push(json!({"from": p, "to": cur, "kind": k}));
                chain.push(p.clone());
                cur = p;
                if cur == from {
                    break;
                }
            }
            chain.reverse();
            path = chain;
            path_edges.reverse();
        }
    }
    Response::ok(
        d,
        json!({"found": !path.is_empty(), "path": path, "edges": path_edges}),
    )
}

fn cmd_index(tree: &TreeLoad) -> Response {
    let index = model::DependentsIndex::build(tree);
    let content = emit::render_dependents(&index);
    let mut map = serde_json::Map::new();
    for (k, v) in &index.dependents {
        map.insert(
            k.clone(),
            Value::Array(
                v.iter()
                    .map(|e| json!({"from": e.from, "kind": e.kind}))
                    .collect(),
            ),
        );
    }
    let files = vec![json!({
        "path": "build/index/dependents.json",
        "kind": "machine",
        "content": content,
        "mode": "0644",
    })];
    Response::ok_with_files(tree.diags.clone(), files, json!({"dependents": Value::Object(map)}))
}

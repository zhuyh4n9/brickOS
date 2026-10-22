//! Composition-time `check` (V-3/V-8/V-12/V-14/V-19): the cross-record / cross-file validation
//! choke point. Single-record shape errors are already reported by `model::load_tree`; this module
//! does not re-report those codes.
//!
//! Scopes: `deps` closure/init cycle, `compat_gen` + range intersection, single-version policy,
//! phase monotonicity (R1/R2), profile gate (`BRV-VER-0004`); `iface` cross-file export
//! classification invariant 3, unit and symbol-family collisions, `requires_iface` info only,
//! `unfreezing` under release (`BRV-IFACE-0009`); `tax` dependency-direction prohibitions and
//! `sched_class` legality; `priv` budget totals and exclusive-resource conflicts (both sides).
//!
//! `data.summary` = `{errors, warnings, infos, profile, closure:{plugins, ram_kib, stack_kib}}`.

use serde_json::{json, Value};

use crate::diag::{Diag, Severity};
use crate::model::{self, Plugin, TreeLoad};
use crate::proto::{Request, Response};
use crate::rules;
use crate::solver::{self, SCOPE_DEPS, SCOPE_IFACE, SCOPE_PRIV, SCOPE_SRC, SCOPE_TAX};
use crate::srcscan;

/// A function prototype (or an `extern` declaration) with no definition in the same `.c`.
pub const CODE_SRC_DECL: &str = "BRV-SRC-0001";
/// A file-scope object that is not `static`, or an object exported through a public header.
pub const CODE_SRC_GLOBAL: &str = "BRV-SRC-0002";
/// A header of a plugin that is not in the consumer's **direct** dependency set (ADR-0038).
pub const CODE_DEP_UNDECLARED: &str = "BRV-DEP-0012";

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

/// `sched_kind`: explicit field first, else inferred from the scheduler plugin name.
pub fn sched_kind(p: &Plugin) -> Option<&'static str> {
    if let Some(k) = &p.sched_kind {
        return match k.as_str() {
            "coop" => Some("coop"),
            "preempt" => Some("preempt"),
            "tt" => Some("tt"),
            _ => None,
        };
    }
    if p.subkind.as_deref() != Some("scheduler") {
        return None;
    }
    let short = rules::name_short(&p.name);
    if short.contains("coop") {
        Some("coop")
    } else if short.contains("preempt") {
        Some("preempt")
    } else if short.contains("tt") {
        Some("tt")
    } else {
        None
    }
}

pub fn run(req: &Request) -> Response {
    let root = std::path::PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    // V-16: the unfreeze window may exist only in a snapshot (unfreeze writes no brick.toml).
    let mut snap_diags = crate::diag::Diags::new();
    let snaps = model::load_snapshots(&root, &mut snap_diags);
    let mut base_diags = tree.diags.clone();
    base_diags.merge(snap_diags);
    let profile = req
        .arg_str("profile")
        .filter(|s| !s.is_empty())
        .or_else(|| tree.product.as_ref().map(|p| p.stage.clone()))
        .unwrap_or_else(|| "dev".to_string());
    if !rules::in_list(&["dev", "release"], &profile) {
        let mut d = base_diags.clone();
        d.usage(
            format!("`--profile {profile}` out of range"),
            "legal values: dev | release (§7.5)",
        );
        return Response::ok(d, json!({}));
    }

    let mut scopes: Vec<String> = req
        .arg("scopes")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    if scopes.is_empty() {
        scopes = vec![
            SCOPE_DEPS.to_string(),
            SCOPE_IFACE.to_string(),
            SCOPE_TAX.to_string(),
            SCOPE_PRIV.to_string(),
            SCOPE_SRC.to_string(),
        ];
    }
    for s in &scopes {
        if !rules::in_list(
            &[SCOPE_DEPS, SCOPE_IFACE, SCOPE_TAX, SCOPE_PRIV, SCOPE_SRC],
            s,
        ) {
            let mut d = base_diags.clone();
            d.usage(
                format!("unknown check scope `{s}`"),
                "legal scopes: deps | iface | tax | priv | src (§7.7, ADR-0038)",
            );
            return Response::ok(d, json!({}));
        }
    }

    let sol = solver::solve(&tree, &profile, true);
    let closure = &sol.closure;

    let mut scoped: Vec<(&'static str, Diag)> = sol.diags;

    if scopes.iter().any(|s| s == SCOPE_DEPS) {
        scoped.extend(deps_usage_checks(&root, &tree, closure));
    }
    if scopes.iter().any(|s| s == SCOPE_IFACE) {
        scoped.extend(iface_checks(&tree, closure, &profile, &snaps));
    }
    if scopes.iter().any(|s| s == SCOPE_TAX) {
        scoped.extend(tax_checks(&tree, closure));
    }
    if scopes.iter().any(|s| s == SCOPE_PRIV) {
        scoped.extend(priv_checks(&tree, closure));
    }
    if scopes.iter().any(|s| s == SCOPE_SRC) {
        scoped.extend(src_checks(&root, &tree, closure));
    }

    let only = req.arg_str("plugin").filter(|s| !s.is_empty());

    let mut summary_diags = base_diags.clone();
    let mut scope_results = serde_json::Map::new();
    for scope in [SCOPE_DEPS, SCOPE_IFACE, SCOPE_TAX, SCOPE_PRIV, SCOPE_SRC] {
        if !scopes.iter().any(|s| s == scope) {
            continue;
        }
        let mut e = 0usize;
        let mut w = 0usize;
        let mut i = 0usize;
        for (tag, diag) in &scoped {
            if *tag != scope {
                continue;
            }
            if let Some(p) = &only {
                if &diag.target != p {
                    continue;
                }
            }
            match diag.severity {
                Severity::Error => e += 1,
                Severity::Warning => w += 1,
                Severity::Info => i += 1,
            }
        }
        scope_results.insert(
            scope.to_string(),
            json!({"scope": scope, "errors": e, "warnings": w, "infos": i}),
        );
    }
    let (le, lw, li) = base_diags.counts();
    if le + lw + li > 0 {
        scope_results.insert(
            "manifest".to_string(),
            json!({"scope": "manifest", "errors": le, "warnings": lw, "infos": li}),
        );
    }
    for (_, diag) in &scoped {
        if let Some(p) = &only {
            if &diag.target != p {
                continue;
            }
        }
        summary_diags.push(diag.clone());
    }

    let (errors, warnings, infos) = summary_diags.counts();
    let data = json!({
        "summary": {
            "errors": errors,
            "warnings": warnings,
            "infos": infos,
            "profile": profile,
            "closure": {
                "plugins": closure.selected.len(),
                "ram_kib": closure.totals_ram,
                "stack_kib": closure.totals_stack,
            },
        },
        "scope_results": Value::Object(scope_results),
        "scopes": scopes,
        "plugin": only,
    });
    Response::ok(summary_diags, data)
}

/// An `#include` of another plugin's header that the consumer did not declare (directly, or
/// through a directly declared skin). Uses the same scan as `build` (`model::cross_plugin_includes`)
/// — the two must not diverge.
///
/// ADR-0038 tightened the criterion from "transitive dependency closure" to "**direct** dependency
/// set ∪ the units a directly declared skin re-exports": using C through B is an implicit
/// dependency on C, which the declaration surface must show. `-I` narrowing alone is not enough:
/// the target toolchain's glibc headers shadow common names like `unistd.h`.
fn deps_usage_checks(
    root: &std::path::Path,
    tree: &TreeLoad,
    closure: &solver::Closure,
) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    for u in model::cross_plugin_includes(root, tree, &closure.selected) {
        // The core plugin is platform-agnostic: it uses the platform *contract* (`br/runtime/**`)
        // without naming a platform plugin, so that include family is exempt (same rule as `build`).
        if u.header.starts_with("br/runtime/")
            && tree
                .plugins
                .iter()
                .any(|p| p.name == u.consumer && p.plugin_type == "core")
        {
            continue;
        }
        // A-2: an APP must not reach below the iface layer. Any *other* consumer is judged by the
        // ordinary dependency rule below (ADR-0038: core is a provider like any other, so its
        // headers need `[[dep]] provider/core` -- no ambient exemption).
        if u.provider == "core/c_core" {
            let is_app = tree
                .plugins
                .iter()
                .any(|p| p.name == u.consumer && p.plugin_type == "app");
            if is_app {
                out.push((
                    SCOPE_DEPS,
                    err(
                        "BRV-MF-0001",
                        &u.consumer,
                        &u.file,
                        "include",
                        format!(
                            "`{}` (APP) directly includes core header `{}`: APP must not depend on interfaces below the iface layer",
                            u.consumer, u.header
                        ),
                        "ADR-0020 / A-2: use it via an Interface skin (e.g. `<iface/min/min.h>` re-exporting br_log_*/br_clock_*), or add an entry for this core surface to a skin",
                    ),
                ));
                continue;
            }
        }
        if model::dep_direct_auth(tree, &u.consumer).contains(&u.provider) {
            continue;
        }
        out.push((
            SCOPE_DEPS,
            err(
                CODE_DEP_UNDECLARED,
                &u.consumer,
                &u.file,
                "include",
                format!(
                    "`{}` includes `{}`'s header `{}`, but `brick.toml` declares no **direct** dependency on `{}`",
                    u.consumer, u.provider, u.header, u.provider
                ),
                "ADR-0038: transitive dependencies do not authorize; add `[[dep]] {provider}` (or a skin that re-exports it), or drop the include",
            ),
        ));
    }
    out
}

/// The `src` scope (ADR-0038): the two source-text disciplines that the declaration surface cannot
/// see. Zero compiler dependency (ruling R-13). Skipped for `third_party/**` sources (vendor code
/// is not this repo's to discipline) and for plugin-private headers that a vendor TU must see; the
/// precise, compiler-backed counterpart is the `.a` symbol audit in `brickie build`.
fn src_checks(
    root: &std::path::Path,
    tree: &TreeLoad,
    closure: &solver::Closure,
) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    let selftest_on = tree
        .product
        .as_ref()
        .map(|p| p.selftest_on())
        .unwrap_or(false);
    let selected: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| closure.selected.contains(&p.name))
        .collect();

    for p in &selected {
        let dir = p.name.clone();
        let b = p.build.clone().unwrap_or_default();
        // A plugin that compiles vendor code shares its translation units' globals on purpose
        // (`fs/uboot-erofs/glue` must define `sbi` for u-boot's own `namei.c`). The *precise*
        // criterion — does a data symbol leave the plugin? — is the `.a` audit, so the textual
        // `static` rule is relaxed only here.
        let vendor = model::carries_vendor_code(&dir, p.build.as_ref());

        // (1) public headers must not export an object at all
        for inc in &b.includes {
            for h in headers_under(root, &format!("{dir}/{inc}")) {
                if is_vendor_path(&h) || vendor {
                    continue;
                }
                let Ok(text) = std::fs::read_to_string(root.join(&h)) else {
                    continue;
                };
                let sc = srcscan::scan(&text);
                for d in sc.extern_objects.iter().chain(sc.nonstatic_objects.iter()) {
                    out.push((
                        SCOPE_SRC,
                        err(
                            CODE_SRC_GLOBAL,
                            &p.name,
                            &h,
                            format!("line {}", d.line),
                            format!(
                                "public header exports object `{}` ({}): a plugin's surface may carry functions, types and macros, but not variables",
                                d.name,
                                if d.text.starts_with("extern") { "extern object" } else { "tentative definition" }
                            ),
                            "ADR-0038 / R2: keep the object `static` in the .c and expose an accessor function, or move the accessor into the header",
                        ),
                    ));
                }
            }
        }

        // (2) each compilation unit is self-contained
        let mut sources: Vec<String> = Vec::new();
        for pat in &b.sources {
            sources.extend(crate::build::expand_glob(root, &dir, pat));
        }
        if selftest_on {
            // The plugin's own suite (ADR-0039), plus the product-level default for the core plugin.
            for pat in &b.selftest_sources {
                sources.extend(crate::build::expand_glob(root, &dir, pat));
            }
            if p.plugin_type == "core" {
                if let Some(pb) = tree.product.as_ref().and_then(|x| x.build.as_ref()) {
                    for pat in &pb.selftest_sources {
                        sources.extend(crate::build::expand_glob(root, "", pat));
                    }
                }
            }
        }
        sources.sort();
        sources.dedup();
        for s in &sources {
            if !s.ends_with(".c") || is_vendor_path(s) {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(root.join(s)) else {
                continue;
            };
            let sc = srcscan::scan(&text);
            for d in sc
                .prototypes
                .iter()
                .filter(|d| !sc.defined.contains(&d.name))
            {
                out.push((
                    SCOPE_SRC,
                    err(
                        CODE_SRC_DECL,
                        &p.name,
                        s,
                        format!("line {}", d.line),
                        format!(
                            "`{}` is declared here but never defined in this file: a `.c` must not carry a declaration it does not implement",
                            d.name
                        ),
                        "ADR-0038 / R1: move the prototype into a header (the declaration surface), or add the definition",
                    ),
                ));
            }
            for d in sc.externs.iter().filter(|d| !sc.defined.contains(&d.name)) {
                out.push((
                    SCOPE_SRC,
                    err(
                        CODE_SRC_DECL,
                        &p.name,
                        s,
                        format!("line {}", d.line),
                        format!(
                            "`extern` declaration of `{}` with no definition in this file (cross-translation-unit declarations belong in a header)",
                            d.name
                        ),
                        "ADR-0038 / R1: declare it in a header (a private `<plugin>/xxx_internal.h` is enough) and include that",
                    ),
                ));
            }
            if !vendor {
                for d in &sc.nonstatic_objects {
                    out.push((
                        SCOPE_SRC,
                        err(
                            CODE_SRC_GLOBAL,
                            &p.name,
                            s,
                            format!("line {}", d.line),
                            format!(
                                "file-scope object `{}` has external linkage: globals must not leave the plugin",
                                d.name
                            ),
                            "ADR-0038 / R2: make it `static` and expose a function when another translation unit genuinely needs it",
                        ),
                    ));
                }
            }
        }
    }
    out
}

/// Vendor code is not this repo's to discipline (and its globals are shared with the vendored
/// translation units on purpose). Accepts un-normalized paths such as
/// `fs/uboot-erofs/../../third_party/uboot-imx/fs/erofs/erofs_fs.h`.
fn is_vendor_path(p: &str) -> bool {
    p.starts_with("third_party/") || p.contains("/third_party/")
}

/// Headers (`*.h`) under one root-relative include directory, recursively, sorted.
fn headers_under(root: &std::path::Path, dir: &str) -> Vec<String> {
    fn walk(root: &std::path::Path, dir: &std::path::Path, out: &mut Vec<String>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        let mut items: Vec<std::path::PathBuf> = entries.flatten().map(|e| e.path()).collect();
        items.sort();
        for path in items {
            if path.is_dir() {
                walk(root, &path, out);
            } else if path.extension().and_then(|s| s.to_str()) == Some("h") {
                if let Ok(rel) = path.strip_prefix(root) {
                    out.push(rel.to_string_lossy().replace('\\', "/"));
                }
            }
        }
    }
    let mut out = Vec::new();
    walk(root, &root.join(dir), &mut out);
    out
}

fn iface_checks(
    tree: &TreeLoad,
    closure: &solver::Closure,
    profile: &str,
    snaps: &[model::Snapshot],
) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    let selected: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| closure.selected.contains(&p.name))
        .collect();

    for p in &selected {
        for (ei, exp) in p.exports.iter().enumerate() {
            if exp.form != "skin" {
                continue;
            }
            for r in &exp.reexport_of {
                let Some((pn, un)) = r.split_once('#') else {
                    continue; // bad shape already reported at load time
                };
                match tree.by_name(pn).and_then(|prov| prov.find_export(un)) {
                    None => out.push((
                        SCOPE_IFACE,
                        err(
                            "BRV-TAX-0018",
                            &p.name,
                            &p.file,
                            format!("export[{ei}].reexport_of"),
                            format!(
                                "`form = \"skin\"` re-exports unit `{r}`, which does not exist (provider `{pn}` or unit `{un}` missing, invariant 3)"
                            ),
                            "§3.5: every re-exported unit in the list must exist",
                        ),
                    )),
                    Some(pe) => {
                        if pe.api_iface != exp.api_iface {
                            out.push((
                                SCOPE_IFACE,
                                err(
                                    "BRV-TAX-0018",
                                    &p.name,
                                    &p.file,
                                    format!("export[{ei}].reexport_of"),
                                    format!(
                                        "skin `{}` has `api_iface = {}`, but re-exported unit `{r}` is `{}` — classifications must be equal (invariant 3)",
                                        p.name, exp.api_iface, pe.api_iface
                                    ),
                                    "§3.5: a skin may only re-export units of the same class (cross-file decision)",
                                ),
                            ));
                        }
                    }
                }
            }
        }
    }

    // Unit identity is `<provider>#<unit>` (§5.6), and that is what must be unique: a unit file
    // lives at `api/iface/<provider>/<unit>.toml`, so two providers may each call a unit `trace`.
    // (ADR-0038 §3 moved this from bare unit names to ids: `core/c_core#trace` and
    // `provider/trace#trace` are different units and now coexist, which "one header = one unit"
    // naming needs.)
    let mut by_id: std::collections::BTreeMap<String, String> = std::collections::BTreeMap::new();
    for p in &selected {
        for e in &p.exports {
            let id = e.unit_id(&p.name);
            if let Some(first) = by_id.get(&id) {
                out.push((
                    SCOPE_IFACE,
                    err(
                        "BRV-MF-0001",
                        &p.name,
                        &p.file,
                        "export.name",
                        format!(
                            "interface unit id `{id}` is declared twice in the closure: {first} and {} (unit ids must be unique)",
                            p.file
                        ),
                        "§5.6: a unit is identified by `<provider>#<unit>`; two `[[export]]` blocks of one plugin cannot share a name",
                    ),
                ));
            } else {
                by_id.insert(id, p.file.clone());
            }
        }
    }

    let mut sym_owner: std::collections::BTreeMap<String, (String, String)> =
        std::collections::BTreeMap::new();
    for p in &selected {
        for e in &p.exports {
            for s in &e.symbols {
                if let Some((owner, file)) = sym_owner.get(s) {
                    out.push((
                        SCOPE_IFACE,
                        err(
                            "BRV-MF-0001",
                            &p.name,
                            &p.file,
                            "export.symbols",
                            format!(
                                "symbol family `{s}` is declared by both `{owner}` ({file}) and `{}` ({}) (collision)",
                                p.name, p.file
                            ),
                            "gap workaround (R-9): no dedicated code for symbol-family collision; §2 item 4",
                        ),
                    ));
                } else {
                    sym_owner.insert(s.clone(), (p.name.clone(), p.file.clone()));
                }
            }
        }
    }

    for p in &selected {
        for (ri, r) in p.compat.requires_iface.iter().enumerate() {
            out.push((
                SCOPE_IFACE,
                info(
                    "BRV-MF-0001",
                    &p.name,
                    &p.file,
                    format!("compat.requires_iface[{ri}]"),
                    format!(
                        "declared-surface interface dependency `{}` (compat_gen={}, range={}): v0.1 does not scan declared-surface interface dependencies, it only records them (V-10③)",
                        r.id, r.compat_gen, r.range_raw
                    ),
                    "§5.6/§10 V-10: requires_iface does not participate in closure solving",
                ),
            ));
        }
    }

    if profile == "release" {
        let snapshot_unfreezing: std::collections::BTreeSet<String> = snaps
            .iter()
            .filter(|s| s.freeze_state.as_deref() == Some("unfreezing"))
            .map(|s| s.id.clone())
            .collect();
        for p in &selected {
            for (ei, e) in p.exports.iter().enumerate() {
                let id = e.unit_id(&p.name);
                let decl_unfreezing = e.freeze_state.as_deref() == Some("unfreezing");
                if decl_unfreezing || snapshot_unfreezing.contains(&id) {
                    out.push((
                        SCOPE_IFACE,
                        err(
                            "BRV-IFACE-0009",
                            &p.name,
                            &p.file,
                            format!("export[{ei}].freeze_state"),
                            format!(
                                "release does not allow an interface unit to stay in the unfreeze window: `{id}` is `unfreezing` (declaration or snapshot)"
                            ),
                            "§5.3.4 rule 3: release must be frozen; run `iface refreeze` to leave the window",
                        ),
                    ));
                }
            }
        }
    }

    out
}

fn tax_checks(tree: &TreeLoad, closure: &solver::Closure) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    let selected: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| closure.selected.contains(&p.name))
        .collect();

    for p in &selected {
        if p.sched_class == "TT_SAFE" && p.sched_tt.is_none() {
            out.push((
                SCOPE_TAX,
                err(
                    "BRV-MF-0001",
                    &p.name,
                    &p.file,
                    "sched_class",
                    format!(
                        "`sched_class = \"TT_SAFE\"` must also declare `[sched.tt]` (period_us/deadline_us/wcet_us); `{}` lacks that table",
                        p.name
                    ),
                    "§8.1: TT_SAFE must also provide period metadata; gap workaround (R-9)",
                ),
            ));
        }
    }
    let schedulers: Vec<(&Plugin, &'static str)> = selected
        .iter()
        .filter(|p| p.subkind.as_deref() == Some("scheduler"))
        .filter_map(|p| sched_kind(p).map(|k| (*p, k)))
        .collect();
    for p in &selected {
        if p.sched_class != "COOP_ONLY" {
            continue;
        }
        for (s, kind) in &schedulers {
            if s.name == p.name {
                continue;
            }
            if *kind != "coop" {
                out.push((
                    SCOPE_TAX,
                    err(
                        "BRV-MF-0001",
                        &p.name,
                        &p.file,
                        "sched_class",
                        format!(
                            "`{}` with `sched_class = \"COOP_ONLY\"` is combined with scheduler `{}` (sched_kind = `{kind}`): a `coop` scheduler is required",
                            p.name, s.name
                        ),
                        "§2 item 3 / §8.1; sched_kind per ruling R-5 (explicit field first, else inferred from name); gap workaround (R-9)",
                    ),
                ));
            }
        }
    }

    out
}

fn priv_checks(tree: &TreeLoad, closure: &solver::Closure) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    let selected: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| closure.selected.contains(&p.name))
        .collect();

    let mut owners: std::collections::BTreeMap<(String, String), (String, String)> =
        std::collections::BTreeMap::new();
    for p in &selected {
        let Some(priv_decl) = &p.privileged else {
            continue;
        };
        let r = &priv_decl.resources;
        let mut items: Vec<(String, String)> = Vec::new();
        for v in &r.irq {
            items.push(("irq".to_string(), v.to_string()));
        }
        for v in &r.dma_channels {
            items.push(("dma_channel".to_string(), v.to_string()));
        }
        for v in &r.pins {
            items.push(("pin".to_string(), v.to_string()));
        }
        for v in &r.device_names {
            items.push(("device_name".to_string(), v.clone()));
        }
        for (kind, val) in items {
            let key = (kind.clone(), val.clone());
            if let Some((owner, file)) = owners.get(&key) {
                out.push((
                    SCOPE_PRIV,
                    err(
                        "BRV-MF-0001",
                        &p.name,
                        &p.file,
                        "privileged.resources",
                        format!(
                            "exclusive-resource conflict: {kind} `{val}` is declared by both `{owner}` ({file}) and `{}` ({})",
                            p.name, p.file
                        ),
                        "§2 item 5 / §8.1: [privileged.resources] is only checked for conflicts; gap workaround (R-9), report both sides",
                    ),
                ));
            } else {
                owners.insert(key, (p.name.clone(), p.file.clone()));
            }
        }
    }

    out
}

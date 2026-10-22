//! `show` family (ADR-0036) -- presentation-oriented views that re-decide nothing:
//!
//! * `show-bricks`     -- the product's closure, grouped for reading, with the budget it spends.
//! * `show-init-flow`  -- **the runtime boot chain**: the order `br_plugin_manager_*` actually
//!   walks (EARLY → CORE → LATE → IRQ on → START → selftest pass), derived from the same facts the
//!   descriptors are generated from.
//!
//! Why `show-init-flow` is not `closure.topo_order`: the runtime sorts with **Kahn's algorithm over
//! a FIFO queue seeded in `.br_plugins` section order** (the order the descriptor objects are
//! linked in, = the sorted `build/gen/**` glob), while `solver::topo_order` breaks ties by plugin
//! name. Both are valid topological orders and they differ; only the former is what boots. The
//! derivation here is validated event-for-event against a real run's log (ADR-0036 §4).

use serde_json::{json, Value};

use crate::model::{Plugin, TreeLoad};
use crate::proto::{Request, Response};
use crate::solver::{self, Closure};

/// One step of the runtime boot chain: a hook about to be called on one plugin.
#[derive(Clone, Debug)]
pub struct Step {
    pub phase: &'static str,
    pub plugin: String,
    pub plugin_type: String,
    pub subkind: Option<String>,
    pub hook: &'static str,
    /// Free-text marker for the steps whose place is a rule rather than a topological fact.
    pub note: &'static str,
}

impl Step {
    fn to_json(&self) -> Value {
        json!({
            "phase": self.phase,
            "plugin": self.plugin,
            "plugin_type": self.plugin_type,
            "subkind": self.subkind,
            "hook": self.hook,
            "note": self.note,
        })
    }
}

/// Whether `early_init` is emitted: all plugins except `core` (no lifecycle at all, ADR-0031) and
/// the **APP** (ADR-0037: it registers through the section scan, so the hook has nothing to do).
fn has_early(p: &Plugin) -> bool {
    p.plugin_type != "core" && p.plugin_type != "app"
}

/// Whether `init` is emitted: `phase = "early"` means "no `init` hook" (ADR-0005 §2.4 rule 4).
fn has_init(p: &Plugin) -> bool {
    p.plugin_type != "core" && p.phase != "early"
}

fn service_like(p: &Plugin) -> bool {
    crate::rules::is_service_like(&p.plugin_type, p.subkind.as_deref())
}

/// The phase a plugin's `init` runs in at runtime: `early` = it has none. APP ⇒ `app` (last of all
/// init, ADR-0037).
fn runtime_init_phase(p: &Plugin) -> &'static str {
    if !has_init(p) {
        "early"
    } else if p.plugin_type == "app" {
        "app"
    } else if service_like(p) {
        "late"
    } else {
        "core"
    }
}

/// Runtime topological order: Kahn over `kind = "init"` edges with a **FIFO queue seeded in
/// section order** -- a faithful port of `topo_sort()` in `plugin_manager.c`, duplicate edges and
/// all (the runtime's `indeg == 0 ⇒ skip` guard is what makes a duplicated `[[dep]]` a cycle
/// there, so it must stay a cycle here too: otherwise this command would print a boot the runtime
/// never reaches).
///
/// Returns `(order, None)` on success, or `(partial_order, Some(unsorted))` when the DAG has a
/// cycle; `unsorted` are the nodes the runtime would refuse to schedule (it panics instead).
pub fn runtime_order(section: &[String], init_edges: &[(String, String)]) -> (Vec<String>, Option<Vec<String>>) {
    let index: std::collections::BTreeMap<&str, usize> =
        section.iter().enumerate().map(|(i, n)| (n.as_str(), i)).collect();
    let n = section.len();
    let mut indeg = vec![0usize; n];
    for (from, _) in init_edges {
        if let Some(&i) = index.get(from.as_str()) {
            indeg[i] += 1;
        }
    }
    let mut queue: std::collections::VecDeque<usize> = (0..n).filter(|i| indeg[*i] == 0).collect();
    let mut order: Vec<usize> = Vec::new();
    while let Some(u) = queue.pop_front() {
        order.push(u);
        for (from, to) in init_edges {
            let (Some(&t), Some(&f)) = (index.get(to.as_str()), index.get(from.as_str())) else {
                continue;
            };
            if t != u || indeg[f] == 0 {
                continue;
            }
            indeg[f] -= 1;
            if indeg[f] == 0 {
                queue.push_back(f);
            }
        }
    }
    let names = |v: &[usize]| v.iter().map(|i| section[*i].clone()).collect::<Vec<_>>();
    if order.len() == n {
        (names(&order), None)
    } else {
        let done: std::collections::BTreeSet<usize> = order.iter().copied().collect();
        let unsorted = (0..n).filter(|i| !done.contains(i)).collect::<Vec<_>>();
        (names(&order), Some(names(&unsorted)))
    }
}

/// The full boot chain, in call order, exactly as the plugin manager walks it.
pub struct Flow {
    pub steps: Vec<Step>,
    /// The runtime's `s_order` (section-order FIFO topological sort) -- the iteration order every
    /// phase uses. Exposed so the two `show` views can be cross-checked against a real run.
    pub order: Vec<String>,
    pub unsorted: Option<Vec<String>>,
    /// Plugins whose `phase` declaration disagrees with the phase their `init` actually runs in.
    /// Empty by construction once `rules::phase_reason` has been enforced -- this is the guard that
    /// proves it, not a report that is expected to fire.
    pub phase_divergence: Vec<(String, String, &'static str)>,
}

/// Derive the runtime boot chain of `closure` from the plugin declarations.
///
/// `selftest_on` is the product switch resolved to a bool (product absent = off, like `brickie
/// build`): it decides whether the trailing selftest pass has any hooks to call.
pub fn boot_flow(tree: &TreeLoad, closure: &Closure, selftest_on: bool) -> Flow {
    // Section order = the order the descriptor objects are linked, which is the sorted
    // `build/gen/**/plugin_desc.c` glob. Tie-breaking in the runtime's Kahn sort uses it.
    let mut section: Vec<String> = closure.selected.iter().cloned().collect();
    section.sort();

    let init_edges: Vec<(String, String)> = closure
        .init_edges
        .iter()
        .map(|e| (e.from.clone(), e.to.clone()))
        .collect();
    let (order, unsorted) = runtime_order(&section, &init_edges);

    let meta: std::collections::BTreeMap<&str, &Plugin> =
        tree.plugins.iter().map(|p| (p.name.as_str(), p)).collect();
    let get = |name: &str| meta.get(name).copied();

    // The runtime finds the platform by `plugin_type`, scanning the section -- the first one wins.
    let platform = order
        .iter()
        .find(|n| get(n).map(|p| p.plugin_type == "runtime").unwrap_or(false))
        .cloned();

    let mut steps: Vec<Step> = Vec::new();
    let mut push = |phase: &'static str, name: &str, hook: &'static str, note: &'static str| {
        let (ptype, sub) = match get(name) {
            Some(p) => (p.plugin_type.clone(), p.subkind.clone()),
            None => (String::new(), None),
        };
        steps.push(Step {
            phase,
            plugin: name.to_string(),
            plugin_type: ptype,
            subkind: sub,
            hook,
            note,
        });
    };

    // 1. platform's early_init is the hard first step (page tables/console are not ready yet).
    if let Some(p) = &platform {
        push("early", p, "early_init", "first: found by plugin_type, not by topology");
    }
    // 2. EARLY: every plugin that emits early_init, in runtime topological order. The APP is never
    //    here: it registers through the section scan, so its descriptor carries NO_HOOK (ADR-0037).
    for name in &order {
        if Some(name) == platform.as_ref() {
            continue;
        }
        if get(name).map(has_early).unwrap_or(false) {
            push("early", name, "early_init", "");
        }
    }
    // 3. CORE, LATE then APP: `init`, phase decided by the category. The APP is last of all init
    //    (ADR-0037), so it runs after every driver/provider/skin, with IRQs still masked.
    for phase in ["core", "late", "app"] {
        for name in &order {
            if get(name).map(|p| runtime_init_phase(p) == phase).unwrap_or(false) {
                let note = if phase == "app" {
                    "APP init last: after every other plugin's init"
                } else {
                    ""
                };
                push(phase, name, "init", note);
            }
        }
    }
    // 4. IRQ on: after every plugin's init returned, before START.
    push("irq", "", "", "br_irq_cpu_enable(): after all init returned, before START");
    // 5. START: non-APP first, APP last -- an explicit rule, not topological order.
    for name in &order {
        if get(name).map(|p| p.plugin_type != "app" && p.plugin_type != "core").unwrap_or(false) {
            push("start", name, "start", "");
        }
    }
    for name in &order {
        if get(name).map(|p| p.plugin_type == "app").unwrap_or(false) {
            push("start", name, "start", "APP last: explicit rule, not topological order");
        }
    }
    // 6. Selftest pass: observation, after START and before the scheduler takes over (ADR-0010).
    for name in &order {
        let Some(p) = get(name) else { continue };
        if !p.has_selftest {
            continue;
        }
        let allowed = tree
            .product
            .as_ref()
            .map(|prod| prod.selftest_for(name))
            .unwrap_or(false);
        if allowed && selftest_on {
            push("selftest", name, "selftest", "observation: failure is recorded, not fatal");
        }
    }

    let phase_divergence = order
        .iter()
        .filter_map(|name| {
            let p = get(name)?;
            let runtime = runtime_init_phase(p);
            if has_init(p) && runtime != p.phase {
                Some((p.name.clone(), p.phase.clone(), runtime))
            } else {
                None
            }
        })
        .collect();

    Flow {
        steps,
        order,
        unsorted,
        phase_divergence,
    }
}

fn profile_of(req: &Request, tree: &TreeLoad) -> String {
    req.arg_str("profile")
        .filter(|s| !s.is_empty())
        .or_else(|| tree.product.as_ref().map(|p| p.stage.clone()))
        .unwrap_or_else(|| "dev".to_string())
}

fn counts(steps: &[Step]) -> Value {
    let mut map = serde_json::Map::new();
    for phase in ["early", "core", "late", "app", "irq", "start", "selftest"] {
        map.insert(
            phase.to_string(),
            json!(steps.iter().filter(|s| s.phase == phase).count()),
        );
    }
    Value::Object(map)
}

/// `show-bricks`: the closure as a reading aid -- who is in it, where it came from (a product root
/// or a `[[dep]]`), what it costs, and how much budget is left.
pub fn cmd_bricks(req: &Request) -> Response {
    let tree = crate::model::load_tree(std::path::Path::new(&req.root));
    let profile = profile_of(req, &tree);
    let sol = solver::solve(&tree, &profile, false);
    let mut d = tree.diags.clone();
    for (_, diag) in sol.diags {
        d.push(diag);
    }
    let c = &sol.closure;

    // Product roots, so each brick can say why it is in the image.
    let roots: std::collections::BTreeMap<String, (String, String)> = tree
        .product
        .as_ref()
        .map(|prod| {
            prod.roots
                .iter()
                .map(|r| (r.brick.clone(), (r.kind.clone(), r.alias.clone())))
                .collect()
        })
        .unwrap_or_default();

    // Every `plugin_type`, in layer order (ADR-0039 added `hal`); a type missing here would be
    // silently absent from the totals line.
    let groups = ["app", "runtime", "core", "hal", "interface", "ability"];
    let mut rows: Vec<(String, Value)> = Vec::new();
    for name in &c.selected {
        let Some(p) = tree.by_name(name) else { continue };
        let (kind, alias) = roots.get(name).cloned().unwrap_or_default();
        let ram = p.res.iter().filter(|r| r.kind == "ram").map(|r| r.used_kib).sum::<i64>();
        let stack = p.res.iter().filter(|r| r.kind == "stack").map(|r| r.used_kib).sum::<i64>();
        // Namespace bucket for the human table: path segment, singleton types folded to their own.
        let bucket = if p.name.contains('/') {
            p.name.split('/').next().unwrap_or("").to_string()
        } else {
            p.plugin_type.clone()
        };
        rows.push((
            bucket,
            json!({
                "name": p.name,
                "namespace": p.name.split('/').next().unwrap_or(""),
                "plugin_type": p.plugin_type,
                "api_type": p.api_type,
                "subkind": p.subkind,
                "phase": p.phase,
                "version": p.version.to_string(),
                "ram_kib": ram,
                "stack_kib": stack,
                "selftest": p.has_selftest,
                "origin": if kind.is_empty() { "dep" } else { "root" },
                "root_kind": kind,
                "root_alias": alias,
            }),
        ));
    }
    rows.sort_by(|a, b| a.1["name"].as_str().cmp(&b.1["name"].as_str()));

    let totals = json!({
        "plugins": c.selected.len(),
        "roots": roots.len(),
        "ram_kib": c.totals_ram,
        "stack_kib": c.totals_stack,
        "budget": {
            "ram_kib": tree.product.as_ref().and_then(|p| p.budget_ram_kib),
            "stack_kib": tree.product.as_ref().and_then(|p| p.budget_stack_kib),
        },
        "runtime_capacity": {"ram_kib": c.capacity_ram, "stack_kib": c.capacity_stack},
        "has_runtime": c.has_runtime,
        "by_type": groups
            .iter()
            .map(|g| {
                (
                    g.to_string(),
                    json!(rows.iter().filter(|(_, r)| r["plugin_type"] == *g).count()),
                )
            })
            .collect::<serde_json::Map<String, Value>>(),
    });

    Response::ok(
        d,
        json!({
            "product": tree.product.as_ref().map(|p| json!({
                "name": p.name,
                "version": p.version.to_string(),
                "stage": p.stage,
                "profile": profile,
            })),
            "has_product": tree.has_product,
            "bricks": rows.into_iter().map(|(_, r)| r).collect::<Vec<_>>(),
            "totals": totals,
            "seeds": c.seeds,
        }),
    )
}

/// `show-init-flow`: the runtime boot chain, step by step.
pub fn cmd_init_flow(req: &Request) -> Response {
    let tree = crate::model::load_tree(std::path::Path::new(&req.root));
    let profile = profile_of(req, &tree);
    let sol = solver::solve(&tree, &profile, false);
    let mut d = tree.diags.clone();
    for (_, diag) in sol.diags {
        d.push(diag);
    }
    let c = &sol.closure;
    let selftest_on = tree
        .product
        .as_ref()
        .map(|p| p.selftest_on())
        .unwrap_or(false);
    let flow = boot_flow(&tree, c, selftest_on);

    // The invariant `rules::phase_reason` enforces, re-proven on the flow itself: if a declared
    // phase ever disagreed with the phase its `init` runs in, the printed chain would be a guess.
    for (name, declared, runtime) in &flow.phase_divergence {
        d.push(
            crate::diag::Diag::new(crate::diag::Severity::Error, format!(
                "plugin `{name}` declares `phase = \"{declared}\"` but its `init` runs in the `{runtime}` phase"
            ))
            .code("BRV-MF-0001")
            .target(name)
            .file("")
            .span("plugin.phase")
            .hint("§7.2: the init completion point is decided by the category (Service/Interface ⇒ late, everything else ⇒ core)"),
        );
    }
    if let Some(unsorted) = &flow.unsorted {
        d.push(
            crate::diag::Diag::new(crate::diag::Severity::Error, format!(
                "the init-DAG has a cycle: {} plugin(s) cannot be scheduled ({})",
                unsorted.len(),
                unsorted.join(", ")
            ))
            .code("BRV-DEP-0009")
            .target("product")
            .file("")
            .span("product")
            .hint("the runtime panics on this (plugin_manager: init-DAG has a cycle); break the cycle"),
        );
    }

    Response::ok(
        d,
        json!({
            "product": tree.product.as_ref().map(|p| json!({
                "name": p.name,
                "version": p.version.to_string(),
                "stage": p.stage,
                "profile": profile,
            })),
            "steps": flow.steps.iter().map(Step::to_json).collect::<Vec<_>>(),
            "counts": counts(&flow.steps),
            "order": flow.order,
            "order_basis": "section order (sorted build/gen/**/plugin_desc.c) + FIFO Kahn over kind=init edges -- mirrors plugin_manager.c topo_sort()",
            "unsorted": flow.unsorted,
            "selftest_on": selftest_on,
        }),
    )
}

pub fn run(req: &Request) -> Response {
    match req.command.as_str() {
        "show-bricks" => cmd_bricks(req),
        "show-init-flow" => cmd_init_flow(req),
        other => {
            let mut d = crate::diag::Diags::new();
            d.usage(format!("unknown show command `{other}`"), "show-bricks | show-init-flow");
            Response::ok(d, json!({}))
        }
    }
}

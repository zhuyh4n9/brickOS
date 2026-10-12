//! **Snippet generation** for `dep-add` / `dep-rm` (ruling R-2).
//!
//! R-2 follows §9.1.1 rule 1 (later explicit decision): **do not write `brick.toml`**, only
//! print the TOML snippet (`data.snippet`) to paste into the declaration and its span
//! (`data.span`). Writing would erase human comments and break the "single source of truth";
//! switching cost = adding `--write` later.
//!
//! Flag discipline (§7.7.1): `dep` flags describe **structural dependencies** only — `--kind`
//! sets the edge type, `--phase` asserts the phase for `kind = "init"`; there is **no**
//! `--compat-gen` / `--frozen-gen` (`[[dep]]` never pins a generation, per F3).

use serde_json::{json, Value};

use crate::diag::Diags;
use crate::model::TreeLoad;
use crate::rules;
use crate::version::Range;

/// Generation result.
#[derive(Clone, Debug, Default)]
pub struct DepSnippet {
    /// Target `brick.toml` (relative to root).
    pub path: String,
    /// TOML path location, e.g. `dep[2]`.
    pub span: String,
    /// TOML snippet ready to paste.
    pub snippet: String,
    /// Whether `dep-rm` actually located the block.
    pub found: bool,
}

impl DepSnippet {
    pub fn to_data(&self) -> Value {
        json!({
            "snippet": self.snippet,
            "path": self.path,
            "span": self.span,
            "found": self.found,
        })
    }
}

fn render_dep_block(name: &str, range: &str, kind: &str, phase: Option<&str>, symbol: Option<&str>) -> String {
    let mut out = String::from("[[dep]]\n");
    out.push_str(&format!("name  = {}\n", crate::emit::toml_string(name)));
    out.push_str(&format!("range = {}\n", crate::emit::toml_string(range)));
    out.push_str(&format!("kind  = {}\n", crate::emit::toml_string(kind)));
    if let Some(p) = phase {
        out.push_str(&format!("phase = {}\n", crate::emit::toml_string(p)));
    }
    if let Some(s) = symbol {
        out.push_str(&format!("symbol = {}\n", crate::emit::toml_string(s)));
    }
    out
}

/// `dep-add`: `{plugin, dep, range?, kind, phase?}` → `data.snippet`.
pub fn dep_add(tree: &TreeLoad, args: &serde_json::Map<String, Value>) -> (Diags, Option<DepSnippet>) {
    let mut d = Diags::new();
    let plugin = args.get("plugin").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let dep = args.get("dep").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let kind = args
        .get("kind")
        .and_then(|v| v.as_str())
        .unwrap_or("init")
        .to_string();
    let phase = args
        .get("phase")
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .map(str::to_string);
    let range = args
        .get("range")
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .unwrap_or("*")
        .to_string();

    if plugin.is_empty() {
        d.shape("BRV-MF-0001", "", "", "args.plugin", "missing required argument `plugin`", "§7.7");
        return (d, None);
    }
    if dep.is_empty() {
        d.shape("BRV-MF-0001", &plugin, "", "args.dep", "missing required argument `dep`", "§7.7");
        return (d, None);
    }
    if !rules::valid_plugin_name(&dep) {
        d.shape("BRV-MF-0001", &plugin, "", "args.dep", format!("dependency name `{dep}` violates the name contract"), "contract: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return (d, None);
    }
    if !rules::in_list(rules::DEP_KINDS, &kind) {
        d.shape("BRV-MF-0001", &plugin, "", "args.kind", format!("unknown dependency kind `{kind}`"), &format!("valid values: {}(§7.1)", rules::list_of(rules::DEP_KINDS)));
        return (d, None);
    }
    if let Some(p) = &phase {
        if !rules::in_list(rules::PHASES, p) {
            d.shape("BRV-MF-0001", &plugin, "", "args.phase", format!("unknown phase `{p}`"), &format!("valid values: {}", rules::list_of(rules::PHASES)));
            return (d, None);
        }
        if kind != "init" {
            d.shape("BRV-MF-0001", &plugin, "", "args.phase", "`phase` only applies to `kind = \"init\"`", "§7.2: the assertion only describes the init completion point");
            return (d, None);
        }
    }
    if let Err(e) = Range::parse(&range) {
        d.shape(e.code, &plugin, "", "args.range", e.message, "§7.4: range has exactly 3 segments (4 is illegal)");
        return (d, None);
    }

    // Target plugin must exist in the tree (otherwise there is nowhere to paste).
    let Some(target) = tree.by_name(&plugin) else {
        d.usage(
            format!("plugin `{plugin}` not found in the plugin tree"),
            "run `brickie new` at the repo root first, or check that `root` points at the plugin tree",
        );
        return (d, None);
    };

    // Note: "dependency already declared" is not diagnosed — BRV-D8 allocates no code for
    // "duplicate declaration", and per ruling R-9 borrowing `BRV-MF-0001` (shape error) would
    // escalate a benign duplicate notice to exit code 2. Registered as a gap.

    let snippet = format!(
        "# paste into the `[[dep]]` section of {} (ruling R-2: brickie does not rewrite human-written files)\n{}",
        target.file,
        render_dep_block(&dep, &range, &kind, phase.as_deref(), None)
    );
    (
        d,
        Some(DepSnippet {
            path: target.file.clone(),
            span: format!("dep[{}]", target.deps.len()),
            snippet,
            found: true,
        }),
    )
}

/// `dep-rm`: `{plugin, dep, kind?}` → **location** of the block to delete + restored snippet.
pub fn dep_rm(tree: &TreeLoad, args: &serde_json::Map<String, Value>) -> (Diags, Option<DepSnippet>) {
    let mut d = Diags::new();
    let plugin = args.get("plugin").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let dep = args.get("dep").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let kind = args.get("kind").and_then(|v| v.as_str()).map(str::to_string);

    if plugin.is_empty() || dep.is_empty() {
        d.shape("BRV-MF-0001", &plugin, "", "args", "`dep-rm` requires both `plugin` and `dep` arguments", "§7.7");
        return (d, None);
    }
    if let Some(k) = &kind {
        if !rules::in_list(rules::DEP_KINDS, k) {
            d.shape("BRV-MF-0001", &plugin, "", "args.kind", format!("unknown dependency kind `{k}`"), &format!("valid values: {}", rules::list_of(rules::DEP_KINDS)));
            return (d, None);
        }
    }
    let Some(target) = tree.by_name(&plugin) else {
        d.usage(format!("plugin `{plugin}` not found in the plugin tree"), "check that `root` points at the plugin tree");
        return (d, None);
    };
    let idx = target.deps.iter().position(|x| {
        x.name == dep && kind.as_deref().map(|k| k == x.kind).unwrap_or(true)
    });
    match idx {
        None => {
            let snippet = format!(
                "# not found: {} has no `[[dep]]` block for `{}`{}\n",
                target.file,
                dep,
                kind.as_deref().map(|k| format!("(kind = {k})")).unwrap_or_default()
            );
            (
                d,
                Some(DepSnippet {
                    path: target.file.clone(),
                    span: String::new(),
                    snippet,
                    found: false,
                }),
            )
        }
        Some(i) => {
            let x = &target.deps[i];
            let snippet = format!(
                "# delete the `dep[{i}]` block in {} (ruling R-2: locate only, do not rewrite human-written files)\n{}",
                target.file,
                render_dep_block(
                    &x.name,
                    &x.range_raw,
                    &x.kind,
                    x.phase.as_deref(),
                    x.symbol.as_deref(),
                )
            );
            (
                d,
                Some(DepSnippet {
                    path: target.file.clone(),
                    span: format!("dep[{i}]"),
                    snippet,
                    found: true,
                }),
            )
        }
    }
}

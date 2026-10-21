//! Symbol-level dependency audit over the per-plugin `.a` archives (ADR-0038 §3).
//!
//! `brickie build` archives every plugin's objects into `build/lib/<plugin>.a`, then runs
//! `nm -A -g` over all archives and hands the text to this module (through `judge`, so the
//! **criteria stay in core** and L5 only executes). The archive is the right evidence: text
//! scanning cannot see a symbol a macro or a vendor translation unit pulls in, and it cannot see
//! which data symbol actually left a plugin.
//!
//! Rules (the two source disciplines, symbol half):
//!
//! * **R2 — a global never leaves the plugin**: an object symbol (`B D G S R C`) defined by a
//!   `native` plugin and referenced from another plugin is a violation *regardless of declared
//!   dependencies*; for other classes the dependency rule below decides.
//! * **R3 — dependencies must be declared explicitly**: a strong undefined symbol used by `A` must
//!   be defined by `A`, by a plugin in `A`'s **direct** dependency set (or re-exported by a
//!   directly declared skin), or be a symbol whose **declaration** lives in a header `A` is
//!   authorized to include (the `core`-declares / `platform`-implements contract:
//!   `br_console_*`, `br_plat_ticks_*`).
//!
//! Skipped on purpose: weak symbols (`W V w v` — `br_plugin_gen_total` is defined weak by every
//! descriptor), and undefined symbols with no definition in any archive (linker labels such as
//! `__br_plugins_start`, libgcc helpers).

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

use crate::diag::{Diag, Diags, Severity};
use crate::model;
use crate::proto::Response;
use serde_json::{json, Value};

/// `nm` type letters that define a **data** object.
const DATA_TYPES: &[char] = &['B', 'D', 'G', 'S', 'R', 'C'];
/// `nm` type letters for weak symbols (defined or undefined): never audited.
const WEAK_TYPES: &[char] = &['W', 'V', 'w', 'v'];

/// One symbol record parsed from `nm -A -g` output.
struct NmLine {
    archive: String,
    member: String,
    symbol: String,
    /// `nm` type letter, `'U'` for undefined (strong) / `'w'`,`'v'` for undefined weak.
    kind: char,
}

fn parse_nm(text: &str) -> Vec<NmLine> {
    let mut out = Vec::new();
    for raw in text.lines() {
        let line = raw.trim_end();
        if line.is_empty() {
            continue;
        }
        // `<archive>:<member>:<value or spaces> <type> <name>`
        let mut it = line.rsplitn(3, ':');
        let rest = match it.next() {
            Some(r) => r,
            None => continue,
        };
        let member = it.next().unwrap_or_default().to_string();
        let archive = it.next().unwrap_or_default().to_string();
        let toks: Vec<&str> = rest.split_whitespace().collect();
        let (kind, symbol) = match toks.len() {
            2 => (toks[0].chars().next().unwrap_or('?'), toks[1]),
            3.. => (
                toks[toks.len() - 2].chars().next().unwrap_or('?'),
                toks[toks.len() - 1],
            ),
            _ => continue,
        };
        out.push(NmLine {
            archive,
            member,
            symbol: symbol.to_string(),
            kind,
        });
    }
    out
}

/// `build/lib/<plugin>.a` ⇒ plugin name.
fn plugin_of_archive(archive: &str) -> Option<String> {
    let a = archive.trim();
    let a = a.strip_prefix("./").unwrap_or(a);
    let a = a.strip_prefix("build/lib/").unwrap_or(a);
    let a = a.strip_suffix(".a")?;
    Some(a.to_string())
}

/// Every identifier appearing in a plugin's **public** headers: the "declaration surface" test used
/// for symbols that a provider implements but another plugin declares (`core` declares
/// `br_console_write`, `platform` defines it).
fn header_idents(root: &Path, p: &model::Plugin) -> BTreeSet<String> {
    let mut out: BTreeSet<String> = BTreeSet::new();
    let Some(b) = &p.build else {
        return out;
    };
    for inc in &b.includes {
        let dir = root.join(&p.name).join(inc);
        collect_idents(&dir, &mut out, 0);
    }
    out
}

fn collect_idents(dir: &Path, out: &mut BTreeSet<String>, depth: usize) {
    if depth > 16 {
        return;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    let mut items: Vec<std::path::PathBuf> = entries.flatten().map(|e| e.path()).collect();
    items.sort();
    for path in items {
        if path.is_dir() {
            collect_idents(&path, out, depth + 1);
        } else if path.extension().and_then(|s| s.to_str()) == Some("h") {
            let Ok(text) = std::fs::read_to_string(&path) else {
                continue;
            };
            for tok in crate::srcscan::strip(&text)
                .split(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
            {
                if !tok.is_empty()
                    && tok
                        .chars()
                        .next()
                        .map(|c| c.is_ascii_alphabetic() || c == '_')
                        .unwrap_or(false)
                {
                    out.insert(tok.to_string());
                }
            }
        }
    }
}

/// Reserved `judge` name: `brickie build`'s audit step declares it, L5 passes the `nm` text.
pub const JUDGE_NAME: &str = "symbol-audit";

/// `judge` command body for the reserved name `symbol-audit`.
pub fn judge(req: &crate::proto::Request) -> Response {
    let root = std::path::PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let mut diags = Diags::new();
    let log = req
        .context("log")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default();
    let lines = parse_nm(log);
    if lines.is_empty() {
        // No symbols at all means the `nm` step produced nothing: say so instead of passing.
        diags.push(
            Diag::new(
                Severity::Error,
                "symbol audit received no `nm` output: cannot prove the dependency disciplines",
            )
            .code(CODE_AUDIT_EMPTY)
            .target("build")
            .hint("the audit step must run `nm -A -g` over `build/lib/*.a`; check the `nm` tool resolution"),
        );
        return Response::ok(diags, json!({"failed": 1}));
    }

    // symbol -> (defining plugin, nm type) for strong definitions
    let mut defined: BTreeMap<String, (String, char)> = BTreeMap::new();
    // per archive: undefined symbols
    let mut undefined: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    for l in &lines {
        let Some(plugin) = plugin_of_archive(&l.archive) else {
            continue;
        };
        if l.kind == 'U' {
            undefined
                .entry(plugin)
                .or_default()
                .insert(l.symbol.clone());
        } else if l.kind == 'u' {
            // undefined unique symbol (GNU extension): treat like undefined strong
            undefined
                .entry(plugin)
                .or_default()
                .insert(l.symbol.clone());
        } else if !WEAK_TYPES.contains(&l.kind) && l.kind != '?' {
            defined.insert(l.symbol.clone(), (plugin, l.kind));
        }
    }

    let mut header_cache: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    let auth_of = |name: &str| model::dep_direct_auth(&tree, name);
    let mut idents_for = |name: &str| -> BTreeSet<String> {
        if let Some(v) = header_cache.get(name) {
            return v.clone();
        }
        let v = match tree.by_name(name) {
            Some(p) => header_idents(&root, p),
            None => BTreeSet::new(),
        };
        header_cache.insert(name.to_string(), v.clone());
        v
    };

    let mut failed = 0usize;
    let mut violations: Vec<Value> = Vec::new();
    for (consumer, undef) in &undefined {
        let Some(cp) = tree.by_name(consumer) else {
            continue;
        };
        let auth = auth_of(consumer);
        // Identifiers declared in headers this consumer may include: itself + authorized providers.
        let mut visible: BTreeSet<String> = idents_for(consumer);
        for a in &auth {
            for id in idents_for(a) {
                visible.insert(id);
            }
        }
        // Documented exception (ADR-0038 §5①): the core plugin is platform-agnostic -- it uses the
        // platform *contract* (`br/platform/**`: the console hooks, `br_plat_ticks_*`) without
        // naming a platform plugin, and the direction table forbids it from declaring one.
        if cp.plugin_type == "core" {
            for p in tree.plugins.iter().filter(|p| p.plugin_type == "platform") {
                for id in idents_for(&p.name) {
                    visible.insert(id);
                }
            }
        }
        for symbol in undef {
            let Some((owner, kind)) = defined.get(symbol) else {
                continue; // linker label / libgcc helper: the link step owns those
            };
            if owner == consumer {
                continue;
            }
            let owner_plugin = tree.by_name(owner);
            // R2 (symbol half): a `native` plugin's data symbol must not be reachable from
            // another plugin at all -- an object is not an interface.
            if DATA_TYPES.contains(kind)
                && owner_plugin
                    .map(|p| p.api_type == "native")
                    .unwrap_or(false)
            {
                failed += 1;
                violations.push(json!({
                    "code": CODE_SRC_GLOBAL,
                    "consumer": consumer,
                    "provider": owner,
                    "symbol": symbol,
                    "rule": "data-symbol",
                }));
                diags.push(
                    Diag::new(
                        Severity::Error,
                        format!(
                            "`{consumer}` references data symbol `{symbol}` defined by `{owner}`: a global must never leave its plugin"
                        ),
                    )
                    .code(CODE_SRC_GLOBAL)
                    .target(consumer)
                    .file(format!("build/lib/{consumer}.a"))
                    .span("nm")
                    .hint("ADR-0038 / R2: expose an accessor function from the owner (`kind = \"func\"`) instead of exporting the object"),
                );
                continue;
            }
            if auth.contains(owner) {
                continue;
            }
            // The declaration lives in a header the consumer may include: the provider implements
            // an interface somebody else declares (platform implements core's console contract).
            if visible.contains(symbol) {
                continue;
            }
            failed += 1;
            violations.push(json!({
                "code": CODE_DEP_UNDECLARED,
                "consumer": consumer,
                "provider": owner,
                "symbol": symbol,
                "rule": "undeclared-dep",
            }));
            diags.push(
                Diag::new(
                    Severity::Error,
                    format!(
                        "`{consumer}` uses symbol `{symbol}` defined by `{owner}`, but declares no direct dependency on it"
                    ),
                )
                .code(CODE_DEP_UNDECLARED)
                .target(consumer)
                .file(format!("build/lib/{consumer}.a"))
                .span("nm")
                .hint(format!(
                    "ADR-0038 / R3: add `[[dep]] {owner}` (or a skin that re-exports it) to {consumer}/brick.toml"
                )),
            );
            let _ = cp;
        }
    }

    Response::ok(diags, json!({"failed": failed, "violations": violations}))
}

/// The audit produced no evidence (empty `nm` output).
pub const CODE_AUDIT_EMPTY: &str = "BRV-BLD-0015";
/// A symbol is used from a plugin that is not a direct dependency (text half: `check --src`).
pub const CODE_DEP_UNDECLARED: &str = "BRV-DEP-0012";
/// A global (object) leaves its plugin.
pub const CODE_SRC_GLOBAL: &str = "BRV-SRC-0002";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_nm_A_g_lines() {
        let text = "build/lib/a.a:m.o:                 U foo\n\
                    build/lib/a.a:m.o:0000000000000000 T bar\n\
                    build/lib/b.a:n.o:0000000000000008 D g_var\n";
        let v = parse_nm(text);
        assert_eq!(v.len(), 3);
        assert_eq!(v[0].symbol, "foo");
        assert_eq!(v[0].kind, 'U');
        assert_eq!(v[1].kind, 'T');
        assert_eq!(v[2].symbol, "g_var");
        assert_eq!(plugin_of_archive(&v[2].archive).as_deref(), Some("b"));
    }
}

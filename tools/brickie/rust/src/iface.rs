//! Version surface and interface publish surface: `ver-show` / `ver-bump` / `iface-*`.
//!
//! Landing point for V-4/V-5/V-6/V-11/V-15/V-16/V-17:
//!
//! - **IFACE-IR canonicalization** (the 10 rules of §6.2) + `sha256` domain-separated hash;
//! - **change set** (§6.3): `ADDED`/`EXTENDED`/`CHANGED`/`SEMANTIC`/`REMOVED`/`STATUS`/
//!   `FREEZE`/`NONE`; append vs modify decided by **entry internal structure** (§5.4);
//! - **impact report**: `dependents.direct/transitive/unsatisfied`;
//! - **version advance** (§5.2 row by row) and **idempotence** (§6.5): unchanged surface ⇒ no-op;
//! - **unfreeze window** (§5.3.4) + `unfreeze` records baseline (ruling R-7), `refreeze` compares;
//! - **write-path discipline** (§9.1.1): emit machine files only, `brick.toml` is never rewritten.
//!
//! ## Implementation rulings (see rust/README.md §10)
//! - **S-8**: **first publish = record creation** (no old snapshot ⇒ advance no segment, only record the current declared version and hash);
//!   later publishes advance per change set. Otherwise every new unit would burn a MINOR on its first publish.
//! - **S-9**: validity of state-machine commands (`deprecate`/`undeprecate`) is judged by **snapshot status** (publish record);
//!   the new surface is built from the declaration ⇒ repeated runs are **no-ops** (idempotent).
//! - **S-10**: the "dependents' compat_gen still matches" gate of `publish --profile release` follows the §6.5 table:
//!   a `COMPAT_GEN` transition with any `unsatisfied` dependent ⇒ refuse to write (`BRV-VER-0001`).
//! - **S-11**: `SEMANTIC` changes have **no declaration-surface input** in v0.1 (pure behavioral semantics are not observable from TOML)
//!   ⇒ the category is never produced; `BRV-IFACE-0010` has nothing to report; logged as a gap.
//! - **S-12**: `iface status` **emits no files** (it is a gate, not a repair command); on mismatch it puts
//!   the "expected snapshot" in `data.expected_snapshot` for manual use.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::diag::{Diag, Diags, Severity};
use crate::emit::{self, TDoc, TTable, TValue};
use crate::model::{self, Plugin, SnapshotEntry, TreeLoad};
use crate::proto::{Request, Response};
use crate::version::{self, Version};

/// Hash domain-separation prefix for IFACE-IR (rule 10: `hash_rev` enters the domain separator).
pub const HASH_REV: &str = "brickie-iface-ir/1";

// ================================================================== IFACE-IR

fn is_ident_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// Replace `from` with `to` in `text` where `from` appears at **identifier boundaries**.
fn replace_token(text: &str, from: &str, to: &str) -> String {
    if from.is_empty() {
        return text.to_string();
    }
    let b = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut i = 0usize;
    while i < text.len() {
        if text[i..].starts_with(from) {
            let before_ok = i == 0 || !is_ident_byte(b[i - 1]);
            let end = i + from.len();
            let after_ok = end >= text.len() || !is_ident_byte(b[end]);
            if before_ok && after_ok {
                out.push_str(to);
                i = end;
                continue;
            }
        }
        let ch = text[i..].chars().next().expect("i is on a char boundary");
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

/// Rule 4: expand type aliases to canonical names (iterate to fixpoint, capped at 8 rounds against alias cycles).
pub fn expand_typedefs(text: &str, typedefs: &[(String, String)]) -> String {
    let mut cur = text.to_string();
    for _ in 0..8 {
        let mut changed = false;
        for (alias, canon) in typedefs {
            let next = replace_token(&cur, alias, canon);
            if next != cur {
                cur = next;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    cur
}

/// Rule 2: normalize whitespace (single space / no trailing); also removes layout differences around operators.
pub fn normalize_ws(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut prev_space = false;
    for c in s.trim().chars() {
        if c.is_whitespace() {
            prev_space = true;
            continue;
        }
        let tight_before = matches!(c, '*' | '&' | ',' | ')' | '(' | '[' | ']');
        let tight_after_prev = out.ends_with('*') || out.ends_with('&') || out.ends_with('(') || out.ends_with('[');
        if prev_space && !tight_before && !tight_after_prev && !out.is_empty() {
            out.push(' ');
        }
        out.push(c);
        prev_space = false;
    }
    out
}

/// Rule 3: **parameter names do not enter** the sig (keep types and order only); kept with `--strict-params`.
pub fn strip_param_names(sig: &str) -> String {
    let open = match sig.find('(') {
        Some(i) => i,
        None => return sig.to_string(),
    };
    let close = match sig.rfind(')') {
        Some(i) if i > open => i,
        _ => return sig.to_string(),
    };
    let head = sig[..open].trim();
    let params = &sig[open + 1..close];
    let tail = sig[close + 1..].trim();
    let mut parts: Vec<String> = Vec::new();
    for raw in params.split(',') {
        let p = raw.trim();
        if p.is_empty() {
            parts.push(String::new());
            continue;
        }
        if p == "void" {
            parts.push("void".to_string());
            continue;
        }
        let toks: Vec<&str> = p.split_whitespace().collect();
        if toks.len() >= 2 {
            let last = toks[toks.len() - 1];
            let is_name = !last.is_empty()
                && last.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
                && last
                    .chars()
                    .next()
                    .map(|c| c.is_ascii_alphabetic() || c == '_')
                    .unwrap_or(false);
            let prev = toks[toks.len() - 2];
            let prev_is_ptr = prev.ends_with('*') || prev.ends_with('&');
            let prev_is_kw = matches!(
                prev,
                "const"
                    | "volatile"
                    | "struct"
                    | "union"
                    | "enum"
                    | "unsigned"
                    | "signed"
                    | "long"
                    | "short"
                    | "void"
            );
            if is_name && !prev_is_ptr && !prev_is_kw {
                parts.push(toks[..toks.len() - 1].join(" "));
                continue;
            }
        }
        parts.push(toks.join(" "));
    }
    let joined = parts.join(",");
    if tail.is_empty() {
        format!("{head}({joined})")
    } else {
        format!("{head}({joined}){tail}")
    }
}

/// Canonicalize the `sig` of a `func`.
pub fn canonical_sig(raw: &str, typedefs: &[(String, String)], strict_params: bool) -> String {
    let expanded = expand_typedefs(raw, typedefs);
    let stripped = if strict_params {
        expanded
    } else {
        strip_param_names(&expanded)
    };
    normalize_ws(&stripped)
}

/// Canonicalize the `layout` of a `type`.
pub fn canonical_layout(raw: &str, typedefs: &[(String, String)]) -> String {
    normalize_ws(&expand_typedefs(raw, typedefs))
}

/// One interface-surface entry (shared by declaration and snapshot surfaces).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Entry {
    pub kind: String,
    pub name: String,
    pub sig: Option<String>,
    pub layout: Option<String>,
    pub value: Option<String>,
    pub ops: Vec<String>,
    pub status: String,
}

impl Entry {
    pub fn from_export(e: &model::ExportEntry) -> Self {
        Entry {
            kind: e.kind.clone(),
            name: e.name.clone(),
            sig: e.sig.clone(),
            layout: e.layout.clone(),
            value: e.value.clone(),
            ops: e.ops.clone().unwrap_or_default(),
            status: e.status.clone(),
        }
    }

    pub fn from_snapshot(e: &SnapshotEntry) -> Self {
        Entry {
            kind: e.kind.clone(),
            name: e.name.clone(),
            sig: e.sig.clone(),
            layout: e.layout.clone(),
            value: e.value.clone(),
            ops: e.ops.clone().unwrap_or_default(),
            status: e.status.clone(),
        }
    }

    pub fn key(&self) -> (String, String) {
        (self.kind.clone(), self.name.clone())
    }

    /// Compare only "surface" fields (`status` does not enter the hash; see rule 6).
    pub fn same_surface(&self, other: &Entry) -> bool {
        self.kind == other.kind
            && self.name == other.name
            && self.sig == other.sig
            && self.layout == other.layout
            && self.value == other.value
            && self.ops == other.ops
    }
}

/// Rules 1 + 5 + 6 + 7 + 9: canonical body (sorting, field selection, enums in declaration order).
pub fn canonical_surface_text(
    id: &str,
    entries: &[Entry],
    typedefs: &[(String, String)],
    strict_params: bool,
) -> String {
    let mut sorted: Vec<&Entry> = entries.iter().collect();
    sorted.sort_by(|a, b| (&a.kind, &a.name).cmp(&(&b.kind, &b.name)));
    let mut out = String::new();
    out.push_str(HASH_REV);
    out.push('\n');
    // Rule 10: domain separator = hash_rev + unit id (prevents cross-unit collisions).
    out.push_str(id);
    out.push('\n');
    for e in sorted {
        out.push_str("-- entry\n");
        out.push_str(&format!("kind {}\n", e.kind));
        out.push_str(&format!("name {}\n", e.name));
        if let Some(s) = &e.sig {
            out.push_str(&format!(
                "sig {}\n",
                canonical_sig(s, typedefs, strict_params)
            ));
        }
        if let Some(l) = &e.layout {
            out.push_str(&format!("layout {}\n", canonical_layout(l, typedefs)));
        }
        // Rule 9: macro/var use value, enum uses the member list (declaration order), service uses the ops slot summary.
        if let Some(v) = &e.value {
            out.push_str(&format!("value {}\n", normalize_ws(v)));
        }
        if !e.ops.is_empty() {
            out.push_str(&format!("ops {}\n", e.ops.join(",")));
        }
    }
    out
}

/// `sha256:<64 hex>` (rule 8: SHA-256 over UTF-8 canonical text).
pub fn surface_hash(
    id: &str,
    entries: &[Entry],
    typedefs: &[(String, String)],
    strict_params: bool,
) -> String {
    let text = canonical_surface_text(id, entries, typedefs, strict_params);
    let digest = Sha256::digest(text.as_bytes());
    let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
    format!("sha256:{hex}")
}

/// Display takes the first 12 hex chars (§5.1).
pub fn short_hash(h: Option<&str>) -> String {
    match h {
        None => String::new(),
        Some(s) => {
            let hex = s.strip_prefix("sha256:").unwrap_or(s);
            hex.chars().take(12).collect()
        }
    }
}

// ================================================================== surface model

/// The "surface" of an interface unit (may come from a declaration or from a snapshot).
#[derive(Clone, Debug, Default)]
pub struct Surface {
    pub id: String,
    pub provider: String,
    pub unit: String,
    pub file: String,
    pub export_index: usize,
    pub api_iface: String,
    pub form: String,
    pub version: Version,
    pub compat_gen: u64,
    pub freeze_state: String,
    pub status: String,
    /// `brick.toml [[export]].hash` (record in the declaration; used by `BRV-IFACE-0012`).
    pub declared_hash: Option<String>,
    pub reexport_of: Vec<String>,
    pub symbols: Vec<String>,
    pub entries: Vec<Entry>,
    pub typedefs: Vec<(String, String)>,
}

impl Surface {
    pub fn from_declaration(tree: &TreeLoad, provider: &str, unit: &str, idx: usize) -> Surface {
        let p = tree.by_name(provider).expect("resolve guarantees existence");
        let e = &p.exports[idx];
        Surface {
            id: format!("{provider}#{unit}"),
            provider: provider.to_string(),
            unit: unit.to_string(),
            file: p.file.clone(),
            export_index: idx,
            api_iface: e.api_iface.clone(),
            form: e.form.clone(),
            version: e.version.unwrap_or_default(),
            compat_gen: e.compat_gen.unwrap_or(0),
            freeze_state: e
                .freeze_state
                .clone()
                .unwrap_or_else(|| "unfrozen".to_string()),
            status: e.status.clone().unwrap_or_else(|| "experimental".to_string()),
            declared_hash: e.hash.clone().filter(|h| !h.is_empty()),
            reexport_of: e.reexport_of.clone(),
            symbols: e.symbols.clone(),
            entries: e.entries.iter().map(Entry::from_export).collect(),
            typedefs: p.typedefs.clone(),
        }
    }

    pub fn from_snapshot(s: &model::Snapshot) -> Surface {
        Surface {
            id: s.id.clone(),
            provider: s.provider.clone(),
            unit: s.unit.clone(),
            file: s.file.clone(),
            export_index: 0,
            api_iface: s.api_iface.clone().unwrap_or_default(),
            form: s.form.clone().unwrap_or_default(),
            version: s.version.unwrap_or_default(),
            compat_gen: s.compat_gen.unwrap_or(0),
            freeze_state: s
                .freeze_state
                .clone()
                .unwrap_or_else(|| "unfrozen".to_string()),
            status: s.status.clone().unwrap_or_else(|| "experimental".to_string()),
            declared_hash: s.hash.clone().filter(|h| !h.is_empty()),
            reexport_of: Vec::new(),
            symbols: Vec::new(),
            entries: s.entries.iter().map(Entry::from_snapshot).collect(),
            typedefs: Vec::new(),
        }
    }

    pub fn hash(&self, strict_params: bool) -> String {
        surface_hash(&self.id, &self.entries, &self.typedefs, strict_params)
    }

    /// A snapshot (for reading).
    pub fn to_snapshot(&self, hash: &str, strict_params: bool, baseline: Option<(&str, &[Entry])>) -> model::Snapshot {
        model::Snapshot {
            file: snapshot_path(&self.provider, &self.unit),
            id: self.id.clone(),
            provider: self.provider.clone(),
            unit: self.unit.clone(),
            api_iface: Some(self.api_iface.clone()),
            form: Some(self.form.clone()),
            version: Some(self.version),
            compat_gen: Some(self.compat_gen),
            freeze_state: Some(self.freeze_state.clone()),
            status: Some(self.status.clone()),
            hash: Some(hash.to_string()),
            hash_scope: Some("decl".to_string()),
            truth: Some("decl".to_string()),
            not_abi: true,
            strict_params: Some(strict_params),
            baseline_hash: baseline.map(|(h, _)| h.to_string()),
            baseline_entries: baseline
                .map(|(_, es)| es.iter().map(entry_to_snapshot).collect())
                .unwrap_or_default(),
            entries: self.entries.iter().map(entry_to_snapshot).collect(),
        }
    }
}

pub fn entry_to_snapshot(e: &Entry) -> SnapshotEntry {
    SnapshotEntry {
        kind: e.kind.clone(),
        name: e.name.clone(),
        sig: e.sig.clone(),
        layout: e.layout.clone(),
        value: e.value.clone(),
        ops: if e.ops.is_empty() {
            None
        } else {
            Some(e.ops.clone())
        },
        status: e.status.clone(),
    }
}

/// Ruling R-11: `api/iface/<provider path segments>/<unit>.toml`.
pub fn snapshot_path(provider: &str, unit: &str) -> String {
    let mut p = String::from("api/iface");
    for seg in provider.split('/') {
        if !seg.is_empty() {
            p.push('/');
            p.push_str(seg);
        }
    }
    p.push('/');
    p.push_str(unit);
    p.push_str(".toml");
    p
}

// ================================================================== change set

/// One change entry (§6.3).
#[derive(Clone, Debug)]
pub struct Change {
    pub kind: &'static str,
    pub name: String,
    pub from: Option<String>,
    pub to: Option<String>,
    /// Governance status of the old entry (used to decide "must unfreeze").
    pub old_status: String,
    pub note: String,
}

impl Change {
    pub fn to_json(&self) -> Value {
        json!({
            "kind": self.kind,
            "name": self.name,
            "entry": self.name,
            "from": self.from,
            "to": self.to,
            "status": self.old_status,
            "note": self.note,
        })
    }

    /// §6.3: `CHANGED`/`REMOVED` touching an already `frozen`/`deprecated` entry ⇒ must unfreeze.
    pub fn is_hard(&self) -> bool {
        matches!(self.kind, "CHANGED" | "REMOVED")
            && matches!(self.old_status.as_str(), "frozen" | "deprecated")
    }
}

/// Compute the change set from `old` (usually the snapshot) → `new` (usually declaration + this change).
pub fn change_set(old: &Surface, new: &Surface) -> Vec<Change> {
    let mut out: Vec<Change> = Vec::new();
    let old_map: BTreeMap<(String, String), &Entry> =
        old.entries.iter().map(|e| (e.key(), e)).collect();
    let new_map: BTreeMap<(String, String), &Entry> =
        new.entries.iter().map(|e| (e.key(), e)).collect();

    for (k, ne) in &new_map {
        match old_map.get(k) {
            None => out.push(Change {
                kind: "ADDED",
                name: ne.name.clone(),
                from: None,
                to: Some(ne.name.clone()),
                old_status: String::new(),
                note: "added entry (append-only, no unfreeze)".to_string(),
            }),
            Some(oe) => {
                if !oe.same_surface(ne) {
                    let kind = classify_modify(oe, ne);
                    out.push(Change {
                        kind,
                        name: ne.name.clone(),
                        from: describe(oe),
                        to: describe(ne),
                        old_status: oe.status.clone(),
                        note: match kind {
                            "EXTENDED" => "legal append to an already frozen entry (member added at end of enum)".to_string(),
                            _ => "sig/layout/value/ops changed on an existing entry (§5.4)".to_string(),
                        },
                    });
                }
                if oe.status != ne.status {
                    out.push(Change {
                        kind: "STATUS",
                        name: ne.name.clone(),
                        from: Some(oe.status.clone()),
                        to: Some(ne.status.clone()),
                        old_status: oe.status.clone(),
                        note: "governance status transition (surface unchanged)".to_string(),
                    });
                }
            }
        }
    }
    for (k, oe) in &old_map {
        if !new_map.contains_key(k) {
            out.push(Change {
                kind: "REMOVED",
                name: oe.name.clone(),
                from: Some(oe.name.clone()),
                to: None,
                old_status: oe.status.clone(),
                note: "entry disappeared (must be deprecated + deprecation period met)".to_string(),
            });
        }
    }
    if old.freeze_state != new.freeze_state {
        out.push(Change {
            kind: "FREEZE",
            name: new.id.clone(),
            from: Some(old.freeze_state.clone()),
            to: Some(new.freeze_state.clone()),
            old_status: String::new(),
            note: "freeze_state transition".to_string(),
        });
    }
    out.sort_by(|a, b| (a.kind, a.name.as_str()).cmp(&(b.kind, b.name.as_str())));
    out
}

/// §5.4 append vs modify decision (based on **entry internal structure**).
pub fn classify_modify(oe: &Entry, ne: &Entry) -> &'static str {
    match oe.kind.as_str() {
        // member appended at the end of an enum ⇒ append; reorder/value change/remove ⇒ modify.
        "enum" => {
            let om: Vec<&str> = oe.value.as_deref().unwrap_or("").split(',').collect();
            let nm: Vec<&str> = ne.value.as_deref().unwrap_or("").split(',').collect();
            if nm.len() > om.len() && nm[..om.len()] == om[..] {
                "EXTENDED"
            } else {
                "CHANGED"
            }
        }
        // service: a new service name = a new entry; adding a slot to an existing ops table = modify.
        "service" => "CHANGED",
        // type (struct): add/remove/reorder fields = modify (D22).
        "type" => "CHANGED",
        // macro/var: changing the value of an existing macro ⇒ modify.
        "macro" | "var" => "CHANGED",
        // func: signature change ⇒ modify (pure semantic changes are not observable from the declaration surface; see S-11).
        _ => "CHANGED",
    }
}

fn describe(e: &Entry) -> Option<String> {
    if let Some(s) = &e.sig {
        return Some(s.clone());
    }
    if let Some(l) = &e.layout {
        return Some(l.clone());
    }
    if let Some(v) = &e.value {
        return Some(v.clone());
    }
    if !e.ops.is_empty() {
        return Some(e.ops.join(","));
    }
    None
}

// ================================================================== impact report

/// Reverse-dependency report (§6.3).
#[derive(Clone, Debug, Default)]
pub struct Dependents {
    pub direct: Vec<String>,
    pub transitive: Vec<String>,
    pub unsatisfied: Vec<Value>,
}

impl Dependents {
    pub fn to_json(&self) -> Value {
        json!({
            "direct": self.direct,
            "transitive": self.transitive,
            "unsatisfied": self.unsatisfied,
        })
    }
}

/// Compute the dependents of a `provider` (per declared `[[dep]]` / `requires_iface` / `reexport_of`).
///
/// `new_cg` / `new_ver` are used to decide `unsatisfied` (mismatched) dependents.
pub fn dependents_of(
    tree: &TreeLoad,
    provider: &str,
    new_cg: u64,
    new_ver: Version,
) -> Dependents {
    let mut direct: BTreeSet<String> = BTreeSet::new();
    let mut unsatisfied: Vec<Value> = Vec::new();
    for p in &tree.plugins {
        if p.name == provider {
            continue;
        }
        let mut is_direct = false;
        for d in &p.deps {
            if d.name == provider {
                is_direct = true;
            }
        }
        for r in &p.compat.requires_iface {
            if r.id.split_once('#').map(|(pn, _)| pn) == Some(provider) {
                is_direct = true;
                let why = if !version::compat_gen_matches(r.compat_gen, new_cg) {
                    format!("compat_gen changed from {} to {new_cg}, reconfirmation needed", r.compat_gen)
                } else if !r.range.matches(new_ver.triple()) {
                    format!(
                        "range `{}` no longer satisfies the new version {}",
                        r.range_raw, new_ver
                    )
                } else {
                    continue;
                };
                unsatisfied.push(json!({
                    "plugin": p.name,
                    "requires": {"id": r.id, "compat_gen": r.compat_gen, "range": r.range_raw},
                    "why": why,
                }));
            }
        }
        for e in &p.exports {
            if e.reexport_of
                .iter()
                .any(|r| r.split_once('#').map(|(pn, _)| pn) == Some(provider))
            {
                is_direct = true;
            }
        }
        if is_direct {
            direct.insert(p.name.clone());
        }
    }

    // Transitive closure (reverse): who depends on the direct set.
    let mut transitive: BTreeSet<String> = direct.clone();
    let mut grew = true;
    while grew {
        grew = false;
        for p in &tree.plugins {
            if transitive.contains(&p.name) {
                continue;
            }
            if p.deps.iter().any(|d| transitive.contains(&d.name)) {
                transitive.insert(p.name.clone());
                grew = true;
            }
        }
    }
    Dependents {
        direct: direct.into_iter().collect(),
        transitive: transitive.into_iter().collect(),
        unsatisfied,
    }
}

// ================================================================== environment

/// Command load environment (IO happens only here).
pub struct Env {
    pub root: PathBuf,
    pub tree: TreeLoad,
    pub snaps: Vec<model::Snapshot>,
    pub diags: Diags,
}

impl Env {
    pub fn load(root: &Path) -> Env {
        let mut diags = Diags::new();
        let tree = model::load_tree(root);
        diags.merge(tree.diags.clone());
        let snaps = model::load_snapshots(root, &mut diags);
        Env {
            root: root.to_path_buf(),
            tree,
            snaps,
            diags,
        }
    }

    pub fn snap(&self, id: &str) -> Option<&model::Snapshot> {
        self.snaps.iter().find(|s| s.id == id)
    }
}

/// Resolve `<provider>#<unit>` or a plugin name (ruling R-3).
fn resolve(tree: &TreeLoad, id: &str) -> Result<(String, String, usize), String> {
    if let Some((prov, unit)) = id.split_once('#') {
        let p = tree
            .by_name(prov)
            .ok_or_else(|| format!("plugin `{prov}` is not in the plugin tree"))?;
        let idx = p
            .exports
            .iter()
            .position(|e| e.name == unit)
            .ok_or_else(|| format!("plugin `{prov}` has no interface unit `{unit}`"))?;
        Ok((prov.to_string(), unit.to_string(), idx))
    } else {
        let p = tree
            .by_name(id)
            .ok_or_else(|| format!("plugin `{id}` is not in the plugin tree"))?;
        if p.exports.len() != 1 {
            return Err(format!(
                "plugin `{id}` has {} `[[export]]` entries, cannot determine a unique unit; use `<provider>#<unit>` (ruling R-3)",
                p.exports.len()
            ));
        }
        Ok((id.to_string(), p.exports[0].name.clone(), 0))
    }
}

/// Note path existence (relative to `root` or the current working directory).
fn note_exists(root: &Path, note: &str) -> bool {
    let p = PathBuf::from(note);
    if p.is_absolute() {
        return p.is_file();
    }
    root.join(note).is_file() || p.is_file()
}

fn machine_file(path: &str, content: String) -> Value {
    json!({"path": path, "kind": "machine", "content": content, "mode": "0644"})
}

/// Effective freeze state: `unfreezing` in either place means the unfreeze window is open.
pub fn effective_freeze(decl: &str, snap: Option<&str>) -> String {
    if decl == "unfreezing" || snap == Some("unfreezing") {
        "unfreezing".to_string()
    } else if let Some(s) = snap {
        s.to_string()
    } else {
        decl.to_string()
    }
}

/// Build the body of `brickie.lock` (plugins from the tree, units from published snapshots + this update).
fn build_lock_content(
    tree: &TreeLoad,
    root: &Path,
    profile: &str,
    updated: Option<model::Snapshot>,
) -> String {
    let mut scratch = Diags::new();
    let existing = model::load_snapshots(root, &mut scratch);
    let mut by_id: BTreeMap<String, model::Snapshot> =
        existing.into_iter().map(|s| (s.id.clone(), s)).collect();
    if let Some(u) = updated {
        by_id.insert(u.id.clone(), u);
    }
    let plugins: Vec<model::LockPlugin> = tree
        .plugins
        .iter()
        .map(|p| model::LockPlugin {
            name: p.name.clone(),
            version: p.version.to_string(),
            compat_gen: p.version.compat_gen,
        })
        .collect();
    let units: Vec<model::LockUnit> = by_id
        .values()
        .map(|s| model::LockUnit {
            id: s.id.clone(),
            provider: s.provider.clone(),
            unit: s.unit.clone(),
            api_iface: s.api_iface.clone().unwrap_or_default(),
            version: s.version.map(|v| v.to_string()).unwrap_or_default(),
            compat_gen: s.compat_gen.unwrap_or(0),
            hash: s.hash.clone().unwrap_or_default(),
            hash_scope: s.hash_scope.clone().unwrap_or_default(),
            truth: s.truth.clone().unwrap_or_default(),
        })
        .collect();
    let lock = model::Lock {
        schema: 1,
        profile: profile.to_string(),
        product: tree.product.as_ref().map(|p| model::LockProduct {
            name: p.name.clone(),
            version: p.version.to_string(),
            stage: p.stage.clone(),
        }),
        plugins,
        units,
    };
    emit::render_lock(&lock)
}

/// Version section of `api/iface/CHANGELOG.md` (ruling R-12: section head = `<unit> v<4 segments> (compat_gen=<N>)`).
fn changelog_content(root: &Path, unit_id: &str, ver: Version, hash: &str, summary: &str) -> String {
    let rel = "api/iface/CHANGELOG.md";
    let abs = root.join(rel);
    let existing = std::fs::read_to_string(&abs).unwrap_or_default();
    let header = "# brickie iface CHANGELOG\n\n\
                  > Machine-owned (produced by `brickie-core`); appended per **version section**, **no timestamp** (contract §4 / R-12).\n";
    let base = if existing.trim().is_empty() {
        format!("{header}\n")
    } else {
        existing.clone()
    };
    let section_head = format!("## {unit_id} v{ver} (compat_gen={})", ver.compat_gen);
    if base.contains(&section_head) {
        return base;
    }
    let mut out = base;
    if !out.ends_with('\n') {
        out.push('\n');
    }
    out.push('\n');
    out.push_str(&section_head);
    out.push('\n');
    out.push_str(&format!("- hash: {hash}\n"));
    if !summary.is_empty() {
        out.push_str(&format!("- changes: {summary}\n"));
    }
    out
}

// ================================================================== command entry

/// Unified entry point for `ver-show` / `ver-bump` / `iface-*`.
pub fn run(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    match req.command.as_str() {
        "ver-show" => cmd_ver_show(req, &root),
        "ver-bump" => cmd_ver_bump(req, &root),
        "iface-list" => cmd_iface_list(req, &root),
        "iface-show" => cmd_iface_show(req, &root),
        "iface-diff" => cmd_iface_diff(req, &root),
        "iface-status" => cmd_iface_status(req, &root),
        "iface-publish" => cmd_iface_publish(req, &root),
        "iface-freeze" => cmd_iface_freeze(req, &root),
        "iface-deprecate" => cmd_iface_state(req, &root, "deprecate"),
        "iface-undeprecate" => cmd_iface_state(req, &root, "undeprecate"),
        "iface-unfreeze" => cmd_iface_unfreeze(req, &root),
        "iface-refreeze" => cmd_iface_refreeze(req, &root),
        other => {
            let env = Env::load(&root);
            let mut d = env.diags.clone();
            d.usage(
                format!("iface does not recognize command `{other}`"),
                "see the command table in contract §5",
            );
            Response::ok(d, json!({}))
        }
    }
}

fn resolve_or_usage(env: &Env, id: &str) -> Result<(String, String, usize), Response> {
    resolve(&env.tree, id).map_err(|msg| {
        let mut d = env.diags.clone();
        d.usage(msg, "contract §5: `id` = `<provider>#<unit>` or a plugin name with exactly one export");
        Response::ok(d, json!({}))
    })
}

// ------------------------------------------------------------------ ver-show

fn cmd_ver_show(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id);
    let (version, compat_gen, freeze_state, status, source, published) = match snap {
        Some(s) => (
            s.version.unwrap_or(decl.version),
            s.compat_gen.unwrap_or(decl.compat_gen),
            s.freeze_state.clone().unwrap_or_else(|| decl.freeze_state.clone()),
            s.status.clone().unwrap_or_else(|| decl.status.clone()),
            "snapshot",
            true,
        ),
        None => (
            decl.version,
            decl.compat_gen,
            decl.freeze_state.clone(),
            decl.status.clone(),
            "declaration",
            false,
        ),
    };
    // RV-11/C-12 require `ver show` to **explicitly mark** that "generation is not compared". This is **output semantics**, not a diagnostic:
    // it is carried by `data.not_comparable = true` + the L5 presentation line (see ver show in present.py),
    // and consumes no BRV code — packing a pure hint into `BRV-MF-0001` would steer machine consumers toward "there is a declaration-surface error here".
    let d = env.diags.clone();
    let source_note = if compat_gen == 0 {
        "never frozen (compat_gen = 0)".to_string()
    } else if published {
        format!("published generation recorded in the snapshot (compat_gen = {compat_gen})")
    } else {
        format!("declared compat_gen = {compat_gen}")
    };
    Response::ok(
        d,
        json!({
            "id": decl.id,
            "plugin": prov,
            "unit": unit,
            "version": version.to_string(),
            "compat_gen": compat_gen,
            "segments": {
                "compat_gen": version.compat_gen,
                "major": version.major,
                "minor": version.minor,
                "revise": version.revise,
            },
            "freeze_state": freeze_state,
            "status": status,
            "source": source,
            "compat_gen_source": {"kind": if compat_gen == 0 { "never-frozen" } else { "refreeze-sequence" }, "note": source_note},
            // RV-11 / C-12: explicitly mark that "generation is not compared". This is **output semantics** (the explicit mark C-12 requires),
            // not a diagnostic — packing a pure hint into a BRV code would steer machine consumers toward "there is a declaration-surface error here".
            "not_comparable": true,
            "published": published,
            "not_abi": true,
            "hash_scope": "decl",
            "truth": "decl",
        }),
    )
}

// ------------------------------------------------------------------ ver-bump

fn cmd_ver_bump(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let rule = req.arg_str("rule").unwrap_or_default();
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    if !crate::rules::in_list(&["major", "minor", "revise"], &rule) {
        let mut d = env.diags.clone();
        d.usage(
            format!("`--rule {rule}` out of range"),
            "legal values: major | minor | revise (§5.2/§7.7: compat_gen is **not** a rule)",
        );
        return Response::ok(d, json!({}));
    }
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let old_ver = snap
        .as_ref()
        .and_then(|s| s.version)
        .unwrap_or(decl.version);
    let event = match rule.as_str() {
        "major" => version::Event::MajorProduct,
        "minor" => version::Event::MinorFeature,
        _ => version::Event::ReviseFix,
    };
    let new_ver = version::advance(old_ver, event);
    let mut d = env.diags.clone();
    if let Err(e) = version::check_no_regress(old_ver, new_ver) {
        d.push(
            Diag::new(Severity::Error, e.message.clone())
                .code(e.code)
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}].version"))
                .hint("§5.2: no version segment may decrease"),
        );
        return Response::ok(d, json!({}));
    }

    // Surface entries: snapshot first (publish record), else declaration.
    let entries: Vec<Entry> = match &snap {
        Some(s) => s.entries.iter().map(Entry::from_snapshot).collect(),
        None => decl.entries.clone(),
    };
    let hash = snap
        .as_ref()
        .and_then(|s| s.hash.clone())
        .filter(|h| !h.is_empty())
        .unwrap_or_else(|| decl.hash(false));
    let mut new_surface = decl.clone();
    new_surface.entries = entries;
    new_surface.version = new_ver;
    // S-19: version advance does not touch the unfreeze window.
    new_surface.freeze_state = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );

    let rel = snapshot_path(&prov, &unit);
    let snap_out = new_surface.to_snapshot(&hash, false, None);
    let mut files = vec![machine_file(&rel, emit::render_snapshot(&snap_out))];
    files.push(machine_file(
        "api/iface/CHANGELOG.md",
        changelog_content(root, &decl.id, new_ver, &hash, &format!("ver bump --rule {rule}")),
    ));

    d.push(
        Diag::new(
            Severity::Info,
            format!(
                "`brick.toml` should set `[[export]].version` to \"{new_ver}\" (ruling R-3: human-written files are never rewritten by tools)"
            ),
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span(format!("export[{idx}].version"))
        .hint("gap substitute (R-9): no dedicated info code for 'what to change it to'"),
    );

    Response::ok_with_files(
        d,
        files,
        json!({
            "id": decl.id,
            "plugin": prov,
            "unit": unit,
            "rule": rule,
            "from": old_ver.to_string(),
            "to": new_ver.to_string(),
            "version": new_ver.to_string(),
            "segments": {
                "compat_gen": new_ver.compat_gen,
                "major": new_ver.major,
                "minor": new_ver.minor,
                "revise": new_ver.revise,
            },
            "compat_gen": new_ver.compat_gen,
            "file": rel,
        }),
    )
}

// ------------------------------------------------------------------ iface-list

fn cmd_iface_list(_req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let mut units: Vec<Value> = Vec::new();
    for p in &env.tree.plugins {
        for (i, e) in p.exports.iter().enumerate() {
            let decl = Surface::from_declaration(&env.tree, &p.name, &e.name, i);
            let snap = env.snap(&decl.id);
            let (version, compat_gen, freeze_state, status, published, hash) = match snap {
                Some(s) => (
                    s.version.unwrap_or(decl.version),
                    s.compat_gen.unwrap_or(decl.compat_gen),
                    // Effective freeze state (S-19): `unfreezing` in either declaration or snapshot counts as in-window —
                    // same rule as check / publish / ver show, so list/show never fight the gates.
                    effective_freeze(&decl.freeze_state, s.freeze_state.as_deref()),
                    s.status.clone().unwrap_or_else(|| decl.status.clone()),
                    true,
                    s.hash.clone().unwrap_or_default(),
                ),
                None => (
                    decl.version,
                    decl.compat_gen,
                    effective_freeze(&decl.freeze_state, None),
                    decl.status.clone(),
                    false,
                    decl.hash(false),
                ),
            };
            units.push(json!({
                "id": decl.id,
                "provider": p.name,
                "unit": e.name,
                "api_iface": decl.api_iface,
                "form": decl.form,
                "version": version.to_string(),
                "compat_gen": compat_gen,
                "freeze_state": freeze_state,
                "status": status,
                "published": published,
                "hash": hash,
                "hash_short": short_hash(Some(&hash)),
                "file": p.file,
            }));
        }
    }
    units.sort_by(|a, b| {
        a["id"]
            .as_str()
            .unwrap_or("")
            .cmp(b["id"].as_str().unwrap_or(""))
    });
    Response::ok(env.diags.clone(), json!({"units": units, "count": units.len()}))
}

// ------------------------------------------------------------------ iface-show

fn cmd_iface_show(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id);
    let decl_hash = decl.hash(false);
    let snap_hash = snap.and_then(|s| s.hash.clone()).unwrap_or_default();
    let not_abi = snap.map(|s| s.not_abi).unwrap_or(true);
    let hs = snap
        .and_then(|s| s.hash_scope.clone())
        .or_else(|| env.tree.by_name(&prov).and_then(|p| p.compat.hash_scope.clone()))
        .unwrap_or_else(|| "decl".to_string());
    let tr = snap
        .and_then(|s| s.truth.clone())
        .or_else(|| env.tree.by_name(&prov).and_then(|p| p.compat.truth.clone()))
        .unwrap_or_else(|| "decl".to_string());

    let mut d = env.diags.clone();
    // NOTE_ABI hint (V-11③) and requires_iface info (V-10③).
    d.push(
        Diag::new(
            Severity::Info,
            "NOT_ABI: declaration-surface hash ≠ ABI compatibility proof (v0.1 truth = declaration surface; §6.6/RV-3)".to_string(),
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span(format!("export[{idx}]"))
        .hint("gap substitute (R-9): NOT_ABI has no dedicated info code"),
    );
    if let Some(p) = env.tree.by_name(&prov) {
        for (ri, r) in p.compat.requires_iface.iter().enumerate() {
            d.push(
                Diag::new(
                    Severity::Info,
                    format!(
                        "declaration-surface interface dependency `{}` (compat_gen={}, range={}): v0.1 does not scan declaration-surface interface dependencies (V-10③)",
                        r.id, r.compat_gen, r.range_raw
                    ),
                )
                .code("BRV-MF-0001")
                .target(&prov)
                .file(&p.file)
                .span(format!("compat.requires_iface[{ri}]"))
                .hint("§5.6/§10 V-10"),
            );
        }
    }

    let entries: Vec<Value> = decl.entries.iter().map(entry_json).collect();
    // Effective values (S-19 rule): the snapshot is the **publish record**, the declaration is the **current intent**; they differ inside an unfreeze window,
    // so unit/data must report the "effective freeze state" (unfreezing anywhere ⇒ unfreezing),
    // otherwise `iface show` would contradict the verdict of `check --profile release` (BRV-IFACE-0009).
    let (eff_version, eff_cg, eff_freeze, eff_status) = match snap {
        Some(s) => (
            s.version.unwrap_or(decl.version).to_string(),
            s.compat_gen.unwrap_or(decl.compat_gen),
            effective_freeze(&decl.freeze_state, s.freeze_state.as_deref()),
            s.status.clone().unwrap_or_else(|| decl.status.clone()),
        ),
        None => (
            decl.version.to_string(),
            decl.compat_gen,
            effective_freeze(&decl.freeze_state, None),
            decl.status.clone(),
        ),
    };
    let decl_freeze = decl.freeze_state.clone();
    let snap_freeze = snap.and_then(|s| s.freeze_state.clone());
    let unit_obj = json!({
        "id": decl.id,
        "provider": decl.provider,
        "unit": decl.unit,
        "api_iface": decl.api_iface,
        "form": decl.form,
        "version": eff_version,
        "compat_gen": eff_cg,
        "freeze_state": eff_freeze,
        "status": eff_status,
        "declaration_freeze_state": decl_freeze,
        "snapshot_freeze_state": snap_freeze,
        "hash_scope": hs,
        "truth": tr,
        "declaration_hash": decl_hash,
        "snapshot_hash": if snap_hash.is_empty() { Value::Null } else { json!(snap_hash) },
        "published": snap.is_some(),
        "reexport_of": decl.reexport_of,
        "symbols": decl.symbols,
        "file": decl.file,
    });
    Response::ok(
        d,
        json!({
            "id": unit_obj["id"].clone(),
            "unit": unit_obj,
            "version": unit_obj["version"].clone(),
            "compat_gen": unit_obj["compat_gen"].clone(),
            "freeze_state": unit_obj["freeze_state"].clone(),
            "status": unit_obj["status"].clone(),
            "entries": entries,
            "source": if snap.is_some() { "snapshot+declaration" } else { "declaration" },
            "not_abi": not_abi,
            "truth": tr,
            "hash_scope": hs,
            "declaration_hash": decl_hash,
            "snapshot_hash": if snap_hash.is_empty() { Value::Null } else { json!(snap_hash) },
            "published": snap.is_some(),
        }),
    )
}

fn entry_json(e: &Entry) -> Value {
    json!({
        "kind": e.kind,
        "name": e.name,
        "sig": e.sig,
        "layout": e.layout,
        "value": e.value,
        "ops": if e.ops.is_empty() { Value::Null } else { json!(e.ops) },
        "status": e.status,
    })
}

// ------------------------------------------------------------------ version advance helpers

struct VersionPlan {
    from: Version,
    to: Version,
    reasons: Vec<String>,
}

fn plan_version(
    snap_version: Option<Version>,
    decl_version: Version,
    changes: &[Change],
    set: Option<Version>,
    initial: bool,
) -> VersionPlan {
    let from = snap_version.unwrap_or(decl_version);
    let hard: Vec<&Change> = changes.iter().filter(|c| c.is_hard()).collect();
    if initial {
        return VersionPlan {
            from,
            to: set.unwrap_or(from),
            reasons: vec!["first publish: record creation, no segment advanced (ruling S-8)".to_string()],
        };
    }
    let event = if !hard.is_empty() {
        version::Event::CompatGenBump
    } else if !changes.is_empty() {
        version::Event::Added
    } else {
        version::Event::NoChange
    };
    let auto = version::advance(from, event);
    let to = set.unwrap_or(auto);
    let mut reasons = Vec::new();
    if !hard.is_empty() {
        reasons.push(format!(
            "COMPAT_GEN+1: existing frozen/deprecated entries changed or removed ({} entries)",
            hard.len()
        ));
        reasons.push("MINOR→0 / REVISE→0".to_string());
    } else if !changes.is_empty() {
        let kinds: BTreeSet<&str> = changes.iter().map(|c| c.kind).collect();
        reasons.push(format!(
            "MINOR+1: change set {{{}}} (additions/appends/status transitions/experimental-only changes)",
            kinds.into_iter().collect::<Vec<_>>().join(",")
        ));
        reasons.push("REVISE→0".to_string());
    } else {
        reasons.push("surface unchanged ⇒ no-op (§6.5)".to_string());
    }
    if set.is_some() {
        reasons.push("--set overrides the unit version written by this run (ruling R-8)".to_string());
    }
    VersionPlan { from, to, reasons }
}

// ------------------------------------------------------------------ iface-diff

fn cmd_iface_diff(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let old = snap.as_ref().map(Surface::from_snapshot).unwrap_or_default();
    let initial = snap.is_none();
    let changes = if initial {
        Vec::new()
    } else {
        change_set(&old, &decl)
    };
    let hard: Vec<&Change> = changes.iter().filter(|c| c.is_hard()).collect();
    let plan = plan_version(
        snap.as_ref().and_then(|s| s.version),
        decl.version,
        &changes,
        None,
        initial,
    );
    let eff_freeze = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    let verdict_red = !hard.is_empty() && eff_freeze != "unfreezing";
    let dependents = dependents_of(&env.tree, &prov, plan.to.compat_gen, plan.to);
    Response::ok(
        env.diags.clone(),
        json!({
            "id": decl.id,
            "changes": changes.iter().map(Change::to_json).collect::<Vec<_>>(),
            "verdict": if verdict_red { "red" } else { "green" },
            "compat_gen_changed": plan.to.compat_gen != plan.from.compat_gen,
            "version": {"from": plan.from.to_string(), "to": plan.to.to_string(), "reasons": plan.reasons},
            "dependents": dependents.to_json(),
            "freeze_state": eff_freeze,
            "published": snap.is_some(),
        }),
    )
}

// ------------------------------------------------------------------ iface-status

fn cmd_iface_status(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let check = req.arg_bool("check").unwrap_or(false);
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let recomputed = decl.hash(false);
    let snapshot_hash = snap.as_ref().and_then(|s| s.hash.clone());
    let mut d = env.diags.clone();

    let mut consistent = true;
    match &snapshot_hash {
        None => {
            consistent = false;
            d.push(
                Diag::new(
                    Severity::Error,
                    format!("`{}` has no publish snapshot yet (unpublished) ⇒ cannot compare", decl.id),
                )
                .code("BRV-MF-0001")
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}]"))
                .hint("run `brickie iface publish` first; gap substitute (R-9)"),
            );
        }
        Some(sh) => {
            if sh != &recomputed {
                consistent = false;
                d.push(
                    Diag::new(
                        Severity::Error,
                        format!(
                            "declaration-surface hash differs from snapshot hash: recomputed {}, snapshot {}",
                            short_hash(Some(&recomputed)),
                            short_hash(Some(sh))
                        ),
                    )
                    .code("BRV-MF-0001")
                    .target(&prov)
                    .file(&decl.file)
                    .span(format!("export[{idx}]"))
                    .hint("gap substitute (R-9): no dedicated code for 'stale snapshot'; hand-edited snapshot ⇒ red (N-3)"),
                );
            }
        }
    }

    // V-11④ / BRV-IFACE-0012: the two records of the same truth must be equal.
    if let (Some(dh), Some(sh)) = (&decl.declared_hash, &snapshot_hash) {
        if !dh.is_empty() && !sh.is_empty() && dh != sh {
            consistent = false;
            d.push(
                Diag::new(
                    Severity::Error,
                    format!(
                        "`brick.toml [[export]].hash`({}) differs from snapshot hash({}) — the hash is one truth recorded in two places",
                        short_hash(Some(dh)),
                        short_hash(Some(sh))
                    ),
                )
                .code("BRV-IFACE-0012")
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}].hash"))
                .hint("§9.1.1 rule 4 / §10 V-11④: editing one place by hand ⇒ red"),
            );
        }
    }

    let files = if !check && !consistent {
        // S-12: status is a gate and writes nothing; the "expected snapshot" goes into data for manual use.
        Vec::new()
    } else {
        Vec::new()
    };
    let mut expected_surface = decl.clone();
    expected_surface.freeze_state = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    let expected = expected_surface.to_snapshot(&recomputed, false, None);
    Response::ok_with_files(
        d,
        files,
        json!({
            "id": decl.id,
            "recomputed_hash": recomputed,
            "snapshot_hash": snapshot_hash,
            "declaration_hash": recomputed,
            "plugin_toml_hash": decl.declared_hash,
            "consistent": consistent,
            "check": check,
            "expected_snapshot": emit::render_snapshot(&expected),
            "expected_snapshot_path": snapshot_path(&prov, &unit),
        }),
    )
}

// ------------------------------------------------------------------ iface-publish

fn cmd_iface_publish(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let check = req.arg_bool("check").unwrap_or(false);
    let note = req.arg_str("note");
    let strict = req.arg_bool("strict_params").unwrap_or(false);
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let profile = req
        .arg_str("profile")
        .filter(|s| !s.is_empty())
        .or_else(|| env.tree.product.as_ref().map(|p| p.stage.clone()))
        .unwrap_or_else(|| "dev".to_string());

    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let decl_hash = decl.hash(strict);

    // `--set` (ruling R-8): overrides the unit version written by this run, still bound by monotonicity.
    let set = match req.arg_str("set") {
        None => None,
        Some(raw) => match version::parse_version(&raw) {
            Ok(v) => Some(v),
            Err(e) => {
                let mut d = env.diags.clone();
                d.shape(
                    e.code,
                    &prov,
                    &decl.file,
                    format!("export[{idx}].version"),
                    e.message,
                    "§5.1: `--set` must be four segments `COMPAT_GEN.MAJOR.MINOR.REVISE`",
                );
                return Response::ok(d, json!({}));
            }
        },
    };

    // ---- idempotence (§6.5): surface content + entry set + status + freeze_state all equal ⇒ no-op ----
    if let Some(s) = &snap {
        let old = Surface::from_snapshot(s);
        let same_entries = entries_equal(&old.entries, &decl.entries);
        // S-19: freeze state compares the "effective freeze state" (declaration OR snapshot) ⇒ repeated publish inside the window is still a no-op.
        let same_freeze = s.freeze_state.clone().unwrap_or_else(|| "unfrozen".into())
            == effective_freeze(
                &decl.freeze_state,
                s.freeze_state.as_deref(),
            );
        if s.hash.as_deref() == Some(decl_hash.as_str()) && same_entries && same_freeze {
            return Response::ok_with_files(
                env.diags.clone(),
                Vec::new(),
                json!({
                    "id": decl.id,
                    "noop": true,
                    "published": true,
                    "version": s.version.map(|v| v.to_string()),
                    "compat_gen": s.compat_gen,
                    "verdict": "green",
                    "changes": [],
                    "version_change": {"from": s.version.map(|v| v.to_string()), "to": s.version.map(|v| v.to_string()), "reasons": ["surface unchanged ⇒ no-op (§6.5)"]},
                    "report": {"dependents": dependents_of(&env.tree, &prov, s.compat_gen.unwrap_or(0), s.version.unwrap_or_default()).to_json()},
                }),
            );
        }
    }

    let initial = snap.is_none();
    let old = snap.as_ref().map(Surface::from_snapshot).unwrap_or_default();
    let changes = if initial {
        decl.entries
            .iter()
            .map(|e| Change {
                kind: "ADDED",
                name: e.name.clone(),
                from: None,
                to: Some(e.name.clone()),
                old_status: String::new(),
                note: "first publish: record creation".to_string(),
            })
            .collect::<Vec<_>>()
    } else {
        change_set(&old, &decl)
    };

    // ---- gates (§6.3/§6.5) ----
    let mut d = env.diags.clone();
    let hard: Vec<&Change> = changes.iter().filter(|c| c.is_hard()).collect();
    let eff_freeze = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    let mut blocked = false;
    if !hard.is_empty() {
        // REMOVED requires the entry to be deprecated first (§6.3).
        for c in &hard {
            if c.kind == "REMOVED" && c.old_status != "deprecated" {
                blocked = true;
                d.push(
                    Diag::new(
                        Severity::Error,
                        format!(
                            "`REMOVED` entry `{}` is still `{}`: removal requires `deprecated` first plus the deprecation period (§6.3/§6.4)",
                            c.name, c.old_status
                        ),
                    )
                    .code("BRV-MF-0001")
                    .target(&prov)
                    .file(&decl.file)
                    .span("export.entries")
                    .hint("run `brickie iface deprecate` first; gap substitute (R-9)"),
                );
            }
        }
        if eff_freeze != "unfreezing" {
            blocked = true;
            let code = if profile == "release" {
                "BRV-IFACE-0009"
            } else {
                "BRV-MF-0001"
            };
            d.push(
                Diag::new(
                    Severity::Error,
                    format!(
                        "change set hits {} 'must unfreeze' entries (CHANGED/REMOVED touching frozen entries), but the unit is not `unfreezing` (currently `{}`)",
                        hard.len(),
                        eff_freeze
                    ),
                )
                .code(code)
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}].freeze_state"))
                .hint("§5.3.3 hard path: unfreeze → change/remove → refreeze; gap substitute (R-9)"),
            );
        }
        if note.is_none() {
            blocked = true;
            d.push(
                Diag::new(
                    Severity::Error,
                    format!(
                        "change set hits 'must unfreeze' (§5.3.4 highest bar) ⇒ `--note <decision record>` required ({} entries)",
                        hard.len()
                    ),
                )
                .code("BRV-MF-0001")
                .target(&prov)
                .file(&decl.file)
                .span("--note")
                .hint("§5.3.4 rule 1 / V-6; gap substitute (R-9): no dedicated code for a missing note"),
            );
        }
    }

    let plan = plan_version(
        snap.as_ref().and_then(|s| s.version),
        decl.version,
        &changes,
        set,
        initial,
    );
    if let Err(e) = version::check_no_regress(plan.from, plan.to) {
        blocked = true;
        d.push(
            Diag::new(Severity::Error, e.message.clone())
                .code(e.code)
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}].version"))
                .hint("§5.2/§6.5: `--set` must be ≥ the current version"),
        );
    }
    let dependents = dependents_of(&env.tree, &prov, plan.to.compat_gen, plan.to);
    if profile == "release"
        && plan.to.compat_gen != plan.from.compat_gen
        && !dependents.unsatisfied.is_empty()
    {
        blocked = true;
        d.push(
            Diag::new(
                Severity::Error,
                format!(
                    "pre-publish gate (release): COMPAT_GEN changes from {} to {}, {} dependents will mismatch",
                    plan.from.compat_gen,
                    plan.to.compat_gen,
                    dependents.unsatisfied.len()
                ),
            )
            .code("BRV-VER-0001")
            .target(&prov)
            .file(&decl.file)
            .span("export.version")
            .hint("§6.5: after the change dependents must still satisfy an exact compat_gen match; coordinate with dependents first"),
        );
    }

    let verdict = if blocked { "red" } else { "green" };
    if blocked {
        return Response::ok(
            d,
            json!({
                "id": decl.id,
                "published": snap.is_some(),
                "verdict": verdict,
                "from": plan.from.to_string(),
                "to": plan.to.to_string(),
                "version": plan.to.to_string(),
                "compat_gen": plan.to.compat_gen,
                "compat_gen_changed": plan.to.compat_gen != plan.from.compat_gen,
                "freeze_state": eff_freeze,
                "changes": changes.iter().map(Change::to_json).collect::<Vec<_>>(),
                "version_change": {"from": plan.from.to_string(), "to": plan.to.to_string(), "reasons": plan.reasons},
                "report": {"dependents": dependents.to_json()},
            }),
        );
    }

    // ---- write machine files: snapshot + CHANGELOG + lock (§5.2 table) ----
    let mut new_surface = decl.clone();
    new_surface.version = plan.to;
    // Ruling S-19: `publish` **must not** erase the unfreeze window — the effective freeze state takes "declaration OR snapshot".
    new_surface.freeze_state = eff_freeze.clone();
    // and a publish inside the window **consumes** the baseline (the new surface becomes the new promise), so a later `refreeze` only closes the window
    // without bumping `COMPAT_GEN` again (avoiding a "publish + refreeze double bump").
    let baseline_owned: Option<(String, Vec<Entry>)> =
        if eff_freeze == "unfreezing" {
            Some((decl_hash.clone(), decl.entries.clone()))
        } else {
            None
        };
    let snap_out = new_surface.to_snapshot(
        &decl_hash,
        strict,
        baseline_owned
            .as_ref()
            .map(|(h, es)| (h.as_str(), es.as_slice())),
    );
    let rel = snapshot_path(&prov, &unit);
    let changelog_summary: String = if initial {
        "first publish (record creation)".to_string()
    } else {
        changes
            .iter()
            .map(|c| format!("{} {}", c.kind, c.name))
            .collect::<Vec<_>>()
            .join("; ")
    };
    let files: Vec<Value> = vec![
        machine_file(&rel, emit::render_snapshot(&snap_out)),
        machine_file(
            "api/iface/CHANGELOG.md",
            changelog_content(root, &decl.id, plan.to, &decl_hash, &changelog_summary),
        ),
        machine_file(
            "brickie.lock",
            build_lock_content(&env.tree, root, &profile, Some(snap_out.clone())),
        ),
    ];
    let _ = check;
    Response::ok_with_files(
        d,
        files,
        json!({
            "id": decl.id,
            "published": true,
            "noop": false,
            "verdict": "green",
            "from": plan.from.to_string(),
            "to": plan.to.to_string(),
            "version": plan.to.to_string(),
            "compat_gen": plan.to.compat_gen,
            "compat_gen_changed": plan.to.compat_gen != plan.from.compat_gen,
            "freeze_state": eff_freeze,
            "hash": decl_hash,
            "hash_short": short_hash(Some(&decl_hash)),
            "strict_params": strict,
            "changes": changes.iter().map(Change::to_json).collect::<Vec<_>>(),
            "version_change": {"from": plan.from.to_string(), "to": plan.to.to_string(), "reasons": plan.reasons},
            "report": {"dependents": dependents.to_json()},
            "dependents": dependents.to_json(),
            "files_planned": [rel, "api/iface/CHANGELOG.md", "brickie.lock"],
        }),
    )
}

fn entries_equal(a: &[Entry], b: &[Entry]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut aa: Vec<&Entry> = a.iter().collect();
    let mut bb: Vec<&Entry> = b.iter().collect();
    aa.sort_by(|x, y| (&x.kind, &x.name).cmp(&(&y.kind, &y.name)));
    bb.sort_by(|x, y| (&x.kind, &x.name).cmp(&(&y.kind, &y.name)));
    aa.iter()
        .zip(bb.iter())
        .all(|(x, y)| x.same_surface(y) && x.status == y.status)
}

// ------------------------------------------------------------------ iface-freeze

fn cmd_iface_freeze(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let entry = req.arg_str("entry");
    let note = req.arg_str("note");
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let mut d = env.diags.clone();

    let Some(note) = note else {
        d.push(
            Diag::new(
                Severity::Error,
                "`iface freeze` requires `--note <decision record>` (§5.3.4 highest bar / A-26)".to_string(),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("gap substitute (R-9): no dedicated code for a missing note"),
        );
        return Response::ok(d, json!({}));
    };
    if !note_exists(root, &note) {
        d.usage(
            format!("`--note` path does not exist: {note}"),
            "§5.3.4: note is a decision-record file; usage error ⇒ exit code 2",
        );
        return Response::ok(d, json!({}));
    }
    if let Some(e) = &entry {
        if !decl.entries.iter().any(|x| x.name == *e) {
            d.usage(
                format!("unit `{}` has no entry `{e}`", decl.id),
                "`entry` must be a `[[export.entries]].name`",
            );
            return Response::ok(d, json!({}));
        }
    }

    // §6.4 / A-26: v0.1 only emits a "promotion proposal"; writes no frozen snapshot and bumps no COMPAT_GEN.
    let proposal_rel = format!("build/gen/proposals/{}.toml", decl.unit);
    let mut doc = TDoc::new()
        .set("schema", TValue::Int(1))
        .set("unit", TValue::Str(decl.id.clone()))
        .set("provider", TValue::Str(prov.clone()))
        .set("kind", TValue::Str("promotion-proposal".to_string()));
    let mut prop = TTable::new(&["proposal"]);
    prop.push_str("requested_entry", entry.clone().unwrap_or_default());
    prop.push_str("current_freeze_state", decl.freeze_state.clone());
    prop.push_str("target_freeze_state", "frozen");
    prop.push_str("current_status", decl.status.clone());
    prop.push_str("note", note.clone());
    prop.push_int("compat_gen", decl.compat_gen as i64);
    prop.push_bool("v01_no_frozen_snapshot", true);
    prop.push_str(
        "reason",
        "v0.1 can only generate a promotion proposal (§6.4 / A-26): no frozen snapshot, no COMPAT_GEN bump; actual promotion is v0.4+",
    );
    doc = doc.push(prop);
    let mut entry_tbl = TTable::new(&["entry"]);
    for e in &decl.entries {
        if let Some(only) = &entry {
            if &e.name != only {
                continue;
            }
        }
        entry_tbl.push_str("kind", e.kind.clone());
        entry_tbl.push_str("name", e.name.clone());
        entry_tbl.push_str("status", e.status.clone());
        doc = doc.push(TTable {
            header: vec!["entry".to_string()],
            array: true,
            values: entry_tbl.values.clone(),
            children: Vec::new(),
        });
        entry_tbl.values.clear();
    }
    let content = format!(
        "{}{}",
        emit::machine_text_header(&format!("interface promotion proposal {}", decl.id)),
        doc.render()
    );
    Response::ok_with_files(
        d,
        vec![machine_file(&proposal_rel, content)],
        json!({
            "id": decl.id,
            "entry": entry,
            "proposal": proposal_rel,
            "freeze_state": decl.freeze_state,
            "status": decl.status,
            "compat_gen": decl.compat_gen,
        }),
    )
}

// ------------------------------------------------------------------ iface-deprecate / undeprecate

fn cmd_iface_state(req: &Request, root: &Path, verb: &str) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let entry = req.arg_str("entry");
    let note = req.arg_str("note");
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let old = snap.as_ref().map(Surface::from_snapshot).unwrap_or_default();
    let mut d = env.diags.clone();

    let Some(note) = note else {
        d.push(
            Diag::new(
                Severity::Error,
                format!("`iface {verb}` requires `--note <decision record>` (§6.5)"),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("gap substitute (R-9): no dedicated code for a missing note"),
        );
        return Response::ok(d, json!({}));
    };
    if !note_exists(root, &note) {
        d.usage(
            format!("`--note` path does not exist: {note}"),
            "§6.5: note is a decision-record file; usage error ⇒ exit code 2",
        );
        return Response::ok(d, json!({}));
    }

    let target_status = if verb == "deprecate" {
        "deprecated"
    } else {
        "frozen"
    };
    let mut new_surface = decl.clone();
    // State-machine commands do not touch the unfreeze window (S-19): the effective freeze state takes "declaration OR snapshot".
    new_surface.freeze_state = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    let mut changed_any = false;
    let mut errors: Vec<String> = Vec::new();

    // Current status = publish record (snapshot) first ⇒ repeated runs are no-ops (ruling S-9).
    let cur_status = |name: &str| -> Option<String> {
        if let Some(s) = &snap {
            if let Some(e) = s.entries.iter().find(|e| e.name == name) {
                return Some(e.status.clone());
            }
            return None;
        }
        decl.entries
            .iter()
            .find(|e| e.name == name)
            .map(|e| e.status.clone())
    };

    let names: Vec<String> = match &entry {
        Some(e) => {
            if !decl.entries.iter().any(|x| x.name == *e) {
                d.usage(
                    format!("unit `{}` has no entry `{e}`", decl.id),
                    "`entry` must be a `[[export.entries]].name`",
                );
                return Response::ok(d, json!({}));
            }
            vec![e.clone()]
        }
        None => decl.entries.iter().map(|e| e.name.clone()).collect(),
    };

    for name in &names {
        let Some(cur) = cur_status(name) else {
            continue;
        };
        let legal = if verb == "deprecate" {
            cur == "frozen"
        } else {
            cur == "deprecated"
        };
        if !legal && cur != target_status {
            errors.push(format!(
                "entry `{name}` is currently `{cur}`, cannot `{verb}` directly to `{target_status}` (illegal state-machine jump)"
            ));
            continue;
        }
        if cur != target_status {
            changed_any = true;
        }
        if let Some(e) = new_surface.entries.iter_mut().find(|e| e.name == *name) {
            e.status = target_status.to_string();
        }
    }
    // Unit-level status (when no entry is specified).
    if entry.is_none() {
        let cur_unit = snap
            .as_ref()
            .and_then(|s| s.status.clone())
            .unwrap_or_else(|| decl.status.clone());
        if cur_unit == target_status {
            // already at the target state
        } else if (verb == "deprecate" && cur_unit == "frozen")
            || (verb == "undeprecate" && cur_unit == "deprecated")
        {
            changed_any = true;
            new_surface.status = target_status.to_string();
        } else {
            errors.push(format!(
                "unit `{}` is currently `{cur_unit}`, cannot `{verb}` directly to `{target_status}`",
                decl.id
            ));
        }
    }

    if !errors.is_empty() {
        for e in errors {
            d.push(
                Diag::new(Severity::Error, e)
                    .code("BRV-MF-0001")
                    .target(&prov)
                    .file(&decl.file)
                    .span("export.status")
                    .hint("§6.3 STATUS / §6.4: state-transition legality is checked by the engine (e.g. an experimental→deprecated jump ⇒ red)"),
            );
        }
        return Response::ok(
            d,
            json!({"id": decl.id, "status": decl.status, "freeze_state": decl.freeze_state}),
        );
    }

    if !changed_any {
        return Response::ok_with_files(
            d,
            Vec::new(),
            json!({
                "id": decl.id,
                "entry": entry,
                "status": new_surface.status,
                "freeze_state": new_surface.freeze_state,
                "compat_gen": decl.compat_gen,
                "noop": true,
                "files_planned": [],
            }),
        );
    }

    let changes = if snap.is_some() {
        change_set(&old, &new_surface)
    } else {
        Vec::new()
    };
    let plan = plan_version(
        snap.as_ref().and_then(|s| s.version),
        decl.version,
        &changes,
        None,
        snap.is_none(),
    );
    new_surface.version = plan.to;
    let hash = new_surface.hash(false);
    let rel = snapshot_path(&prov, &unit);
    let snap_out = new_surface.to_snapshot(&hash, false, None);
    d.push(
        Diag::new(
            Severity::Info,
            format!(
                "`brick.toml` `[[export]]`/`[[export.entries]]` `status` should be changed to `{target_status}` accordingly (ruling R-3)"
            ),
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span("export.status")
        .hint("gap substitute (R-9): no dedicated info code for 'what to change it to'"),
    );
    let files = vec![
        machine_file(&rel, emit::render_snapshot(&snap_out)),
        machine_file(
            "api/iface/CHANGELOG.md",
            changelog_content(root, &decl.id, plan.to, &hash, &format!("{verb} → {target_status}")),
        ),
    ];
    Response::ok_with_files(
        d,
        files,
        json!({
            "id": decl.id,
            "entry": entry,
            "status": target_status,
            "freeze_state": new_surface.freeze_state,
            "compat_gen": plan.to.compat_gen,
            "from": plan.from.to_string(),
            "to": plan.to.to_string(),
            "version": plan.to.to_string(),
            "changes": changes.iter().map(Change::to_json).collect::<Vec<_>>(),
            "files_planned": [rel, "api/iface/CHANGELOG.md"],
        }),
    )
}

// ------------------------------------------------------------------ iface-unfreeze

fn cmd_iface_unfreeze(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let note = req.arg_str("note");
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let mut d = env.diags.clone();

    let Some(note) = note else {
        d.push(
            Diag::new(
                Severity::Error,
                "`iface unfreeze` requires `--note <decision record>` (§5.3.4 rule 1: highest-bar RFC)".to_string(),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("gap substitute (R-9): no dedicated code for a missing note; V-16 observation point 1"),
        );
        return Response::ok(
            d,
            json!({"id": decl.id, "freeze_state": decl.freeze_state}),
        );
    };
    if !note_exists(root, &note) {
        d.usage(
            format!("`--note` path does not exist: {note}"),
            "§5.3.4: note is a decision-record file; usage error ⇒ exit code 2",
        );
        return Response::ok(d, json!({}));
    }

    let cur = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    if cur != "frozen" {
        d.push(
            Diag::new(
                Severity::Error,
                format!("only units with `freeze_state = frozen` can unfreeze; `{}` is currently `{cur}`", decl.id),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span(format!("export[{idx}].freeze_state"))
            .hint("§5.3.4: `frozen → unfreezing`; gap substitute (R-9)"),
        );
        return Response::ok(d, json!({"id": decl.id, "freeze_state": cur}));
    }

    // Baseline = current surface hash + entry table (ruling R-7).
    let entries: Vec<Entry> = match &snap {
        Some(s) if !s.entries.is_empty() => s.entries.iter().map(Entry::from_snapshot).collect(),
        _ => decl.entries.clone(),
    };
    let base_hash = surface_hash(&decl.id, &entries, &decl.typedefs, false);
    let mut new_surface = decl.clone();
    new_surface.freeze_state = "unfreezing".to_string();
    new_surface.entries = entries.clone();
    new_surface.version = snap
        .as_ref()
        .and_then(|s| s.version)
        .unwrap_or(decl.version);
    new_surface.compat_gen = snap
        .as_ref()
        .and_then(|s| s.compat_gen)
        .unwrap_or(decl.compat_gen);
    let rel = snapshot_path(&prov, &unit);
    let snap_out = new_surface.to_snapshot(&base_hash, false, Some((&base_hash, &entries)));
    let files = vec![machine_file(&rel, emit::render_snapshot(&snap_out))];
    Response::ok_with_files(
        d,
        files,
        json!({
            "id": decl.id,
            "freeze_state": "unfreezing",
            "status": new_surface.status,
            "compat_gen": new_surface.compat_gen,
            "version": new_surface.version.to_string(),
            "baseline_hash": base_hash,
            "note": note,
            "files_planned": [rel],
        }),
    )
}

// ------------------------------------------------------------------ iface-refreeze

fn cmd_iface_refreeze(req: &Request, root: &Path) -> Response {
    let env = Env::load(root);
    let id = req.arg_str("id").unwrap_or_default();
    let note = req.arg_str("note");
    let (prov, unit, idx) = match resolve_or_usage(&env, &id) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let decl = Surface::from_declaration(&env.tree, &prov, &unit, idx);
    let snap = env.snap(&decl.id).cloned();
    let mut d = env.diags.clone();

    if let Some(n) = &note {
        if !note_exists(root, n) {
            d.usage(
                format!("`--note` path does not exist: {n}"),
                "§5.3.4: note is a decision-record file; usage error ⇒ exit code 2",
            );
            return Response::ok(d, json!({}));
        }
    }

    let cur = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    if cur != "unfreezing" {
        d.push(
            Diag::new(
                Severity::Error,
                format!("only units in `unfreezing` can refreeze; `{}` is currently `{cur}`", decl.id),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span(format!("export[{idx}].freeze_state"))
            .hint("run `brickie iface unfreeze` first; gap substitute (R-9)"),
        );
        return Response::ok(d, json!({"id": decl.id, "freeze_state": cur}));
    }
    let Some(s) = &snap else {
        d.push(
            Diag::new(
                Severity::Error,
                format!("`{}` has no snapshot/baseline, cannot tell whether frozen entries changed (ruling R-7)", decl.id),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span(format!("export[{idx}].freeze_state"))
            .hint("run `unfreeze` first (it writes the baseline); gap substitute (R-9)"),
        );
        return Response::ok(d, json!({"id": decl.id, "freeze_state": cur}));
    };

    // Baseline surface (ruling R-7): compare with the baseline; COMPAT_GEN+1 only if a frozen entry was actually changed/removed.
    let baseline = Surface {
        id: decl.id.clone(),
        provider: prov.clone(),
        unit: unit.clone(),
        file: s.file.clone(),
        export_index: 0,
        api_iface: decl.api_iface.clone(),
        form: decl.form.clone(),
        version: s.version.unwrap_or(decl.version),
        compat_gen: s.compat_gen.unwrap_or(0),
        freeze_state: "frozen".to_string(),
        status: s.status.clone().unwrap_or_else(|| "frozen".to_string()),
        declared_hash: s.baseline_hash.clone(),
        reexport_of: Vec::new(),
        symbols: Vec::new(),
        entries: if s.baseline_entries.is_empty() {
            s.entries.iter().map(Entry::from_snapshot).collect()
        } else {
            s.baseline_entries.iter().map(Entry::from_snapshot).collect()
        },
        typedefs: Vec::new(),
    };
    let mut new_surface = decl.clone();
    new_surface.freeze_state = "frozen".to_string();
    new_surface.version = s.version.unwrap_or(decl.version);
    new_surface.compat_gen = s.compat_gen.unwrap_or(decl.compat_gen);
    new_surface.entries = baseline.entries.clone();
    // Use the declaration surface's current entries (equal to the baseline if the declaration is unchanged).
    let mut current = decl.clone();
    current.entries = if decl.entries.is_empty() {
        baseline.entries.clone()
    } else {
        decl.entries.clone()
    };
    let changes = change_set(&baseline, &current);
    let hard: Vec<&Change> = changes.iter().filter(|c| c.is_hard()).collect();
    let empty_unfreeze = changes.is_empty();
    let plan = plan_version(
        Some(s.version.unwrap_or(decl.version)),
        decl.version,
        &changes,
        None,
        empty_unfreeze,
    );
    new_surface.version = plan.to;
    new_surface.compat_gen = plan.to.compat_gen;
    if !changes.is_empty() {
        new_surface.entries = current.entries.clone();
    }
    if !hard.is_empty() && note.is_none() {
        // Changing/removing a frozen entry requires a decision record.
        d.push(
            Diag::new(
                Severity::Error,
                format!(
                    "`refreeze` hits 'frozen entries changed/removed' ({} entries) ⇒ `--note <decision record>` required (§5.3.4)",
                    hard.len()
                ),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("gap substitute (R-9)"),
        );
        return Response::ok(
            d,
            json!({"id": decl.id, "freeze_state": "unfreezing", "compat_gen": s.compat_gen}),
        );
    }
    let hash = new_surface.hash(false);
    let rel = snapshot_path(&prov, &unit);
    let snap_out = new_surface.to_snapshot(&hash, false, None);
    let files = vec![
        machine_file(&rel, emit::render_snapshot(&snap_out)),
        machine_file(
            "api/iface/CHANGELOG.md",
            changelog_content(
                root,
                &decl.id,
                plan.to,
                &hash,
                if empty_unfreeze {
                    "empty-unfreeze refreeze (all four segments unchanged)"
                } else {
                    "refreeze (frozen surface modified)"
                },
            ),
        ),
    ];
    d.push(
        Diag::new(
            Severity::Info,
            if empty_unfreeze {
                format!(
                    "empty unfreeze: `{}` changed/removed no frozen entry ⇒ all four segments unchanged (§5.3.4 rule 2 / V-16)",
                    decl.id
                )
            } else {
                format!(
                    "refreeze of `{}` recorded {} changes ({} of which must unfreeze)",
                    decl.id,
                    changes.len(),
                    hard.len()
                )
            },
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span(format!("export[{idx}].freeze_state"))
        .hint("gap substitute (R-9): no dedicated info code for the unfreeze-window verdict"),
    );
    Response::ok_with_files(
        d,
        files,
        json!({
            "id": decl.id,
            "freeze_state": "frozen",
            "status": new_surface.status,
            "from": plan.from.to_string(),
            "to": plan.to.to_string(),
            "version": plan.to.to_string(),
            "compat_gen": plan.to.compat_gen,
            "compat_gen_changed": plan.to.compat_gen != plan.from.compat_gen,
            "empty_unfreeze": empty_unfreeze,
            "changes": changes.iter().map(Change::to_json).collect::<Vec<_>>(),
            "files_planned": [rel, "api/iface/CHANGELOG.md"],
        }),
    )
}

/// Reused by selftest / check: get the declaration surface from a plugin and unit.
pub fn declaration_surface(tree: &TreeLoad, provider: &str, unit: &str, idx: usize) -> Surface {
    Surface::from_declaration(tree, provider, unit, idx)
}

/// Reused by selftest: plugin model (easy to construct) ⇒ declaration surface.
pub fn surface_of_export(p: &Plugin, idx: usize) -> Surface {
    let e = &p.exports[idx];
    Surface {
        id: format!("{}#{}", p.name, e.name),
        provider: p.name.clone(),
        unit: e.name.clone(),
        file: p.file.clone(),
        export_index: idx,
        api_iface: e.api_iface.clone(),
        form: e.form.clone(),
        version: e.version.unwrap_or_default(),
        compat_gen: e.compat_gen.unwrap_or(0),
        freeze_state: e
            .freeze_state
            .clone()
            .unwrap_or_else(|| "unfrozen".to_string()),
        status: e.status.clone().unwrap_or_else(|| "experimental".to_string()),
        declared_hash: e.hash.clone().filter(|h| !h.is_empty()),
        reexport_of: e.reexport_of.clone(),
        symbols: e.symbols.clone(),
        entries: e.entries.iter().map(Entry::from_export).collect(),
        typedefs: p.typedefs.clone(),
    }
}

/// Reused by selftest: snapshot paths of all units in a plugin.
pub fn unit_ids_of(p: &Plugin) -> Vec<String> {
    p.unit_ids()
}

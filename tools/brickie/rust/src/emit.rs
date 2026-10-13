//! Deterministic TOML / JSON text emitter for machine files. Same input run twice ⇒ byte-identical
//! text: no timestamps, random numbers or host-specific info. This module never sorts; ordering
//! comes from the caller.

use crate::model::{Lock, Snapshot};

/// First-line marker of generated files (same literal as cxx `text.h` `kGeneratedMarker`).
pub const GENERATED_MARKER: &str = "brickie:generated";

#[derive(Clone, Debug)]
pub enum TValue {
    Str(String),
    Int(i64),
    Bool(bool),
    StrArray(Vec<String>),
    IntArray(Vec<i64>),
}

impl TValue {
    fn render(&self) -> String {
        match self {
            TValue::Str(s) => toml_string(s),
            TValue::Int(i) => i.to_string(),
            TValue::Bool(b) => b.to_string(),
            TValue::StrArray(v) => {
                let items: Vec<String> = v.iter().map(|s| toml_string(s)).collect();
                format!("[{}]", items.join(", "))
            }
            TValue::IntArray(v) => {
                let items: Vec<String> = v.iter().map(|i| i.to_string()).collect();
                format!("[{}]", items.join(", "))
            }
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct TTable {
    /// Header path segments, e.g. `["plugin", "export"]`.
    pub header: Vec<String>,
    /// `true` ⇒ `[[header]]` (array table).
    pub array: bool,
    pub values: Vec<(String, TValue)>,
    pub children: Vec<TTable>,
}

impl TTable {
    pub fn new(header: &[&str]) -> Self {
        TTable {
            header: header.iter().map(|s| s.to_string()).collect(),
            array: false,
            values: Vec::new(),
            children: Vec::new(),
        }
    }

    pub fn array_table(header: &[&str]) -> Self {
        let mut t = TTable::new(header);
        t.array = true;
        t
    }

    pub fn set(mut self, key: &str, value: TValue) -> Self {
        self.values.push((key.to_string(), value));
        self
    }

    pub fn push(mut self, child: TTable) -> Self {
        self.children.push(child);
        self
    }

    pub fn push_str(&mut self, key: &str, value: impl Into<String>) {
        self.values.push((key.to_string(), TValue::Str(value.into())));
    }

    pub fn push_int(&mut self, key: &str, value: i64) {
        self.values.push((key.to_string(), TValue::Int(value)));
    }

    pub fn push_bool(&mut self, key: &str, value: bool) {
        self.values.push((key.to_string(), TValue::Bool(value)));
    }

    pub fn push_str_array(&mut self, key: &str, value: Vec<String>) {
        self.values
            .push((key.to_string(), TValue::StrArray(value)));
    }

    pub fn push_int_array(&mut self, key: &str, value: Vec<i64>) {
        self.values.push((key.to_string(), TValue::IntArray(value)));
    }

    pub fn render_into(&self, out: &mut String) {
        if !out.is_empty() {
            out.push('\n');
        }
        let header = self.header.join(".");
        if self.array {
            out.push_str(&format!("[[{header}]]\n"));
        } else {
            out.push_str(&format!("[{header}]\n"));
        }
        for (k, v) in &self.values {
            out.push_str(&format!("{k} = {}\n", v.render()));
        }
        for child in &self.children {
            child.render_into(out);
        }
    }
}

/// A TOML document: root key-values + tables, emitted in insertion order.
#[derive(Clone, Debug, Default)]
pub struct TDoc {
    pub values: Vec<(String, TValue)>,
    pub tables: Vec<TTable>,
}

impl TDoc {
    pub fn new() -> Self {
        TDoc::default()
    }

    pub fn set(mut self, key: &str, value: TValue) -> Self {
        self.values.push((key.to_string(), value));
        self
    }

    pub fn push(mut self, table: TTable) -> Self {
        self.tables.push(table);
        self
    }

    /// Render to text; ends with a newline, no BOM, `\n` line endings (LF).
    pub fn render(&self) -> String {
        let mut out = String::new();
        for (k, v) in &self.values {
            out.push_str(&format!("{k} = {}\n", v.render()));
        }
        for t in &self.tables {
            t.render_into(&mut out);
        }
        out
    }
}

/// TOML basic-string escaping (supports basic strings only, no multi-line strings).
pub fn toml_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04X}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Common header for machine TOML files: first-line marker + "do not edit by hand" note.
fn machine_header(what: &str) -> String {
    format!(
        "# {GENERATED_MARKER}\n# {what} — machine-owned file, do not edit by hand; byte-identical across runs on the same input (contract §4)\n\n"
    )
}

/// Canonical text of `brickie.lock` (no timestamp); shape follows the authoritative
/// `schema/lock.schema.json` (`[lock.product]` is permitted by `additionalProperties: true`).
pub fn render_lock(lock: &Lock) -> String {
    let mut doc = TDoc::new().set("schema", TValue::Int(lock.schema));

    doc = doc.push(TTable::new(&["lock"]).set("profile", TValue::Str(lock.profile.clone())));

    if let Some(p) = &lock.product {
        doc = doc.push(
            TTable::new(&["lock", "product"])
                .set("name", TValue::Str(p.name.clone()))
                .set("version", TValue::Str(p.version.clone()))
                .set("stage", TValue::Str(p.stage.clone())),
        );
    }

    for p in &lock.plugins {
        doc = doc.push(
            TTable::array_table(&["lock", "plugins"])
                .set("name", TValue::Str(p.name.clone()))
                .set("version", TValue::Str(p.version.clone()))
                .set("compat_gen", TValue::Int(p.compat_gen as i64)),
        );
    }

    for u in &lock.units {
        doc = doc.push(
            TTable::array_table(&["lock", "units"])
                .set("id", TValue::Str(u.id.clone()))
                .set("provider", TValue::Str(u.provider.clone()))
                .set("unit", TValue::Str(u.unit.clone()))
                .set("api_iface", TValue::Str(u.api_iface.clone()))
                .set("version", TValue::Str(u.version.clone()))
                .set("compat_gen", TValue::Int(u.compat_gen as i64))
                .set("hash", TValue::Str(u.hash.clone()))
                .set("hash_scope", TValue::Str(u.hash_scope.clone()))
                .set("truth", TValue::Str(u.truth.clone())),
        );
    }

    format!("{}{}", machine_header("brickie.lock"), doc.render())
}

/// Canonical text of `build/index/dependents.json` (keys sorted, no timestamp).
pub fn render_dependents(index: &crate::model::DependentsIndex) -> String {
    let mut map = serde_json::Map::new();
    for (plugin, edges) in &index.dependents {
        let arr: Vec<serde_json::Value> = edges
            .iter()
            .map(|e| serde_json::json!({"from": e.from, "kind": e.kind}))
            .collect();
        map.insert(plugin.clone(), serde_json::Value::Array(arr));
    }
    let root = serde_json::json!({
        "schema": index.schema,
        "dependents": serde_json::Value::Object(map),
    });
    format!("{}\n", serde_json::to_string_pretty(&root).expect("JSON must be serializable"))
}

/// Snapshot emit primitives reused by iface.rs; shape example only, no hash computation. The
/// header carries `hash_scope` / `truth` and the `NOT_ABI` note (RV-3 / V-11).
pub fn render_snapshot(snap: &Snapshot) -> String {
    let mut doc = TDoc::new()
        .set("schema", TValue::Int(1))
        .set("unit", TValue::Str(snap.id.clone()))
        .set("provider", TValue::Str(snap.provider.clone()))
        .set("NOT_ABI", TValue::Str(
            "declaration-surface hash ≠ proof of ABI compatibility (v0.1 truth=decl, §6.6/RV-3)".to_string(),
        ));

    let mut head = TTable::new(&["unit_meta"]);
    head.push_str("api_iface", snap.api_iface.clone().unwrap_or_default());
    head.push_str("form", snap.form.clone().unwrap_or_default());
    head.push_str("version", snap.version.map(|v| v.to_string()).unwrap_or_default());
    head.push_int("compat_gen", snap.compat_gen.unwrap_or(0) as i64);
    head.push_str("freeze_state", snap.freeze_state.clone().unwrap_or_default());
    head.push_str("status", snap.status.clone().unwrap_or_default());
    head.push_str("hash", snap.hash.clone().unwrap_or_default());
    head.push_str("hash_scope", snap.hash_scope.clone().unwrap_or_default());
    head.push_str("truth", snap.truth.clone().unwrap_or_default());
    // IFACE-IR rule 10: `strict_params` must be stored, else hashes vary across environments.
    head.push_bool("strict_params", snap.strict_params.unwrap_or(false));
    // Ruling R-7: unfreeze-window baseline; present only inside such a window.
    if let Some(b) = &snap.baseline_hash {
        head.push_str("baseline_hash", b.clone());
    }
    doc = doc.push(head);

    for e in &snap.entries {
        let mut t = TTable::array_table(&["entry"]);
        t.push_str("kind", e.kind.clone());
        t.push_str("name", e.name.clone());
        if let Some(s) = &e.sig {
            t.push_str("sig", s.clone());
        }
        if let Some(l) = &e.layout {
            t.push_str("layout", l.clone());
        }
        if let Some(v) = &e.value {
            t.push_str("value", v.clone());
        }
        if let Some(o) = &e.ops {
            t.push_str_array("ops", o.clone());
        }
        t.push_str("status", e.status.clone());
        doc = doc.push(t);
    }

    for e in &snap.baseline_entries {
        let mut t = TTable::array_table(&["baseline_entry"]);
        t.push_str("kind", e.kind.clone());
        t.push_str("name", e.name.clone());
        if let Some(s) = &e.sig {
            t.push_str("sig", s.clone());
        }
        if let Some(l) = &e.layout {
            t.push_str("layout", l.clone());
        }
        if let Some(v) = &e.value {
            t.push_str("value", v.clone());
        }
        if let Some(o) = &e.ops {
            t.push_str_array("ops", o.clone());
        }
        t.push_str("status", e.status.clone());
        doc = doc.push(t);
    }

    format!("{}{}", machine_header(&format!("api/iface snapshot {}", snap.file)), doc.render())
}

pub fn machine_text_header(what: &str) -> String {
    machine_header(what)
}

//! 确定性 TOML / JSON 文本发射器(机器文件用)。
//!
//! 纪律(contract §4): 机器文件里**不得出现时间戳 / 随机数 / 主机相关信息**;
//! 同一输入两次运行 ⇒ 逐字节相同。本模块只做"结构 → 文本", 不排序语义 ——
//! 顺序由调用方(本 crate 内部)显式给定, 保证可复现。
//!
//! 对外导出:
//! - [`TDoc`] / [`TTable`] / [`TValue`] —— 可复用的"TOML 表 / 数组表"发射原语
//!   (快照序列化由 agent-B 在 iface.rs 里复用这些原语, 见 README)。
//! - [`render_lock`] —— `brickie.lock` 的规范化文本。
//! - [`render_dependents`] —— `build/index/dependents.json` 的规范化文本。

use crate::model::{Lock, Snapshot};

/// 生成物首行标记(与 cxx `text.h` 的 `kGeneratedMarker` / contract §6 同字面量)。
/// "识别这个文件是不是本工具的生成物"用。
pub const GENERATED_MARKER: &str = "brickie:generated";

/// TOML 标量 / 数组值。
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

/// 一个 TOML 表(或 `[[数组表]]`)节点。
#[derive(Clone, Debug, Default)]
pub struct TTable {
    /// 表头路径段, 如 `["plugin", "export"]`。
    pub header: Vec<String>,
    /// `true` ⇒ `[[header]]`(数组表)。
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

/// 一份 TOML 文档: 根键值 + 表(按插入序发射)。
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

    /// 渲染为文本; 以换行结尾, 无 BOM, `\n` 行尾(LF)。
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

/// TOML 基本字符串转义(只支持 basic string, 不用多行字符串)。
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

// ------------------------------------------------------------------ 机器文件

/// 机器 TOML 文件的公共文件头: 首行标记 + 一句"请勿手改"(无时间戳)。
fn machine_header(what: &str) -> String {
    format!(
        "# {GENERATED_MARKER}\n# {what} — 机器拥有的文件, 请勿手改; 同一输入两次运行逐字节相同(contract §4)\n\n"
    )
}

/// `brickie.lock` 的规范化文本(幂等 / 无时间戳)。
///
/// 形状 = `schema/lock.schema.json`(权威): `schema` + `[lock]` 下的 `profile` /
/// `plugins[]` / `units[]`。`[lock.product]` 是本实现的扩展(schema 的
/// `additionalProperties: true` 允许), 便于记录产品头。
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

/// `build/index/dependents.json` 的规范化文本(键排序, 无时间戳)。
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
    format!("{}\n", serde_json::to_string_pretty(&root).expect("JSON 必可序列化"))
}

/// 快照序列化的**可复用原语**(agent-B: iface.rs 用这些拼 `api/iface/<provider>/<unit>.toml`)。
///
/// 本函数给出一个**形状示例**(不含 hash 计算 —— hash 归 agent-B 的 IFACE-IR):
/// 快照头强制 `hash_scope` / `truth` 与 `NOT_ABI` 提示(RV-3 / V-11)。
pub fn render_snapshot(snap: &Snapshot) -> String {
    let mut doc = TDoc::new()
        .set("schema", TValue::Int(1))
        .set("unit", TValue::Str(snap.id.clone()))
        .set("provider", TValue::Str(snap.provider.clone()))
        .set("NOT_ABI", TValue::Str(
            "声明面 hash ≠ ABI 兼容证明(v0.1 truth=decl, §6.6/RV-3)".to_string(),
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
    // IFACE-IR 规则 10: `strict_params` 必须存进快照(否则跨环境 hash 抖动)。
    head.push_bool("strict_params", snap.strict_params.unwrap_or(false));
    // 裁定 R-7: 解冻窗口的基线(只有处于解冻窗口时才有)。
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

    format!("{}{}", machine_header(&format!("api/iface 快照 {}", snap.file)), doc.render())
}

/// 机器 TOML 文件的公共文件头(供 iface.rs 的提案/其它机器文件复用)。
pub fn machine_text_header(what: &str) -> String {
    machine_header(what)
}

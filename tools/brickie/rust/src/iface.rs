//! 版本面与接口发布面: `ver-show` / `ver-bump` / `iface-*`。
//!
//! 本模块是 V-4/V-5/V-6/V-11/V-15/V-16/V-17 的落点:
//!
//! - **IFACE-IR 规范化**(§6.2 的 10 条规则)+ `sha256` 域分隔 hash;
//! - **变更集**(§6.3): `ADDED`/`EXTENDED`/`CHANGED`/`SEMANTIC`/`REMOVED`/`STATUS`/
//!   `FREEZE`/`NONE`, append vs modify 按**条目内部结构**判(§5.4);
//! - **影响报告**: `dependents.direct/transitive/unsatisfied`;
//! - **版本推进**(§5.2 逐行)与**幂等**(§6.5): 面未变 ⇒ 空操作;
//! - **解冻窗口**(§5.3.4)+ `unfreeze` 记 baseline(裁定 R-7)、`refreeze` 比对;
//! - **写路径纪律**(§9.1.1): 只产出机器文件, `plugin.toml` 永不改写。
//!
//! ## 实现期裁定(见 rust/README.md §10)
//! - **S-8**: **首次发布 = 建档**(无旧快照 ⇒ 不推进任何段, 只记录当前声明版本与 hash);
//!   之后每次 publish 才按变更集推进。否则每个新单元一发布就烧掉一个 MINOR。
//! - **S-9**: 状态机命令(`deprecate`/`undeprecate`)的合法性按**快照状态**(发布记录)
//!   判定, 新面按声明面构造 ⇒ 重复执行是**空操作**(幂等)。
//! - **S-10**: `publish --profile release` 的"依赖方 compat_gen 仍匹配"门钩按 §6.5 表
//!   执行: 有 `COMPAT_GEN` 跃迁且存在 `unsatisfied` ⇒ 拒绝落盘(`BRV-VER-0001`)。
//! - **S-11**: `SEMANTIC` 变更在 v0.1 **无声明面输入**(纯行为语义不可从 TOML 观测)
//!   ⇒ 不产生该类别, `BRV-IFACE-0010` 无可报; 登记为缺口。
//! - **S-12**: `iface status` **不产 files**(它是门禁, 不是修复命令); 不一致时把
//!   "应有的快照"放在 `data.expected_snapshot` 里供人取用。

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::diag::{Diag, Diags, Severity};
use crate::emit::{self, TDoc, TTable, TValue};
use crate::model::{self, Plugin, SnapshotEntry, TreeLoad};
use crate::proto::{Request, Response};
use crate::version::{self, Version};

/// IFACE-IR 的 hash 域分隔前缀(规则 10: `hash_rev` 进域分隔)。
pub const HASH_REV: &str = "brickie-iface-ir/1";

// ================================================================== IFACE-IR

fn is_ident_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// `text` 里以**标识符边界**出现的 `from` 替换成 `to`。
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
        let ch = text[i..].chars().next().expect("i 在 char 边界上");
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

/// 规则 4: 类型别名展开到规范名(迭代到不动点, 限 8 轮防别名成环)。
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

/// 规则 2: 空白归一(单空格 / 无尾随); 同时消去运算符周围的排版差异。
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

/// 规则 3: **参数名不参与** sig(只留类型与顺序); `--strict-params` 时保留。
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

/// 规范化一个 `func` 的 `sig`。
pub fn canonical_sig(raw: &str, typedefs: &[(String, String)], strict_params: bool) -> String {
    let expanded = expand_typedefs(raw, typedefs);
    let stripped = if strict_params {
        expanded
    } else {
        strip_param_names(&expanded)
    };
    normalize_ws(&stripped)
}

/// 规范化 `type` 的 `layout`。
pub fn canonical_layout(raw: &str, typedefs: &[(String, String)]) -> String {
    normalize_ws(&expand_typedefs(raw, typedefs))
}

/// 一个接口面条目(声明面 / 快照面共用)。
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

    /// 只比"面"的字段(`status` 不进 hash, 见规则 6)。
    pub fn same_surface(&self, other: &Entry) -> bool {
        self.kind == other.kind
            && self.name == other.name
            && self.sig == other.sig
            && self.layout == other.layout
            && self.value == other.value
            && self.ops == other.ops
    }
}

/// 规则 1 + 5 + 6 + 7 + 9: 规范化正文(排序、字段选择、枚举按声明序)。
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
    // 规则 10: 域分隔 = hash_rev + unit id(防跨单元碰撞)。
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
        // 规则 9: macro/var 取 value、enum 取成员表(声明序)、service 取 ops 槽位摘要。
        if let Some(v) = &e.value {
            out.push_str(&format!("value {}\n", normalize_ws(v)));
        }
        if !e.ops.is_empty() {
            out.push_str(&format!("ops {}\n", e.ops.join(",")));
        }
    }
    out
}

/// `sha256:<64 hex>`(规则 8: SHA-256 over UTF-8 canonical text)。
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

/// 展示取前 12 位 hex(§5.1)。
pub fn short_hash(h: Option<&str>) -> String {
    match h {
        None => String::new(),
        Some(s) => {
            let hex = s.strip_prefix("sha256:").unwrap_or(s);
            hex.chars().take(12).collect()
        }
    }
}

// ================================================================== 面模型

/// 一个接口单元的"面"(可来自声明, 也可来自快照)。
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
    /// `plugin.toml [[export]].hash`(声明里的记录; 用于 `BRV-IFACE-0012`)。
    pub declared_hash: Option<String>,
    pub reexport_of: Vec<String>,
    pub symbols: Vec<String>,
    pub entries: Vec<Entry>,
    pub typedefs: Vec<(String, String)>,
}

impl Surface {
    pub fn from_declaration(tree: &TreeLoad, provider: &str, unit: &str, idx: usize) -> Surface {
        let p = tree.by_name(provider).expect("resolve 已保证存在");
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

    /// 一份快照(读取用)。
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

/// 裁定 R-11: `api/iface/<provider 路径段>/<unit>.toml`。
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

// ================================================================== 变更集

/// 一个变更条目(§6.3)。
#[derive(Clone, Debug)]
pub struct Change {
    pub kind: &'static str,
    pub name: String,
    pub from: Option<String>,
    pub to: Option<String>,
    /// 旧条目的治理状态(判"须解冻"用)。
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

    /// §6.3: `CHANGED`/`REMOVED` 且涉及已 `frozen`/`deprecated` 条目 ⇒ 须解冻。
    pub fn is_hard(&self) -> bool {
        matches!(self.kind, "CHANGED" | "REMOVED")
            && matches!(self.old_status.as_str(), "frozen" | "deprecated")
    }
}

/// 计算 `old`(通常 = 快照) → `new`(通常 = 声明 + 本次变更)的变更集。
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
                note: "新增条目(append-only, 免解冻)".to_string(),
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
                            "EXTENDED" => "已冻结条目的合法追加(枚举末尾加成员)".to_string(),
                            _ => "已有条目的 sig/layout/value/ops 变化(§5.4)".to_string(),
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
                        note: "治理状态转移(面未动)".to_string(),
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
                note: "条目消失(须已 deprecated + 满足弃用周期)".to_string(),
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
            note: "freeze_state 转移".to_string(),
        });
    }
    out.sort_by(|a, b| (a.kind, a.name.as_str()).cmp(&(b.kind, b.name.as_str())));
    out
}

/// §5.4 的 append vs modify 判定(落在**条目内部结构**上)。
pub fn classify_modify(oe: &Entry, ne: &Entry) -> &'static str {
    match oe.kind.as_str() {
        // 枚举末尾追加成员 ⇒ 追加; 重排/改值/删除 ⇒ 修改。
        "enum" => {
            let om: Vec<&str> = oe.value.as_deref().unwrap_or("").split(',').collect();
            let nm: Vec<&str> = ne.value.as_deref().unwrap_or("").split(',').collect();
            if nm.len() > om.len() && nm[..om.len()] == om[..] {
                "EXTENDED"
            } else {
                "CHANGED"
            }
        }
        // service: 新增 service 名 = 新增条目; 已有 ops 表加槽 = 修改。
        "service" => "CHANGED",
        // type(结构体): 加/删/重排字段 = 修改(D22)。
        "type" => "CHANGED",
        // macro/var: 改已有宏的值 ⇒ 修改。
        "macro" | "var" => "CHANGED",
        // func: 签名变化 ⇒ 修改(纯语义变更不可从声明面观测, 见 S-11)。
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

// ================================================================== 影响报告

/// 反向依赖报告(§6.3)。
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

/// 计算某个 `provider` 的依赖者(按已声明的 `[[dep]]` / `requires_iface` / `reexport_of`)。
///
/// `new_cg` / `new_ver` 用于判定 `unsatisfied`(失配者)。
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
                    format!("compat_gen 已由 {} 变 {new_cg}, 需重新确认", r.compat_gen)
                } else if !r.range.matches(new_ver.triple()) {
                    format!(
                        "range `{}` 不再满足新版本 {}",
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

    // 传递闭包(反向): 谁依赖 direct 集合。
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

// ================================================================== 环境

/// 命令的加载环境(IO 只在这里)。
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

/// 解析 `<provider>#<unit>` 或插件名(裁定 R-3)。
fn resolve(tree: &TreeLoad, id: &str) -> Result<(String, String, usize), String> {
    if let Some((prov, unit)) = id.split_once('#') {
        let p = tree
            .by_name(prov)
            .ok_or_else(|| format!("插件 `{prov}` 不在插件树里"))?;
        let idx = p
            .exports
            .iter()
            .position(|e| e.name == unit)
            .ok_or_else(|| format!("插件 `{prov}` 没有接口单元 `{unit}`"))?;
        Ok((prov.to_string(), unit.to_string(), idx))
    } else {
        let p = tree
            .by_name(id)
            .ok_or_else(|| format!("插件 `{id}` 不在插件树里"))?;
        if p.exports.len() != 1 {
            return Err(format!(
                "插件 `{id}` 有 {} 个 `[[export]]`, 不能唯一确定单元; 请用 `<provider>#<unit>`(裁定 R-3)",
                p.exports.len()
            ));
        }
        Ok((id.to_string(), p.exports[0].name.clone(), 0))
    }
}

/// note 路径存在性(相对 `root` 或当前工作目录)。
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

/// 有效冻结态: 任一处声明 `unfreezing` 即视为在解冻窗口。
pub fn effective_freeze(decl: &str, snap: Option<&str>) -> String {
    if decl == "unfreezing" || snap == Some("unfreezing") {
        "unfreezing".to_string()
    } else if let Some(s) = snap {
        s.to_string()
    } else {
        decl.to_string()
    }
}

/// 生成 `brickie.lock` 的正文(插件来自树, 单元来自已发布快照 + 本次更新)。
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

/// `api/iface/CHANGELOG.md` 的版本段(裁定 R-12: 段头 = `<unit> v<4 段> (compat_gen=<N>)`)。
fn changelog_content(root: &Path, unit_id: &str, ver: Version, hash: &str, summary: &str) -> String {
    let rel = "api/iface/CHANGELOG.md";
    let abs = root.join(rel);
    let existing = std::fs::read_to_string(&abs).unwrap_or_default();
    let header = "# brickie iface CHANGELOG\n\n\
                  > 机器拥有(`brickie-core` 产出); 按**版本段**追加, **无时间戳**(contract §4 / R-12)。\n";
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
        out.push_str(&format!("- 变更: {summary}\n"));
    }
    out
}

// ================================================================== 命令入口

/// `ver-show` / `ver-bump` / `iface-*` 的统一入口。
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
                format!("iface 不认识命令 `{other}`"),
                "见 contract §5 的命令表",
            );
            Response::ok(d, json!({}))
        }
    }
}

fn resolve_or_usage(env: &Env, id: &str) -> Result<(String, String, usize), Response> {
    resolve(&env.tree, id).map_err(|msg| {
        let mut d = env.diags.clone();
        d.usage(msg, "contract §5: `id` = `<provider>#<unit>` 或恰有一个 export 的插件名");
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
    // RV-11/C-12 要求 `ver show` **显式标注**"代不参与比较"。这是**输出语义**, 不是诊断:
    // 它由 `data.not_comparable = true` + L5 的呈现行承担(见 present.py 的 ver show),
    // 不占用 BRV 码 —— 用 `BRV-MF-0001` 去装一条纯提示会把机读消费方引向"这里有个声明面错误"。
    let d = env.diags.clone();
    let source_note = if compat_gen == 0 {
        "从未冻结(compat_gen = 0)".to_string()
    } else if published {
        format!("快照记录的已发布代(compat_gen = {compat_gen})")
    } else {
        format!("声明的 compat_gen = {compat_gen}")
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
            // RV-11 / C-12: 显式标注"代不参与比较"。这是**输出语义**(C-12 要求的显式标注),
            // 不是诊断 —— 用 BRV 码去装一条纯提示会把机读消费方引向"这里有个声明面错误"。
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
            format!("`--rule {rule}` 越界"),
            "合法值: major | minor | revise(§5.2/§7.7: compat_gen **不在** rule 里)",
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
                .hint("§5.2: 任一段只增不减"),
        );
        return Response::ok(d, json!({}));
    }

    // 面的条目: 快照优先(发布记录), 否则声明。
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
    // S-19: 版本推进不动解冻窗口。
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
                "`plugin.toml` 里应把 `[[export]].version` 改成 \"{new_ver}\"(裁定 R-3: 人写文件永不被工具改写)"
            ),
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span(format!("export[{idx}].version"))
        .hint("缺口代用(R-9): '应改成什么'无专属信息码"),
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
                    // 有效冻结态(S-19): 声明或快照任一处是 `unfreezing` 就算在窗口里 ——
                    // 与 check / publish / ver show 同一口径, 免得 list/show 与门禁互相打架。
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
    // NOTE_ABI 提示(V-11③)与 requires_iface 的 info(V-10③)。
    d.push(
        Diag::new(
            Severity::Info,
            "NOT_ABI: 声明面 hash ≠ ABI 兼容证明(v0.1 truth = 声明面; §6.6/RV-3)".to_string(),
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span(format!("export[{idx}]"))
        .hint("缺口代用(R-9): NOT_ABI 无专属信息码"),
    );
    if let Some(p) = env.tree.by_name(&prov) {
        for (ri, r) in p.compat.requires_iface.iter().enumerate() {
            d.push(
                Diag::new(
                    Severity::Info,
                    format!(
                        "声明面接口依赖 `{}`(compat_gen={}, range={}): v0.1 不扫描声明面接口依赖(V-10③)",
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
    // 有效值(S-19 口径): 快照是**发布记录**, 声明是**当前意图**; 解冻窗口里二者会不同,
    // 因此 unit/data 给出的必须是"有效冻结态"(任一处 unfreezing 即 unfreezing),
    // 否则 `iface show` 会与 `check --profile release`(BRV-IFACE-0009)的判定互相矛盾。
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

// ------------------------------------------------------------------ 版本推进辅助

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
            reasons: vec!["首次发布: 建档, 不推进任何段(裁定 S-8)".to_string()],
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
            "COMPAT_GEN+1: 已有 frozen/deprecated 条目被改或删({} 条)",
            hard.len()
        ));
        reasons.push("MINOR→0 / REVISE→0".to_string());
    } else if !changes.is_empty() {
        let kinds: BTreeSet<&str> = changes.iter().map(|c| c.kind).collect();
        reasons.push(format!(
            "MINOR+1: 变更集 {{{}}}(新增/追加/状态转移/仅 experimental 改动)",
            kinds.into_iter().collect::<Vec<_>>().join(",")
        ));
        reasons.push("REVISE→0".to_string());
    } else {
        reasons.push("面未变 ⇒ 空操作(§6.5)".to_string());
    }
    if set.is_some() {
        reasons.push("--set 覆盖本次写入的单元版本(裁定 R-8)".to_string());
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
                    format!("`{}` 尚无发布快照(未发布)⇒ 无法比对", decl.id),
                )
                .code("BRV-MF-0001")
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}]"))
                .hint("先 `brickie iface publish`; 缺口代用(R-9)"),
            );
        }
        Some(sh) => {
            if sh != &recomputed {
                consistent = false;
                d.push(
                    Diag::new(
                        Severity::Error,
                        format!(
                            "声明面 hash 与快照 hash 不等: 重算 {}, 快照 {}",
                            short_hash(Some(&recomputed)),
                            short_hash(Some(sh))
                        ),
                    )
                    .code("BRV-MF-0001")
                    .target(&prov)
                    .file(&decl.file)
                    .span(format!("export[{idx}]"))
                    .hint("缺口代用(R-9): '快照过期'无专属码; 手编快照 ⇒ 红(N-3)"),
                );
            }
        }
    }

    // V-11④ / BRV-IFACE-0012: 同一真值的两处记录必须相等。
    if let (Some(dh), Some(sh)) = (&decl.declared_hash, &snapshot_hash) {
        if !dh.is_empty() && !sh.is_empty() && dh != sh {
            consistent = false;
            d.push(
                Diag::new(
                    Severity::Error,
                    format!(
                        "`plugin.toml [[export]].hash`({}) 与快照 hash({}) 不等 —— hash 是同一真值的两处记录",
                        short_hash(Some(dh)),
                        short_hash(Some(sh))
                    ),
                )
                .code("BRV-IFACE-0012")
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}].hash"))
                .hint("§9.1.1 规则 4 / §10 V-11④: 手工改一处 ⇒ 红"),
            );
        }
    }

    let files = if !check && !consistent {
        // S-12: status 是门禁, 不写盘; 把"应有的快照"放进 data 供人取用。
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

    // `--set`(裁定 R-8): 覆盖本次写入的单元版本, 仍受单调约束。
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
                    "§5.1: `--set` 必须是四段 `COMPAT_GEN.MAJOR.MINOR.REVISE`",
                );
                return Response::ok(d, json!({}));
            }
        },
    };

    // ---- 幂等(§6.5): 面内容 + 条目集合 + status + freeze_state 全等 ⇒ 空操作 ----
    if let Some(s) = &snap {
        let old = Surface::from_snapshot(s);
        let same_entries = entries_equal(&old.entries, &decl.entries);
        // S-19: 冻结态比"有效冻结态"(声明 OR 快照)⇒ 窗口内重复 publish 仍是空操作。
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
                    "version_change": {"from": s.version.map(|v| v.to_string()), "to": s.version.map(|v| v.to_string()), "reasons": ["面未变 ⇒ 空操作(§6.5)"]},
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
                note: "首次发布: 建档".to_string(),
            })
            .collect::<Vec<_>>()
    } else {
        change_set(&old, &decl)
    };

    // ---- 门钩(§6.3/§6.5) ----
    let mut d = env.diags.clone();
    let hard: Vec<&Change> = changes.iter().filter(|c| c.is_hard()).collect();
    let eff_freeze = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    let mut blocked = false;
    if !hard.is_empty() {
        // REMOVED 必须已 deprecated(§6.3)。
        for c in &hard {
            if c.kind == "REMOVED" && c.old_status != "deprecated" {
                blocked = true;
                d.push(
                    Diag::new(
                        Severity::Error,
                        format!(
                            "`REMOVED` 条目 `{}` 仍是 `{}`: 删除必须先 `deprecated` 并满足弃用周期(§6.3/§6.4)",
                            c.name, c.old_status
                        ),
                    )
                    .code("BRV-MF-0001")
                    .target(&prov)
                    .file(&decl.file)
                    .span("export.entries")
                    .hint("先 `brickie iface deprecate`; 缺口代用(R-9)"),
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
                        "变更集命中'须解冻'的 {} 条(CHANGED/REMOVED 涉及已冻结条目), 但单元不在 `unfreezing`(当前 `{}`)",
                        hard.len(),
                        eff_freeze
                    ),
                )
                .code(code)
                .target(&prov)
                .file(&decl.file)
                .span(format!("export[{idx}].freeze_state"))
                .hint("§5.3.3 硬路径: unfreeze → 改/删 → refreeze; 缺口代用(R-9)"),
            );
        }
        if note.is_none() {
            blocked = true;
            d.push(
                Diag::new(
                    Severity::Error,
                    format!(
                        "变更集命中'须解冻'(§5.3.4 最高门槛)⇒ 必须带 `--note <决策记录>`({} 条)",
                        hard.len()
                    ),
                )
                .code("BRV-MF-0001")
                .target(&prov)
                .file(&decl.file)
                .span("--note")
                .hint("§5.3.4 规则 1 / V-6; 缺口代用(R-9): 缺 note 无专属码"),
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
                .hint("§5.2/§6.5: `--set` 必须 ≥ 当前版本"),
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
                    "发布前门钩(release): COMPAT_GEN 由 {} 变 {}, {} 个依赖方将失配",
                    plan.from.compat_gen,
                    plan.to.compat_gen,
                    dependents.unsatisfied.len()
                ),
            )
            .code("BRV-VER-0001")
            .target(&prov)
            .file(&decl.file)
            .span("export.version")
            .hint("§6.5: 变更后依赖方仍须满足 compat_gen 精确匹配; 先协调依赖方"),
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

    // ---- 写机器文件: 快照 + CHANGELOG + lock(§5.2 表) ----
    let mut new_surface = decl.clone();
    new_surface.version = plan.to;
    // 裁定 S-19: `publish` **不得**抹掉解冻窗口 —— 有效冻结态取"声明 OR 快照"。
    new_surface.freeze_state = eff_freeze.clone();
    // 且窗口内发布**消费**基线(新面成为新的承诺面), 使随后的 `refreeze` 只关窗口、
    // 不重复 bump `COMPAT_GEN`(避免"publish + refreeze 双 bump")。
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
        "首次发布(建档)".to_string()
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
                "`iface freeze` 需要 `--note <决策记录>`(§5.3.4 最高门槛 / A-26)".to_string(),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("缺口代用(R-9): 缺 note 无专属码"),
        );
        return Response::ok(d, json!({}));
    };
    if !note_exists(root, &note) {
        d.usage(
            format!("`--note` 指向的路径不存在: {note}"),
            "§5.3.4: note 是决策记录文件; 用法错 ⇒ 退出码 2",
        );
        return Response::ok(d, json!({}));
    }
    if let Some(e) = &entry {
        if !decl.entries.iter().any(|x| x.name == *e) {
            d.usage(
                format!("单元 `{}` 没有条目 `{e}`", decl.id),
                "`entry` 必须是 `[[export.entries]].name`",
            );
            return Response::ok(d, json!({}));
        }
    }

    // §6.4 / A-26: v0.1 只出"待升格提案", 不落 frozen 快照、不 bump COMPAT_GEN。
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
        "v0.1 只能生成待升格提案(§6.4 / A-26): 不落 frozen 快照、不 bump COMPAT_GEN; 真正升格在 v0.4+",
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
        emit::machine_text_header(&format!("接口升格提案 {}", decl.id)),
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
                format!("`iface {verb}` 需要 `--note <决策记录>`(§6.5)"),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("缺口代用(R-9): 缺 note 无专属码"),
        );
        return Response::ok(d, json!({}));
    };
    if !note_exists(root, &note) {
        d.usage(
            format!("`--note` 指向的路径不存在: {note}"),
            "§6.5: note 是决策记录文件; 用法错 ⇒ 退出码 2",
        );
        return Response::ok(d, json!({}));
    }

    let target_status = if verb == "deprecate" {
        "deprecated"
    } else {
        "frozen"
    };
    let mut new_surface = decl.clone();
    // 状态机命令不改冻结窗口(S-19): 有效冻结态取"声明 OR 快照"。
    new_surface.freeze_state = effective_freeze(
        &decl.freeze_state,
        snap.as_ref().and_then(|s| s.freeze_state.as_deref()),
    );
    let mut changed_any = false;
    let mut errors: Vec<String> = Vec::new();

    // 当前状态 = 发布记录(快照)优先 ⇒ 重复执行是空操作(裁定 S-9)。
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
                    format!("单元 `{}` 没有条目 `{e}`", decl.id),
                    "`entry` 必须是 `[[export.entries]].name`",
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
                "条目 `{name}` 当前是 `{cur}`, 不能直接 `{verb}` 到 `{target_status}`(状态机非法跳跃)"
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
    // 单元级状态(无 entry 指定时)。
    if entry.is_none() {
        let cur_unit = snap
            .as_ref()
            .and_then(|s| s.status.clone())
            .unwrap_or_else(|| decl.status.clone());
        if cur_unit == target_status {
            // 已是目标态
        } else if (verb == "deprecate" && cur_unit == "frozen")
            || (verb == "undeprecate" && cur_unit == "deprecated")
        {
            changed_any = true;
            new_surface.status = target_status.to_string();
        } else {
            errors.push(format!(
                "单元 `{}` 当前状态 `{cur_unit}`, 不能直接 `{verb}` 到 `{target_status}`",
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
                    .hint("§6.3 STATUS / §6.4: 状态转移合法性由引擎校验(如 experimental→deprecated 直跳 ⇒ 红)"),
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
                "`plugin.toml` 里 `[[export]]`/`[[export.entries]]` 的 `status` 应相应改成 `{target_status}`(裁定 R-3)"
            ),
        )
        .code("BRV-MF-0001")
        .target(&prov)
        .file(&decl.file)
        .span("export.status")
        .hint("缺口代用(R-9): '应改成什么'无专属信息码"),
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
                "`iface unfreeze` 需要 `--note <决策记录>`(§5.3.4 规则 1: 最高门槛 RFC)".to_string(),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("缺口代用(R-9): 缺 note 无专属码; V-16 观测点 1"),
        );
        return Response::ok(
            d,
            json!({"id": decl.id, "freeze_state": decl.freeze_state}),
        );
    };
    if !note_exists(root, &note) {
        d.usage(
            format!("`--note` 指向的路径不存在: {note}"),
            "§5.3.4: note 是决策记录文件; 用法错 ⇒ 退出码 2",
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
                format!("只有 `freeze_state = frozen` 的单元才能解冻; `{}` 当前是 `{cur}`", decl.id),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span(format!("export[{idx}].freeze_state"))
            .hint("§5.3.4: `frozen → unfreezing`; 缺口代用(R-9)"),
        );
        return Response::ok(d, json!({"id": decl.id, "freeze_state": cur}));
    }

    // 基线 = 当前面 hash + 条目表(裁定 R-7)。
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
                format!("`--note` 指向的路径不存在: {n}"),
                "§5.3.4: note 是决策记录文件; 用法错 ⇒ 退出码 2",
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
                format!("只有处于 `unfreezing` 的单元才能重新冻结; `{}` 当前是 `{cur}`", decl.id),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span(format!("export[{idx}].freeze_state"))
            .hint("先 `brickie iface unfreeze`; 缺口代用(R-9)"),
        );
        return Response::ok(d, json!({"id": decl.id, "freeze_state": cur}));
    }
    let Some(s) = &snap else {
        d.push(
            Diag::new(
                Severity::Error,
                format!("`{}` 没有快照/基线, 无法判定是否改了已冻结条目(裁定 R-7)", decl.id),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span(format!("export[{idx}].freeze_state"))
            .hint("先 `unfreeze`(它写基线); 缺口代用(R-9)"),
        );
        return Response::ok(d, json!({"id": decl.id, "freeze_state": cur}));
    };

    // 基线面(裁定 R-7): 与 baseline 比, 确有"改/删已冻结条目"才 COMPAT_GEN+1。
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
    // 用声明面的当前条目(若声明面没变则等于基线)。
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
        // 改/删已冻结条目必须带决策记录。
        d.push(
            Diag::new(
                Severity::Error,
                format!(
                    "`refreeze` 命中'改/删已冻结条目'({} 条)⇒ 需要 `--note <决策记录>`(§5.3.4)",
                    hard.len()
                ),
            )
            .code("BRV-MF-0001")
            .target(&prov)
            .file(&decl.file)
            .span("--note")
            .hint("缺口代用(R-9)"),
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
                    "空解冻 refreeze(四段全不动)"
                } else {
                    "refreeze(修改已冻结面)"
                },
            ),
        ),
    ];
    d.push(
        Diag::new(
            Severity::Info,
            if empty_unfreeze {
                format!(
                    "空解冻: `{}` 未改/删任何已冻结条目 ⇒ 四段全不动(§5.3.4 规则 2 / V-16)",
                    decl.id
                )
            } else {
                format!(
                    "`{}` 的 refreeze 记录了 {} 条变更(其中须解冻 {} 条)",
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
        .hint("缺口代用(R-9): 解冻窗口结论无专属信息码"),
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

/// 供 selftest / check 复用的: 由插件与单元取出声明面。
pub fn declaration_surface(tree: &TreeLoad, provider: &str, unit: &str, idx: usize) -> Surface {
    Surface::from_declaration(tree, provider, unit, idx)
}

/// 供 selftest 复用的: 插件模型(便于构造) ⇒ 声明面。
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

/// 供 selftest 复用的: 一个插件里所有单元的快照路径。
pub fn unit_ids_of(p: &Plugin) -> Vec<String> {
    p.unit_ids()
}

//! L0 声明面模型 + 加载器(contract §7.1 / §7.2 的字段表)。
//!
//! - 扫描 `root` 下的插件树: 递归找 `plugin.toml`, 跳过 `.git` / `build` /
//!   `prebuilts` / `tools` / `api` / `docs` / `Design` 与任何以 `.` 开头的目录,
//!   只挑真正含 `plugin.toml` 的目录。
//! - `Product` 可选(`root/product.toml`)。
//! - 形状错(缺必填 / 类型不符 / 名字契约不符 / 枚举越界 / 4 段版本串 / 4 段 range)
//!   ⇒ `BRV-MF-0001` 或 `BRV-VER-0005/0006`, 退出码 2(BRV-D2: schema 只表达
//!   单条记录形状; 跨字段不变量归引擎)。
//! - 单**记录内**的跨字段不变量(导出分类 TAX-0016..19、相位、特权)也在加载期报出,
//!   以满足 V-18a(`brickie-core --selftest` 独立可证)。**agent-B 的 check.rs 请勿重复报**。
//! - `Snapshot`(`api/iface/**`)与 `Lock`(`brickie.lock`)只**读**; 读不到就当"未发布"。
//!
//! 全部文本解析用 `toml::Value`(不依赖 serde derive), 判定在此模块 + `rules`。

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use crate::diag::{Diags, Severity};
use crate::rules;
use crate::version::{self, Range, Version};

/// 扫描时跳过的目录名(contract §7.1 的加载器纪律)。
pub const SKIP_DIRS: &[&str] = &[".git", "build", "prebuilts", "tools", "api", "docs", "Design"];

// ------------------------------------------------------------------ 模型类型

/// `[[res]]` 一条。
#[derive(Clone, Debug, Default)]
pub struct Res {
    pub kind: String,
    pub used_kib: i64,
    pub notes: Option<String>,
}

/// `[sched.tt]`(仅 `sched_class = "TT_SAFE"`)。
#[derive(Clone, Debug, Default)]
pub struct SchedTt {
    pub period_us: i64,
    pub deadline_us: i64,
    pub wcet_us: i64,
}

/// `[[compat.requires_iface]]`(v0.1 只校验 schema, 不参与求解 —— V-10)。
#[derive(Clone, Debug, Default)]
pub struct RequiresIface {
    pub id: String,
    pub api_iface: String,
    pub compat_gen: u64,
    pub range: Range,
    pub range_raw: String,
    pub mode: String,
}

/// `[compat]`。
#[derive(Clone, Debug, Default)]
pub struct Compat {
    pub core: Option<Range>,
    pub core_raw: Option<String>,
    pub api_rev: Option<i64>,
    pub hash_scope: Option<String>,
    pub truth: Option<String>,
    pub abi_id: Option<String>,
    pub requires_iface: Vec<RequiresIface>,
}

/// `[[dep]]` 一条(结构依赖; 一律**不钉** `compat_gen`, 依 F3)。
#[derive(Clone, Debug, Default)]
pub struct Dep {
    pub name: String,
    pub range: Range,
    pub range_raw: String,
    pub kind: String,
    /// 相位**断言**(仅 `kind = "init"`; 缺省 = 依赖方自身相位, §7.2 R2)。
    pub phase: Option<String>,
    pub symbol: Option<String>,
}

/// `[[export.entries]]` 一条。
#[derive(Clone, Debug, Default)]
pub struct ExportEntry {
    pub kind: String,
    pub name: String,
    pub sig: Option<String>,
    pub layout: Option<String>,
    pub value: Option<String>,
    pub ops: Option<Vec<String>>,
    pub status: String,
}

/// `[[export]]` 一条(接口单元 = 冻结与版本的基本粒度, F1)。
#[derive(Clone, Debug, Default)]
pub struct Export {
    pub api_iface: String,
    pub form: String,
    pub name: String,
    pub version: Option<Version>,
    pub version_raw: Option<String>,
    pub compat_gen: Option<u64>,
    pub freeze_state: Option<String>,
    pub hash: Option<String>,
    pub status: Option<String>,
    pub reexport_of: Vec<String>,
    pub symbols: Vec<String>,
    pub entries: Vec<ExportEntry>,
}

impl Export {
    /// 接口单元 id = `<provider>#<unit>`。
    pub fn unit_id(&self, plugin_name: &str) -> String {
        format!("{}#{}", plugin_name, self.name)
    }
}

/// `[privileged.resources]`。
#[derive(Clone, Debug, Default)]
pub struct PrivilegedResources {
    pub irq: Vec<i64>,
    pub dma_channels: Vec<i64>,
    pub pins: Vec<i64>,
    pub device_names: Vec<String>,
}

impl PrivilegedResources {
    pub fn is_empty(&self) -> bool {
        self.irq.is_empty()
            && self.dma_channels.is_empty()
            && self.pins.is_empty()
            && self.device_names.is_empty()
    }
}

/// `[privileged]`。
#[derive(Clone, Debug, Default)]
pub struct PrivilegedDecl {
    pub level: Option<String>,
    pub memory: Vec<rules::MemoryEntry>,
    pub resources: PrivilegedResources,
}

/// `[build]`(v0.1 只记录)。
#[derive(Clone, Debug, Default)]
pub struct BuildDecl {
    pub sources: Vec<String>,
    pub includes: Vec<String>,
}

/// 一个插件的规范化模型。
#[derive(Clone, Debug, Default)]
pub struct Plugin {
    /// 相对 `root` 的 `plugin.toml` 路径。
    pub file: String,
    /// 相对 `root` 的插件目录。
    pub dir: String,
    pub schema: i64,
    pub name: String,
    pub plugin_type: String,
    pub api_type: String,
    pub subkind: Option<String>,
    pub lang: String,
    pub phase: String,
    pub sched_class: String,
    pub sched_kind: Option<String>,
    pub version: Version,
    pub version_raw: String,
    pub summary: Option<String>,
    pub license: Option<String>,
    pub res: Vec<Res>,
    pub sched_tt: Option<SchedTt>,
    pub compat: Compat,
    pub typedefs: Vec<(String, String)>,
    pub deps: Vec<Dep>,
    pub exports: Vec<Export>,
    pub privileged: Option<PrivilegedDecl>,
    pub build: Option<BuildDecl>,
}

impl Plugin {
    /// `[[res]]` 求和: `(ram_kib, stack_kib)`。
    pub fn res_totals(&self) -> (i64, i64) {
        let mut ram = 0;
        let mut stack = 0;
        for r in &self.res {
            match r.kind.as_str() {
                "ram" => ram += r.used_kib,
                "stack" => stack += r.used_kib,
                _ => {}
            }
        }
        (ram, stack)
    }

    /// 导出单元 id 列表。
    pub fn unit_ids(&self) -> Vec<String> {
        self.exports.iter().map(|e| e.unit_id(&self.name)).collect()
    }

    pub fn find_export(&self, unit: &str) -> Option<&Export> {
        self.exports.iter().find(|e| e.name == unit)
    }

    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let res: Vec<_> = self
            .res
            .iter()
            .map(|r| json!({"kind": r.kind, "used_kib": r.used_kib, "notes": r.notes}))
            .collect();
        let deps: Vec<_> = self
            .deps
            .iter()
            .map(|d| {
                json!({
                    "name": d.name,
                    "range": d.range.canonical(),
                    "range_raw": d.range_raw,
                    "kind": d.kind,
                    "phase": d.phase,
                    "symbol": d.symbol,
                })
            })
            .collect();
        let exports: Vec<_> = self
            .exports
            .iter()
            .map(|e| {
                let entries: Vec<_> = e
                    .entries
                    .iter()
                    .map(|en| {
                        json!({
                            "kind": en.kind,
                            "name": en.name,
                            "sig": en.sig,
                            "layout": en.layout,
                            "value": en.value,
                            "ops": en.ops,
                            "status": en.status,
                        })
                    })
                    .collect();
                json!({
                    "id": e.unit_id(&self.name),
                    "api_iface": e.api_iface,
                    "form": e.form,
                    "name": e.name,
                    "version": e.version.map(|v| v.to_string()),
                    "compat_gen": e.compat_gen,
                    "freeze_state": e.freeze_state,
                    "hash": e.hash,
                    "status": e.status,
                    "reexport_of": e.reexport_of,
                    "symbols": e.symbols,
                    "entries": entries,
                })
            })
            .collect();
        let requires: Vec<_> = self
            .compat
            .requires_iface
            .iter()
            .map(|r| {
                json!({
                    "id": r.id,
                    "api_iface": r.api_iface,
                    "compat_gen": r.compat_gen,
                    "range": r.range.canonical(),
                    "mode": r.mode,
                })
            })
            .collect();
        let typedefs: serde_json::Map<String, serde_json::Value> = self
            .typedefs
            .iter()
            .map(|(k, v)| (k.clone(), serde_json::Value::String(v.clone())))
            .collect();
        let privileged = self.privileged.as_ref().map(|p| {
            let mem: Vec<_> = p
                .memory
                .iter()
                .map(|m| {
                    json!({
                        "granularity": m.granularity,
                        "ops": m.ops,
                        "regions": m.regions,
                    })
                })
                .collect();
            json!({
                "level": p.level,
                "memory": mem,
                "resources": {
                    "irq": p.resources.irq,
                    "dma_channels": p.resources.dma_channels,
                    "pins": p.resources.pins,
                    "device_names": p.resources.device_names,
                },
            })
        });
        json!({
            "file": self.file,
            "dir": self.dir,
            "schema": self.schema,
            "name": self.name,
            "plugin_type": self.plugin_type,
            "api_type": self.api_type,
            "subkind": self.subkind,
            "lang": self.lang,
            "phase": self.phase,
            "sched_class": self.sched_class,
            "sched_kind": self.sched_kind,
            "version": self.version.to_string(),
            "summary": self.summary,
            "license": self.license,
            "res": res,
            "res_totals": {"ram_kib": self.res_totals().0, "stack_kib": self.res_totals().1},
            "sched_tt": self.sched_tt.as_ref().map(|t| json!({
                "period_us": t.period_us, "deadline_us": t.deadline_us, "wcet_us": t.wcet_us,
            })),
            "compat": {
                "core": self.compat.core.as_ref().map(|r| r.canonical()),
                "api_rev": self.compat.api_rev,
                "hash_scope": self.compat.hash_scope,
                "truth": self.compat.truth,
                "abi_id": self.compat.abi_id,
                "requires_iface": requires,
            },
            "iface": {"typedefs": serde_json::Value::Object(typedefs)},
            "dep": deps,
            "export": exports,
            "privileged": privileged,
            "build": self.build.as_ref().map(|b| json!({
                "sources": b.sources, "includes": b.includes,
            })),
            "unit_ids": self.unit_ids(),
        })
    }
}

/// `product.toml` 的规范化模型。
#[derive(Clone, Debug, Default)]
pub struct Product {
    pub file: String,
    pub schema: i64,
    pub name: String,
    pub version: Version,
    pub app: String,
    pub core: Option<Range>,
    pub stage: String,
    pub select: Vec<String>,
    pub budget_ram_kib: Option<i64>,
    pub budget_stack_kib: Option<i64>,
    pub lint_frozen_deps: String,
    pub allow_edges: Vec<(String, String)>,
}

impl Product {
    /// §7.5: `[lint].frozen_deps` 覆盖 `stage` 的默认。
    pub fn release_strict(&self) -> bool {
        match self.lint_frozen_deps.as_str() {
            "deny" => true,
            "allow" => false,
            _ => self.stage == "release",
        }
    }

    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let edges: Vec<_> = self
            .allow_edges
            .iter()
            .map(|(a, b)| json!([a, b]))
            .collect();
        json!({
            "file": self.file,
            "schema": self.schema,
            "name": self.name,
            "version": self.version.to_string(),
            "app": self.app,
            "core": self.core.as_ref().map(|r| r.canonical()),
            "stage": self.stage,
            "select": self.select,
            "budget": {"ram_kib": self.budget_ram_kib, "stack_kib": self.budget_stack_kib},
            "lint": {"frozen_deps": self.lint_frozen_deps, "allow_edges": edges},
        })
    }
}

/// 插件树加载结果。
#[derive(Clone, Debug, Default)]
pub struct TreeLoad {
    pub diags: Diags,
    pub plugins: Vec<Plugin>,
    pub product: Option<Product>,
    /// 是否找到 `product.toml`(R-1 的分支判据)。
    pub has_product: bool,
}

impl TreeLoad {
    pub fn by_name(&self, name: &str) -> Option<&Plugin> {
        self.plugins.iter().find(|p| p.name == name)
    }

    pub fn allow_edges(&self) -> Vec<(String, String)> {
        self.product
            .as_ref()
            .map(|p| p.allow_edges.clone())
            .unwrap_or_default()
    }

    /// 裁定 R-6: `plugin_type = "platform"` 的 `[[res]]` 同时是**平台容量**。
    ///
    /// `(ram_kib, stack_kib)`; 多个 platform 时求和(理论上每镜像恰 1)。
    pub fn platform_capacity(&self) -> (i64, i64) {
        let mut ram = 0;
        let mut stack = 0;
        for p in &self.plugins {
            if p.plugin_type == "platform" {
                let (r, s) = p.res_totals();
                ram += r;
                stack += s;
            }
        }
        (ram, stack)
    }

    /// 裁定 R-1: 无 `product.toml` ⇒ 校验整棵插件树, 跳过产品级数量约束。
    pub fn product_level_constraints_apply(&self) -> bool {
        self.has_product
    }
}

// ------------------------------------------------------------------ 形状辅助

fn req_str(
    t: &toml::Table,
    key: &str,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<String> {
    match t.get(key) {
        None => {
            d.shape(
                "BRV-MF-0001",
                target,
                file,
                format!("{span}.{key}"),
                format!("缺少必填字段 `{key}`"),
                "contract §7 字段表",
            );
            None
        }
        Some(v) => match v.as_str() {
            Some(s) => Some(s.to_string()),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("字段 `{key}` 形状不符: 期望字符串"),
                    "contract §7 字段表",
                );
                None
            }
        },
    }
}

fn opt_str(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Option<String> {
    match t.get(key) {
        None => None,
        Some(v) => match v.as_str() {
            Some(s) => Some(s.to_string()),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("字段 `{key}` 形状不符: 期望字符串"),
                    "contract §7 字段表",
                );
                None
            }
        },
    }
}

fn req_int(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Option<i64> {
    match t.get(key) {
        None => {
            d.shape(
                "BRV-MF-0001",
                target,
                file,
                format!("{span}.{key}"),
                format!("缺少必填字段 `{key}`"),
                "contract §7 字段表",
            );
            None
        }
        Some(v) => match v.as_integer() {
            Some(i) => Some(i),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("字段 `{key}` 形状不符: 期望整数"),
                    "contract §7 字段表",
                );
                None
            }
        },
    }
}

fn opt_int(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Option<i64> {
    match t.get(key) {
        None => None,
        Some(v) => match v.as_integer() {
            Some(i) => Some(i),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("字段 `{key}` 形状不符: 期望整数"),
                    "contract §7 字段表",
                );
                None
            }
        },
    }
}

fn str_array(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Vec<String> {
    match t.get(key) {
        None => Vec::new(),
        Some(v) => match v.as_array() {
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("字段 `{key}` 形状不符: 期望字符串数组"),
                    "contract §7 字段表",
                );
                Vec::new()
            }
            Some(arr) => {
                let mut out = Vec::new();
                for (i, item) in arr.iter().enumerate() {
                    match item.as_str() {
                        Some(s) => out.push(s.to_string()),
                        None => d.shape(
                            "BRV-MF-0001",
                            target,
                            file,
                            format!("{span}.{key}[{i}]"),
                            format!("字段 `{key}[{i}]` 形状不符: 期望字符串"),
                            "contract §7 字段表",
                        ),
                    }
                }
                out
            }
        },
    }
}

fn int_array(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Vec<i64> {
    match t.get(key) {
        None => Vec::new(),
        Some(v) => match v.as_array() {
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("字段 `{key}` 形状不符: 期望整数数组"),
                    "contract §7 字段表",
                );
                Vec::new()
            }
            Some(arr) => {
                let mut out = Vec::new();
                for (i, item) in arr.iter().enumerate() {
                    match item.as_integer() {
                        Some(n) => out.push(n),
                        None => d.shape(
                            "BRV-MF-0001",
                            target,
                            file,
                            format!("{span}.{key}[{i}]"),
                            format!("字段 `{key}[{i}]` 形状不符: 期望整数"),
                            "contract §7 字段表",
                        ),
                    }
                }
                out
            }
        },
    }
}

fn opt_table<'a>(t: &'a toml::Table, key: &str) -> Option<&'a toml::Table> {
    t.get(key).and_then(|v| v.as_table())
}

/// 按路径取嵌套值(`[sched.tt]` ⇒ `["sched","tt"]`)。
fn dig<'a>(t: &'a toml::Table, path: &[&str]) -> Option<&'a toml::Value> {
    let (first, rest) = path.split_first()?;
    let mut cur = t.get(*first)?;
    for k in rest {
        cur = cur.as_table()?.get(*k)?;
    }
    Some(cur)
}

fn opt_sub_table<'a>(t: &'a toml::Table, path: &[&str]) -> Option<&'a toml::Table> {
    dig(t, path).and_then(|v| v.as_table())
}

fn array_of_tables<'a>(t: &'a toml::Table, key: &str) -> Vec<&'a toml::Table> {
    match t.get(key).and_then(|v| v.as_array()) {
        None => Vec::new(),
        Some(arr) => arr.iter().filter_map(|v| v.as_table()).collect(),
    }
}

/// `[[export.entries]].value`: 裁定 R-4 / schema 的 `oneOf[string, string[]]`。
///
/// 数组形态规范化为 `,` 连接(进 hash 的输入必须是单一确定字符串; 见 IFACE-IR 规则 9)。
fn load_entry_value(
    t: &toml::Table,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<String> {
    let v = t.get("value")?;
    if let Some(s) = v.as_str() {
        return Some(s.to_string());
    }
    if let Some(arr) = v.as_array() {
        let mut parts = Vec::with_capacity(arr.len());
        let mut ok = true;
        for (i, item) in arr.iter().enumerate() {
            match item.as_str() {
                Some(s) => parts.push(s.to_string()),
                None => {
                    ok = false;
                    d.shape(
                        "BRV-MF-0001",
                        target,
                        file,
                        format!("{span}.value[{i}]"),
                        "`value` 数组的元素必须是字符串(裁定 R-4)",
                        "contract §7.1 / §6.2 规则 9",
                    );
                }
            }
        }
        return if ok { Some(parts.join(",")) } else { None };
    }
    d.shape(
        "BRV-MF-0001",
        target,
        file,
        format!("{span}.value"),
        "`value` 形状不符: 期望字符串或字符串数组(裁定 R-4)",
        "contract §7.1",
    );
    None
}

fn shape_enum(
    d: &mut Diags,
    target: &str,
    file: &str,
    span: &str,
    key: &str,
    val: &str,
    allowed: &[&str],
) {
    d.shape(
        "BRV-MF-0001",
        target,
        file,
        format!("{span}.{key}"),
        format!(
            "字段 `{key}` 取值 `{val}` 越界; 合法值: {}",
            rules::list_of(allowed)
        ),
        "contract §7 字段表",
    );
}

fn check_enum(
    d: &mut Diags,
    target: &str,
    file: &str,
    span: &str,
    key: &str,
    val: &str,
    allowed: &[&str],
) -> bool {
    if rules::in_list(allowed, val) {
        true
    } else {
        shape_enum(d, target, file, span, key, val, allowed);
        false
    }
}

/// 解析 `version` 字段(四段); 形状错 ⇒ `BRV-VER-0005` + 退出码 2。
fn parse_version_field(
    raw: &str,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<Version> {
    match version::parse_version(raw) {
        Ok(v) => Some(v),
        Err(e) => {
            d.shape(e.code, target, file, span, e.message, "§5.1: 四段 `COMPAT_GEN.MAJOR.MINOR.REVISE`");
            None
        }
    }
}

/// 解析 `range` 字段; 4 段 ⇒ `BRV-VER-0006`; 其它形状错 ⇒ `BRV-MF-0001`。均退出码 2。
fn parse_range_field(
    raw: &str,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<Range> {
    match Range::parse(raw) {
        Ok(r) => Some(r),
        Err(e) => {
            let code = if e.code == version::VER_0006 {
                version::VER_0006
            } else {
                "BRV-MF-0001"
            };
            d.shape(code, target, file, span, e.message, "§7.4: range 固定 3 段, 缺段右补 0, 4 段非法");
            None
        }
    }
}

// ------------------------------------------------------------------ plugin.toml

fn rel_path(root: &Path, p: &Path) -> String {
    p.strip_prefix(root)
        .unwrap_or(p)
        .to_string_lossy()
        .replace('\\', "/")
}

/// 解析一个 `plugin.toml`。返回 `None` 只在"连 TOML 都读不成"时。
pub fn load_plugin_file(root: &Path, abs: &Path, d: &mut Diags) -> Option<Plugin> {
    let file = rel_path(root, abs);
    let dir = abs
        .parent()
        .map(|p| rel_path(root, p))
        .unwrap_or_default();
    let text = match fs::read_to_string(abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("读不到 `{file}`: {e}"), "检查路径与权限");
            return None;
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(
                format!("`{file}` 不是合法 TOML: {e}"),
                "BRV-D9 退出码 2(环境/用法错, 无码)",
            );
            return None;
        }
    };
    let top = match value.as_table() {
        Some(t) => t,
        None => {
            d.env(format!("`{file}` 顶层必须是 TOML 表"), "contract §7.1");
            return None;
        }
    };
    let target = file.as_str();

    let mut p = Plugin {
        file: file.clone(),
        dir,
        schema: 0,
        lang: "c".to_string(), // 缺省按 §8.1 的 c; 仍要求显式(见下)
        sched_class: "SAFE_PREEMPT".to_string(),
        version: Version::new(0, 0, 0, 0),
        ..Default::default()
    };

    // schema
    match req_int(top, "schema", target, &file, "", d) {
        Some(1) => p.schema = 1,
        Some(other) => d.shape(
            "BRV-MF-0001",
            target,
            &file,
            "schema",
            format!("`schema` 只支持 1, 收到 {other}"),
            "contract §7.1",
        ),
        None => {}
    }

    let Some(plugin) = opt_table(top, "plugin") else {
        d.shape(
            "BRV-MF-0001",
            target,
            &file,
            "plugin",
            "缺少必填表 `[plugin]`",
            "contract §7.1",
        );
        return Some(p);
    };

    p.name = req_str(plugin, "name", target, &file, "plugin", d).unwrap_or_default();
    if !p.name.is_empty() && !rules::valid_plugin_name(&p.name) {
        d.shape(
            "BRV-MF-0001",
            &p.name,
            &file,
            "plugin.name",
            format!("插件名 `{}` 不符名字契约", p.name),
            "契约: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)",
        );
    }
    p.plugin_type =
        req_str(plugin, "plugin_type", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.plugin_type.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "plugin_type", &p.plugin_type, rules::PLUGIN_TYPES);
    }
    p.api_type = req_str(plugin, "api_type", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.api_type.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "api_type", &p.api_type, rules::API_TYPES);
    }
    p.subkind = opt_str(plugin, "subkind", &p.name, &file, "plugin", d);
    if let Some(sk) = &p.subkind {
        check_enum(d, &p.name, &file, "plugin", "subkind", sk, rules::SUBKINDS);
    }
    p.lang = req_str(plugin, "lang", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.lang.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "lang", &p.lang, rules::LANGS);
    }
    p.phase = req_str(plugin, "phase", &p.name, &file, "plugin", d).unwrap_or_default();
    p.sched_class = opt_str(plugin, "sched_class", &p.name, &file, "plugin", d)
        .unwrap_or_else(|| "SAFE_PREEMPT".to_string());
    if !p.sched_class.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "sched_class", &p.sched_class, rules::SCHED_CLASSES);
    }
    p.sched_kind = opt_str(plugin, "sched_kind", &p.name, &file, "plugin", d);
    if let Some(k) = &p.sched_kind {
        check_enum(d, &p.name, &file, "plugin", "sched_kind", k, rules::SCHED_KINDS);
        if p.subkind.as_deref() != Some("scheduler") {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                "plugin.sched_kind",
                "`sched_kind` 只对 `subkind = \"scheduler\"` 有意义",
                "contract §7.1 / 裁定 R-5",
            );
        }
    }
    p.version_raw = req_str(plugin, "version", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.version_raw.is_empty() {
        if let Some(v) = parse_version_field(&p.version_raw, &p.name, &file, "plugin.version", d) {
            p.version = v;
        }
    }
    p.summary = opt_str(plugin, "summary", &p.name, &file, "plugin", d);
    p.license = opt_str(plugin, "license", &p.name, &file, "plugin", d);

    // ---- subkind 推导与冲突(§8.4 / BRV-TAX-0015)----
    validate_subkind(&mut p, d);

    // ---- 相位与 plugin_type 约束(§7.2; schema/规则错)----
    if !p.phase.is_empty() && !p.plugin_type.is_empty() {
        if let Some(reason) = rules::phase_reason(&p.plugin_type, p.subkind.as_deref(), &p.phase) {
            d.shape("BRV-MF-0001", &p.name, &file, "plugin.phase", reason, "§7.2");
        }
    }

    // ---- [[res]] ----
    for (i, t) in array_of_tables(top, "res").into_iter().enumerate() {
        let span = format!("res[{i}]");
        let kind = req_str(t, "kind", &p.name, &file, &span, d).unwrap_or_default();
        if !kind.is_empty() {
            check_enum(d, &p.name, &file, &span, "kind", &kind, rules::RES_KINDS);
        }
        let used = req_int(t, "used_kib", &p.name, &file, &span, d).unwrap_or(0);
        let notes = opt_str(t, "notes", &p.name, &file, &span, d);
        p.res.push(Res {
            kind,
            used_kib: used,
            notes,
        });
    }

    // ---- [sched.tt] ----
    if let Some(tt) = opt_sub_table(top, &["sched", "tt"]) {
        p.sched_tt = Some(SchedTt {
            period_us: req_int(tt, "period_us", &p.name, &file, "sched.tt", d).unwrap_or(0),
            deadline_us: req_int(tt, "deadline_us", &p.name, &file, "sched.tt", d).unwrap_or(0),
            wcet_us: req_int(tt, "wcet_us", &p.name, &file, "sched.tt", d).unwrap_or(0),
        });
    } else if p.sched_class == "TT_SAFE" {
        d.shape(
            "BRV-MF-0001",
            &p.name,
            &file,
            "sched.tt",
            "`sched_class = \"TT_SAFE\"` 必须同时给 `[sched.tt]` 周期元数据",
            "§8.1",
        );
    }

    // ---- [compat] ----
    load_compat(top, &mut p, d);

    // ---- [iface.typedefs] ----
    if let Some(td) = opt_sub_table(top, &["iface", "typedefs"]) {
        for (k, v) in td {
            match v.as_str() {
                Some(s) => p.typedefs.push((k.clone(), s.to_string())),
                None => d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    format!("iface.typedefs.{k}"),
                    format!("类型别名 `{k}` 的目标必须是字符串(规范名)"),
                    "contract §7.1 / IFACE-IR 规则 4",
                ),
            }
        }
        p.typedefs.sort();
    }

    // ---- [[dep]] ----
    for (i, t) in array_of_tables(top, "dep").into_iter().enumerate() {
        let span = format!("dep[{i}]");
        let name = req_str(t, "name", &p.name, &file, &span, d).unwrap_or_default();
        if !name.is_empty() && !rules::valid_plugin_name(&name) {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                format!("{span}.name"),
                format!("依赖名 `{name}` 不符名字契约"),
                "契约: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)",
            );
        }
        if t.contains_key("compat_gen") {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                format!("{span}.compat_gen"),
                "结构依赖(`[[dep]]`)一律不钉 `compat_gen`(依 F3)",
                "§7.1: 钉代只属接口依赖(`requires_iface`)",
            );
        }
        let range_raw = opt_str(t, "range", &p.name, &file, &span, d).unwrap_or_else(|| "*".to_string());
        let range = parse_range_field(&range_raw, &p.name, &file, format!("{span}.range").as_str(), d)
            .unwrap_or_else(Range::any);
        let kind = req_str(t, "kind", &p.name, &file, &span, d).unwrap_or_default();
        if !kind.is_empty() {
            check_enum(d, &p.name, &file, &span, "kind", &kind, rules::DEP_KINDS);
        }
        let phase = opt_str(t, "phase", &p.name, &file, &span, d);
        if let Some(ph) = &phase {
            if !rules::in_list(rules::PHASES, ph) {
                shape_enum(d, &p.name, &file, &span, "phase", ph, rules::PHASES);
            }
            if kind != "init" {
                d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    format!("{span}.phase"),
                    "`[[dep]].phase` 只对 `kind = \"init\"` 有意义",
                    "§7.2: 断言只描述 init 完成点",
                );
            }
        }
        let symbol = opt_str(t, "symbol", &p.name, &file, &span, d);
        if symbol.is_some() && kind != "runtime" {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                format!("{span}.symbol"),
                "`[[dep]].symbol` 只对 `kind = \"runtime\"` 有意义",
                "contract §7.1",
            );
        }
        p.deps.push(Dep {
            name,
            range,
            range_raw,
            kind,
            phase,
            symbol,
        });
    }

    // ---- [[export]] ----
    for (i, t) in array_of_tables(top, "export").into_iter().enumerate() {
        let span = format!("export[{i}]");
        let mut e = Export::default();
        e.api_iface = req_str(t, "api_iface", &p.name, &file, &span, d).unwrap_or_default();
        if !e.api_iface.is_empty() {
            check_enum(d, &p.name, &file, &span, "api_iface", &e.api_iface, rules::API_TYPES);
        }
        e.form = req_str(t, "form", &p.name, &file, &span, d).unwrap_or_default();
        if !e.form.is_empty() {
            check_enum(d, &p.name, &file, &span, "form", &e.form, rules::EXPORT_FORMS);
        }
        e.name = req_str(t, "name", &p.name, &file, &span, d).unwrap_or_default();
        // schema/plugin.schema.json: `version` / `compat_gen` / `freeze_state` / `status` 均 required。
        let vraw = req_str(t, "version", &p.name, &file, &span, d);
        e.version_raw = vraw.clone();
        if let Some(raw) = &vraw {
            e.version = parse_version_field(raw, &p.name, &file, format!("{span}.version").as_str(), d);
        }
        e.compat_gen = req_int(t, "compat_gen", &p.name, &file, &span, d).map(|v| v.max(0) as u64);
        e.freeze_state = req_str(t, "freeze_state", &p.name, &file, &span, d);
        if let Some(fs) = &e.freeze_state {
            check_enum(d, &p.name, &file, &span, "freeze_state", fs, rules::FREEZE_STATES);
        }
        e.hash = opt_str(t, "hash", &p.name, &file, &span, d);
        e.status = req_str(t, "status", &p.name, &file, &span, d);
        if let Some(st) = &e.status {
            check_enum(d, &p.name, &file, &span, "status", st, rules::ENTRY_STATUSES);
        }
        e.reexport_of = str_array(t, "reexport_of", &p.name, &file, &span, d);
        e.symbols = str_array(t, "symbols", &p.name, &file, &span, d);
        for (j, et) in array_of_tables(t, "entries").into_iter().enumerate() {
            let espan = format!("{span}.entries[{j}]");
            let mut en = ExportEntry::default();
            en.kind = req_str(et, "kind", &p.name, &file, &espan, d).unwrap_or_default();
            if !en.kind.is_empty() {
                check_enum(d, &p.name, &file, &espan, "kind", &en.kind, rules::ENTRY_KINDS);
            }
            en.name = req_str(et, "name", &p.name, &file, &espan, d).unwrap_or_default();
            en.sig = opt_str(et, "sig", &p.name, &file, &espan, d);
            en.layout = opt_str(et, "layout", &p.name, &file, &espan, d);
            // 裁定 R-4 / schema: `value` = 字符串**或**字符串数组。
            en.value = load_entry_value(et, &p.name, &file, &espan, d);
            en.ops = et.get("ops").map(|_| str_array(et, "ops", &p.name, &file, &espan, d));
            en.status = opt_str(et, "status", &p.name, &file, &espan, d)
                .unwrap_or_else(|| "experimental".to_string());
            if !rules::in_list(rules::ENTRY_STATUSES, &en.status) {
                shape_enum(d, &p.name, &file, &espan, "status", &en.status, rules::ENTRY_STATUSES);
            }
            e.entries.push(en);
        }
        p.exports.push(e);
    }

    // ---- 导出面分类不变量(§3.5; 单记录内可判的部分)----
    check_export_invariants(&p, d);

    // ---- [privileged] ----
    if let Some(priv_t) = opt_table(top, "privileged") {
        let level = opt_str(priv_t, "level", &p.name, &file, "privileged", d);
        if let Some(l) = &level {
            if !rules::in_list(rules::PRIV_LEVELS, l) {
                shape_enum(d, &p.name, &file, "privileged", "level", l, rules::PRIV_LEVELS);
            }
        }
        let mut mem = Vec::new();
        for (i, mt) in array_of_tables(priv_t, "memory").into_iter().enumerate() {
            let span = format!("privileged.memory[{i}]");
            let granularity = req_str(mt, "granularity", &p.name, &file, &span, d).unwrap_or_default();
            // schema: `ops` required(缺 ⇒ 形状错; 空表另由 BRV-PRIV-0002 处理)。
            if !mt.contains_key("ops") {
                d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    format!("{span}.ops"),
                    "缺少必填字段 `ops`",
                    "contract §7.1 / schema/plugin.schema.json",
                );
            }
            mem.push(rules::MemoryEntry {
                granularity,
                ops: str_array(mt, "ops", &p.name, &file, &span, d),
                regions: str_array(mt, "regions", &p.name, &file, &span, d),
            });
        }
        // `[[privileged.resources]]` 是**数组表**(contract §7.1 / schema):
        // 多条按字段累加; 是否有内容决定 P1 的下限。
        let mut resources = PrivilegedResources::default();
        for rt in array_of_tables(priv_t, "resources") {
            resources
                .irq
                .extend(int_array(rt, "irq", &p.name, &file, "privileged.resources", d));
            resources.dma_channels.extend(int_array(
                rt,
                "dma_channels",
                &p.name,
                &file,
                "privileged.resources",
                d,
            ));
            resources
                .pins
                .extend(int_array(rt, "pins", &p.name, &file, "privileged.resources", d));
            resources.device_names.extend(str_array(
                rt,
                "device_names",
                &p.name,
                &file,
                "privileged.resources",
                d,
            ));
        }
        let has_resources = !resources.is_empty();
        let input = rules::PrivInput {
            plugin_type: &p.plugin_type,
            subkind: p.subkind.as_deref(),
            target: &p.name,
            file: &file,
            level: level.as_deref(),
            memory: &mem,
            has_resources,
        };
        for diag in rules::check_privileged(&input) {
            d.push(diag);
        }
        p.privileged = Some(PrivilegedDecl {
            level,
            memory: mem,
            resources,
        });
    }

    // ---- [build] ----
    if let Some(bt) = opt_table(top, "build") {
        p.build = Some(BuildDecl {
            sources: str_array(bt, "sources", &p.name, &file, "build", d),
            includes: str_array(bt, "includes", &p.name, &file, "build", d),
        });
    }

    Some(p)
}

/// `subkind` 推导与冲突(§8.4 / `BRV-TAX-0015`; error ⇒ 退出码 1)。
fn validate_subkind(p: &mut Plugin, d: &mut Diags) {
    if p.plugin_type.is_empty() {
        return;
    }
    if p.plugin_type != "ability" {
        if p.subkind.is_some() {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &p.file,
                "plugin.subkind",
                format!("`subkind` 只对 `plugin_type = \"ability\"` 有意义, `{}` 不接受", p.plugin_type),
                "§8.1",
            );
        }
        return;
    }
    let derived = rules::derive_subkind(&p.name);
    match (derived, &p.subkind) {
        (Some(der), Some(explicit)) => {
            if der != explicit {
                d.coded(
                    "BRV-TAX-0015",
                    Severity::Error,
                    &p.name,
                    &p.file,
                    "plugin.subkind",
                    format!(
                        "subkind 推导与显式声明冲突: 由名字 namespace 推导 = `{der}`, 显式 = `{explicit}`"
                    ),
                    "去掉显式 `subkind`, 或改用与 namespace 相符的名字(§8.4)",
                );
            }
        }
        (Some(der), None) => {
            // 名字可推导: 补全模型里的 subkind(不写回文件)。
            p.subkind = Some(der.to_string());
        }
        (None, None) => {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &p.file,
                "plugin.subkind",
                format!(
                    "ability 插件 `{}` 无法由名字推导 `subkind`, 必须显式声明",
                    p.name
                ),
                format!("合法值: {}; 或改用可推导的名字(service/ sched/ framework/ io/ fs/)", rules::list_of(rules::SUBKINDS)),
            );
        }
        (None, Some(_)) => {}
    }
}

/// 导出面分类不变量(§3.5, `BRV-TAX-0016/0017/0018/0019`)。
///
/// 不变量 3 的"列表中每个被再导出单元的 `api_iface` 与自身相等"是**跨文件**判定,
/// 归 agent-B 的 `iface`/`check`(本函数只判"非空 + 元素形状")。
fn check_export_invariants(p: &Plugin, d: &mut Diags) {
    let views: Vec<rules::ExportView<'_>> = p
        .exports
        .iter()
        .map(|e| rules::ExportView {
            api_iface: &e.api_iface,
            form: &e.form,
            reexport_of: &e.reexport_of,
            symbols: &e.symbols,
        })
        .collect();
    for (i, code, msg) in rules::check_export_invariants(&p.api_type, &views) {
        d.coded(
            code,
            Severity::Error,
            &p.name,
            &p.file,
            format!("export[{i}]"),
            msg,
            "§3.5 导出面分类不变量(V-12)",
        );
    }
}

// ------------------------------------------------------------------ compat

fn load_compat(top: &toml::Table, p: &mut Plugin, d: &mut Diags) {
    let Some(c) = opt_table(top, "compat") else {
        return;
    };
    // schema/plugin.schema.json: `[compat]` 存在时 `core` required。
    if c.contains_key("core") {
        if let Some(raw) = opt_str(c, "core", &p.name, &p.file, "compat", d) {
            p.compat.core = parse_range_field(&raw, &p.name, &p.file, "compat.core", d);
            p.compat.core_raw = Some(raw);
        }
    } else {
        d.shape(
            "BRV-MF-0001",
            &p.name,
            &p.file,
            "compat.core",
            "缺少必填字段 `core`(`[compat]` 存在时必填)",
            "contract §7.1 / schema/plugin.schema.json",
        );
    }
    p.compat.api_rev = opt_int(c, "api_rev", &p.name, &p.file, "compat", d);
    p.compat.hash_scope = opt_str(c, "hash_scope", &p.name, &p.file, "compat", d);
    p.compat.truth = opt_str(c, "truth", &p.name, &p.file, "compat", d);
    p.compat.abi_id = opt_str(c, "abi_id", &p.name, &p.file, "compat", d);

    // v0.1 只允许 hash_scope/truth 都是 "decl"(BRV-IFACE-0011, §6.6 / V-11②)。
    let hs = p.compat.hash_scope.clone().unwrap_or_else(|| "decl".to_string());
    let tr = p.compat.truth.clone().unwrap_or_else(|| "decl".to_string());
    if hs != "decl" || tr != "decl" {
        d.coded(
            "BRV-IFACE-0011",
            Severity::Error,
            &p.name,
            &p.file,
            "compat",
            format!(
                "`truth`/`hash_scope` 必须成对且 v0.1 只允许 `decl`: hash_scope=`{hs}`, truth=`{tr}`"
            ),
            "§6.6/§10 V-11②: sym/header 是 v0.x; 不得静默迁移",
        );
    }

    for (i, t) in array_of_tables(c, "requires_iface").into_iter().enumerate() {
        let span = format!("compat.requires_iface[{i}]");
        let id = req_str(t, "id", &p.name, &p.file, &span, d).unwrap_or_default();
        if !id.is_empty() && !id.contains('#') {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &p.file,
                format!("{span}.id"),
                format!("接口单元 id `{id}` 形状不符: 应为 `<provider>#<unit>`"),
                "§6.1",
            );
        }
        let api_iface = req_str(t, "api_iface", &p.name, &p.file, &span, d).unwrap_or_default();
        if !api_iface.is_empty() {
            check_enum(d, &p.name, &p.file, &span, "api_iface", &api_iface, rules::API_TYPES);
        }
        // 必填: compat_gen 与 range 分开(§5.6)。
        let compat_gen = req_int(t, "compat_gen", &p.name, &p.file, &span, d)
            .map(|v| v.max(0) as u64)
            .unwrap_or(0);
        let range_raw = req_str(t, "range", &p.name, &p.file, &span, d).unwrap_or_default();
        let range = if range_raw.is_empty() {
            Range::any()
        } else {
            parse_range_field(&range_raw, &p.name, &p.file, format!("{span}.range").as_str(), d)
                .unwrap_or_else(Range::any)
        };
        let mode = opt_str(t, "mode", &p.name, &p.file, &span, d)
            .unwrap_or_else(|| "decl".to_string());
        if !rules::in_list(&["decl", "sym"], &mode) {
            shape_enum(d, &p.name, &p.file, &span, "mode", &mode, &["decl", "sym"]);
        }
        p.compat.requires_iface.push(RequiresIface {
            id,
            api_iface,
            compat_gen,
            range,
            range_raw,
            mode,
        });
    }

    // 分类相容(§3.5 消费方 2)在 v0.1 只报 info('v0.1 不扫描声明面接口依赖', V-10③)。
    for r in &p.compat.requires_iface {
        if p.api_type == "native" && r.api_iface == "runtime_adapter" {
            d.coded(
                "BRV-TAX-0016",
                Severity::Info,
                &p.name,
                &p.file,
                "compat.requires_iface",
                format!(
                    "`native` 插件引用了 `runtime_adapter` 分类的单元 `{}`(v0.1 不扫描接口依赖, 仅提示; v0.2 起为红)",
                    r.id
                ),
                "§3.5 消费方 2 / V-10③",
            );
        }
    }
}

// ------------------------------------------------------------------ product.toml

/// 加载 `root/product.toml`(可选)。返回 `(是否有文件, 模型)`。
pub fn load_product(root: &Path, d: &mut Diags) -> (bool, Option<Product>) {
    let abs = root.join("product.toml");
    if !abs.is_file() {
        return (false, None);
    }
    let file = "product.toml".to_string();
    let text = match fs::read_to_string(&abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("读不到 `{file}`: {e}"), "检查路径与权限");
            return (true, None);
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(format!("`{file}` 不是合法 TOML: {e}"), "BRV-D9 退出码 2");
            return (true, None);
        }
    };
    let Some(top) = value.as_table() else {
        d.env(format!("`{file}` 顶层必须是 TOML 表"), "contract §7.2");
        return (true, None);
    };
    let mut prod = Product {
        file: file.clone(),
        stage: "dev".to_string(),
        lint_frozen_deps: "inherit".to_string(),
        ..Default::default()
    };
    match req_int(top, "schema", &file, &file, "", d) {
        Some(1) => prod.schema = 1,
        Some(other) => d.shape(
            "BRV-MF-0001",
            &file,
            &file,
            "schema",
            format!("`schema` 只支持 1, 收到 {other}"),
            "contract §7.2",
        ),
        None => {}
    }

    let Some(product) = opt_table(top, "product") else {
        d.shape(
            "BRV-MF-0001",
            &file,
            &file,
            "product",
            "缺少必填表 `[product]`",
            "contract §7.2",
        );
        return (true, Some(prod));
    };
    prod.name = req_str(product, "name", &file, &file, "product", d).unwrap_or_default();
    // schema/product.schema.json: `version` 在 `[product]` 内**非** required; 缺省 0.0.0.0。
    if let Some(vraw) = opt_str(product, "version", &file, &file, "product", d) {
        if let Some(v) = parse_version_field(&vraw, &file, &file, "product.version", d) {
            prod.version = v;
        }
    }
    prod.app = req_str(product, "app", &file, &file, "product", d).unwrap_or_default();
    if !prod.app.is_empty() && !rules::valid_plugin_name(&prod.app) {
        d.shape(
            "BRV-MF-0001",
            &file,
            &file,
            "product.app",
            format!("`app` 名 `{}` 不符名字契约", prod.app),
            "§8.3",
        );
    }
    if let Some(core) = opt_str(product, "core", &file, &file, "product", d) {
        prod.core = parse_range_field(&core, &file, &file, "product.core", d);
    }
    prod.stage = opt_str(product, "stage", &file, &file, "product", d)
        .unwrap_or_else(|| "dev".to_string());
    if !rules::in_list(&["dev", "release"], &prod.stage) {
        shape_enum(d, &file, &file, "product", "stage", &prod.stage, &["dev", "release"]);
    }

    if let Some(sel) = opt_table(top, "select") {
        prod.select = str_array(sel, "plugins", &file, &file, "select", d);
    }
    if let Some(b) = opt_table(top, "budget") {
        prod.budget_ram_kib = opt_int(b, "ram_kib", &file, &file, "budget", d);
        prod.budget_stack_kib = opt_int(b, "stack_kib", &file, &file, "budget", d);
    }
    if let Some(l) = opt_table(top, "lint") {
        prod.lint_frozen_deps = opt_str(l, "frozen_deps", &file, &file, "lint", d)
            .unwrap_or_else(|| "inherit".to_string());
        if !rules::in_list(&["inherit", "allow", "deny"], &prod.lint_frozen_deps) {
            shape_enum(
                d,
                &file,
                &file,
                "lint",
                "frozen_deps",
                &prod.lint_frozen_deps,
                &["inherit", "allow", "deny"],
            );
        }
        if let Some(arr) = l.get("allow_edges").and_then(|v| v.as_array()) {
            for (i, item) in arr.iter().enumerate() {
                let pair = item.as_array();
                let ok = match pair {
                    Some(pair) if pair.len() == 2 => {
                        let a = pair[0].as_str();
                        let b = pair[1].as_str();
                        if let (Some(a), Some(b)) = (a, b) {
                            prod.allow_edges.push((a.to_string(), b.to_string()));
                            true
                        } else {
                            false
                        }
                    }
                    _ => false,
                };
                if !ok {
                    d.shape(
                        "BRV-MF-0001",
                        &file,
                        &file,
                        format!("lint.allow_edges[{i}]"),
                        "`allow_edges` 的每一项必须是 `[<consumer>, <provider>]` 两个字符串",
                        "§7.3 / §8.2",
                    );
                }
            }
        }
    }

    (true, Some(prod))
}

// ------------------------------------------------------------------ 树扫描

fn scan_plugin_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    let mut items: Vec<(String, PathBuf, bool)> = Vec::new();
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
        items.push((name, e.path(), is_dir));
    }
    items.sort_by(|a, b| a.0.cmp(&b.0));
    for (name, path, is_dir) in items {
        if is_dir {
            if name.starts_with('.') || rules::in_list(SKIP_DIRS, &name) {
                continue;
            }
            scan_plugin_files(&path, out);
        } else if name == "plugin.toml" {
            out.push(path);
        }
    }
}

/// 加载整棵插件树(裁定 R-1: 无 `product.toml` 时校验整棵树)。
pub fn load_tree(root: &Path) -> TreeLoad {
    let mut load = TreeLoad::default();
    let mut files = Vec::new();
    scan_plugin_files(root, &mut files);
    files.sort();
    for f in &files {
        if let Some(p) = load_plugin_file(root, f, &mut load.diags) {
            load.plugins.push(p);
        }
    }
    // 名字全局唯一(BRV-D4 唯一真值 / §8.3): 重名 ⇒ 形状错。
    let mut seen: BTreeMap<String, String> = BTreeMap::new();
    let mut dups: Vec<(String, String, String)> = Vec::new();
    for p in &load.plugins {
        if p.name.is_empty() {
            continue;
        }
        if let Some(first) = seen.get(&p.name) {
            dups.push((p.name.clone(), first.clone(), p.file.clone()));
        } else {
            seen.insert(p.name.clone(), p.file.clone());
        }
    }
    for (name, first, second) in dups {
        load.diags.shape(
            "BRV-MF-0001",
            &name,
            &second,
            "plugin.name",
            format!("插件名 `{name}` 在树内重复: {first} 与 {second}"),
            "§8.3: 插件名全局唯一",
        );
    }

    let (has_product, product) = load_product(root, &mut load.diags);
    load.has_product = has_product;
    load.product = product;
    load.plugins.sort_by(|a, b| a.name.cmp(&b.name));
    load
}

// ------------------------------------------------------------------ 快照 / lock

/// 接口面快照的一个条目(读取用; 与 model 的 [`ExportEntry`] 同形)。
#[derive(Clone, Debug, Default)]
pub struct SnapshotEntry {
    pub kind: String,
    pub name: String,
    pub sig: Option<String>,
    pub layout: Option<String>,
    pub value: Option<String>,
    pub ops: Option<Vec<String>>,
    pub status: String,
}

/// `api/iface/<provider>/<unit>.toml` 的一份快照(**只读**)。
#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    /// 相对 `root`。
    pub file: String,
    /// `<provider>#<unit>`。
    pub id: String,
    pub provider: String,
    pub unit: String,
    pub api_iface: Option<String>,
    pub form: Option<String>,
    pub version: Option<Version>,
    pub compat_gen: Option<u64>,
    pub freeze_state: Option<String>,
    pub status: Option<String>,
    pub hash: Option<String>,
    pub hash_scope: Option<String>,
    pub truth: Option<String>,
    pub not_abi: bool,
    /// `strict_params`(IFACE-IR 规则 10: 必须存进快照, 否则跨环境 hash 抖动)。
    pub strict_params: Option<bool>,
    /// 解冻窗口的基线(裁定 R-7): `unfreeze` 时写入, `refreeze` 与它比。
    pub baseline_hash: Option<String>,
    pub baseline_entries: Vec<SnapshotEntry>,
    pub entries: Vec<SnapshotEntry>,
}

/// 读取 `api/iface/**` 的全部快照(读不到 ⇒ 空表 = "未发布")。
pub fn load_snapshots(root: &Path, d: &mut Diags) -> Vec<Snapshot> {
    let base = root.join("api").join("iface");
    if !base.is_dir() {
        return Vec::new();
    }
    let mut files = Vec::new();
    collect_files(&base, &mut files);
    files.sort();
    let mut out = Vec::new();
    for f in files {
        if f.extension().and_then(|e| e.to_str()) != Some("toml") {
            continue;
        }
        let file = rel_path(root, &f);
        let text = match fs::read_to_string(&f) {
            Ok(t) => t,
            Err(e) => {
                d.env(format!("读不到快照 `{file}`: {e}"), "api/iface/** 只读");
                continue;
            }
        };
        let value: toml::Value = match text.parse() {
            Ok(v) => v,
            Err(e) => {
                d.env(format!("快照 `{file}` 不是合法 TOML: {e}"), "agent-B 写入; 本刀只读");
                continue;
            }
        };
        let Some(top) = value.as_table() else { continue };
        out.push(snapshot_from_table(&file, &base, top));
    }
    out
}

fn collect_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    for e in entries.flatten() {
        let path = e.path();
        let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
        if is_dir {
            collect_files(&path, out);
        } else {
            out.push(path);
        }
    }
}

fn snapshot_from_table(file: &str, _base: &Path, top: &toml::Table) -> Snapshot {
    let unit = std::path::Path::new(file)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    // provider = api/iface 与文件名之间的路径段
    let provider = {
        let p = std::path::Path::new(file);
        let mut comps: Vec<String> = p
            .components()
            .map(|c| c.as_os_str().to_string_lossy().to_string())
            .collect();
        // 去掉 "api" "iface" 与文件名
        if comps.len() >= 3 {
            comps.drain(0..2);
            comps.pop();
            comps.join("/")
        } else {
            String::new()
        }
    };

    // 字段可能平铺, 也可能在 `[unit_meta]` 子表里(B 的形状待定 ⇒ 两种都读)。
    let meta = opt_table(top, "unit_meta");
    let get_str = |key: &str| -> Option<String> {
        top.get(key)
            .and_then(|v| v.as_str())
            .map(str::to_string)
            .or_else(|| meta.and_then(|m| m.get(key)).and_then(|v| v.as_str()).map(str::to_string))
    };
    let get_int = |key: &str| -> Option<i64> {
        top.get(key)
            .and_then(|v| v.as_integer())
            .or_else(|| meta.and_then(|m| m.get(key)).and_then(|v| v.as_integer()))
    };

    let version = get_str("version").and_then(|s| version::parse_version(&s).ok());
    let entries_src = if top.contains_key("entry") {
        array_of_tables(top, "entry")
    } else if top.contains_key("entries") {
        array_of_tables(top, "entries")
    } else if let Some(e) = opt_table(top, "export") {
        array_of_tables(e, "entries")
    } else {
        Vec::new()
    };
    let entries = entries_src
        .into_iter()
        .map(snapshot_entry_from_table)
        .collect();
    let baseline_entries = array_of_tables(top, "baseline_entry")
        .into_iter()
        .map(snapshot_entry_from_table)
        .collect();
    let strict_params = top
        .get("strict_params")
        .and_then(|v| v.as_bool())
        .or_else(|| meta.and_then(|m| m.get("strict_params")).and_then(|v| v.as_bool()));

    Snapshot {
        file: file.to_string(),
        id: if provider.is_empty() {
            unit.clone()
        } else {
            format!("{provider}#{unit}")
        },
        provider,
        unit,
        api_iface: get_str("api_iface"),
        form: get_str("form"),
        version,
        compat_gen: get_int("compat_gen").map(|v| v.max(0) as u64),
        freeze_state: get_str("freeze_state"),
        status: get_str("status"),
        hash: get_str("hash"),
        hash_scope: get_str("hash_scope"),
        truth: get_str("truth"),
        not_abi: top.contains_key("NOT_ABI")
            || meta.map(|m| m.contains_key("NOT_ABI")).unwrap_or(false),
        strict_params,
        baseline_hash: get_str("baseline_hash"),
        baseline_entries,
        entries,
    }
}

fn snapshot_entry_from_table(t: &toml::Table) -> SnapshotEntry {
    SnapshotEntry {
        kind: t.get("kind").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        name: t.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        sig: t.get("sig").and_then(|v| v.as_str()).map(str::to_string),
        layout: t.get("layout").and_then(|v| v.as_str()).map(str::to_string),
        value: t.get("value").and_then(|v| v.as_str()).map(str::to_string),
        ops: t
            .get("ops")
            .and_then(|v| v.as_array())
            .map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect()),
        status: t
            .get("status")
            .and_then(|v| v.as_str())
            .unwrap_or("experimental")
            .to_string(),
    }
}

impl Snapshot {
    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let entries: Vec<_> = self
            .entries
            .iter()
            .map(|e| {
                json!({
                    "kind": e.kind, "name": e.name, "sig": e.sig, "layout": e.layout,
                    "value": e.value, "ops": e.ops, "status": e.status,
                })
            })
            .collect();
        json!({
            "file": self.file,
            "id": self.id,
            "provider": self.provider,
            "unit": self.unit,
            "api_iface": self.api_iface,
            "form": self.form,
            "version": self.version.map(|v| v.to_string()),
            "compat_gen": self.compat_gen,
            "freeze_state": self.freeze_state,
            "status": self.status,
            "hash": self.hash,
            "hash_scope": self.hash_scope,
            "truth": self.truth,
            "not_abi": self.not_abi,
            "strict_params": self.strict_params,
            "baseline_hash": self.baseline_hash,
            "entries": entries,
        })
    }
}

/// `brickie.lock` 的一条插件记录(`schema/lock.schema.json` 的 `lock.plugins[]`)。
#[derive(Clone, Debug, Default)]
pub struct LockPlugin {
    pub name: String,
    pub version: String,
    pub compat_gen: u64,
}

/// `brickie.lock` 的一条接口单元记录(`schema/lock.schema.json` 的 `lock.units[]`)。
#[derive(Clone, Debug, Default)]
pub struct LockUnit {
    pub id: String,
    pub provider: String,
    pub unit: String,
    pub api_iface: String,
    pub version: String,
    pub compat_gen: u64,
    pub hash: String,
    pub hash_scope: String,
    pub truth: String,
}

/// `[lock.product]` —— 本实现的**扩展字段**。
///
/// 权威 schema(`schema/lock.schema.json`)在顶层与 `lock` 上都声明
/// `additionalProperties: true`, 故这是合法扩展(便于 core 记录产品头);
/// 若将来 schema 收紧, 删掉即可。
#[derive(Clone, Debug, Default)]
pub struct LockProduct {
    pub name: String,
    pub version: String,
    pub stage: String,
}

/// `brickie.lock` 的规范化模型(**只读**; agent-B 负责写)。
#[derive(Clone, Debug)]
pub struct Lock {
    pub schema: i64,
    pub profile: String,
    pub product: Option<LockProduct>,
    pub plugins: Vec<LockPlugin>,
    pub units: Vec<LockUnit>,
}

impl Default for Lock {
    fn default() -> Self {
        Lock {
            schema: 1,
            profile: "dev".to_string(),
            product: None,
            plugins: Vec::new(),
            units: Vec::new(),
        }
    }
}

/// 读取 `brickie.lock`(不存在 ⇒ `None` = "未发布"/"还没求解过")。
///
/// 形状 = `schema/lock.schema.json`(v0.1 弱校验: 顶层无 required)。
pub fn load_lock(root: &Path, d: &mut Diags) -> Option<Lock> {
    let abs = root.join("brickie.lock");
    if !abs.is_file() {
        return None;
    }
    let text = match fs::read_to_string(&abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("读不到 `brickie.lock`: {e}"), "锁文件只读");
            return None;
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(format!("`brickie.lock` 不是合法 TOML: {e}"), "agent-B 写入; 本刀只读");
            return None;
        }
    };
    let Some(top) = value.as_table() else { return None };
    let mut lock = Lock {
        schema: top.get("schema").and_then(|v| v.as_integer()).unwrap_or(1),
        ..Default::default()
    };
    let Some(lk) = opt_table(top, "lock") else {
        // schema 的 `lock` 不是 required(v0.1 弱校验): 有 schema 无 lock 也算存在。
        return Some(lock);
    };
    lock.profile = lk
        .get("profile")
        .and_then(|v| v.as_str())
        .unwrap_or("dev")
        .to_string();
    if let Some(p) = opt_table(lk, "product") {
        lock.product = Some(LockProduct {
            name: p.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            version: p.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            stage: p.get("stage").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        });
    }
    for t in array_of_tables(lk, "plugins") {
        lock.plugins.push(LockPlugin {
            name: t.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            version: t.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            compat_gen: t.get("compat_gen").and_then(|v| v.as_integer()).unwrap_or(0).max(0) as u64,
        });
    }
    for t in array_of_tables(lk, "units") {
        lock.units.push(LockUnit {
            id: t.get("id").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            provider: t.get("provider").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            unit: t.get("unit").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            api_iface: t.get("api_iface").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            version: t.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            compat_gen: t.get("compat_gen").and_then(|v| v.as_integer()).unwrap_or(0).max(0) as u64,
            hash: t.get("hash").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            hash_scope: t.get("hash_scope").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            truth: t.get("truth").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        });
    }
    Some(lock)
}

// ------------------------------------------------------------------ 反向依赖索引

/// `build/index/dependents.json` 的一条反向边。
#[derive(Clone, Debug, Default)]
pub struct DependentEdge {
    pub from: String,
    pub kind: String,
}

/// 反向依赖索引(权威 schema 由 `schema/index.schema.json` 给出)。
#[derive(Clone, Debug, Default)]
pub struct DependentsIndex {
    pub schema: i64,
    /// 被依赖插件 → 依赖它的边(键已排序)。
    pub dependents: BTreeMap<String, Vec<DependentEdge>>,
}

impl DependentsIndex {
    /// 从插件树全量重建(§6.3: `brickie dep index` 的语义)。
    ///
    /// 只按**已声明的** `[[dep]]` 建边; 运行期注册表取用不产生边(§3.1 / BRV-Q13)。
    pub fn build(tree: &TreeLoad) -> Self {
        let mut idx = DependentsIndex {
            schema: 1,
            dependents: BTreeMap::new(),
        };
        for p in &tree.plugins {
            for d in &p.deps {
                idx.dependents
                    .entry(d.name.clone())
                    .or_default()
                    .push(DependentEdge {
                        from: p.name.clone(),
                        kind: d.kind.clone(),
                    });
            }
        }
        for edges in idx.dependents.values_mut() {
            edges.sort_by(|a, b| (a.from.as_str(), a.kind.as_str()).cmp(&(b.from.as_str(), b.kind.as_str())));
        }
        idx
    }
}

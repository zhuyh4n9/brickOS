//! L1 闭包求解(§7.6)+ `dep` 族命令。
//!
//! `solve` 是**纯函数**(除入参 `TreeLoad` 外无 IO): 产品选择集 → init/runtime/type
//! 三类边闭包 → `compat_gen` 精确匹配 → `range` 交集(单版本)→ 拓扑 + init 环检测
//! → 相位单调 → 分类学禁则 → 预算合计 → profile 判定(§7.5)。
//!
//! 命令面:
//! - `closure`   —— 闭包 + 拓扑序 + 预算合计(contract §5.1 的 `data`);
//! - `dep-tree`  —— 依赖树(`{roots, nodes, kind}`);
//! - `dep-graph` —— `dot` / `mermaid` / `json`(`json` 时 `data.text` 是规范化 JSON 串);
//! - `dep-why`   —— 最短依赖路径(找不到 ⇒ `data.found = false`, 仍 exit 0);
//! - `dep-index` —— `build/index/dependents.json`(确定性序列化)。
//!
//! 诊断码: `BRV-DEP-0009/0010/0011` / `BRV-VER-0001/0002/0004` /
//! `BRV-MF-0001`(缺口代用: 环 / 缺依赖 / 预算超限 / 依赖方向禁则 —— 裁定 R-9;
//! 消息里必须点名**冲突双方 / 差值 / 完整环路径**)。
//!
//! ## 实现期裁定(已登记 rust/README.md §10)
//! - **S-1**: `requires_iface` 的版本校验只在 `check` 里做(`scan_iface = true`);
//!   `closure` **完全不看**该字段 ⇒ `dep closure --json` 在加/去 `requires_iface`
//!   时逐字节相同(§10 V-10②)。这与 V-7 的"`compat_gen` 冲突可构造"不矛盾:
//!   冲突在 `check --deps` 里报。
//! - **S-2**: 结构依赖 `[[dep]].range` 与**提供方插件的四段版本**的
//!   `MAJOR.MINOR.REVISE` 比对(结构依赖不钉代, 依 F3)⇒ 越界 `BRV-VER-0002`。
//! - **S-3**: `range` 约束的**交集为空** ⇒ `BRV-DEP-0011`(单版本政策下"同插件
//!   多版本共存"在 v0.1 唯一可构造的形式: 两个依赖方要求互斥的区间)。
//! - **S-4**: `type` 边只把提供方拉进闭包, **不**沿其 `init`/`runtime` 边继续扩张
//!   (§7.1 "闭包参与 ✅(但不拉入运行期依赖)")。

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::PathBuf;

use serde_json::{json, Value};

use crate::diag::{Diag, Diags, Severity};
use crate::emit;
use crate::model::{self, Plugin, TreeLoad};
use crate::proto::{Request, Response};
use crate::rules;
use crate::version::{self, Range};

/// 域标签(contract §5.1 的 `scopes`)。
pub const SCOPE_DEPS: &str = "deps";
pub const SCOPE_IFACE: &str = "iface";
pub const SCOPE_TAX: &str = "tax";
pub const SCOPE_PRIV: &str = "priv";

// ------------------------------------------------------------------ 结构

/// 已声明的一条结构依赖边(消费者 → 提供者)。
#[derive(Clone, Debug, Default)]
pub struct Edge {
    pub from: String,
    pub to: String,
    /// `init` | `runtime` | `type`。
    pub kind: String,
    /// 规范化 `range`(展开后)。
    pub range: String,
    /// `[[dep]].phase` 断言(**仅 init 边**; §7.2 R2)。
    pub phase: Option<String>,
    pub symbol: Option<String>,
    /// `[[dep]]` 在该插件声明里的下标(诊断 span 用)。
    pub index: usize,
}

/// 一个环(结构化: `path` = 节点闭合路径, `edges` = 逐条边)。
#[derive(Clone, Debug, Default)]
pub struct Cycle {
    pub path: Vec<String>,
    pub edges: Vec<Edge>,
}

/// 求解得到的闭包。
#[derive(Clone, Debug, Default)]
pub struct Closure {
    /// 闭包内的插件名(字典序)。
    pub selected: BTreeSet<String>,
    /// 选择集(产品 manifest 的根; 无 product ⇒ 全树)。
    pub seeds: Vec<String>,
    pub init_edges: Vec<Edge>,
    pub runtime_edges: Vec<Edge>,
    pub type_edges: Vec<Edge>,
    /// init 边的拓扑序(有环时: 无环部分在前, 其余按字典序补尾)。
    pub topo_order: Vec<String>,
    /// init 环(结构化; V-3)。
    pub cycles: Vec<Cycle>,
    pub totals_ram: i64,
    pub totals_stack: i64,
    pub capacity_ram: i64,
    pub capacity_stack: i64,
    /// 树里是否存在 `plugin_type = "platform"`(容量是否有意义)。
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

/// `solve` 的结果: 闭包 + **带域标签**的诊断。
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

/// profile 相关的严重度(`BRV-VER-0004`: dev ⇒ info / release ⇒ error; §7.5)。
fn sev_for_profile(profile: &str) -> Severity {
    if profile == "release" {
        Severity::Error
    } else {
        Severity::Info
    }
}

// ------------------------------------------------------------------ 求解

const REACH_TYPE_ONLY: u8 = 1;
const REACH_FULL: u8 = 2;

/// 纯函数闭包求解(§7.6)。
pub fn solve(tree: &TreeLoad, profile: &str, scan_iface: bool) -> SolveResult {
    let by_name: BTreeMap<&str, &Plugin> =
        tree.plugins.iter().map(|p| (p.name.as_str(), p)).collect();
    let mut diags: Vec<(&'static str, Diag)> = Vec::new();
    let mut closure = Closure::default();

    // ---- 1. 产品选择集(裁定 R-1: 无 product ⇒ 整棵树) ----
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
            // product 存在但选择集为空 ⇒ 退回"整棵树"(R-1 的精神: 有声明就该被校验)
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
                    format!("选择集里的插件 `{s}` 不在插件树里(缺 `{s}/plugin.toml`)"),
                    "缺口代用(R-9): 选择/依赖引用完整性无专属码; 补上插件或从选择集删除",
                ),
            ));
        }
    }

    // ---- 2. 三类边闭包(type 边不拉入运行期依赖, 见 S-4) ----
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

    // ---- 3. 边集合 + 缺依赖 ----
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
                                "`{}` 依赖 `{}`({})但插件树里没有它(缺 `{}/plugin.toml`)",
                                p.name, d.name, d.kind, d.name
                            ),
                            "§7.6: 缺失报'依赖名 + 哪条边引入'; 缺口代用(R-9)",
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

    // ---- 4. `range` 越界(BRV-VER-0002)+ 单版本交集(BRV-DEP-0011) ----
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
                        "同代内 range 越界: `{}` 要求 `{}` 满足 `{}`, 但提供方当前版本是 {}",
                        e.from, e.to, e.range, prov.version
                    ),
                    "§7.4: range 只比较 MAJOR.MINOR.REVISE(缺段右补 0); 修 range 或推进提供方",
                ),
            ));
        }
        let slot = constraints
            .entry(e.to.clone())
            .or_insert_with(|| (Range::any(), Vec::new()));
        slot.0 = slot.0.intersect(&r);
        slot.1.push(e.from.clone());
    }

    // ---- 5. `requires_iface` 的版本校验(仅 check; 见 S-1) ----
    if scan_iface {
        for name in &closure.selected {
            let p = by_name[name.as_str()];
            for (ri, r) in p.compat.requires_iface.iter().enumerate() {
                let span = format!("compat.requires_iface[{ri}]");
                let Some((prov_name, unit)) = r.id.split_once('#') else {
                    continue; // 形状错已在加载期报
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
                                "`requires_iface` 指向的提供方 `{prov_name}` 不在插件树里(单元 `{}`)",
                                r.id
                            ),
                            "缺口代用(R-9): 接口引用完整性无专属码",
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
                            format!("`requires_iface` 指向的接口单元 `{}` 不存在", r.id),
                            "缺口代用(R-9): 接口引用完整性无专属码",
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
                                "compat_gen 不匹配: `{}` 钉 `{}` 的 compat_gen={}, 而提供方当前在 {}",
                                p.name, r.id, r.compat_gen, prov_cg
                            ),
                            "§5.5/§7.4: compat_gen 精确匹配, 跨代比较无意义",
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
                                "同代内 range 越界: `{}` 要求 `{}` 满足 `{}`, 但提供方版本是 {}",
                                p.name, r.id, r.range_raw, prov_ver
                            ),
                            "§7.4: range 只比较 MAJOR.MINOR.REVISE",
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
                                "接口消费 `{}`: 单元 `{}` 尚未冻结(compat_gen = 0), 未受保护(profile={profile})",
                                p.name, r.id
                            ),
                        )
                        .code("BRV-VER-0004")
                        .target(&p.name)
                        .file(&p.file)
                        .span(span.clone())
                        .hint("§7.5: dev 允许(info)/ release 禁止(error)"),
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
                        "单版本政策下区间交集为空: 插件 `{prov}` 被 {} 以互斥区间要求(`{}`)",
                        consumers.join(", "),
                        r.canonical()
                    ),
                    "§7.4: 同一插件名在闭包内只允许一个版本 ⇒ BRV-DEP-0011",
                ),
            ));
        }
    }

    // ---- 6. skin 再导出边的 profile 门禁(V-14; 不看 requires_iface) ----
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
                                "皮肤 `{}`(export[{ei}])再导出的单元 `{r}` 尚未冻结(compat_gen = 0)(profile={profile})",
                                p.name
                            ),
                        )
                        .code("BRV-VER-0004")
                        .target(&p.name)
                        .file(&p.file)
                        .span(format!("export[{ei}].reexport_of"))
                        .hint("§7.5: 接口消费在 release 下不允许未冻结接口"),
                    ));
                }
            }
        }
    }

    // ---- 7. 相位单调(R1/R2; 仅已声明的 init 边) ----
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
                            "相位单调违例(R1): `{}` 在 `{}` 完成(rank {}), 晚于依赖方 `{}` 的 `{}`(rank {}), 但 `{}` 依赖 `{}`",
                            b.name, b.phase, rb, a.name, a.phase, ra, a.name, b.name
                        ),
                        "§7.2: rank(complete(B)) ≤ rank(complete(A)); 只覆盖已声明的 init 边",
                    ),
                ));
            }
        }
        if let Some(assert) = &e.phase {
            let rank_assert = rules::phase_rank(assert);
            if let (Some(rank_assert), Some(rb)) = (rank_assert, rb) {
                // §8.1 口径: "提供方自述晚于依赖方断言 ⇒ 红"(见 README §10 S-6)。
                if rb > rank_assert {
                    diags.push((
                        SCOPE_DEPS,
                        err(
                            "BRV-DEP-0010",
                            &a.name,
                            &a.file,
                            format!("dep[{}].phase", e.index),
                            format!(
                                "`[[dep]].phase` 断言与提供方自述冲突: `{}` 断言 `{}` 在 `{assert}` 完成, 但提供方自述 `{}`",
                                a.name, b.name, b.phase
                            ),
                            "§7.2 R2 / §8.1: rank(complete(B)) ≤ rank(assertion)",
                        ),
                    ));
                }
            }
        }
    }

    // ---- 8. 分类学禁则(依赖方向; 裁定 R-9 ⇒ BRV-MF-0001) ----
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
            // §3.5 消费方 3: `form = skin` 的再导出边**豁免 api_type 禁则**(方向表仍适用)。
            let prov_is_tp = b.api_type == "third_party";
            let whitelisted = allow.iter().any(|(c, p)| c == &a.name && p == &b.name);
            if !whitelisted && !rules::dep_direction_ok(&a.plugin_type, &b.plugin_type, prov_is_tp) {
                Some(format!(
                    "依赖方向禁则(§7.3): `plugin_type = {}` 的 `{}` 不得依赖 `plugin_type = {}` 的 `{}`",
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
                    format!("消费者 `{}` → 提供者 `{}`: {msg}", a.name, b.name),
                    "缺口代用(R-9): 依赖方向禁则无专属码; allow_edges 只豁免方向表",
                ),
            ));
        }
    }

    // ---- 9. 预算合计(R-6: platform 的 [[res]] 是**容量**) ----
    // 见 README §10 S-14: `plugin_type = platform` 的 `[[res]]` 作容量, **不**计入
    // 消费者侧的 Σ —— 否则"platform 自身耗用"会让物理上限恒红(§8.1 注①失去意义)。
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
                        "Σ ram_kib = {used_ram} 超出 platform 容量 {cap_ram}, 差值 {} KiB(物理上限, 无豁免)",
                        used_ram - cap_ram
                    ),
                    "§8.1: Σ [[res]] ≤ platform 提供量; 缺口代用(R-9)",
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
                        "Σ stack_kib = {used_stack} 超出 platform 容量 {cap_stack}, 差值 {} KiB",
                        used_stack - cap_stack
                    ),
                    "§8.1: Σ [[res]] ≤ platform 提供量; 缺口代用(R-9)",
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
                            "Σ ram_kib = {used_ram} 超出产品预算 {b}, 差值 {} KiB",
                            used_ram - b
                        ),
                        "§8.1: Σ [[res]] ≤ [budget]; 可评审地放宽 [budget]; 缺口代用(R-9)",
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
                            "Σ stack_kib = {used_stack} 超出产品预算 {b}, 差值 {} KiB",
                            used_stack - b
                        ),
                        "§8.1: Σ [[res]] ≤ [budget]; 缺口代用(R-9)",
                    ),
                ));
            }
        }
    }

    // ---- 10. 环检测(init ⇒ 硬错误 + 完整路径; runtime/type ⇒ 只报 info) ----
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
                format!("init 依赖成环: {}", cyc.path.join(" → ")),
                "§7.1: init 环禁止(拓扑硬错误); 缺口代用(R-9), V-3 要求完整路径",
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
                        "`{kind}` 依赖成环(允许, 仅登记): {}",
                        cyc.path.join(" → ")
                    ),
                    "§7.1: runtime/type 环只报 info",
                ),
            ));
        }
    }

    SolveResult { closure, diags }
}

// ------------------------------------------------------------------ 环 / 拓扑

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

/// 找全部初等环(以"回溯栈上的节点"为准; 去重按旋转后的最小字典序)。
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

/// init 边的**初始化**拓扑序: 提供方在前、消费者在后(Kahn; 就绪集按字典序 ⇒ 确定性)。
///
/// 边的方向是 `消费者 → 提供方`(A 依赖 B), 而"初始化顺序"要求 B 先于 A ⇒
/// Kahn 用**反向**邻接表。有环时剩余节点按字典序补尾。
pub fn topo_order(nodes: &BTreeSet<String>, edges: &[Edge]) -> Vec<String> {
    // 去重(同一对 (from,to) 只算一条边)。
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
        // 初始化顺序的边: 提供方(`to`)→ 消费者(`from`)。
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

// ------------------------------------------------------------------ JSON 视图

fn edges_json(edges: &[Edge]) -> Value {
    Value::Array(
        edges
            .iter()
            .map(|e| json!({"from": e.from, "to": e.to}))
            .collect(),
    )
}

/// `closure` 命令的 `data`(contract §5.1)。
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

/// 取 `args.profile`(缺省 = product.stage / dev)。
pub fn profile_of(req: &Request, tree: &TreeLoad) -> String {
    req.arg_str("profile")
        .filter(|s| !s.is_empty())
        .or_else(|| tree.product.as_ref().map(|p| p.stage.clone()))
        .unwrap_or_else(|| "dev".to_string())
}

// ------------------------------------------------------------------ 命令入口

/// `closure` / `dep-tree` / `dep-graph` / `dep-why` / `dep-index` 的统一入口。
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
                format!("solver 不认识命令 `{other}`"),
                "见 contract §5.1 的命令表",
            );
            Response::ok(d, json!({}))
        }
    }
}

fn cmd_closure(req: &Request, tree: &TreeLoad) -> Response {
    let profile = profile_of(req, tree);
    // S-1: `closure` **不**扫描 `requires_iface` ⇒ V-10② 的逐字节可复现成立。
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
            format!("`--kind {kind}` 越界"),
            "合法值: init | runtime | type | all(§7.7)",
        );
        return Response::ok(d, json!({}));
    }
    let only = req.arg_str("plugin").filter(|s| !s.is_empty());
    let selected = &sol.closure.selected;

    let mut nodes = serde_json::Map::new();
    for name in selected {
        let p = tree.by_name(name).expect("闭包内插件必在树里");
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

/// 按 `kind` 取边, 稳定排序 + 去重。
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
            format!("`--format {format}` 越界"),
            "合法值: dot | mermaid | json(§7.7)",
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
            .expect("JSON 必可序列化"),
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

    // BFS(消费者 → 提供者方向, 最短路径)。
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
        d.usage("`dep why` 需要 `from` 与 `to`", "contract §5.1: {from,to}");
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

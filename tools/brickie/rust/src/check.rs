//! 组合期校验 `check`(§7.7 / §7.5; V-3/V-8/V-12/V-14/V-19)。
//!
//! `check` 是**跨记录 / 跨文件**的校验收口: 单记录内的形状与不变量已由
//! [`crate::model::load_tree`] 在加载期报出(见 rust/README.md §5.3 的清单),
//! 本模块**不重复报**那些码。
//!
//! 域(`scopes`)与覆盖:
//! - `deps`  —— 闭包 / init 环(完整路径)/ `compat_gen` 精确 + `range` 交集 /
//!              单版本政策 / 相位单调(R1/R2)/ profile 门禁(`BRV-VER-0004`);
//! - `iface` —— 导出分类不变量 3 的**跨文件**部分 / 单元级与符号族碰撞 /
//!              `requires_iface` 只报 info(V-10③)/ release 下 `unfreezing`
//!              (`BRV-IFACE-0009`);
//! - `tax`   —— 依赖方向禁则(复用 solver)/ `sched_class` 组合合法性;
//! - `priv`  —— 预算合计(复用 solver)/ IRQ·DMA·引脚·设备名**独占冲突**(报双方)。
//!
//! `data.summary` 形状(contract §5.1):
//! `{errors, warnings, infos, profile, closure:{plugins, ram_kib, stack_kib}}`。

use serde_json::{json, Value};

use crate::diag::{Diag, Severity};
use crate::model::{self, Plugin, TreeLoad};
use crate::proto::{Request, Response};
use crate::rules;
use crate::solver::{self, SCOPE_DEPS, SCOPE_IFACE, SCOPE_PRIV, SCOPE_TAX};

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

/// `sched_kind`(裁定 R-5: 显式字段优先, 缺省按调度器插件名推断)。
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

/// `check` 的入口。
pub fn run(req: &Request) -> Response {
    let root = std::path::PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    // V-16: 解冻窗口也可能只记在快照里(`unfreeze` 不写 `plugin.toml`)⇒ 两处都看。
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
            format!("`--profile {profile}` 越界"),
            "合法值: dev | release(§7.5)",
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
        ];
    }
    for s in &scopes {
        if !rules::in_list(&[SCOPE_DEPS, SCOPE_IFACE, SCOPE_TAX, SCOPE_PRIV], s) {
            let mut d = base_diags.clone();
            d.usage(
                format!("未知校验域 `{s}`"),
                "合法域: deps | iface | tax | priv(§7.7)",
            );
            return Response::ok(d, json!({}));
        }
    }

    let sol = solver::solve(&tree, &profile, true);
    let closure = &sol.closure;

    // ---- 域内诊断(带域标签) ----
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

    // `plugin?` 过滤(按诊断的 target; 语义见 README §10 S-7)
    let only = req.arg_str("plugin").filter(|s| !s.is_empty());

    let mut summary_diags = base_diags.clone();
    let mut scope_results = serde_json::Map::new();
    for scope in [SCOPE_DEPS, SCOPE_IFACE, SCOPE_TAX, SCOPE_PRIV] {
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

// ------------------------------------------------------------------ deps 域: 声明 ↔ 使用

/// 源码 `#include` 了别家插件的头, 但 `plugin.toml` 没声明依赖(直接或传递)。
///
/// 与 `build` 用**同一份扫描**(`model::cross_plugin_includes`)—— 两处口径不能分叉。
/// 为什么单靠 `-I` 收窄不够: 目标工具链的 glibc 头会兜住 `unistd.h` 这类常见名,
/// 收窄后不是"找不到头"而是静默改用 glibc 声明(ADR-0018)。
fn deps_usage_checks(
    root: &std::path::Path,
    tree: &TreeLoad,
    closure: &solver::Closure,
) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    for u in model::cross_plugin_includes(root, tree, &closure.selected) {
        // A-2(设计 1-01 §7.3 / D18; ADR-0020): **APP 不许直连 core 头**。core 是内核面,
        // 其它插件本来就用它并直接 include `<br/core/...>`, 所以只有消费者是 app 才判违规。
        if u.provider == "core" {
            let is_app = tree
                .plugins
                .iter()
                .any(|p| p.name == u.consumer && p.plugin_type == "app");
            if !is_app {
                continue;
            }
            out.push((
                SCOPE_DEPS,
                err(
                    "BRV-MF-0001",
                    &u.consumer,
                    &u.file,
                    "include",
                    format!(
                        "`{}`(APP)直接 include 了 core 头 `{}`: APP 不许依赖 iface 层以下的接口",
                        u.consumer, u.header
                    ),
                    "ADR-0020 / A-2: 经 Interface 皮肤取用(如 `<iface/min/min.h>` 转出 br_log_*/br_clock_*), 或为该 core 面在皮肤里追加条目",
                ),
            ));
            continue;
        }
        if model::dep_closure(tree, &u.consumer).contains(&u.provider) {
            continue;
        }
        out.push((
            SCOPE_DEPS,
            err(
                "BRV-MF-0001",
                &u.consumer,
                &u.file,
                "include",
                format!(
                    "`{}` include 了 `{}` 的头 `{}`, 但 `plugin.toml` 没有声明依赖(直接或传递)",
                    u.consumer, u.provider, u.header
                ),
                "ADR-0018: 编译期包含面 = 声明依赖闭包; 补 `[[dep]]` 或删掉这个 include(缺口代用 R-9: 无专属码)",
            ),
        ));
    }
    out
}

// ------------------------------------------------------------------ iface 域

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

    // ---- ① 导出分类不变量 3 的**跨文件**部分(§3.5 / V-12) ----
    for p in &selected {
        for (ei, exp) in p.exports.iter().enumerate() {
            if exp.form != "skin" {
                continue;
            }
            for r in &exp.reexport_of {
                let Some((pn, un)) = r.split_once('#') else {
                    continue; // 形状错已在加载期报
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
                                "`form = \"skin\"` 再导出的单元 `{r}` 不存在(提供方 `{pn}` 或单元 `{un}` 缺失, 不变量 3)"
                            ),
                            "§3.5: 列表里每一个被再导出单元都必须存在",
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
                                        "皮肤 `{}` 的 `api_iface = {}`, 但被再导出单元 `{r}` 是 `{}` —— 分类必须相等(不变量 3)",
                                        p.name, exp.api_iface, pe.api_iface
                                    ),
                                    "§3.5: 皮肤只再导出同类单元(跨文件判定)",
                                ),
                            ));
                        }
                    }
                }
            }
        }
    }

    // ---- ② 单元级碰撞(同 `name` 的单元在两个插件里) ----
    let mut by_unit: std::collections::BTreeMap<String, Vec<String>> =
        std::collections::BTreeMap::new();
    for p in &selected {
        for e in &p.exports {
            by_unit
                .entry(e.name.clone())
                .or_default()
                .push(p.name.clone());
        }
    }
    for (unit, providers) in &by_unit {
        if providers.len() > 1 {
            let file = tree
                .by_name(&providers[1])
                .map(|p| p.file.clone())
                .unwrap_or_default();
            out.push((
                SCOPE_IFACE,
                err(
                    "BRV-MF-0001",
                    &providers[1],
                    &file,
                    "export.name",
                    format!(
                        "单元名 `{unit}` 在闭包内被多个插件导出: {}(接口单元 id 必须唯一)",
                        providers.join(", ")
                    ),
                    "缺口代用(R-9): 单元级碰撞无专属码; §2 第 4 项只要求单元级可查",
                ),
            ));
        }
    }

    // ---- ③ 符号族碰撞(`symbols` 重叠) ----
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
                                "符号族 `{s}` 被 `{owner}`({file})与 `{}`({})同时声明(碰撞)",
                                p.name, p.file
                            ),
                            "缺口代用(R-9): 符号族碰撞无专属码; §2 第 4 项",
                        ),
                    ));
                } else {
                    sym_owner.insert(s.clone(), (p.name.clone(), p.file.clone()));
                }
            }
        }
    }

    // ---- ④ `requires_iface` 只报 info(V-10③; 绝不参与闭包) ----
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
                        "声明面接口依赖 `{}`(compat_gen={}, range={}): v0.1 不扫描声明面接口依赖, 仅登记(V-10③)",
                        r.id, r.compat_gen, r.range_raw
                    ),
                    "§5.6/§10 V-10: requires_iface 不参与闭包求解",
                ),
            ));
        }
    }

    // ---- ⑤ release 下 `unfreezing` 阻断(BRV-IFACE-0009, §5.3.4 规则 3) ----
    if profile == "release" {
        // `unfreeze` 只写快照(不改 `plugin.toml`)⇒ 两处都算(V-16 的端到端观测点)。
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
                                "release 不允许接口单元停在解冻窗口: `{id}` 处于 `unfreezing`(声明或快照)"
                            ),
                            "§5.3.4 规则 3: release 必须在冻结态; 跑 `iface refreeze` 退出窗口",
                        ),
                    ));
                }
            }
        }
    }

    out
}

// ------------------------------------------------------------------ tax 域

fn tax_checks(tree: &TreeLoad, closure: &solver::Closure) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    let selected: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| closure.selected.contains(&p.name))
        .collect();

    // ---- `sched_class` 组合合法性(§2 第 3 项 / §8.1) ----
    for p in &selected {
        // TT_SAFE 必须同时有 [sched.tt]
        if p.sched_class == "TT_SAFE" && p.sched_tt.is_none() {
            out.push((
                SCOPE_TAX,
                err(
                    "BRV-MF-0001",
                    &p.name,
                    &p.file,
                    "sched_class",
                    format!(
                        "`sched_class = \"TT_SAFE\"` 必须同时声明 `[sched.tt]`(period_us/deadline_us/wcet_us); `{}` 缺该表",
                        p.name
                    ),
                    "§8.1: TT_SAFE 须同时给周期元数据; 缺口代用(R-9)",
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
                            "`sched_class = \"COOP_ONLY\"` 的 `{}` 与调度器 `{}`(sched_kind = `{kind}`)同组合: 需要 `coop` 调度器",
                            p.name, s.name
                        ),
                        "§2 第 3 项 / §8.1; sched_kind 见裁定 R-5(显式字段优先, 缺省按名字推断); 缺口代用(R-9)",
                    ),
                ));
            }
        }
    }

    out
}

// ------------------------------------------------------------------ priv 域

fn priv_checks(tree: &TreeLoad, closure: &solver::Closure) -> Vec<(&'static str, Diag)> {
    let mut out: Vec<(&'static str, Diag)> = Vec::new();
    let selected: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| closure.selected.contains(&p.name))
        .collect();

    // 独占资源冲突(IRQ / DMA 通道 / 引脚 / 设备名)⇒ 报**双方**(R-9 / V-19④)。
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
                            "资源独占冲突: {kind} `{val}` 被 `{owner}`({file})与 `{}`({})同时声明",
                            p.name, p.file
                        ),
                        "§2 第 5 项 / §8.1: [privileged.resources] 只做冲突检测; 缺口代用(R-9), 报双方",
                    ),
                ));
            } else {
                owners.insert(key, (p.name.clone(), p.file.clone()));
            }
        }
    }

    out
}

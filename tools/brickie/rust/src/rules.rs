//! 分类学、名字契约、相位、特权与依赖方向规则(§3 / §7.2 / §7.3 / §8.3 / §8.4)。
//!
//! 本模块是**纯规则表**(无 IO、无诊断集合状态), 供 model.rs 与 check.rs(agent-B)
//! 复用。凡"规则"都在这里, 不要把判定散到 L5。
//!
//! 编码纪律: 只用 BRV-D8 的已分配码; 未分配码的缺陷按裁定 R-9 用 `BRV-MF-0001`
//! 并在 `rust/README.md` 登记缺口。

use crate::diag::{Diag, Severity};

// ------------------------------------------------------------------ 枚举全集

pub const PLUGIN_TYPES: &[&str] = &["app", "interface", "ability", "platform"];
pub const API_TYPES: &[&str] = &["native", "runtime_adapter", "third_party"];
pub const SUBKINDS: &[&str] = &["scheduler", "framework", "io", "fs", "service"];
pub const NAMESPACES: &[&str] = &["app", "iface", "platform", "sched", "framework", "io", "fs", "service"];
pub const PHASES: &[&str] = &["early", "core", "late", "app"];
pub const SCHED_CLASSES: &[&str] = &["SAFE_PREEMPT", "COOP_ONLY", "TT_SAFE"];
pub const SCHED_KINDS: &[&str] = &["coop", "preempt", "tt"];
pub const LANGS: &[&str] = &["c", "cxx", "rust"];
pub const PRIV_LEVELS: &[&str] = &["P0", "P1", "P2", "P3", "P4"];
pub const GRANULARITIES: &[&str] = &["byte", "page", "region", "pool"];
pub const MEM_OPS: &[&str] = &["alloc", "free", "map", "protect", "flush", "invalidate", "unmap"];
pub const REGIONS: &[&str] = &["heap", "dma", "contig", "page", "mmio", "reserved"];
pub const ENTRY_KINDS: &[&str] = &["func", "var", "macro", "type", "enum", "service", "symbol-family"];
pub const EXPORT_FORMS: &[&str] = &["api", "skin", "service"];
pub const DEP_KINDS: &[&str] = &["init", "runtime", "type"];
pub const FREEZE_STATES: &[&str] = &["unfrozen", "frozen", "unfreezing"];
pub const ENTRY_STATUSES: &[&str] = &["experimental", "frozen", "deprecated"];
pub const RES_KINDS: &[&str] = &["ram", "stack"];

/// v0.1 已交付的模板维度(`BRV-TAX-0014` 的判据, §8.4)。
pub const TEMPLATE_API_TYPES: &[&str] = &["native"];
pub const TEMPLATE_LANGS: &[&str] = &["c"];

pub fn in_list(list: &[&str], v: &str) -> bool {
    list.contains(&v)
}

/// 把列表渲染成 `a | b | c`, 供诊断 hint。
pub fn list_of(list: &[&str]) -> String {
    list.join(" | ")
}

// ------------------------------------------------------------------ 名字契约(§8.3)

fn is_alnum_lower(c: u8) -> bool {
    c.is_ascii_lowercase() || c.is_ascii_digit()
}

fn is_separator(c: u8) -> bool {
    c == b'-' || c == b'_' || c == b'/'
}

/// `^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$`
pub fn valid_plugin_name(name: &str) -> bool {
    let b = name.as_bytes();
    if b.is_empty() || !b[0].is_ascii_lowercase() {
        return false;
    }
    let mut i = 1;
    while i < b.len() && is_alnum_lower(b[i]) {
        i += 1;
    }
    while i < b.len() {
        if !is_separator(b[i]) {
            return false;
        }
        i += 1;
        let seg = i;
        while i < b.len() && is_alnum_lower(b[i]) {
            i += 1;
        }
        if i == seg {
            return false; // 分隔符后必须至少一个 [a-z0-9]
        }
    }
    true
}

/// 名字的 namespace(第一个 `/` 之前); 无 `/` ⇒ 空串(裸名)。
pub fn name_namespace(name: &str) -> &str {
    match name.find('/') {
        Some(p) => &name[..p],
        None => "",
    }
}

/// 名字的 short(最后一个 `/` 之后)。
pub fn name_short(name: &str) -> &str {
    match name.rfind('/') {
        Some(p) => &name[p + 1..],
        None => name,
    }
}

/// 是否推荐形态 `<namespace>/<short>`(§8.3 ①)。
pub fn is_recommended_form(name: &str) -> bool {
    let ns = name_namespace(name);
    !ns.is_empty() && in_list(NAMESPACES, ns)
}

/// namespace → `plugin_type`(§8.3 ①)。
pub fn namespace_plugin_type(ns: &str) -> Option<&'static str> {
    match ns {
        "app" => Some("app"),
        "iface" => Some("interface"),
        "platform" => Some("platform"),
        "sched" | "framework" | "io" | "fs" | "service" => Some("ability"),
        _ => None,
    }
}

/// namespace → `subkind`(§8.3 ①; 仅 ability 系)。
pub fn namespace_subkind(ns: &str) -> Option<&'static str> {
    match ns {
        "sched" => Some("scheduler"),
        "framework" => Some("framework"),
        "io" => Some("io"),
        "fs" => Some("fs"),
        "service" => Some("service"),
        _ => None,
    }
}

/// 按名字 namespace 推导 `subkind`(§8.4)。裸名 / 非 ability namespace ⇒ `None`。
pub fn derive_subkind(name: &str) -> Option<&'static str> {
    let ns = name_namespace(name);
    if ns.is_empty() {
        return None;
    }
    namespace_subkind(ns)
}

// ------------------------------------------------------------------ 相位(§7.2 + 父 agent 澄清)

/// 相位序号: `EARLY=0 < CORE=1 < LATE=2 < APP=3`。
pub fn phase_rank(phase: &str) -> Option<u8> {
    match phase {
        "early" => Some(0),
        "core" => Some(1),
        "late" => Some(2),
        "app" => Some(3),
        _ => None,
    }
}

/// "service 形" ⇒ 第 ② 完成点在 LATE: `plugin_type = interface`
/// 或 `plugin_type = ability` 且 `subkind = service`。
pub fn is_service_like(plugin_type: &str, subkind: Option<&str>) -> bool {
    plugin_type == "interface" || (plugin_type == "ability" && subkind == Some("service"))
}

/// 推导 `[plugin].phase`(第 ② 个完成点)。
///
/// - 无 `init` 钩子 ⇒ `early`(② 与 ① 重合的特例)。
/// - service 形(interface / ability.service)⇒ `late`。
/// - `app` ⇒ `app`。
/// - 其余(platform / ability 其它 subkind)⇒ `core`。
pub fn derive_phase(plugin_type: &str, subkind: Option<&str>, has_init: bool) -> &'static str {
    if !has_init {
        return "early";
    }
    if is_service_like(plugin_type, subkind) {
        "late"
    } else if plugin_type == "app" {
        "app"
    } else {
        "core"
    }
}

/// `phase` 对给定 `plugin_type`/`subkind` 是否合法。`None` = 合法。
///
/// 允许集(**父 agent 2026 澄清口径**):
/// - `interface` 或 `ability.service` ⇒ `{early, late}`(给 `core` ⇒ 错)。
/// - `platform` / `ability`(其它 subkind)⇒ `{early, core}`。
/// - `app` ⇒ `{early, core, app}`(M0 的 `app/hello` 无 `init`、只有 `start`
///   主循环, 声明 `phase = "app"` 必须通过)。
///
/// `early` 恒合法 = "本插件没有 `init` 钩子"。
pub fn phase_reason(plugin_type: &str, subkind: Option<&str>, phase: &str) -> Option<String> {
    if !in_list(PHASES, phase) {
        return Some(format!(
            "未知 phase `{phase}`; 合法值: {}(§7.2)",
            list_of(PHASES)
        ));
    }
    if phase == "early" {
        return None;
    }
    let allowed: &[&str] = if is_service_like(plugin_type, subkind) {
        &["late"]
    } else if plugin_type == "app" {
        &["core", "app"]
    } else {
        &["core"]
    };
    if allowed.contains(&phase) {
        return None;
    }
    Some(format!(
        "phase `{phase}` 与 `plugin_type = {plugin_type}`{}{} 不相容; 该组合的 `init` 完成点只允许 {}(`early` 恒可, 用于无 `init` 钩子) (§7.2)",
        subkind.map(|s| format!(" / `subkind = {s}`")).unwrap_or_default(),
        "",
        list_of(allowed)
    ))
}

// ------------------------------------------------------------------ 特权(§3.4)

/// 级别 → 序号(`P0`=0 … `P4`=4)。
pub fn level_rank(level: &str) -> Option<u8> {
    PRIV_LEVELS
        .iter()
        .position(|l| *l == level)
        .map(|i| i as u8)
}

pub fn level_name(rank: u8) -> &'static str {
    PRIV_LEVELS[(rank as usize).min(4)]
}

/// `plugin_type`/`subkind` 允许的最高特权级(§3.2 + §3.4)。
pub fn max_privilege_level(plugin_type: &str, subkind: Option<&str>) -> &'static str {
    match plugin_type {
        "app" | "interface" => "P0",
        "platform" => "P4",
        "ability" => {
            if subkind == Some("scheduler") {
                "P3"
            } else {
                "P2"
            }
        }
        _ => "P0",
    }
}

/// 一个 memory op 所需的最低级别(§3.4 正交表)。
pub fn min_level_for_memory_op(op: &str) -> Option<u8> {
    match op {
        "alloc" | "free" | "flush" | "invalidate" => Some(2), // P2
        "map" | "protect" | "unmap" => Some(4),               // P4(machine)
        _ => None,
    }
}

/// `(ops × granularity × region)` 组合是否合法(§3.4 正交表)。
///
/// 非法 ⇒ `BRV-PRIV-0002`; 合法但所需级别高于声明级别 ⇒ `BRV-PRIV-0001`。
pub fn memory_combo_legal(op: &str, granularity: &str, region: &str) -> bool {
    if !in_list(GRANULARITIES, granularity) || !in_list(REGIONS, region) {
        return false;
    }
    match op {
        // alloc/free: 池生命周期(pool)只对 contig/page 两个专用池;
        // 其余粒度允许 heap(仅 byte/page/region)/contig/page/dma。
        "alloc" | "free" => {
            if granularity == "pool" {
                matches!(region, "contig" | "page")
            } else {
                matches!(region, "heap" | "contig" | "page" | "dma")
            }
        }
        // map/protect: MMU/页表面 ⇒ P4; regions = mmio / reserved / heap(仅恒等区属性)。
        "map" | "protect" | "unmap" => {
            matches!(granularity, "page" | "region") && matches!(region, "mmio" | "reserved" | "heap")
        }
        // flush/invalidate: cache 维护。
        "flush" | "invalidate" => {
            matches!(granularity, "byte" | "page" | "region")
                && matches!(region, "dma" | "heap" | "contig")
        }
        _ => false,
    }
}

/// 一条 `[[privileged.memory]]` 声明(与 model 解耦的输入形状)。
#[derive(Clone, Debug, Default)]
pub struct MemoryEntry {
    pub granularity: String,
    pub ops: Vec<String>,
    pub regions: Vec<String>,
}

/// 特权声明校验的输入(与 model 解耦)。
#[derive(Clone, Debug, Default)]
pub struct PrivInput<'a> {
    pub plugin_type: &'a str,
    pub subkind: Option<&'a str>,
    pub target: &'a str,
    pub file: &'a str,
    /// `[privileged].level`; `[privileged]` 存在时必填。
    pub level: Option<&'a str>,
    pub memory: &'a [MemoryEntry],
    /// `[privileged.resources]` 是否有内容(IRQ/DMA/引脚/设备名 ⇒ P1+)。
    pub has_resources: bool,
}

/// 特权声明校验(§3.4 / §10 V-19)。返回诊断(码固定 `BRV-PRIV-0001/0002`)。
pub fn check_privileged(input: &PrivInput<'_>) -> Vec<Diag> {
    let mut out = Vec::new();
    if input.level.is_none() && input.memory.is_empty() && !input.has_resources {
        return out; // 没有声明特权面
    }

    let declared = match input.level {
        Some(l) => {
            if level_rank(l).is_none() {
                out.push(
                    Diag::new(Severity::Error, format!(
                        "未知特权级别 `{l}`; 合法值: {}(§3.4)",
                        list_of(PRIV_LEVELS)
                    ))
                    .code("BRV-PRIV-0001")
                    .target(input.target)
                    .file(input.file)
                    .span("privileged.level")
                    .hint("P0 none / P1 resource / P2 memory / P3 core / P4 machine"),
                );
                return out;
            }
            level_rank(l).unwrap()
        }
        None => {
            out.push(
                Diag::new(Severity::Error, "声明了特权面但缺 `[privileged].level`")
                    .code("BRV-PRIV-0001")
                    .target(input.target)
                    .file(input.file)
                    .span("privileged.level")
                    .hint(format!("合法值: {}", list_of(PRIV_LEVELS))),
            );
            return out;
        }
    };

    // ① 声明的级别本身不得超过 plugin_type 允许的最高级。
    let cap_name = max_privilege_level(input.plugin_type, input.subkind);
    let cap = level_rank(cap_name).unwrap_or(0);
    if declared > cap {
        out.push(
            Diag::new(Severity::Error, format!(
                "`plugin_type = {}`{} 最多允许特权级 {cap_name}, 声明了 {}",
                input.plugin_type,
                input
                    .subkind
                    .map(|s| format!(" / `subkind = {s}`"))
                    .unwrap_or_default(),
                level_name(declared)
            ))
            .code("BRV-PRIV-0001")
            .target(input.target)
            .file(input.file)
            .span("privileged.level")
            .hint("§3.2: app/interface ≤ P0; ability ≤ P2(scheduler ≤ P3); platform ≤ P4"),
        );
    }

    // ② `[privileged.resources]`(IRQ/DMA/引脚/设备名)属 P1。
    if input.has_resources && declared < 1 {
        out.push(
            Diag::new(
                Severity::Error,
                "声明了 `[privileged.resources]`(IRQ/DMA/引脚/设备名)但级别是 P0; 资源独占属 P1",
            )
            .code("BRV-PRIV-0001")
            .target(input.target)
            .file(input.file)
            .span("privileged.resources")
            .hint("§3.4: P1 resource 才允许资源独占声明"),
        );
    }

    // ③ 逐 memory 条目: 组合合法性 + 最低级别。
    for (mi, m) in input.memory.iter().enumerate() {
        if m.ops.is_empty() {
            out.push(
                Diag::new(Severity::Error, "`[[privileged.memory]]` 缺 `ops`")
                    .code("BRV-PRIV-0002")
                    .target(input.target)
                    .file(input.file)
                    .span(format!("privileged.memory[{mi}].ops"))
                    .hint(format!("合法 op: {}", list_of(MEM_OPS))),
            );
        }
        if m.regions.is_empty() {
            out.push(
                Diag::new(Severity::Error, "`[[privileged.memory]]` 缺 `regions`")
                    .code("BRV-PRIV-0002")
                    .target(input.target)
                    .file(input.file)
                    .span(format!("privileged.memory[{mi}].regions"))
                    .hint(format!("合法 region: {}", list_of(REGIONS))),
            );
        }
        for (oi, op) in m.ops.iter().enumerate() {
            if !in_list(MEM_OPS, op) {
                out.push(
                    Diag::new(Severity::Error, format!(
                        "未知 memory op `{op}`; 合法值: {}",
                        list_of(MEM_OPS)
                    ))
                    .code("BRV-PRIV-0002")
                    .target(input.target)
                    .file(input.file)
                    .span(format!("privileged.memory[{mi}].ops[{oi}]"))
                    .hint("§3.4 正交表"),
                );
                continue;
            }
            for (ri, region) in m.regions.iter().enumerate() {
                if !memory_combo_legal(op, &m.granularity, region) {
                    out.push(
                        Diag::new(Severity::Error, format!(
                            "非法的 (op × granularity × region) 组合: `{op}` × `{}` × `{region}`",
                            m.granularity
                        ))
                        .code("BRV-PRIV-0002")
                        .target(input.target)
                        .file(input.file)
                        .span(format!("privileged.memory[{mi}].regions[{ri}]"))
                        .hint("§3.4 正交表: alloc/free→P2(池级只对 contig/page); map/protect/unmap→P4(page/region × mmio/reserved/heap); flush/invalidate→P2"),
                    );
                }
            }
            if let Some(need) = min_level_for_memory_op(op) {
                if declared < need {
                    out.push(
                        Diag::new(Severity::Error, format!(
                            "op `{op}` 最低要求 {}, 但声明级别是 {}",
                            level_name(need),
                            level_name(declared)
                        ))
                        .code("BRV-PRIV-0001")
                        .target(input.target)
                        .file(input.file)
                        .span(format!("privileged.memory[{mi}]"))
                        .hint("§3.4: map/protect/unmap 属 P4(machine); alloc/free/flush/invalidate 属 P2"),
                    );
                }
            }
        }
    }
    out
}

// ------------------------------------------------------------------ 依赖方向(§7.3)

/// `plugin_type` 方向表(§7.3): 消费者 → 提供者 是否许可。
///
/// 表里的 `—`(platform → platform)按"不适用/不许可"处理: `platform` 每镜像恰 1,
/// 同代单版本政策下不可能自依赖。
pub fn dep_direction_ok(consumer_pt: &str, provider_pt: &str, provider_is_third_party: bool) -> bool {
    match consumer_pt {
        // platform: 全列 ✗(platform 是 SoC 级最底层, 不依赖任何插件)
        "platform" => false,
        // ability: platform ✅ / ability ✅(单向) / third_party ✅ / interface ✗
        "ability" => {
            provider_is_third_party
                || matches!(provider_pt, "platform" | "ability")
        }
        // interface: platform/ability/interface ✅(仅再导出) / third_party ✗
        "interface" => {
            !provider_is_third_party && matches!(provider_pt, "platform" | "ability" | "interface")
        }
        // app: 仅 interface ✅(不得直调 native ability / platform);
        // **例外**: `api_type = third_party` 的 ability(如 `service/sqlite`)——
        // §7.3 表的 app 行括注是"不得直调 **native**", 而 §10 V-12② 要求
        // `app → service/sqlite` 必须通过(见 rust/README §10 S-13)。
        "app" => provider_pt == "interface" || provider_is_third_party,
        _ => false,
    }
}

/// `api_type` 禁则(§3.1; **只对有面的提供方** —— `app`/`interface` 是纯消费者)。
///
/// 违规返回规则说明; 合法返回 `None`。
pub fn api_type_edge_reason(
    consumer_pt: &str,
    consumer_api: &str,
    provider_api: &str,
    kind: &str,
) -> Option<String> {
    // 纯消费者(app/interface)不参与 api_type 禁则(§3.1 口径收窄)。
    if !matches!(consumer_pt, "ability" | "platform") {
        return None;
    }
    if consumer_api == "native" && provider_api == "runtime_adapter" {
        return Some(
            "禁则(2): `native` 不得依赖 `runtime_adapter` 的能力面(§3.1/§7.3)".to_string(),
        );
    }
    if consumer_api == "native" && provider_api == "third_party" {
        return Some(
            "`native` 插件不得依赖 `api_type = \"third_party\"` 的插件(§3.1 表 / §7.3 第四条)".to_string(),
        );
    }
    if consumer_api == "runtime_adapter" && provider_api == "third_party" && kind != "type" {
        return Some(format!(
            "`runtime_adapter` 只允许对 `third_party` 声明 `kind = \"type\"` 的编译期类型边, 收到 `kind = \"{kind}\"`(§3.1 显式例外)"
        ));
    }
    None
}

/// 依赖边的完整判定(方向表 + api_type 禁则 + `allow_edges` 白名单)。
///
/// `allow_edges` 为 `product.toml [lint].allow_edges` 的 `(consumer, provider)` 对;
/// 它**只**豁免 `plugin_type` 方向表(框架件特例), **不**豁免 `api_type` 硬禁则。
///
/// 返回 `Some(message)` = 违规。调用方按裁定 R-9 用 `BRV-MF-0001` 报码
/// (BRV-D8 未给依赖方向禁则分配码; 已在 README 登记缺口)。
pub fn dep_edge_violation(
    consumer_name: &str,
    consumer_pt: &str,
    consumer_api: &str,
    provider_name: &str,
    provider_pt: &str,
    provider_api: &str,
    kind: &str,
    allow_edges: &[(String, String)],
) -> Option<String> {
    let provider_is_tp = provider_api == "third_party";
    let whitelisted = allow_edges
        .iter()
        .any(|(c, p)| c == consumer_name && p == provider_name);

    if !whitelisted && !dep_direction_ok(consumer_pt, provider_pt, provider_is_tp) {
        return Some(format!(
            "依赖方向禁则(§7.3): `plugin_type = {consumer_pt}` 的 `{consumer_name}` 不得依赖 `plugin_type = {provider_pt}`{} 的 `{provider_name}`",
            if provider_is_tp { "(`api_type = third_party`)" } else { "" }
        ));
    }
    api_type_edge_reason(consumer_pt, consumer_api, provider_api, kind)
}

// ------------------------------------------------------------------ 导出面分类不变量(§3.5)

/// 一个 `[[export]]` 的只读视图(供 [`check_export_invariants`])。
#[derive(Clone, Debug)]
pub struct ExportView<'a> {
    pub api_iface: &'a str,
    pub form: &'a str,
    pub reexport_of: &'a [String],
    pub symbols: &'a [String],
}

/// 判定 `reexport_of` 的元素形状 `<provider>#<unit>`(§6.1)。
pub fn valid_unit_ref(s: &str) -> bool {
    match s.split_once('#') {
        Some((provider, unit)) => {
            !provider.is_empty() && !unit.is_empty() && !unit.contains('#') && valid_plugin_name(provider)
        }
        None => false,
    }
}

/// 导出面分类不变量(§3.5 不变量 1–4)。返回 `(export 下标, 码, 消息)`。
///
/// 分工说明: 不变量 3 的"列表中每个被再导出单元的 `api_iface` **与自身相等**"
/// 需要读**其它插件**的导出面 ⇒ 是跨文件判定, 归 agent-B(`iface`/`check`);
/// 本函数只判单记录内可判的部分(非空 + 元素形状 + 反向不变量 4)。
pub fn check_export_invariants(
    plugin_api_type: &str,
    exports: &[ExportView<'_>],
) -> Vec<(usize, &'static str, String)> {
    let mut out = Vec::new();
    for (i, e) in exports.iter().enumerate() {
        // 不变量 1 + 2: api_iface 必须等于 plugin.api_type; third_party 不得抛异类面。
        // 二者对 `third_party` 的场景做**分区**, 避免同一个问题报两条码。
        if plugin_api_type == "third_party" {
            if e.api_iface != "third_party" {
                out.push((
                    i,
                    "BRV-TAX-0017",
                    format!(
                        "`api_type = third_party` 的插件不得抛出 `{}` 分类的接口面(不变量 2)",
                        e.api_iface
                    ),
                ));
            }
        } else if e.api_iface != plugin_api_type {
            out.push((
                i,
                "BRV-TAX-0016",
                format!(
                    "`export.api_iface = {}` 必须等于 `[plugin].api_type = {}`(不变量 1)",
                    e.api_iface, plugin_api_type
                ),
            ));
        }

        // 不变量 3: form = skin ⇒ reexport_of 非空, 且每个元素形状合法。
        if e.form == "skin" {
            if e.reexport_of.is_empty() {
                out.push((
                    i,
                    "BRV-TAX-0018",
                    "`form = \"skin\"` 必须声明非空的 `reexport_of`(不变量 3)".to_string(),
                ));
            }
            for r in e.reexport_of {
                if !valid_unit_ref(r) {
                    out.push((
                        i,
                        "BRV-TAX-0018",
                        format!("`reexport_of` 元素 `{r}` 形状不符: 应为 `<provider>#<unit>`"),
                    ));
                }
            }
        } else {
            // 不变量 4: form != skin ⇒ 不得声明 reexport_of / symbols。
            if !e.reexport_of.is_empty() {
                out.push((
                    i,
                    "BRV-TAX-0019",
                    format!(
                        "`form = \"{}\"` 却声明了 `reexport_of`(只有 skin 允许; 不变量 4)",
                        e.form
                    ),
                ));
            }
            if !e.symbols.is_empty() {
                out.push((
                    i,
                    "BRV-TAX-0019",
                    format!(
                        "`form = \"{}\"` 却声明了 `symbols`(只有 skin 允许; 不变量 4)",
                        e.form
                    ),
                ));
            }
        }
    }
    out
}

// ------------------------------------------------------------------ 模板可得性(§8.4)

/// v0.1 模板是否已交付。未交付 ⇒ `BRV-TAX-0014`(severity=info, 但命令未完成 ⇒ 退出码 2, G-1)。
pub fn template_available(api_type: &str, lang: &str) -> bool {
    in_list(TEMPLATE_API_TYPES, api_type) && in_list(TEMPLATE_LANGS, lang)
}

/// 模板未交付的说明(hint 用)。
pub fn template_unavailable_reason(api_type: &str, lang: &str) -> String {
    format!(
        "v0.1 只交付 `native` × `c` 模板; 请求的是 `{api_type}` × `{lang}`(unavailable = v0.x 交付)"
    )
}

//! 生成计划: `plan-new` / `plan-init` / `gen-plan`(contract §5.2)。
//!
//! **只给计划, 不给内容**: 返回 `data.artifacts: [{path, kind, template, vars}]`,
//! 内容由 L2 `brickie-gen render` 渲染(两段式: plan → 扫盘 → render)。
//!
//! 纪律:
//! - **不覆盖人写文件**: 已存在的 `human` 路径 ⇒ `BRV-GEN-0002`(error, 退出码 1)。
//! - `--force` **只对生成物目录**生效, 且只在"路径上放的不是我们的生成物"时才需要。
//! - 生成物靠首行标记 `brickie:generated` 识别(`first_line` 由 L5 扫描后回传)。
//!
//! 变量命名: 同时提供**两套**占位符名 —— 现行模板用小写(`{{name}}` / `{{phase}}`),
//! cxx 首刀用大写(`{{PLUGIN_NAME}}`)。多给变量对渲染无副作用(渲染器按名查找),
//! 这样模板侧无论取哪套都能渲染。

use std::collections::BTreeMap;

use serde_json::{json, Value};

use crate::diag::Diags;
use crate::model::Plugin;
use crate::rules;
use crate::version;

/// 生成物首行标记 —— **单一来源**在 [`crate::emit::GENERATED_MARKER`]。
pub use crate::emit::GENERATED_MARKER;

/// 一条生成计划。
#[derive(Clone, Debug)]
pub struct Artifact {
    /// 相对 `root`。
    pub path: String,
    /// `human` | `generated` | `machine`。
    pub kind: String,
    /// 模板 id(相对 `templates/`); `machine` 文件由 core 自己写, 为空。
    pub template: String,
    pub vars: BTreeMap<String, String>,
}

impl Artifact {
    pub fn to_json(&self) -> Value {
        json!({
            "path": self.path,
            "kind": self.kind,
            "template": self.template,
            "vars": self.vars,
        })
    }
}

/// 一条 `[[dep]]` 的展开事实(描述符里的 `br_dep_t` 一条)。
#[derive(Clone, Debug)]
pub struct DepFact {
    pub name: String,
    pub kind: String,
    pub phase: Option<String>,
}

/// 一个插件的"事实"(供变量展开), 与 model::Plugin 解耦以便 plan-new 用在建模之前。
#[derive(Clone, Debug)]
pub struct PluginFacts<'a> {
    pub name: &'a str,
    pub plugin_type: &'a str,
    pub api_type: &'a str,
    pub subkind: Option<&'a str>,
    pub lang: &'a str,
    pub phase: &'a str,
    pub sched_class: &'a str,
    /// 钩子/描述符符号前缀覆盖(见 `model::Plugin::symbol_prefix`); `None` = 按 short 推导。
    pub symbol_prefix: Option<String>,
    pub version: &'a str,
    /// `[compat].api_rev`(缺省 1)。
    pub api_rev: i64,
    /// `[compat].abi_id`(缺省空串)。
    pub abi_id: String,
    /// `[[res]]` 求和: RAM / 栈(KiB)。
    pub res_ram_kib: i64,
    pub res_stack_kib: i64,
    /// 结构依赖逐条(描述符的 `.deps` 数组)。
    pub deps: Vec<DepFact>,
    /// 生成物数量(弱符号 `br_plugin_gen_total` 的值; 见 ADR-0005 §2.4)。
    pub gen_total: u32,
    /// 描述符的声明面锚头(相对插件 `include/` 根的路径)。
    pub decl_include: String,
    /// 有 `init` 钩子吗(由 `phase != "early"` 推导, 见 rules::derive_phase)。
    pub has_init: bool,
    /// 这次生成要不要发 `selftest` 钩子(ADR-0010)。
    ///
    /// = 插件声明了 `[selftest]` **且** 产品开关(含名单)放行。为假时生成物写
    /// `BR_PLUGIN_NO_HOOK` ⇒ 测试代码无人引用 ⇒ `--gc-sections` 把它整段裁出镜像。
    pub emit_selftest: bool,
}

/// `descriptor_include` 的回落值: 元契约头(任何组合里都存在)。
pub const FALLBACK_DECL_INCLUDE: &str = "br/core/br_plugin.h";

// ------------------------------------------------------------------ 语法面 → C 常量

fn plugin_type_const(t: &str) -> &'static str {
    match t {
        "platform" => "BR_PLUGIN_TYPE_PLATFORM",
        "app" => "BR_PLUGIN_TYPE_APP",
        "interface" => "BR_PLUGIN_TYPE_INTERFACE",
        _ => "BR_PLUGIN_TYPE_ABILITY",
    }
}

fn subkind_const(s: Option<&str>) -> &'static str {
    match s.unwrap_or("") {
        "scheduler" => "BR_SUBKIND_SCHEDULER",
        "framework" => "BR_SUBKIND_FRAMEWORK",
        "io" => "BR_SUBKIND_IO",
        "fs" => "BR_SUBKIND_FS",
        "service" => "BR_SUBKIND_SERVICE",
        _ => "BR_SUBKIND_NONE",
    }
}

fn sched_class_const(c: &str) -> &'static str {
    match c {
        "COOP_ONLY" => "BR_SCHED_CLASS_COOP_ONLY",
        "TT_SAFE" => "BR_SCHED_CLASS_TT_SAFE",
        _ => "BR_SCHED_CLASS_SAFE_PREEMPT",
    }
}

fn dep_kind_const(kind: &str) -> &'static str {
    match kind {
        "init" => "BR_DEP_INIT",
        "type" => "BR_DEP_TYPE",
        _ => "BR_DEP_RUNTIME",
    }
}

/// `br_dep_t.phase`: 只有 `kind = "init"` 的断言有意义; 其余/缺省 = `BR_PHASE_NONE`
/// (冻结头 `br_plugin.h`: "不约束完成点")。
fn dep_phase_const(kind: &str, phase: Option<&str>) -> &'static str {
    if kind != "init" {
        return "BR_PHASE_NONE";
    }
    match phase {
        Some("early") => "BR_PHASE_EARLY",
        Some("core") => "BR_PHASE_CORE",
        Some("late") => "BR_PHASE_LATE",
        Some("app") => "BR_PHASE_APP",
        _ => "BR_PHASE_NONE",
    }
}

/// 依赖数组的 C 文本 + `.deps` 的取值表达式(无边 ⇒ `BR_NULL`)。
fn deps_decl(tag: &str, deps: &[DepFact]) -> (String, String) {
    if deps.is_empty() {
        return (String::new(), "BR_NULL".to_string());
    }
    let sym = format!("_br_deps_{tag}");
    let mut out = String::new();
    out.push_str(&format!(
        "static const br_dep_t {sym}[] __attribute__((used)) = {{\n"
    ));
    for d in deps {
        out.push_str(&format!(
            "    {{ .name = \"{}\", .kind = {}, .phase = {}, .compat_gen = 0u }},\n",
            d.name,
            dep_kind_const(&d.kind),
            dep_phase_const(&d.kind, d.phase.as_deref())
        ));
    }
    out.push_str("    { .name = BR_NULL, .kind = 0u, .phase = BR_PHASE_NONE, .compat_gen = 0u },\n};\n");
    (out, sym)
}

/// 扫插件 `include/` 下的对外头, 选**声明面锚头**: 声明了该插件最多
/// `[[export.entries]]` 符号的那个(同数 ⇒ 字典序第一个, 保证确定性); 一个头都没有
/// (或读不到内容)⇒ 回落到元契约头。
///
/// 为什么不是"哪个头都行": 锚头进生成物, `gen --check` 会因它变化而报红 ——
/// 规则必须确定性且与**声明面事实**挂钩, 而不是与目录遍历顺序挂钩。
fn scan_decl_include(root: &std::path::Path, plugin: &str, symbols: &[String]) -> Option<String> {
    let base = root.join(plugin).join("include");
    let mut found: Vec<String> = Vec::new();
    collect_headers(&base, &base, &mut found);
    found.sort();
    if found.is_empty() {
        return None;
    }
    let mut best: Option<(usize, String)> = None;
    for rel in &found {
        let text = std::fs::read_to_string(base.join(rel)).unwrap_or_default();
        let hits = symbols.iter().filter(|s| !s.is_empty() && text.contains(s.as_str())).count();
        match &best {
            Some((n, _)) if *n >= hits => {}
            _ => best = Some((hits, rel.clone())),
        }
    }
    best.map(|(_, rel)| rel)
}

fn collect_headers(base: &std::path::Path, dir: &std::path::Path, out: &mut Vec<String>) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect_headers(base, &p, out);
        } else if p.extension().map(|x| x == "h").unwrap_or(false) {
            if let Ok(rel) = p.strip_prefix(base) {
                out.push(rel.to_string_lossy().replace('\\', "/"));
            }
        }
    }
}

// ------------------------------------------------------------------ 名字形态工具

/// 符号前缀: 非 `[a-z0-9]` → `_`, 末尾补 `_`(与 cxx `rules.cpp` 同口径)。
pub fn symbol_prefix(short: &str) -> String {
    let mut out = String::with_capacity(short.len() + 1);
    for c in short.chars() {
        if c.is_ascii_lowercase() || c.is_ascii_digit() {
            out.push(c);
        } else if c.is_ascii_uppercase() {
            out.push(c.to_ascii_lowercase());
        } else {
            out.push('_');
        }
    }
    out.push('_');
    out
}

/// 头文件保护宏。
pub fn include_guard(ns: &str, short: &str) -> String {
    let base = if ns.is_empty() {
        short.to_string()
    } else {
        format!("{ns}_{short}")
    };
    let mut up = String::new();
    for c in base.chars() {
        if c.is_ascii_lowercase() {
            up.push(c.to_ascii_uppercase());
        } else if c.is_ascii_uppercase() || c.is_ascii_digit() {
            up.push(c);
        } else {
            up.push('_');
        }
    }
    format!("{up}_H")
}

/// 描述符宏实参(符号前缀去掉尾下划线)。
pub fn descriptor_tag(short: &str) -> String {
    let mut p = symbol_prefix(short);
    p.pop();
    p
}

/// 描述符实例符号名。
pub fn descriptor_symbol(short: &str) -> String {
    format!("_br_plugin_{}", descriptor_tag(short))
}

/// 展开一个插件的占位符表(**大小写两套**, 见模块注释)。
pub fn plugin_vars(f: &PluginFacts<'_>) -> BTreeMap<String, String> {
    let ns = rules::name_namespace(f.name).to_string();
    let short = rules::name_short(f.name).to_string();
    let sub = f.subkind.unwrap_or("").to_string();
    let sym = f
        .symbol_prefix
        .clone()
        .unwrap_or_else(|| symbol_prefix(&short));
    let guard = include_guard(&ns, &short);
    let dtag = descriptor_tag(&short);
    let dsym = descriptor_symbol(&short);
    let parsed = version::parse_version(f.version).ok();

    let mut v: BTreeMap<String, String> = BTreeMap::new();
    // ---- 小写(现行模板) ----
    v.insert("name".into(), f.name.to_string());
    v.insert("schema".into(), "1".into());
    v.insert("plugin_type".into(), f.plugin_type.to_string());
    v.insert("api_type".into(), f.api_type.to_string());
    v.insert("subkind".into(), sub.clone());
    v.insert("lang".into(), f.lang.to_string());
    v.insert("phase".into(), f.phase.to_string());
    v.insert("version".into(), f.version.to_string());
    v.insert("sched_class".into(), f.sched_class.to_string());
    v.insert("summary".into(), "TODO: 一句话描述本插件的能力面(见 §8.1)".into());
    v.insert("license".into(), "WTFPL".into());
    v.insert("short".into(), short.clone());
    v.insert("ns".into(), ns.clone());
    v.insert("header_guard".into(), guard.clone());
    v.insert("symbol_prefix".into(), sym.clone());
    v.insert("descriptor_symbol".into(), dsym.clone());
    // 描述符里 `#include "{{descriptor_include}}"`: 声明面锚头。规则(ADR-0005 §2.4):
    // 插件 `include/` 下**恰有一个**对外头 ⇒ 用它; 否则回落元契约头(多头插件如 platform
    // 无法机器判定"主头")。旧规则 `{short}/{short}.h` 指向的是**不存在**的路径。
    v.insert("descriptor_include".into(), f.decl_include.clone());
    // ---- 描述符实例化所需的字段(语法面 → C 常量/表达式) ----
    v.insert("plugin_type_id".into(), plugin_type_const(f.plugin_type).into());
    v.insert("subkind_id".into(), subkind_const(f.subkind).into());
    v.insert("sched_class_id".into(), sched_class_const(f.sched_class).into());
    v.insert("api_rev".into(), f.api_rev.to_string());
    v.insert("abi_id".into(), f.abi_id.clone());
    v.insert("res_ram_kib".into(), f.res_ram_kib.max(0).to_string());
    v.insert("res_stack_kib".into(), f.res_stack_kib.max(0).to_string());
    v.insert("gen_total".into(), f.gen_total.to_string());
    // 钩子发射规则(ADR-0005 §2.4): early_init/start 对全部插件(§6.2 的表);
    // init 仅在有 init 钩子(phase != "early")时发射, 否则 BR_PLUGIN_NO_HOOK。
    v.insert(
        "init_hook".into(),
        if f.has_init {
            format!("{sym}init")
        } else {
            "BR_PLUGIN_NO_HOOK".into()
        },
    );
    v.insert("early_proto".into(), format!("int {sym}early_init(void);"));
    v.insert(
        "init_proto".into(),
        if f.has_init {
            format!("int {sym}init(void);")
        } else {
            String::new()
        },
    );
    v.insert("start_proto".into(), format!("int {sym}start(void);"));
    // selftest(ADR-0010): 钩子名同样按 symbol_prefix 推导; 生成的**原型**与 init 一样
    // 只在本插件真的发钩子时给出(否则会引用一个不存在的符号 → -Wmissing-prototypes 与
    // 链接错误都会来)。关掉自检 ⇒ proto 为空 + hook = NO_HOOK。
    v.insert(
        "selftest_proto".into(),
        if f.emit_selftest {
            format!("int {sym}selftest(void);")
        } else {
            String::new()
        },
    );
    v.insert(
        "selftest_hook".into(),
        if f.emit_selftest {
            format!("{sym}selftest")
        } else {
            "BR_PLUGIN_NO_HOOK".into()
        },
    );
    let (deps_array, deps_expr) = deps_decl(&dtag, &f.deps);
    v.insert("deps_array".into(), deps_array);
    v.insert("deps_expr".into(), deps_expr);
    v.insert("export_api_iface".into(), f.api_type.to_string());
    v.insert("export_form".into(), "api".into()); // §8.4 / 裁定 G-3
    v.insert("export_name".into(), short.clone());
    v.insert("app".into(), f.name.to_string());

    // ---- 大写(cxx 首刀的变量名; 保留兼容) ----
    v.insert("PLUGIN_NAME".into(), f.name.to_string());
    v.insert("NS".into(), ns);
    v.insert("SHORT".into(), short.clone());
    v.insert("PLUGIN_TYPE".into(), f.plugin_type.to_string());
    v.insert("API_TYPE".into(), f.api_type.to_string());
    v.insert("SUBKIND".into(), sub);
    v.insert("LANG".into(), f.lang.to_string());
    v.insert("PHASE".into(), f.phase.to_string());
    v.insert("VERSION".into(), f.version.to_string());
    v.insert(
        "COMPAT_GEN".into(),
        parsed.map(|p| p.compat_gen.to_string()).unwrap_or_else(|| "0".into()),
    );
    v.insert(
        "MAJOR".into(),
        parsed.map(|p| p.major.to_string()).unwrap_or_else(|| "1".into()),
    );
    v.insert(
        "MINOR".into(),
        parsed.map(|p| p.minor.to_string()).unwrap_or_else(|| "0".into()),
    );
    v.insert(
        "REVISE".into(),
        parsed.map(|p| p.revise.to_string()).unwrap_or_else(|| "0".into()),
    );
    v.insert("SUMMARY".into(), v["summary"].clone());
    v.insert("LICENSE".into(), v["license"].clone());
    v.insert("CORE_RANGE".into(), ">=1.0.0".into());
    v.insert("API_REV".into(), "1".into());
    v.insert("SYMPREFIX".into(), sym);
    v.insert("GUARD".into(), guard);
    v.insert("DESC_TAG".into(), dtag);
    v.insert("DESC_SYMBOL".into(), dsym);
    v.insert("DESC_INCLUDE".into(), f.decl_include.clone());
    v.insert("EXPORT_UNIT".into(), short);
    v
}

/// 路径布局(§8.4 / contract §5.2)。返回 `(path, kind, template)` 六件。
pub fn plugin_paths(f: &PluginFacts<'_>) -> Vec<(String, String, String)> {
    let short = rules::name_short(f.name);
    let skel = format!("{}/{}/{}/", f.api_type, f.plugin_type, f.lang);
    let desc = format!("descriptor/{}/{}/", f.api_type, f.lang);
    vec![
        (
            format!("{}/plugin.toml", f.name),
            "human".into(),
            format!("{skel}plugin.toml.tmpl"),
        ),
        (
            format!("{}/src/{}.c", f.name, short),
            "human".into(),
            format!("{skel}src.c.tmpl"),
        ),
        (
            format!("{}/include/{}/{}.h", f.name, short, short),
            "human".into(),
            format!("{skel}include.h.tmpl"),
        ),
        (
            format!("{}/tests/smoke.toml", f.name),
            "human".into(),
            format!("{skel}smoke.toml.tmpl"),
        ),
        (
            format!("{}/README.md", f.name),
            "human".into(),
            format!("{skel}README.md.tmpl"),
        ),
        (
            format!("build/gen/{}/plugin_desc.c", f.name),
            "generated".into(),
            format!("{desc}plugin_desc.c.tmpl"),
        ),
    ]
}

// ------------------------------------------------------------------ 冲突判定

fn existing_index(context: &serde_json::Map<String, Value>) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    if let Some(arr) = context.get("existing").and_then(|v| v.as_array()) {
        for e in arr {
            if let (Some(p), h) = (
                e.get("path").and_then(|v| v.as_str()),
                e.get("first_line").and_then(|v| v.as_str()),
            ) {
                out.insert(p.to_string(), h.unwrap_or("").to_string());
            }
        }
    }
    out
}

fn looks_generated(first_line: &str) -> bool {
    first_line.contains(GENERATED_MARKER)
}

/// 冲突判定(§8.4): 人写文件一律不覆盖; 生成物仅在不是本工具生成物时冲突
/// (`--force` 可越过, 且 `--force` **只对生成物目录生效**)。
fn apply_conflicts(
    artifacts: &mut Vec<Artifact>,
    existing: &BTreeMap<String, String>,
    force: bool,
    target: &str,
    d: &mut Diags,
) {
    let mut keep = Vec::with_capacity(artifacts.len());
    for a in artifacts.drain(..) {
        match existing.get(&a.path) {
            None => keep.push(a),
            Some(first) => {
                if a.kind == "human" {
                    d.coded(
                        "BRV-GEN-0002",
                        crate::diag::Severity::Error,
                        target,
                        &a.path,
                        "",
                        format!("生成目标 `{}` 已存在且为人写文件, 不覆盖", a.path),
                        "人写文件不由工具改写; 换插件名或先移走该文件(§8.4)",
                    );
                    continue;
                }
                if looks_generated(first) {
                    keep.push(a); // 已是生成物 ⇒ 正常重建
                } else if force {
                    keep.push(a); // --force 只对生成物目录生效
                } else {
                    d.coded(
                        "BRV-GEN-0002",
                        crate::diag::Severity::Error,
                        target,
                        &a.path,
                        "",
                        format!("生成目标 `{}` 已存在且不是本工具的生成物", a.path),
                        "确认可覆盖后加 --force(--force 只对生成物目录生效)(§8.4)",
                    );
                }
            }
        }
    }
    *artifacts = keep;
}

// ------------------------------------------------------------------ plan-new

/// `plan-new`: 校验入参 → 推导 → 冲突判定 → 计划。
pub fn plan_new(args: &serde_json::Map<String, Value>, context: &serde_json::Map<String, Value>) -> (Diags, Vec<Artifact>) {
    let mut d = Diags::new();
    let get = |k: &str| args.get(k).and_then(|v| v.as_str()).map(str::to_string);

    let plugin_type = get("plugin_type").unwrap_or_default();
    let name = get("name").unwrap_or_default();
    let api_type = get("api_type").unwrap_or_else(|| "native".to_string());
    let lang = get("lang").unwrap_or_else(|| "c".to_string());
    let explicit_subkind = get("subkind").filter(|s| !s.is_empty());
    let force = args.get("force").and_then(|v| v.as_bool()).unwrap_or(false);

    // ---- 1. 形状(contract §7 / §8.1) ----
    if plugin_type.is_empty() {
        d.shape("BRV-MF-0001", &name, "", "args.plugin_type", "缺少必填参数 `plugin_type`", &format!("合法值: {}", rules::list_of(rules::PLUGIN_TYPES)));
        return (d, Vec::new());
    }
    if !rules::in_list(rules::PLUGIN_TYPES, &plugin_type) {
        d.shape("BRV-MF-0001", &name, "", "args.plugin_type", format!("未知插件类别 `{plugin_type}`"), &format!("合法值: {}(§8.1)", rules::list_of(rules::PLUGIN_TYPES)));
        return (d, Vec::new());
    }
    if !rules::in_list(rules::API_TYPES, &api_type) {
        d.shape("BRV-MF-0001", &name, "", "args.api_type", format!("未知 api_type `{api_type}`"), &format!("合法值: {}(§3.1)", rules::list_of(rules::API_TYPES)));
        return (d, Vec::new());
    }
    if name.is_empty() {
        d.shape("BRV-MF-0001", "", "", "args.name", "缺少必填参数 `name`", "推荐 `<namespace>/<short>`(§8.3)");
        return (d, Vec::new());
    }
    if !rules::valid_plugin_name(&name) {
        d.shape("BRV-MF-0001", &name, "", "args.name", format!("插件名 `{name}` 不符名字契约"), "契约: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return (d, Vec::new());
    }
    if !rules::in_list(rules::LANGS, &lang) {
        d.shape("BRV-MF-0001", &name, "", "args.lang", format!("未知 lang `{lang}`"), &format!("合法值: {}(§8.1)", rules::list_of(rules::LANGS)));
        return (d, Vec::new());
    }
    if !rules::in_list(rules::TEMPLATE_LANGS, &lang) {
        // v0.1 只交付 c 模板; rust 排 v2.0, cxx 待 BRV-Q8。
        d.usage(
            format!("v0.1 只交付 `c` 模板, 收到 `{lang}`"),
            "`rust` 排 v2.0(1-03); `cxx` 待 BRV-Q8 拍板",
        );
        return (d, Vec::new());
    }

    // ---- 2. api_type 模板交付状态(§8.4) ----
    // BRV-TAX-0014 的 severity = info, 但命令**无法完成** ⇒ 显式置退出码 2(G-1)。
    if !rules::template_available(&api_type, &lang) {
        d.coded(
            "BRV-TAX-0014",
            crate::diag::Severity::Info,
            &name,
            "",
            "args.api_type",
            format!("`{api_type}` 模板在 v0.1 未交付(目录已预留)"),
            rules::template_unavailable_reason(&api_type, &lang),
        );
        d.set_exit_override(crate::diag::EXIT_USAGE);
        return (d, Vec::new());
    }

    // ---- 3. subkind(§8.4) ----
    let mut subkind = String::new();
    if plugin_type == "ability" {
        match (rules::derive_subkind(&name), &explicit_subkind) {
            (Some(der), Some(explicit)) if der != explicit => {
                d.coded(
                    "BRV-TAX-0015",
                    crate::diag::Severity::Error,
                    &name,
                    "",
                    "args.subkind",
                    format!("subkind 推导与显式声明冲突: 由名字推导 = `{der}`, 显式 = `{explicit}`"),
                    "去掉 `--subkind`, 或改用与 namespace 相符的名字(§8.4)",
                );
                return (d, Vec::new());
            }
            (Some(der), _) => subkind = der.to_string(),
            (None, None) => {
                d.shape(
                    "BRV-MF-0001",
                    &name,
                    "",
                    "args.subkind",
                    format!("ability 插件 `{name}` 必须显式给 `subkind`(裸名无从推导)"),
                    &format!("合法值: {}; 或改用可推导的名字(service/ sched/ framework/ io/ fs/)(§8.4)", rules::list_of(rules::SUBKINDS)),
                );
                return (d, Vec::new());
            }
            (None, Some(explicit)) => {
                if !rules::in_list(rules::SUBKINDS, explicit) {
                    d.shape("BRV-MF-0001", &name, "", "args.subkind", format!("未知 subkind `{explicit}`"), &format!("合法值: {}", rules::list_of(rules::SUBKINDS)));
                    return (d, Vec::new());
                }
                subkind = explicit.clone();
            }
        }
    } else if explicit_subkind.is_some() {
        d.shape(
            "BRV-MF-0001",
            &name,
            "",
            "args.subkind",
            format!("`subkind` 只对 ability 插件有意义, `{plugin_type}` 不接受"),
            "§8.1: subkind 仅 ability 细分",
        );
        return (d, Vec::new());
    }

    // ---- 4. 推荐形态 lint(§8.3; warning, 不阻断) ----
    if !rules::is_recommended_form(&name) {
        d.coded(
            "BRV-TAX-0013",
            crate::diag::Severity::Warning,
            &name,
            "",
            "args.name",
            "插件名不符推荐形态 `<namespace>/<short>`",
            format!("推荐 namespace: {}(§8.3)", rules::list_of(rules::NAMESPACES)),
        );
    }

    // ---- 5. 相位推导 + 计划 ----
    // 说明: `new` 的骨架含 `init` 桩(§8.4 的 src.c: early_init/init/start) ⇒ has_init = true。
    let sub_for_phase = if plugin_type == "ability" {
        Some(subkind.as_str())
    } else {
        None
    };
    let phase = rules::derive_phase(&plugin_type, sub_for_phase, true);
    let facts = PluginFacts {
        name: &name,
        plugin_type: &plugin_type,
        api_type: &api_type,
        subkind: Some(&subkind),
        lang: &lang,
        phase,
        sched_class: "SAFE_PREEMPT",
        symbol_prefix: None,
        version: "0.1.0.0",
        api_rev: 1,
        abi_id: String::new(),
        res_ram_kib: 0,
        res_stack_kib: 0,
        deps: Vec::new(),
        // 骨架期: 生成物数量按"本插件一件"给; 入树后 `brickie gen` 会按插件树重算。
        gen_total: 1,
        // 骨架期: 计划自己会创建 `include/<short>/<short>.h`(要新建的工作区首个头)。
        decl_include: format!(
            "{short}/{short}.h",
            short = rules::name_short(&name)
        ),
        has_init: true,
        emit_selftest: false,
    };
    let vars = plugin_vars(&facts);
    let mut artifacts: Vec<Artifact> = plugin_paths(&facts)
        .into_iter()
        .map(|(path, kind, template)| Artifact {
            path,
            kind,
            template,
            vars: vars.clone(),
        })
        .collect();

    // ---- 6. 冲突判定(两段式的第二段: L5 把扫到的 existing 回传) ----
    let existing = existing_index(context);
    if context.contains_key("existing") {
        apply_conflicts(&mut artifacts, &existing, force, &name, &mut d);
    }
    if d.has_error() {
        return (d, Vec::new());
    }
    (d, artifacts)
}

// ------------------------------------------------------------------ plan-init

/// `plan-init`: `product.toml` + `app/<name>/` 骨架 + `brickie.lock` 初版。
///
/// `brickie.lock` 是**机器文件**(core 拥有内容, contract §4), 因此计划里给
/// `kind = "machine"` 且 `template = ""`; 文本同时放在 `data.lock_content` 里
/// (由 [`crate::emit::render_lock`] 确定性生成)。见 README 的「与 contract 的偏差」。
pub fn plan_init(
    args: &serde_json::Map<String, Value>,
    context: &serde_json::Map<String, Value>,
) -> (Diags, Vec<Artifact>, Option<String>) {
    let mut d = Diags::new();
    let name = args.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string();
    if name.is_empty() {
        d.shape("BRV-MF-0001", "", "", "args.name", "缺少必填参数 `name`", "产品名(§8.2)");
        return (d, Vec::new(), None);
    }
    if !rules::valid_plugin_name(&name) {
        d.shape("BRV-MF-0001", &name, "", "args.name", format!("产品名 `{name}` 不符名字契约"), "契约: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return (d, Vec::new(), None);
    }
    let app = args
        .get("app")
        .and_then(|v| v.as_str())
        .map(str::to_string)
        .unwrap_or_else(|| format!("app/{name}"));
    if !rules::valid_plugin_name(&app) {
        d.shape("BRV-MF-0001", &name, "", "args.app", format!("app 名 `{app}` 不符名字契约"), "§8.3");
        return (d, Vec::new(), None);
    }
    let lang = args.get("lang").and_then(|v| v.as_str()).unwrap_or("c").to_string();
    if !rules::in_list(rules::TEMPLATE_LANGS, &lang) {
        d.usage(format!("v0.1 只交付 `c` 模板, 收到 `{lang}`"), "product 模板在 `templates/product/<lang>/`");
        return (d, Vec::new(), None);
    }

    // product.toml
    let mut pvars: BTreeMap<String, String> = BTreeMap::new();
    pvars.insert("name".into(), name.clone());
    pvars.insert("schema".into(), "1".into());
    pvars.insert("version".into(), "0.1.0.0".into());
    pvars.insert("app".into(), app.clone());
    pvars.insert("PRODUCT_NAME".into(), name.clone());
    pvars.insert("PRODUCT_VERSION".into(), "0.1.0.0".into());
    pvars.insert("APP".into(), app.clone());
    pvars.insert("CORE_RANGE".into(), ">=1.0.0".into());
    pvars.insert("STAGE".into(), "dev".into());
    let mut artifacts = vec![Artifact {
        path: "product.toml".into(),
        kind: "human".into(),
        template: format!("product/{lang}/product.toml.tmpl"),
        vars: pvars,
    }];

    // app/<name>/ 骨架(plugin_type = app)
    let app_facts = PluginFacts {
        name: &app,
        plugin_type: "app",
        api_type: "native",
        subkind: None,
        lang: &lang,
        phase: "app",
        sched_class: "SAFE_PREEMPT",
        symbol_prefix: None,
        version: "0.1.0.0",
        api_rev: 1,
        abi_id: String::new(),
        res_ram_kib: 0,
        res_stack_kib: 0,
        deps: Vec::new(),
        gen_total: 1,
        decl_include: format!("{short}/{short}.h", short = rules::name_short(&app)),
        has_init: true,
        emit_selftest: false,
    };
    let app_vars = plugin_vars(&app_facts);
    for (path, kind, template) in plugin_paths(&app_facts) {
        artifacts.push(Artifact {
            path,
            kind,
            template,
            vars: app_vars.clone(),
        });
    }

    // brickie.lock(机器文件; 内容由 core 给)。形状 = schema/lock.schema.json。
    let lock = crate::model::Lock {
        schema: 1,
        profile: "dev".into(),
        product: Some(crate::model::LockProduct {
            name: name.clone(),
            version: "0.1.0.0".into(),
            stage: "dev".into(),
        }),
        plugins: Vec::new(),
        units: Vec::new(),
    };
    let lock_content = crate::emit::render_lock(&lock);
    let mut lvars: BTreeMap<String, String> = BTreeMap::new();
    lvars.insert("product".into(), name.clone());
    lvars.insert("version".into(), "0.1.0.0".into());
    artifacts.push(Artifact {
        path: "brickie.lock".into(),
        kind: "machine".into(),
        template: String::new(),
        vars: lvars,
    });

    let existing = existing_index(context);
    if context.contains_key("existing") {
        apply_conflicts(&mut artifacts, &existing, false, &name, &mut d);
    }
    if d.has_error() {
        return (d, Vec::new(), None);
    }
    (d, artifacts, Some(lock_content))
}

// ------------------------------------------------------------------ gen-plan

/// `gen-plan`: 重建 `build/gen/**` 的计划(描述符)。
///
/// v0.1 的 `include/<short>/<short>.h` 是**人写**文件(§8.4), 因此这里只出描述符;
/// 把"头文件也生成"留给 v0.3(见 README 的偏差表)。
pub fn gen_plan(
    root: &std::path::Path,
    args: &serde_json::Map<String, Value>,
) -> (Diags, Vec<Artifact>) {
    let only = args.get("plugin").and_then(|v| v.as_str()).map(str::to_string);
    let tree = crate::model::load_tree(root);
    let mut d = tree.diags.clone();
    let mut out = Vec::new();
    // 生成物数量 = **会被编进镜像的**描述符数。每个生成物都带同一个值, 于是弱符号
    // `br_plugin_gen_total` 任取一条都一致(见 ADR-0005 §2.4)。
    //
    // ★ 为什么是**闭包**而不是"插件树里的插件数": core 的 TC-PLUG-001 断言
    //   "`.br_plugins` 段里的条数 == 本符号的值"。而 build 会**按闭包过滤生成物**
    //   (见 build.rs 的 filter_gen_sources: 未选中的插件, 它的描述符不进镜像),
    //   于是"树里的插件数"在有**未选中插件**的树里必然大于段条数 —— 那个断言会红。
    //   生产里第一次撞上它是 sched/coop 与 sched/rr 共存、而产品只能选一个调度器
    //   (ADR-0011)。⇒ 口径改为闭包大小: 它才是"镜像里有几条描述符"的真值。
    //   没有 product.toml 的树(纯插件树校验, R-1)退回整棵树。
    let gen_total = match &tree.product {
        Some(prod) => {
            let sol = crate::solver::solve(&tree, &prod.stage, false);
            sol.closure.selected.len() as u32
        }
        None => tree.plugins.len() as u32,
    };
    for p in &tree.plugins {
        if let Some(name) = &only {
            if &p.name != name {
                continue;
            }
        }
        let mut facts = facts_of(p);
        facts.gen_total = gen_total;
        // 自检开关(ADR-0010 §2.3): 生成期裁决 —— 关掉时描述符写 BR_PLUGIN_NO_HOOK,
        // 于是"关掉自检"= 测试代码**不进镜像**, 而不只是"不跑"。
        // 没有 product.toml 的树(纯插件树校验/R-1)按"不发"处理: 那是不完整的组合。
        facts.emit_selftest = p.has_selftest
            && tree
                .product
                .as_ref()
                .map(|prod| prod.selftest_for(&p.name))
                .unwrap_or(false);
        let symbols: Vec<String> = p
            .exports
            .iter()
            .flat_map(|e| e.entries.iter().map(|x| x.name.clone()))
            .collect();
        facts.decl_include = scan_decl_include(root, &p.name, &symbols)
            .unwrap_or_else(|| FALLBACK_DECL_INCLUDE.to_string());
        out.push(Artifact {
            path: format!("build/gen/{}/plugin_desc.c", p.name),
            kind: "generated".into(),
            template: format!("descriptor/{}/{}/plugin_desc.c.tmpl", p.api_type, p.lang),
            vars: plugin_vars(&facts),
        });
    }
    if let Some(name) = &only {
        if !tree.plugins.iter().any(|p| &p.name == name) {
            d.usage(format!("插件树里找不到插件 `{name}`"), "`gen-plan` 的 `plugin` 参数可选");
        }
    }
    (d, out)
}

/// 从模型插件构造 [`PluginFacts`]。
///
/// `gen_total` / `decl_include` 是**计划级**事实(要扫盘/数插件树), 由调用方填:
/// `gen_plan` 用真值, `plan_new` / `plan_init` 用骨架期的预测值。
pub fn facts_of(p: &Plugin) -> PluginFacts<'_> {
    let (ram, stack) = p.res_totals();
    PluginFacts {
        name: &p.name,
        plugin_type: &p.plugin_type,
        api_type: &p.api_type,
        subkind: p.subkind.as_deref(),
        lang: &p.lang,
        phase: &p.phase,
        sched_class: &p.sched_class,
        symbol_prefix: p.symbol_prefix.clone(),
        version: &p.version_raw,
        api_rev: p.compat.api_rev.unwrap_or(1),
        abi_id: p.compat.abi_id.clone().unwrap_or_default(),
        res_ram_kib: ram,
        res_stack_kib: stack,
        deps: p
            .deps
            .iter()
            .map(|d| DepFact {
                name: d.name.clone(),
                kind: d.kind.clone(),
                phase: d.phase.clone(),
            })
            .collect(),
        gen_total: 0,
        decl_include: String::new(),
        has_init: p.phase != "early",
        // 缺省**不发**: 只有 gen_plan(知道产品开关)会把它打开。
        // 这样 plan_new/plan_init 的骨架期预测也自然是"不带自检"。
        emit_selftest: false,
    }
}

/// 把计划渲染成 `data` 载荷。
pub fn artifacts_data(artifacts: &[Artifact]) -> Value {
    let arr: Vec<Value> = artifacts.iter().map(|a| a.to_json()).collect();
    let planned: Vec<Value> = artifacts
        .iter()
        .map(|a| json!({"path": a.path, "kind": a.kind}))
        .collect();
    json!({"artifacts": arr, "planned": planned, "count": artifacts.len()})
}

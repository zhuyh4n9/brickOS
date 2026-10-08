//! `dep-add` / `dep-rm` 的**片段生成**(裁定 R-2)。
//!
//! R-2 取 §9.1.1 规则 1(后出的显式决策): **不写 `plugin.toml`**, 只打印可直接粘进
//! 声明面的 TOML 片段(`data.snippet`)与定位(`data.span`)。写库会抹掉人写注释且违反
//! "唯一真值"; 切换成本 = 将来加 `--write`。
//!
//! 旗标纪律(§7.7.1): `dep` 族的旗标只描述**结构依赖** —— `--kind` 决定边类型,
//! `--phase` 是 `kind = "init"` 的相位断言;**没有** `--compat-gen` / `--frozen-gen`
//! (`[[dep]]` 一律不钉代, 依 F3)。

use serde_json::{json, Value};

use crate::diag::Diags;
use crate::model::TreeLoad;
use crate::rules;
use crate::version::Range;

/// 生成结果。
#[derive(Clone, Debug, Default)]
pub struct DepSnippet {
    /// 目标 `plugin.toml`(相对 root)。
    pub path: String,
    /// TOML 路径定位, 如 `dep[2]`。
    pub span: String,
    /// 可直接粘贴的 TOML 片段。
    pub snippet: String,
    /// `dep-rm` 是否真的定位到了块。
    pub found: bool,
}

impl DepSnippet {
    pub fn to_data(&self) -> Value {
        json!({
            "snippet": self.snippet,
            "path": self.path,
            "span": self.span,
            "found": self.found,
        })
    }
}

fn render_dep_block(name: &str, range: &str, kind: &str, phase: Option<&str>, symbol: Option<&str>) -> String {
    let mut out = String::from("[[dep]]\n");
    out.push_str(&format!("name  = {}\n", crate::emit::toml_string(name)));
    out.push_str(&format!("range = {}\n", crate::emit::toml_string(range)));
    out.push_str(&format!("kind  = {}\n", crate::emit::toml_string(kind)));
    if let Some(p) = phase {
        out.push_str(&format!("phase = {}\n", crate::emit::toml_string(p)));
    }
    if let Some(s) = symbol {
        out.push_str(&format!("symbol = {}\n", crate::emit::toml_string(s)));
    }
    out
}

/// `dep-add`: `{plugin, dep, range?, kind, phase?}` → `data.snippet`。
pub fn dep_add(tree: &TreeLoad, args: &serde_json::Map<String, Value>) -> (Diags, Option<DepSnippet>) {
    let mut d = Diags::new();
    let plugin = args.get("plugin").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let dep = args.get("dep").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let kind = args
        .get("kind")
        .and_then(|v| v.as_str())
        .unwrap_or("init")
        .to_string();
    let phase = args
        .get("phase")
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .map(str::to_string);
    let range = args
        .get("range")
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .unwrap_or("*")
        .to_string();

    if plugin.is_empty() {
        d.shape("BRV-MF-0001", "", "", "args.plugin", "缺少必填参数 `plugin`", "§7.7");
        return (d, None);
    }
    if dep.is_empty() {
        d.shape("BRV-MF-0001", &plugin, "", "args.dep", "缺少必填参数 `dep`", "§7.7");
        return (d, None);
    }
    if !rules::valid_plugin_name(&dep) {
        d.shape("BRV-MF-0001", &plugin, "", "args.dep", format!("依赖名 `{dep}` 不符名字契约"), "契约: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return (d, None);
    }
    if !rules::in_list(rules::DEP_KINDS, &kind) {
        d.shape("BRV-MF-0001", &plugin, "", "args.kind", format!("未知依赖类别 `{kind}`"), &format!("合法值: {}(§7.1)", rules::list_of(rules::DEP_KINDS)));
        return (d, None);
    }
    if let Some(p) = &phase {
        if !rules::in_list(rules::PHASES, p) {
            d.shape("BRV-MF-0001", &plugin, "", "args.phase", format!("未知 phase `{p}`"), &format!("合法值: {}", rules::list_of(rules::PHASES)));
            return (d, None);
        }
        if kind != "init" {
            d.shape("BRV-MF-0001", &plugin, "", "args.phase", "`phase` 只对 `kind = \"init\"` 有意义", "§7.2: 断言只描述 init 完成点");
            return (d, None);
        }
    }
    if let Err(e) = Range::parse(&range) {
        d.shape(e.code, &plugin, "", "args.range", e.message, "§7.4: range 固定 3 段(4 段非法)");
        return (d, None);
    }

    // 目标插件必须在树里(否则片段无处可粘)。
    let Some(target) = tree.by_name(&plugin) else {
        d.usage(
            format!("插件树里找不到插件 `{plugin}`"),
            "先在仓库根跑 `brickie new` 或确认 `root` 指向插件树",
        );
        return (d, None);
    };

    // 注: "该依赖已声明"不做诊断 —— BRV-D8 没有为"重复声明"分配码, 而按裁定 R-9
    // 借用 `BRV-MF-0001`(形状错)会把一次良性的重复提示升成退出码 2。已登记为缺口。

    let snippet = format!(
        "# 粘贴到 {} 的 `[[dep]]` 区(裁定 R-2: brickie 不改写人写文件)\n{}",
        target.file,
        render_dep_block(&dep, &range, &kind, phase.as_deref(), None)
    );
    (
        d,
        Some(DepSnippet {
            path: target.file.clone(),
            span: format!("dep[{}]", target.deps.len()),
            snippet,
            found: true,
        }),
    )
}

/// `dep-rm`: `{plugin, dep, kind?}` → 待删除块的**定位** + 还原片段。
pub fn dep_rm(tree: &TreeLoad, args: &serde_json::Map<String, Value>) -> (Diags, Option<DepSnippet>) {
    let mut d = Diags::new();
    let plugin = args.get("plugin").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let dep = args.get("dep").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let kind = args.get("kind").and_then(|v| v.as_str()).map(str::to_string);

    if plugin.is_empty() || dep.is_empty() {
        d.shape("BRV-MF-0001", &plugin, "", "args", "`dep-rm` 需要 `plugin` 与 `dep` 两个参数", "§7.7");
        return (d, None);
    }
    if let Some(k) = &kind {
        if !rules::in_list(rules::DEP_KINDS, k) {
            d.shape("BRV-MF-0001", &plugin, "", "args.kind", format!("未知依赖类别 `{k}`"), &format!("合法值: {}", rules::list_of(rules::DEP_KINDS)));
            return (d, None);
        }
    }
    let Some(target) = tree.by_name(&plugin) else {
        d.usage(format!("插件树里找不到插件 `{plugin}`"), "确认 `root` 指向插件树");
        return (d, None);
    };
    let idx = target.deps.iter().position(|x| {
        x.name == dep && kind.as_deref().map(|k| k == x.kind).unwrap_or(true)
    });
    match idx {
        None => {
            let snippet = format!(
                "# 未找到: {} 里没有对 `{}`{} 的 `[[dep]]` 块\n",
                target.file,
                dep,
                kind.as_deref().map(|k| format!("(kind = {k})")).unwrap_or_default()
            );
            (
                d,
                Some(DepSnippet {
                    path: target.file.clone(),
                    span: String::new(),
                    snippet,
                    found: false,
                }),
            )
        }
        Some(i) => {
            let x = &target.deps[i];
            let snippet = format!(
                "# 删除 {} 中的 `dep[{i}]` 块(裁定 R-2: 只定位, 不改写人写文件)\n{}",
                target.file,
                render_dep_block(
                    &x.name,
                    &x.range_raw,
                    &x.kind,
                    x.phase.as_deref(),
                    x.symbol.as_deref(),
                )
            );
            (
                d,
                Some(DepSnippet {
                    path: target.file.clone(),
                    span: format!("dep[{i}]"),
                    snippet,
                    found: true,
                }),
            )
        }
    }
}

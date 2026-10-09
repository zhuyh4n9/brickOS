//! `brickie build` — **声明面驱动的构建编排**(core 侧: 出计划 + 判定)。
//!
//! ## 为什么在 core
//!
//! 设计 `2-02` §1 把"插件发现 / manifest 解析 / 闭包求解 / 拓扑与环检测 / 生成物 /
//! **构建编排**"全部划给 brickie; `Design/docs/decisions/0003`(构建归属转移)把它
//! 排成 S1–S3 三个台阶。本模块是那张表的落地:
//!
//! * **哪些源码进镜像** ← `product.toml [select]` 的闭包(求解器)+ 每个插件的
//!   `[build].sources`(声明面)—— 不再是顶层 `Makefile` 里的 `wildcard` 字面路径;
//! * **怎么编** ← `[build.target]`(platform 插件: arch/triple/链接脚本/QEMU 型号)+
//!   `product.toml [build]`(标志/产物落点);
//! * **跑什么门禁** ← `tests/gates.toml`(人写: 宿主用例 / QEMU 日志判据 / 脚本门禁)。
//!
//! L5(Python)只做"执行 + 落盘 + 呈现": 它拿到的 `data.steps[i].argv` 是**具体命令**,
//! 不含任何判定。日志判据(`judge`)由 core 按 `tests/gates.toml` 的正则判 —— 于是
//! "红绿"只有一处真值, `--json` 与文本是同一 `data` 的两种序列化。
//!
//! ## 三件事的归属(裁定 S-B1…S-B6, 逐条见 `rust/README.md` §10)
//!
//! | 事 | 归属 | 理由 |
//! |---|---|---|
//! | 工具**候选序**(`gcc` → `gcc-16` → …)与**解析结果** | core | 生成物(make/ninja 后端文件)必须把解析结果烧进去; L5 只执行 argv |
//! | 增量判定(`stamp` = argv + 输入(size,mtime)) | core | 纯函数, 可用 `context.files` 注入 ⇒ 自检可复现 |
//! | 进程编排(并发 / 超时 / 收日志) | L5 | §8 的"调子进程"; core 不碰时钟与并发 |
//!
//! ## 与设计的分歧(登记 `docs/contract.md` §9)
//!
//! * `brickie-v0.1.md` §0 的边界纪律写"v0.1 任何命令不得要求 cc/cargo/nm 在场"。
//!   本命令族**故意违反**这一条 —— 它就是"要求交叉编译器在场"的命令。纪律的准确
//!   表述应为"**组合期**命令不要求编译器在场"(见 ADR-0004 §偏离登记)。
//! * `plugin.toml [build]` 在 `brickie-v0.1.md` §7.1 注为"v0.1 只记录; v0.3 起被消费"
//!   —— 本刀**提前消费**它(ADR-0004 的 S1–S3), 并新增两个字段(`defines`,
//!   `[build.target]` 整表), 已在 contract §7.1 补齐。

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::diag::{Diags, Severity, EXIT_OK, EXIT_RED};
use crate::model::{self, Plugin, ProductBuild, TreeLoad};
use crate::proto::{Request, Response};
use crate::solver;

// ------------------------------------------------------------------ 诊断码
//
// `BRV-D8` 的编码表没有 BUILD 族 ⇒ 本刀新开一族, 逐条登记在 `docs/contract.md` §9
// 的 R-13/标 与 `rust/README.md` §10(设计侧回灌项: `BRV-D8` 需要 BUILD 族)。

/// `product.toml` 缺 `[build]`(或整个 product 缺席)。
pub const CODE_NO_BUILD: &str = "BRV-BLD-0001";
/// `[build].sources` 展开后为空(声明了却不匹配任何文件)。
pub const CODE_EMPTY_SOURCES: &str = "BRV-BLD-0002";
/// 没有 platform 插件声明 `[build.target]`(无从得知 arch/链接脚本)。
pub const CODE_NO_TARGET: &str = "BRV-BLD-0003";
/// 多个 platform 插件声明 `[build.target]`(谁是真值不明)。
pub const CODE_TARGET_AMBIGUOUS: &str = "BRV-BLD-0004";
/// 工具解析失败(交叉编译器 / objcopy / qemu 不在场)。**环境错**(退出码 2)。
pub const CODE_TOOL_MISSING: &str = "BRV-BLD-0005";
/// 闭包内的插件没有 `[build]`(它会被组合, 却没说怎么编)。
pub const CODE_UNIT_NO_BUILD: &str = "BRV-BLD-0006";
/// `[build].sources` 里的**字面**路径不存在(通配不匹配不算错, 见 0002)。
pub const CODE_MISSING_SOURCE: &str = "BRV-BLD-0007";
/// `tests/gates.toml` 里没有这个名字的门禁(或整文件缺席)。
pub const CODE_GATE_UNKNOWN: &str = "BRV-BLD-0008";
/// 门禁判红(日志缺 require / 命中 forbid / 缺 PASS 用例 tag)。
pub const CODE_GATE_FAILED: &str = "BRV-BLD-0009";
/// `[build]` 的形状问题(产物落点 / 标志表非法)。
pub const CODE_BUILD_SHAPE: &str = "BRV-BLD-0010";
/// 执行的步骤失败(编译/链接/脚本非零退出) —— L5 检出, **码由 core 给**(见 `Step::to_json`)。
pub const CODE_STEP_FAILED: &str = "BRV-BLD-0011";
/// `tests/gates.toml` 里的正则超出 core 支持的**子集**(写了就报, 不静默不匹配)。
pub const CODE_GATE_SHAPE: &str = "BRV-BLD-0012";
/// 生成物属于**不在闭包内**的插件 ⇒ 已跳过(info: 不是缺陷, 但必须看得见)。
pub const CODE_GEN_NOT_SELECTED: &str = "BRV-BLD-0013";

// ------------------------------------------------------------------ 入口

/// build 命令族的 dispatch(由 `main.rs` 转发)。
pub fn run(req: &Request) -> Response {
    match req.command.as_str() {
        "build" => cmd_build(req),
        "clean" => cmd_clean(req),
        "run" => cmd_run(req),
        "size" => cmd_size(req),
        "disasm" => cmd_disasm(req),
        "test" => cmd_test(req),
        "judge" => cmd_judge(req),
        other => {
            let mut d = Diags::new();
            d.usage(
                format!("build 族里没有命令 `{other}`"),
                "见 docs/contract.md §5.4 的 build 命令族",
            );
            Response::ok(d, json!({}))
        }
    }
}

// ================================================================== 规划器

/// 一个编译单元(核心本体 / 插件 / 生成物)。
#[derive(Clone, Debug, Default)]
struct Unit {
    /// 展示名: 插件名(`service/trace`)或 `core` / `build/gen(生成物)`。
    name: String,
    /// 相对 root 的单元目录(`""` = 仓库根, 核心与生成物用)。
    dir: String,
    /// 相对 root 的源文件(通配已展开, 字典序)。
    sources: Vec<String>,
    /// 相对 root 的 include 目录。
    includes: Vec<String>,
    defines: Vec<String>,
    plugin: Option<String>,
}

/// 目标事实(来自 platform 插件的 `[build.target]`)。
#[derive(Clone, Debug, Default)]
struct Target {
    arch: String,
    cross: String,
    arch_flags: Vec<String>,
    linker_script: Option<String>,
    qemu_binary: String,
    qemu_machine: String,
    qemu_cpu: String,
    qemu_memory: String,
    qemu_extra: Vec<String>,
    owner: String,
}

/// 一条待执行命令。
#[derive(Clone, Debug, Default)]
struct Step {
    label: String,
    /// `compile` / `link` / `objcopy` / `run` / `script` / `hosttest` / `hostrun` / `backend`
    kind: String,
    argv: Vec<String>,
    /// 同一 `group` 的步骤可并发(L5 的编排口径)。
    group: u32,
    /// 相对 root 的日志落点(`None` = 直接继承 stdio)。
    log: Option<String>,
    timeout_s: Option<u64>,
    /// 该步骤**预期**以超时结束(QEMU 门禁: 镜像本来就跑到被掐)。
    expect_timeout: bool,
    /// 需要 core 判定的门禁名(见 `cmd_judge`)。
    judge: Option<String>,
    /// 交给 stdout 的行数上限(展开 `--head`)。
    stdout_lines: Option<u64>,
    /// 该步骤写出的文件(增量状态与清理用)。
    outputs: Vec<String>,
    /// 该步骤**消费**的、由本计划里其它步骤产出的文件。
    ///
    /// 用途只有一个: 上游步骤失败时, L5 要能**跳过**下游而不是拿上一次的旧产物
    /// 去跑(旧对象也能链接成功 ⇒ 日志会打印 "ok ld" 而实际产物是陈旧的 —— 误导)。
    consumes: Vec<String>,
}

impl Step {
    fn to_json(&self) -> Value {
        json!({
            "label": self.label,
            "kind": self.kind,
            "argv": self.argv,
            "group": self.group,
            "log": self.log,
            "timeout_s": self.timeout_s,
            "expect_timeout": self.expect_timeout,
            "judge": self.judge,
            "stdout_lines": self.stdout_lines,
            "outputs": self.outputs,
            "consumes": self.consumes,
            // 失败码随步骤一起给 L5: "码"只有一处真值(contract §3), L5 不自己编。
            "fail_code": CODE_STEP_FAILED,
            "tool_code": CODE_TOOL_MISSING,
        })
    }
}

/// 解析好的构建配置 + 静态计划。
struct Ctx {
    root: PathBuf,
    profile: String,
    backend: String,
    tag: String,
    units: Vec<Unit>,
    target: Target,
    pb: ProductBuild,
    obj_dir: String,
    elf: String,
    bin: String,
    map: String,
    state_path: String,
    gen_dir: String,
    cflags: Vec<String>,
    asflags: Vec<String>,
    ldflags: Vec<String>,
    /// 工具名 → 候选(按序)。
    tools: BTreeMap<String, Vec<String>>,
    /// 工具名 → 解析结果(绝对路径或裸名)。
    resolved: BTreeMap<String, String>,
    /// tool 名 → 它是什么(呈现用)。
    tool_what: BTreeMap<String, String>,
    /// `context.files[path] = (size, mtime_ns)`; `Some` 时**不碰磁盘**(自检用)。
    stat_override: Option<BTreeMap<String, (u64, i64)>>,
    force: bool,
}

// ------------------------------------------------------------------ 小工具

fn hex_of(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// `sha256:<64 hex>`。
fn sha256_tag(parts: &[String]) -> String {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p.as_bytes());
        h.update([0u8]);
    }
    format!("sha256:{}", hex_of(&h.finalize()))
}

/// 简单的通配匹配: `*`(不跨 `/`)与 `?`。
fn glob_match(pat: &str, name: &str) -> bool {
    let p: Vec<char> = pat.chars().collect();
    let n: Vec<char> = name.chars().collect();
    // 迭代式双指针(带回溯点), 避免递归爆栈。
    let (mut pi, mut ni) = (0usize, 0usize);
    let (mut star, mut mark) = (usize::MAX, 0usize);
    while ni < n.len() {
        if pi < p.len() && (p[pi] == '?' || p[pi] == n[ni]) {
            pi += 1;
            ni += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = pi;
            mark = ni;
            pi += 1;
        } else if star != usize::MAX {
            pi = star + 1;
            mark += 1;
            ni = mark;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

/// 有没有通配符(决定"不匹配"是错(字面)还是空(通配))。
fn has_wildcard(pat: &str) -> bool {
    pat.contains('*') || pat.contains('?')
}

/// 展开一个相对 `base_dir` 的通配, 返回**相对 root** 的文件路径(字典序)。
///
/// 支持 `*`/`?`(**不跨 `/`**)与 `**`(跨任意层目录; `**` 可以匹配**零层**)。
/// 为什么需要 `**`: 生成物路径的层级 = 插件名的命名空间深度(`build/gen/service/dump/...`
/// 是两层, `build/gen/foo/...` 是一层)。写 `build/gen/*/*/plugin_desc.c` 就会把
/// "工具能否发现生成物"绑死在"插件名恰好两段"上 —— 那是巧合, 不是契约。
fn expand_glob(root: &Path, base_dir: &str, pattern: &str) -> Vec<String> {
    let base = if base_dir.is_empty() {
        root.to_path_buf()
    } else {
        root.join(base_dir)
    };
    let comps: Vec<&str> = pattern.split('/').filter(|c| !c.is_empty()).collect();
    let mut abs: Vec<PathBuf> = Vec::new();
    glob_walk(&base, &comps, &mut abs);
    abs.sort();
    abs.dedup();
    let mut out: Vec<String> = Vec::new();
    for p in abs {
        if let Ok(rel) = p.strip_prefix(root) {
            out.push(rel.to_string_lossy().replace('\\', "/"));
        }
    }
    out.sort();
    out.dedup();
    out
}

/// 递归通配匹配的一层: `comps` 是**剩余**的模式分量。
fn glob_walk(dir: &Path, comps: &[&str], out: &mut Vec<PathBuf>) {
    let Some(comp) = comps.first().copied() else {
        return;
    };
    let rest = &comps[1..];
    if comp == "**" {
        // `**` 匹配零层 ⇒ 在原地继续用 rest; 也匹配一层以上 ⇒ 下钻且**保留 `**`**。
        glob_walk(dir, rest, out);
        let mut subs: Vec<PathBuf> = match fs::read_dir(dir) {
            Ok(entries) => entries
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect(),
            Err(_) => Vec::new(),
        };
        subs.sort();
        for sub in subs {
            glob_walk(&sub, comps, out);
        }
        return;
    }
    let last = rest.is_empty();
    let mut hits: Vec<PathBuf> = Vec::new();
    if has_wildcard(comp) {
        if let Ok(entries) = fs::read_dir(dir) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if glob_match(comp, &name) {
                    hits.push(entry.path());
                }
            }
        }
    } else {
        let path = dir.join(comp);
        if path.exists() {
            hits.push(path);
        }
    }
    hits.sort();
    for path in hits {
        if last {
            if path.is_file() {
                out.push(path);
            }
        } else if path.is_dir() {
            glob_walk(&path, rest, out);
        }
    }
}

/// 把插件名变成相对仓库根的插件目录(布局约定 `4-02`: 顶层目录 = namespace)。
fn plugin_dir_of(name: &str) -> String {
    name.to_string()
}

/// 解析 `.d` 文件(make 语法: `target: dep dep …`, 支持 `\` 续行)。
fn parse_depfile(path: &Path, root: &Path) -> Vec<String> {
    let text = match fs::read_to_string(path) {
        Ok(t) => t,
        Err(_) => return Vec::new(),
    };
    let joined = text.replace("\\\n", " ");
    let mut out: Vec<String> = Vec::new();
    for line in joined.lines() {
        let Some((_, rhs)) = line.split_once(':') else {
            continue;
        };
        for tok in rhs.split_whitespace() {
            if tok.is_empty() {
                continue;
            }
            // `.d` 里的路径是**相对于编译时 cwd**(= 仓库根)的, 直接可用。
            let rel = tok.replace('\\', "/");
            if rel.starts_with('/') {
                if let Ok(p) = Path::new(&rel).strip_prefix(root) {
                    out.push(p.to_string_lossy().replace('\\', "/"));
                }
            } else {
                out.push(rel);
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

// ================================================================== 配置装载

/// `[build]` 的缺省产物落点(与顶层 Makefile 的历史口径一致)。
fn default_obj_dir() -> String {
    "build/obj".to_string()
}

fn load_ctx(req: &Request, tree: &TreeLoad) -> Result<(Ctx, Diags), Diags> {
    let mut d = Diags::new();
    let root = PathBuf::from(&req.root);

    let Some(prod) = tree.product.clone() else {
        d.shape(
            CODE_NO_BUILD,
            "",
            "product.toml",
            "",
            "没有 `product.toml`: 构建需要产品级声明(选哪些插件 + 怎么编)",
            "先 `brickie init <product>` 或补上 product.toml; contract §7.2",
        );
        return Err(d);
    };
    let Some(pb) = prod.build.clone() else {
        d.shape(
            CODE_NO_BUILD,
            &prod.name,
            "product.toml",
            "build",
            "`product.toml` 缺 `[build]`(标志表与产物落点)",
            "补 `[build]`: cflags/asflags/ldflags + 产物落点; 见 contract §7.2",
        );
        return Err(d);
    };

    // ---- profile: `--profile` 覆盖 `[product].stage`(§7.5) ----
    let profile = req
        .arg_str("profile")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| prod.stage.clone());
    if !matches!(profile.as_str(), "dev" | "release") {
        d.usage(
            format!("`--profile` 只能是 dev | release(收到 {profile})"),
            "BRV-D9 退出码 2",
        );
        return Err(d);
    }

    let backend = req
        .arg_str("backend")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "direct".to_string());
    if !matches!(backend.as_str(), "direct" | "make" | "ninja") {
        d.usage(
            format!("`--backend` 只能是 direct | make | ninja(收到 {backend})"),
            "BRV-D9 退出码 2",
        );
        return Err(d);
    }

    let tag = req.arg_str("tag").unwrap_or_default();
    let tag_suffix = if tag.is_empty() {
        String::new()
    } else {
        format!("-{tag}")
    };

    // ---- 目标事实: 恰一个 platform 声明 `[build.target]` ----
    let owners: Vec<&Plugin> = tree
        .plugins
        .iter()
        .filter(|p| p.build_target.is_some())
        .collect();
    let mut target = Target {
        arch: String::new(),
        cross: "aarch64-linux-gnu-".to_string(),
        arch_flags: Vec::new(),
        linker_script: None,
        qemu_binary: "qemu-system-aarch64".to_string(),
        qemu_machine: "virt".to_string(),
        qemu_cpu: String::new(),
        qemu_memory: "128M".to_string(),
        qemu_extra: Vec::new(),
        owner: String::new(),
    };
    match owners.len() {
        0 => {
            d.shape(
                CODE_NO_TARGET,
                "",
                "product.toml",
                "",
                "没有插件声明 `[build.target]`: 无从得知 arch / 交叉前缀 / 链接脚本",
                "platform 插件补 `[build.target]`(每镜像恰一个 platform, §8.3 ④)",
            );
            return Err(d);
        }
        1 => {
            let p = owners[0];
            let t = p.build_target.clone().unwrap_or_default();
            target.arch = t.arch.clone();
            if !t.cross.is_empty() {
                target.cross = t.cross.clone();
            }
            target.arch_flags = t.arch_flags.clone();
            target.linker_script = if t.linker_script.is_empty() {
                None
            } else {
                Some(format!("{}/{}", plugin_dir_of(&p.name), t.linker_script))
            };
            if !t.qemu_binary.is_empty() {
                target.qemu_binary = t.qemu_binary.clone();
            }
            if !t.qemu_machine.is_empty() {
                target.qemu_machine = t.qemu_machine.clone();
            }
            target.qemu_cpu = t.qemu_cpu.clone();
            if !t.qemu_memory.is_empty() {
                target.qemu_memory = t.qemu_memory.clone();
            }
            target.qemu_extra = t.qemu_extra.clone();
            target.owner = p.name.clone();
        }
        _ => {
            d.shape(
                CODE_TARGET_AMBIGUOUS,
                "",
                "product.toml",
                "",
                format!(
                    "有 {} 个插件声明了 `[build.target]`: {}",
                    owners.len(),
                    owners
                        .iter()
                        .map(|p| p.name.clone())
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
                "每镜像恰一个 platform(§8.3 ④); 其余插件的编译事实进 `[build]`",
            );
            return Err(d);
        }
    }

    // ---- 闭包: 选中的单元 = 核心本体 + 闭包内插件 + 生成物 ----
    let sol = solver::solve(tree, &profile, false);
    let closure = sol.closure.clone();
    let mut diags = sol.into_diags();
    if diags.has_error() {
        diags.merge(d);
        return Err(diags);
    }
    let mut units: Vec<Unit> = Vec::new();

    // 自检总开关(ADR-0010 §2.3)的**唯一**读取点(`Product::selftest_on` 是唯一裁决函数):
    // 它同时决定 core 自检源要不要编、以及生成物里发不发 selftest 钩子(后者在 gen_plan 里)。
    let selftest_on = tree
        .product
        .as_ref()
        .map(|p| p.selftest_on())
        .unwrap_or(false);

    // (a) 核心本体(**不是插件**, `[compat].core` 引用它)
    // core 的自检套件(`core/selftest/**`)只在产品开关放行时编进来(ADR-0010 §2.3):
    // 关掉自检 ⇒ 这些文件**不参与编译** ⇒ `br_core_selftest()` 缺席 ⇒ plugin_mgr 的
    // 弱引用读到 NULL 就跳过整个 core 侧自检。这就是"关掉 = 代码不进镜像"的落地方式。
    let mut core_sources =
        expand_many(&root, "", &pb.core_sources, "core", "product.toml", "build.core_sources", &mut diags);
    if selftest_on {
        core_sources.extend(expand_many(
            &root,
            "",
            &pb.selftest_sources,
            "core/selftest",
            "product.toml",
            "build.selftest_sources",
            &mut diags,
        ));
    }
    units.push(Unit {
        name: "core".to_string(),
        dir: String::new(),
        sources: core_sources,
        includes: pb.core_includes.clone(),
        defines: Vec::new(),
        plugin: None,
    });

    // (b) 闭包内的插件(按拓扑序: init 在前, 让链接顺序也可复现)
    let mut ordered: Vec<String> = closure.topo_order.clone();
    for name in &closure.selected {
        if !ordered.contains(name) {
            ordered.push(name.clone());
        }
    }
    ordered.retain(|n| closure.selected.contains(n));
    for name in &ordered {
        let Some(p) = tree.by_name(name) else { continue };
        let dir = plugin_dir_of(&p.name);
        let b = p.build.clone().unwrap_or_default();
        if b.sources.is_empty() {
            diags.shape(
                CODE_UNIT_NO_BUILD,
                &p.name,
                format!("{dir}/plugin.toml"),
                "build.sources",
                format!("闭包内的插件 `{}` 没有 `[build].sources`: 它会被组合, 却没说怎么编", p.name),
                "补 `[build] sources/includes`; contract §7.1",
            );
            continue;
        }
        let sources = expand_many(&root, &dir, &b.sources, &p.name, &format!("{dir}/plugin.toml"), "build.sources", &mut diags);
        units.push(Unit {
            name: p.name.clone(),
            dir: dir.clone(),
            sources,
            includes: b
                .includes
                .iter()
                .map(|i| format!("{dir}/{i}"))
                .collect(),
            defines: b.defines.clone(),
            plugin: Some(p.name.clone()),
        });
    }

    // (c) 生成物(`build/gen/**`): 由 `brickie gen` 产出, 声明面在 `product.toml`
    if !pb.gen_sources.is_empty() {
        let sources = expand_many(&root, "", &pb.gen_sources, "build/gen", "product.toml", "build.gen_sources", &mut diags);
        // **按闭包过滤**: 生成物属于某个插件(路径 = `build/gen/<插件名>/…`), 而镜像只
        // 由闭包内的插件组成。不过滤的话, "插件树里有、但没被产品选中"的插件, 它的
        // 描述符会被编进镜像, 而它的 .c 不会 ⇒ **链接期未定义符号**(F1 实测踩到)。
        // 这是"声明面有第二处真值"的典型: 生成物的 glob 与 `[select]` 必须由工具对齐。
        let (sources, skipped) = filter_gen_sources(&sources, tree, &closure.selected);
        for path in &skipped {
            diags.push(
                crate::diag::Diag::new(
                    Severity::Info,
                    format!("生成物 `{path}` 属于不在闭包内的插件, 已跳过(不进镜像)"),
                )
                .code(CODE_GEN_NOT_SELECTED)
                .file("product.toml")
                .span("build.gen_sources")
                .hint("把该插件加进 [select].plugins, 或从生成物 glob 里排除它"),
            );
        }
        if !sources.is_empty() {
            units.push(Unit {
                name: "build/gen".to_string(),
                dir: String::new(),
                sources,
                // 生成物 include 自己的插件目录(模板里 `#include "qemu-aarch64/qemu-aarch64.h"`)
                includes: tree
                    .plugins
                    .iter()
                    .filter(|p| closure.selected.contains(&p.name))
                    .map(|p| format!("{}/{}", plugin_dir_of(&p.name), "include"))
                    .collect(),
                defines: Vec::new(),
                plugin: None,
            });
        }
    }

    // ---- 标志 ----
    let cflags = if profile == "release" {
        let mut v = pb.cflags.clone();
        v.extend(pb.release_cflags_extra.clone());
        v
    } else {
        pb.cflags.clone()
    };
    let asflags = if profile == "release" {
        let mut v = pb.asflags.clone();
        v.extend(pb.release_asflags_extra.clone());
        v
    } else {
        pb.asflags.clone()
    };
    let ldflags = if profile == "release" {
        let mut v = pb.ldflags.clone();
        v.extend(pb.release_ldflags_extra.clone());
        v
    } else {
        pb.ldflags.clone()
    };
    if cflags.is_empty() {
        diags.shape(
            CODE_BUILD_SHAPE,
            &prod.name,
            "product.toml",
            "build.cflags",
            "`[build].cflags` 为空: 编译选项必须由产品声明(工具不自带缺省策略)",
            "把 `-std=c11`/`-O2`/`-ffreestanding` 等显式写进 `[build].cflags`",
        );
    }

    let obj_dir = pb.obj_dir.clone().unwrap_or_else(default_obj_dir);
    let elf = format!("build/brick{tag_suffix}.elf");
    let bin = format!("build/brick{tag_suffix}.bin");
    let map = format!("build/brick{tag_suffix}.map");
    let state_path = if tag.is_empty() {
        "build/gen/build-state.json".to_string()
    } else {
        format!("build/gen/build-state{tag_suffix}.json")
    };
    let gen_dir = "build/gen".to_string();
    // 带 tag 的构建把对象也挪走, 免得两种后端互相踩(后端对比门禁要用)。
    let obj_dir = if tag.is_empty() {
        obj_dir
    } else {
        format!("{obj_dir}{tag_suffix}")
    };

    // ---- 工具候选(顺序 = 策略) ----
    let cross = &target.cross;
    let mut tools: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut cc_cands: Vec<String> = vec![format!("{cross}gcc")];
    for v in ["16", "15", "14", "13"] {
        cc_cands.push(format!("{cross}gcc-{v}"));
    }
    cc_cands.push("cc".to_string());
    tools.insert("cc".to_string(), cc_cands);
    tools.insert(
        "objcopy".to_string(),
        vec![format!("{cross}objcopy"), "objcopy".to_string()],
    );
    tools.insert(
        "objdump".to_string(),
        vec![format!("{cross}objdump"), "objdump".to_string()],
    );
    tools.insert(
        "size".to_string(),
        vec![format!("{cross}size"), "size".to_string()],
    );
    tools.insert("qemu".to_string(), vec![target.qemu_binary.clone()]);
    tools.insert("make".to_string(), vec!["make".to_string()]);
    tools.insert("ninja".to_string(), vec!["ninja".to_string()]);

    let mut tool_what: BTreeMap<String, String> = BTreeMap::new();
    tool_what.insert("cc".into(), "交叉 C 编译器(br-wa-toolchain-001 过渡)".into());
    tool_what.insert("objcopy".into(), "目标文件转二进制".into());
    tool_what.insert("objdump".into(), "反汇编(门禁用)".into());
    tool_what.insert("size".into(), "段体积".into());
    tool_what.insert("qemu".into(), "目标机模拟器".into());
    tool_what.insert("make".into(), "BR-D4 后端 A".into());
    tool_what.insert("ninja".into(), "BR-D4 后端 B".into());

    // ---- 工具解析(core 做: 生成物要把结果烧进去; 裁定 S-B1) ----
    let path_env = req
        .context("path")
        .and_then(Value::as_str)
        .map(str::to_string)
        .or_else(|| std::env::var("PATH").ok())
        .unwrap_or_default();
    let mut resolved: BTreeMap<String, String> = BTreeMap::new();
    for (name, cands) in &tools {
        if let Some(found) = resolve_tool(&root, cands, &path_env) {
            resolved.insert(name.clone(), found);
        }
    }

    // ---- `context.files`: 自检用的确定性 stat(给了就不碰磁盘) ----
    let stat_override = req.context("files").and_then(Value::as_object).map(|m| {
        let mut out: BTreeMap<String, (u64, i64)> = BTreeMap::new();
        for (k, v) in m {
            let size = v.get("size").and_then(Value::as_u64).unwrap_or(0);
            let mtime = v.get("mtime_ns").and_then(Value::as_i64).unwrap_or(0);
            out.insert(k.clone(), (size, mtime));
        }
        out
    });

    diags.merge(d);
    if diags.has_error() {
        return Err(diags);
    }

    // 注意: **不能**在这里把 diags 丢掉 —— 装载期也会产出 info/warning
    // (例如"生成物属于不在闭包内的插件, 已跳过"), 它们是调用方必须呈现的东西。
    Ok((Ctx {
        root,
        profile,
        backend,
        tag,
        units,
        target,
        pb,
        obj_dir,
        elf,
        bin,
        map,
        state_path,
        gen_dir,
        cflags,
        asflags,
        ldflags,
        tools,
        resolved,
        tool_what,
        stat_override,
        force: req.arg_bool("force").unwrap_or(false),
    }, diags))
}

/// 展开一组通配; 字面路径不存在 ⇒ `BRV-BLD-0007`(通配匹配为空 ⇒ 静默, 由 0002 兜底)。
fn expand_many(
    root: &Path,
    base_dir: &str,
    patterns: &[String],
    unit: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for pat in patterns {
        let hits = expand_glob(root, base_dir, pat);
        if hits.is_empty() {
            if has_wildcard(pat) {
                continue;
            }
            d.shape(
                CODE_MISSING_SOURCE,
                unit,
                file,
                span,
                format!("`{pat}` 不存在(字面路径, 不是通配)"),
                "检查路径拼写与工作目录; 通配不匹配**不算**错(见 BRV-BLD-0002)",
            );
            continue;
        }
        out.extend(hits);
    }
    out.sort();
    out.dedup();
    out
}

/// 工具解析: `<root>/prebuilts/toolchain/bin/` → PATH。
///
/// 顺序是**策略**(裁定 S-B1): 仓库自带的 prebuilt 优先(可复现), 再退宿主 PATH。
fn resolve_tool(root: &Path, candidates: &[String], path_env: &str) -> Option<String> {
    let dirs: Vec<PathBuf> = path_env
        .split(':')
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .collect();
    for cand in candidates {
        if cand.contains('/') {
            let p = root.join(cand);
            if p.is_file() {
                return Some(cand.clone());
            }
            continue;
        }
        // (1) 仓库自带的 prebuilt 工具链
        let local = root.join("prebuilts/toolchain/bin").join(cand);
        if local.is_file() {
            return Some(format!("prebuilts/toolchain/bin/{cand}"));
        }
        // (2) PATH
        for dir in &dirs {
            if dir.join(cand).is_file() {
                return Some(cand.clone());
            }
        }
    }
    None
}

impl Ctx {
    fn stat(&self, rel: &str) -> Option<(u64, i64)> {
        if let Some(map) = &self.stat_override {
            return map.get(rel).copied();
        }
        let meta = fs::metadata(self.root.join(rel)).ok()?;
        let size = meta.len();
        let mtime = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_nanos() as i64)
            .unwrap_or(0);
        Some((size, mtime))
    }

    fn obj_of(&self, src: &str) -> String {
        let stem = src.rsplit_once('.').map(|(a, _)| a).unwrap_or(src);
        format!("{}/{}.o", self.obj_dir, stem)
    }

    fn dep_of(&self, src: &str) -> String {
        format!("{}.d", self.obj_of(src))
    }

    fn tool(&self, name: &str) -> String {
        self.resolved
            .get(name)
            .cloned()
            .unwrap_or_else(|| format!("@{name}"))
    }

    /// 全部插件的 include 目录(核心本体也可能引用 platform 头, 见 br-wa-boot-001)。
    fn all_includes(&self) -> Vec<String> {
        let mut out: Vec<String> = Vec::new();
        for u in &self.units {
            for inc in &u.includes {
                out.push(inc.clone());
            }
        }
        out.sort();
        out.dedup();
        out
    }
}

// ================================================================== 计划

/// 生成全量步骤(未做增量过滤)。
fn all_steps(ctx: &Ctx) -> Vec<Step> {
    let mut steps: Vec<Step> = Vec::new();
    let cc = ctx.tool("cc");
    let objcopy = ctx.tool("objcopy");
    let includes: Vec<String> = ctx
        .all_includes()
        .iter()
        .map(|i| format!("-I{i}"))
        .collect();
    let arch_flags = ctx.target.arch_flags.clone();
    let arch_in_cflags = !arch_flags.is_empty();
    let arch_in_asflags = !arch_flags.is_empty();

    let mut srcs: Vec<(String, String)> = Vec::new(); // (src, unit)
    for u in &ctx.units {
        for s in &u.sources {
            srcs.push((s.clone(), u.name.clone()));
        }
    }
    srcs.sort();

    for (src, unit) in &srcs {
        let obj = ctx.obj_of(src);
        let dep = ctx.dep_of(src);
        let is_asm = src.ends_with(".S") || src.ends_with(".s");
        let mut argv: Vec<String> = vec![cc.clone()];
        if is_asm {
            if arch_in_asflags {
                argv.extend(arch_flags.clone());
            }
            argv.extend(ctx.asflags.clone());
        } else {
            argv.extend(ctx.cflags.clone());
            if arch_in_cflags {
                argv.extend(arch_flags.clone());
            }
            let unit_defines: Vec<String> = ctx
                .units
                .iter()
                .find(|u| &u.name == unit)
                .map(|u| u.defines.iter().map(|d| format!("-D{d}")).collect())
                .unwrap_or_default();
            argv.extend(includes.clone());
            argv.extend(unit_defines);
        }
        if is_asm {
            argv.extend(includes.clone());
        }
        argv.push("-MMD".to_string());
        argv.push("-MF".to_string());
        argv.push(dep.clone());
        argv.push("-c".to_string());
        argv.push(src.clone());
        argv.push("-o".to_string());
        argv.push(obj.clone());
        steps.push(Step {
            label: format!("cc {src}"),
            kind: "compile".to_string(),
            argv,
            group: 0,
            log: None,
            timeout_s: None,
            expect_timeout: false,
            judge: None,
            stdout_lines: None,
            outputs: vec![obj, dep],
            consumes: Vec::new(),
        });
    }

    let objs: Vec<String> = srcs.iter().map(|(s, _)| ctx.obj_of(s)).collect();
    let mut link_argv: Vec<String> = vec![cc.clone()];
    link_argv.extend(ctx.ldflags.clone());
    if let Some(ld) = &ctx.target.linker_script {
        link_argv.push(format!("-Wl,-T,{ld}"));
    }
    link_argv.push(format!("-Wl,-Map,{}", ctx.map));
    link_argv.extend(objs.iter().cloned());
    link_argv.push("-o".to_string());
    link_argv.push(ctx.elf.clone());
    steps.push(Step {
        label: format!("ld {}", ctx.elf),
        kind: "link".to_string(),
        argv: link_argv,
        group: 1,
        log: None,
        timeout_s: None,
        expect_timeout: false,
        judge: None,
        stdout_lines: None,
        outputs: vec![ctx.elf.clone(), ctx.map.clone()],
        consumes: objs.clone(),
    });

    steps.push(Step {
        label: format!("objcopy {}", ctx.bin),
        kind: "objcopy".to_string(),
        argv: vec![
            objcopy,
            "-O".to_string(),
            "binary".to_string(),
            ctx.elf.clone(),
            ctx.bin.clone(),
        ],
        group: 2,
        log: None,
        timeout_s: None,
        expect_timeout: false,
        judge: None,
        stdout_lines: None,
        outputs: vec![ctx.bin.clone()],
        consumes: vec![ctx.elf.clone()],
    });

    steps
}

/// 一条步骤的输入集合(源 + `.d` 里列的依赖)。
fn step_inputs(ctx: &Ctx, step: &Step) -> Vec<String> {
    let mut inputs: Vec<String> = Vec::new();
    match step.kind.as_str() {
        "compile" => {
            // argv 末尾形态: … -c <src> -o <obj>
            let mut it = step.argv.iter().rev();
            let _obj = it.next();
            let _o = it.next();
            if let Some(src) = it.next() {
                inputs.push(src.clone());
                let dep = format!("{}.d", _obj.cloned().unwrap_or_default());
                if ctx.stat_override.is_some() {
                    // 自检模式下 `.d` 由 context 提供(`context.files` 里带 `deps:<obj>` 键)
                } else {
                    let deps = parse_depfile(&ctx.root.join(&dep), &ctx.root);
                    inputs.extend(deps);
                }
            }
        }
        "link" => {
            for a in &step.argv {
                if a.starts_with("-Wl,-T,") {
                    inputs.push(a.trim_start_matches("-Wl,-T,").to_string());
                } else if a.ends_with(".o") {
                    inputs.push(a.clone());
                }
            }
        }
        "objcopy" => {
            if step.argv.len() >= 4 {
                inputs.push(step.argv[3].clone());
            }
        }
        _ => {}
    }
    inputs.sort();
    inputs.dedup();
    inputs
}

/// 把 `build/gen/**` 展开出来的路径按闭包过滤。
///
/// 归属判定: 路径形如 `build/gen/<插件名>/<文件>`(`<插件名>` 可能带命名空间,
/// 如 `service/dump`), 于是"哪个插件拥有它"= 树里**名字是它的路径前缀**的那个插件。
/// 认不出归属的路径(不以 `build/gen/<已知插件>/` 开头)保持原样 —— 那是人写的
/// 生成物或别的工具的产物, 工具不该替它做决定。
fn filter_gen_sources(
    sources: &[String],
    tree: &TreeLoad,
    selected: &BTreeSet<String>,
) -> (Vec<String>, Vec<String>) {
    let mut kept: Vec<String> = Vec::new();
    let mut skipped: Vec<String> = Vec::new();
    for path in sources {
        let owner = tree.plugins.iter().find(|p| {
            path.starts_with(&format!("build/gen/{}/", p.name))
        });
        match owner {
            Some(p) if !selected.contains(&p.name) => skipped.push(path.clone()),
            _ => kept.push(path.clone()),
        }
    }
    (kept, skipped)
}

/// argv 指纹: **命令本身**变了(改标志 / 换编译器 / 改落点) ⇒ 必须重跑。
fn argv_hash_of(step: &Step) -> String {
    sha256_tag(&[step.kind.clone(), step.argv.join("\u{1}")])
}

/// 上一次的状态(机器文件, core 产出 / L5 落盘; 缺省 = 全量重建)。
#[derive(Clone, Debug, Default)]
struct StateEntry {
    argv_hash: String,
    inputs: Vec<String>,
}

fn load_state(ctx: &Ctx) -> BTreeMap<String, StateEntry> {
    let mut out: BTreeMap<String, StateEntry> = BTreeMap::new();
    let Ok(text) = fs::read_to_string(ctx.root.join(&ctx.state_path)) else {
        return out;
    };
    let Ok(v) = text.parse::<toml::Value>() else {
        // 状态文件损坏 = 当作"没有状态"(全量重建), 不是错 —— 它是派生物。
        return out;
    };
    if let Some(jobs) = v.get("jobs").and_then(|j| j.as_array()) {
        for j in jobs {
            let Some(target) = j.get("target").and_then(|t| t.as_str()) else {
                continue;
            };
            out.insert(
                target.to_string(),
                StateEntry {
                    argv_hash: j
                        .get("argv_hash")
                        .and_then(|t| t.as_str())
                        .unwrap_or_default()
                        .to_string(),
                    inputs: j
                        .get("inputs")
                        .and_then(|t| t.as_array())
                        .map(|a| {
                            a.iter()
                                .filter_map(|x| x.as_str())
                                .map(str::to_string)
                                .collect()
                        })
                        .unwrap_or_default(),
                },
            );
        }
    }
    out
}

/// 构建计划: 要跑的步骤 + 状态文件内容 + 统计。
struct BuildPlan {
    todo: Vec<Step>,
    stale: Vec<String>,
    state_content: String,
    up_to_date: usize,
    total: usize,
    plan_hash: String,
}

/// 增量口径(**两条判据, 与 make / ninja 同族**):
///
/// 1. **命令指纹**: `argv` 变了(改标志 / 换编译器 / 改落点) ⇒ 重跑;
/// 2. **时间戳**: 目标不存在, 或任一输入(源 + `.d` 里列的头 + 链接用的对象与脚本)
///    比目标**新**, 或输入**不见了** ⇒ 重跑。
///
/// 为什么不用"输入集合 + 内容戳": `.d` 是**构建的产物**(第一次编译后才存在), 于是
/// "按输入集合算戳"会让头一次构建之后的两次运行必然不一致 —— 头依赖永远多编一轮
/// (踩过)。时间戳口径没有这个自指: `.d` 缺失 ⇒ 重编一次, 之后每轮都读它比时间。
fn make_plan(ctx: &Ctx) -> BuildPlan {
    let steps = all_steps(ctx);
    let old = load_state(ctx);
    let mut todo: Vec<Step> = Vec::new();
    let mut jobs: Vec<Value> = Vec::new();
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut up_to_date = 0usize;
    let mut plan_parts: Vec<String> = Vec::new();

    // 上游要重建 ⇒ 下游也必须重建。**必须显式传递**: 计划是在构建**之前**算的,
    // 那时对象文件还是旧的 —— 光看时间戳会得出"链接不用跑"的错误结论。
    let mut dirty: BTreeSet<String> = BTreeSet::new();
    for step in &steps {
        let target = step.outputs.first().cloned().unwrap_or_default();
        seen.insert(target.clone());
        let inputs = step_inputs(ctx, step);
        let argv_hash = argv_hash_of(step);
        plan_parts.push(format!("{target}\u{1}{}", step.argv.join("\u{1}")));

        let mut needs = ctx.force;
        let target_stat = ctx.stat(&target);
        if !needs && target_stat.is_none() {
            needs = true;
        }
        if !needs {
            if old.get(&target).map(|e| e.argv_hash.as_str()) != Some(argv_hash.as_str()) {
                needs = true;
            }
        }
        if !needs && step.kind == "compile" {
            // 缺 `.d` ⇒ 头依赖不可知 ⇒ 重编一次(宁多编一次, 不错链一次)。
            if ctx.stat(&format!("{target}.d")).is_none() {
                needs = true;
            }
        }
        if !needs {
            let target_mtime = target_stat.map(|(_, m)| m).unwrap_or(i64::MIN);
            for inp in &inputs {
                match ctx.stat(inp) {
                    // 输入比目标新, 或输入不见了 ⇒ 重跑(后者会以编译错误暴露)
                    Some((_, m)) if m <= target_mtime => {}
                    _ => {
                        needs = true;
                        break;
                    }
                }
            }
        }
        if !needs {
            for inp in &inputs {
                if dirty.contains(inp) {
                    needs = true;
                    break;
                }
            }
        }
        if needs {
            dirty.insert(target.clone());
            todo.push(step.clone());
        } else {
            up_to_date += 1;
        }
        jobs.push(json!({
            "target": target,
            "argv_hash": argv_hash,
            "inputs": inputs,
            "kind": step.kind,
        }));
    }

    let stale: Vec<String> = old
        .keys()
        .filter(|k| !seen.contains(*k) && k.starts_with(&ctx.obj_dir))
        .cloned()
        .collect();

    let state_content = format!(
        "# brickie:generated — 增量状态(派生品, 可删)\n\
         schema = 1\n\
         profile = \"{}\"\n\
         plan = \"{}\"\n\
         \n{}\n",
        ctx.profile,
        sha256_tag(&plan_parts),
        jobs.iter()
            .map(|j| {
                let inputs = j["inputs"]
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .filter_map(|x| x.as_str())
                            .map(|x| format!("\"{x}\""))
                            .collect::<Vec<_>>()
                            .join(", ")
                    })
                    .unwrap_or_default();
                format!(
                    "[[jobs]]\ntarget = \"{}\"\nargv_hash = \"{}\"\nkind = \"{}\"\ninputs = [{}]\n",
                    j["target"].as_str().unwrap_or_default(),
                    j["argv_hash"].as_str().unwrap_or_default(),
                    j["kind"].as_str().unwrap_or_default(),
                    inputs,
                )
            })
            .collect::<Vec<_>>()
            .join("\n")
    );

    BuildPlan {
        total: steps.len(),
        todo,
        stale,
        state_content,
        up_to_date,
        plan_hash: sha256_tag(&plan_parts),
    }
}

// ================================================================== 后端文件

/// 渲染 make 后端(BR-D4 的 A): 生成物, 不手改。
fn render_make(ctx: &Ctx, steps: &[Step]) -> String {
    let mut s = String::new();
    s.push_str("# brickie:generated — 生成物, 请勿手改。\n");
    s.push_str("# 重新生成: brickie build --backend make\n");
    s.push_str("# 来源: product.toml [build] + 各插件 [build] + platform [build.target](唯一真值)\n");
    s.push_str("# 本文件是 BR-D4 的**后端 A**(make 干活); `brickie build` 的 direct 后端不读它。\n\n");
    s.push_str(".DEFAULT_GOAL := all\n.PHONY: all clean\n\n");
    s.push_str(&format!("OBJ_DIR := {}\nELF := {}\nBIN := {}\nMAP := {}\n\n", ctx.obj_dir, ctx.elf, ctx.bin, ctx.map));
    let compiles: Vec<&Step> = steps.iter().filter(|s| s.kind == "compile").collect();
    let objs: Vec<String> = compiles
        .iter()
        .map(|s| s.outputs.first().cloned().unwrap_or_default())
        .collect();
    s.push_str(&format!("OBJS := {}\n", objs.join(" ")));
    s.push_str("DEPS := $(OBJS:.o=.d)\n\n");
    s.push_str("all: $(ELF) $(BIN)\n\n");
    for step in compiles {
        let obj = step.outputs.first().cloned().unwrap_or_default();
        let src = step
            .argv
            .iter()
            .position(|a| a == "-c")
            .and_then(|i| step.argv.get(i + 1))
            .cloned()
            .unwrap_or_default();
        s.push_str(&format!("{obj}: {src}\n"));
        s.push_str(&format!("\t@mkdir -p $(dir $@)\n\t{}\n\n", shell_join(&step.argv)));
    }
    if let Some(link) = steps.iter().find(|s| s.kind == "link") {
        s.push_str(&format!("$(ELF): $(OBJS)\n\t@mkdir -p $(dir $@)\n\t{}\n\n", shell_join(&link.argv)));
    }
    if let Some(cp) = steps.iter().find(|s| s.kind == "objcopy") {
        s.push_str(&format!("$(BIN): $(ELF)\n\t{}\n\n", shell_join(&cp.argv)));
    }
    s.push_str("clean:\n\trm -rf $(OBJ_DIR) $(ELF) $(BIN) $(MAP)\n\n");
    s.push_str("-include $(DEPS)\n");
    s
}

/// 渲染 ninja 后端(BR-D4 的 B)。
///
/// 两条 rule: `cc`(C)与 `asm`(汇编)。**必须分开** —— C 与 `.S` 的 argv 不同
/// (`-std=c11`/`-O2`/警告旗标 vs `-g3`), 用一条模板渲染两类源就是把汇编当成 C 编。
/// 模板从**该类的第一条步骤**的 argv 派生, 于是生成物与 `direct` 后端的命令逐字一致。
fn render_ninja(ctx: &Ctx, steps: &[Step]) -> String {
    let mut s = String::new();
    s.push_str("# brickie:generated — 生成物, 请勿手改。\n");
    s.push_str("# 重新生成: brickie build --backend ninja\n");
    s.push_str("# 本文件是 BR-D4 的**后端 B**(ninja 干活); 与 A 后端出**逐字节一致**的镜像。\n\n");
    s.push_str(&format!("cc = {}\n\n", ctx.tool("cc")));
    // ninja 的 `.ninja_log` / `.ninja_deps` 默认落在**调用目录**。钉到 build/ 下,
    // 否则一次 `brickie build --backend ninja` 就会往仓库根丢两个状态文件(踩过)。
    s.push_str(&format!("builddir = {}/ninja\n\n", ctx.gen_dir));

    let compiles: Vec<&Step> = steps.iter().filter(|s| s.kind == "compile").collect();
    let src_of = |st: &Step| -> String {
        st.argv
            .iter()
            .position(|a| a == "-c")
            .and_then(|i| st.argv.get(i + 1))
            .cloned()
            .unwrap_or_default()
    };
    let is_asm = |st: &Step| src_of(st).ends_with(".S") || src_of(st).ends_with(".s");

    // 每类一条 rule(模板 = 该类第一条步骤的 argv, 把具体 in/out 换成 $in/$out)
    for (tag, want_asm) in [("cc", false), ("asm", true)] {
        let Some(tmpl_step) = compiles.iter().find(|st| is_asm(st) == want_asm) else {
            continue;
        };
        let obj = tmpl_step.outputs.first().cloned().unwrap_or_default();
        let src = src_of(tmpl_step);
        let line = tmpl_step
            .argv
            .iter()
            .map(|a| {
                if a == &ctx.tool("cc") {
                    "$cc".to_string()
                } else if a == &obj {
                    "$out".to_string()
                } else if a == &src {
                    "$in".to_string()
                } else {
                    a.replace('$', "$$")
                }
            })
            .collect::<Vec<_>>()
            .join(" ");
        s.push_str(&format!("rule {tag}\n  command = {line}\n"));
        s.push_str("  depfile = $out.d\n  deps = gcc\n");
        s.push_str(&format!("  description = {tag} $in\n\n"));
    }
    for step in &compiles {
        let obj = step.outputs.first().cloned().unwrap_or_default();
        let src = src_of(step);
        let rule = if is_asm(step) { "asm" } else { "cc" };
        s.push_str(&format!("build {obj}: {rule} {src}\n"));
    }
    s.push('\n');
    if let Some(link) = steps.iter().find(|s| s.kind == "link") {
        s.push_str(&format!(
            "rule link\n  command = {}\n  description = link $out\n\n",
            shell_join(&link.argv)
        ));
        let objs: Vec<String> = compiles
            .iter()
            .map(|st| st.outputs.first().cloned().unwrap_or_default())
            .collect();
        s.push_str(&format!("build {}: link {}\n\n", ctx.elf, objs.join(" ")));
    }
    if let Some(cp) = steps.iter().find(|s| s.kind == "objcopy") {
        s.push_str(&format!(
            "rule objcopy\n  command = {}\n  description = objcopy $out\n\n",
            shell_join(&cp.argv)
        ));
        s.push_str(&format!("build {}: objcopy {}\n\n", ctx.bin, ctx.elf));
    }
    s.push_str(&format!("default {}\n", ctx.bin));
    s
}

/// 极简 shell 引号(生成物里的命令要能被 make 原样执行)。
fn shell_join(argv: &[String]) -> String {
    argv.iter()
        .map(|a| {
            if a.is_empty()
                || a.contains(|c: char| c.is_whitespace() || "\"'$`\\*?()[]{}|&;<>#~!".contains(c))
            {
                format!("'{}'", a.replace('\'', "'\\''"))
            } else {
                a.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

// ================================================================== 呈现数据

fn summary_json(ctx: &Ctx, plan: &BuildPlan, steps: &[Step], extra: Value) -> Value {
    let mut units: Vec<Value> = ctx
        .units
        .iter()
        .map(|u| {
            json!({
                "name": u.name,
                "dir": u.dir,
                "plugin": u.plugin,
                "sources": u.sources.len(),
                "includes": u.includes,
                "defines": u.defines,
                "sources_list": u.sources,
            })
        })
        .collect();
    units.sort_by_key(|u| u["name"].as_str().unwrap_or_default().to_string());
    let tools: Vec<Value> = ctx
        .tools
        .iter()
        .map(|(k, cands)| {
            json!({
                "name": k,
                "what": ctx.tool_what.get(k).cloned().unwrap_or_default(),
                "candidates": cands,
                "resolved": ctx.resolved.get(k),
            })
        })
        .collect();
    let mut data = json!({
        "profile": ctx.profile,
        "backend": ctx.backend,
        "tag": ctx.tag,
        "target": {
            "owner": ctx.target.owner,
            "arch": ctx.target.arch,
            "cross": ctx.target.cross,
            "arch_flags": ctx.target.arch_flags,
            "linker_script": ctx.target.linker_script,
            "qemu": {
                "binary": ctx.target.qemu_binary,
                "machine": ctx.target.qemu_machine,
                "cpu": ctx.target.qemu_cpu,
                "memory": ctx.target.qemu_memory,
                "extra": ctx.target.qemu_extra,
            },
        },
        "outputs": {"obj_dir": ctx.obj_dir, "elf": ctx.elf, "bin": ctx.bin, "map": ctx.map},
        "state_path": ctx.state_path,
        "gen_dir": ctx.gen_dir,
        "units": units,
        "unit_count": ctx.units.len(),
        "source_count": ctx.units.iter().map(|u| u.sources.len()).sum::<usize>(),
        "tools": tools,
        "steps": steps.iter().map(Step::to_json).collect::<Vec<_>>(),
        "steps_total": plan.total,
        "steps_todo": plan.todo.len(),
        "up_to_date": plan.up_to_date,
        "stale_outputs": plan.stale,
        "plan_hash": plan.plan_hash,
        "flags": {"cflags": ctx.cflags, "asflags": ctx.asflags, "ldflags": ctx.ldflags},
    });
    if let (Some(obj), Some(extra)) = (data.as_object_mut(), extra.as_object()) {
        for (k, v) in extra {
            obj.insert(k.clone(), v.clone());
        }
    }
    data
}

// ================================================================== 命令: build

fn cmd_build(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (ctx, mut diags) = match load_ctx(req, &tree) {
        Ok(c) => c,
        Err(mut d) => {
            d.merge(tree.diags);
            return Response::ok(d, json!({}));
        }
    };
    diags.merge(tree.diags.clone());
    let plan = make_plan(&ctx);
    let steps = all_steps(&ctx);

    let dry_run = req.arg_bool("dry_run").unwrap_or(false);
    let emit_backends = req.arg_bool("emit_backends").unwrap_or(false);

    // ---- 后端文件(生成物) ----
    let mut files: Vec<Value> = Vec::new();
    let want_make = emit_backends || ctx.backend == "make";
    let want_ninja = emit_backends || ctx.backend == "ninja";
    if want_make {
        files.push(json!({
            "path": format!("{}/build.mk", ctx.gen_dir),
            "kind": "machine",
            "content": render_make(&ctx, &steps),
            "mode": "0644",
        }));
    }
    if want_ninja {
        files.push(json!({
            "path": format!("{}/build.ninja", ctx.gen_dir),
            "kind": "machine",
            "content": render_ninja(&ctx, &steps),
            "mode": "0644",
        }));
    }

    // ---- 要执行的步骤 ----
    let exec_steps: Vec<Step> = match ctx.backend.as_str() {
        "make" => {
            let jobs = req.arg_u64("jobs").unwrap_or(0);
            let mut argv = vec![ctx.tool("make"), "-f".to_string(), format!("{}/build.mk", ctx.gen_dir)];
            if jobs > 0 {
                argv.push(format!("-j{jobs}"));
            }
            vec![Step {
                label: "make -f build/gen/build.mk".to_string(),
                kind: "backend".to_string(),
                argv,
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: vec![ctx.elf.clone(), ctx.bin.clone()],
                consumes: Vec::new(),
            }]
        }
        "ninja" => {
            let jobs = req.arg_u64("jobs").unwrap_or(0);
            let mut argv = vec![
                ctx.tool("ninja"),
                "-f".to_string(),
                format!("{}/build.ninja", ctx.gen_dir),
            ];
            if jobs > 0 {
                argv.push(format!("-j{jobs}"));
            }
            vec![Step {
                label: "ninja -f build/gen/build.ninja".to_string(),
                kind: "backend".to_string(),
                argv,
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: vec![ctx.elf.clone(), ctx.bin.clone()],
                consumes: Vec::new(),
            }]
        }
        _ => plan.todo.clone(),
    };

    // ---- 工具解析失败 ⇒ 环境错(退出码 2) ----
    if !dry_run && ctx.backend == "direct" && !plan.todo.is_empty() {
        for name in ["cc", "objcopy"] {
            if !ctx.resolved.contains_key(name) {
                diags.shape(
                    CODE_TOOL_MISSING,
                    "",
                    "product.toml",
                    "build.target",
                    format!(
                        "找不到工具 `{name}`(候选: {})",
                        ctx.tools.get(name).cloned().unwrap_or_default().join(", ")
                    ),
                    "装交叉工具链或取 prebuilt: make prebuilt; 也可用 `[build.target].cross` 指到别处",
                );
                return Response::ok(diags, json!({}));
            }
        }
    }

    let state_file = json!({
        "path": ctx.state_path.clone(),
        "kind": "machine",
        "content": plan.state_content.clone(),
        "mode": "0644",
    });
    // 状态文件只在 direct 后端有意义(make/ninja 自己记时间戳)。
    if ctx.backend == "direct" && !dry_run {
        files.push(state_file);
    }

    if ctx.profile == "release" && plan.total == 0 {
        diags.coded(
            CODE_BUILD_SHAPE,
            Severity::Info,
            "",
            "product.toml",
            "build.release",
            "release 与 dev 的编译标志相同(只有组合期检查严格度不同)",
            "镜像内断言轴(debug=panic / release=trace, 1-01 §13)尚未落地, 是 v0.2 的登记项",
        );
    }

    let show_steps: Vec<Step> = if dry_run { steps.clone() } else { exec_steps.clone() };
    let data = summary_json(
        &ctx,
        &plan,
        &show_steps,
        json!({
            "dry_run": dry_run,
            "emit_backends": emit_backends,
            "steps": show_steps.iter().map(Step::to_json).collect::<Vec<_>>(),
        }),
    );
    Response::ok_with_files(diags, files, data)
}

// ================================================================== 命令: clean / run / size / disasm

fn cmd_clean(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let all = req.arg_bool("all").unwrap_or(false);
    let ctx = load_ctx(req, &tree).ok().map(|(c, _)| c);
    let mut d = tree.diags.clone();
    let mut paths: Vec<String> = Vec::new();
    if let Some(ctx) = &ctx {
        paths.push(ctx.obj_dir.clone());
        paths.push(ctx.elf.clone());
        paths.push(ctx.bin.clone());
        paths.push(ctx.map.clone());
        paths.push(ctx.state_path.clone());
    } else {
        // 声明面还没成形(如缺 [build])也要能清: 退回到历史约定路径。
        d = Diags::new();
        paths.push(default_obj_dir());
        for f in ["build/brick.elf", "build/brick.bin", "build/brick.map"] {
            paths.push(f.to_string());
        }
    }
    for log in ["smoke", "irq", "dbg"] {
        paths.push(format!("build/logs/{log}.log"));
    }
    paths.push("build/hosttest".to_string());
    if all {
        paths.push("build/gen".to_string());
        paths.push("build/index".to_string());
        paths.push("build/logs".to_string());
        paths.push("build/toolchain".to_string());
    }
    paths.sort();
    paths.dedup();
    let data = json!({
        "all": all,
        "paths": paths,
        "note": "L5 逐个删除(每个路径都必须落在 root 之内; 不含 build/host 的工具产物)",
    });
    Response::ok(d, data)
}

fn cmd_run(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (ctx, mut diags) = match load_ctx(req, &tree) {
        Ok(c) => c,
        Err(mut d) => {
            d.merge(tree.diags);
            return Response::ok(d, json!({}));
        }
    };
    diags.merge(tree.diags.clone());
    let argv = qemu_argv(&ctx);
    if !ctx.resolved.contains_key("qemu") {
        diags.shape(
            CODE_TOOL_MISSING,
            "",
            "product.toml",
            "build.target.qemu",
            format!("找不到 QEMU(`{}`)", ctx.target.qemu_binary),
            "装 qemu-system-arm; 或用 `[build.target.qemu].binary` 指到别处",
        );
        return Response::ok(diags, json!({}));
    }
    let steps = vec![Step {
        label: "qemu run".to_string(),
        kind: "run".to_string(),
        argv,
        group: 9,
        log: None,
        timeout_s: req.arg_u64("timeout_s"),
        expect_timeout: false,
        judge: None,
        stdout_lines: None,
        outputs: Vec::new(),
        consumes: Vec::new(),
    }];
    let plan = BuildPlan {
        todo: Vec::new(),
        stale: Vec::new(),
        state_content: String::new(),
        up_to_date: 0,
        total: 0,
        plan_hash: String::new(),
    };
    let data = summary_json(&ctx, &plan, &steps, json!({"mode": "run"}));
    Response::ok(diags, data)
}

fn qemu_argv(ctx: &Ctx) -> Vec<String> {
    let mut argv = vec![ctx.tool("qemu")];
    if !ctx.target.qemu_machine.is_empty() {
        argv.push("-M".to_string());
        argv.push(ctx.target.qemu_machine.clone());
    }
    if !ctx.target.qemu_cpu.is_empty() {
        argv.push("-cpu".to_string());
        argv.push(ctx.target.qemu_cpu.clone());
    }
    if !ctx.target.qemu_memory.is_empty() {
        argv.push("-m".to_string());
        argv.push(ctx.target.qemu_memory.clone());
    }
    argv.extend(ctx.target.qemu_extra.clone());
    argv.push("-kernel".to_string());
    argv.push(ctx.elf.clone());
    argv
}

fn cmd_size(req: &Request) -> Response {
    binutils_cmd(req, "size")
}

fn cmd_disasm(req: &Request) -> Response {
    binutils_cmd(req, "objdump")
}

fn binutils_cmd(req: &Request, which: &str) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (ctx, mut diags) = match load_ctx(req, &tree) {
        Ok(c) => c,
        Err(mut d) => {
            d.merge(tree.diags);
            return Response::ok(d, json!({}));
        }
    };
    diags.merge(tree.diags.clone());
    let tool = ctx.tool(which);
    if !ctx.resolved.contains_key(which) {
        diags.shape(
            CODE_TOOL_MISSING,
            "",
            "product.toml",
            "build.target",
            format!("找不到工具 `{which}`(候选: {})", ctx.tools.get(which).cloned().unwrap_or_default().join(", ")),
            "装 aarch64 binutils 或取 prebuilt: make prebuilt",
        );
        return Response::ok(diags, json!({}));
    }
    let steps = if which == "size" {
        vec![
            Step {
                label: "size -A -x".to_string(),
                kind: "print".to_string(),
                argv: vec![tool.clone(), "-A".to_string(), "-x".to_string(), ctx.elf.clone()],
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: Vec::new(),
                consumes: Vec::new(),
            },
            Step {
                label: "size".to_string(),
                kind: "print".to_string(),
                argv: vec![tool, ctx.elf.clone()],
                group: 9,
                log: None,
                timeout_s: None,
                expect_timeout: false,
                judge: None,
                stdout_lines: None,
                outputs: Vec::new(),
                consumes: Vec::new(),
            },
        ]
    } else {
        vec![Step {
            label: "objdump -d".to_string(),
            kind: "print".to_string(),
            argv: vec![tool, "-d".to_string(), ctx.elf.clone()],
            group: 9,
            log: None,
            timeout_s: None,
            expect_timeout: false,
            judge: None,
            stdout_lines: Some(req.arg_u64("head").unwrap_or(120)),
            outputs: Vec::new(),
            consumes: Vec::new(),
        }]
    };
    let plan = BuildPlan {
        todo: Vec::new(),
        stale: Vec::new(),
        state_content: String::new(),
        up_to_date: 0,
        total: 0,
        plan_hash: String::new(),
    };
    let data = summary_json(&ctx, &plan, &steps, json!({"mode": which}));
    Response::ok(diags, data)
}

// ================================================================== gates.toml

#[derive(Clone, Debug, Default)]
struct GateSpec {
    name: String,
    timeout_s: Option<u64>,
    log: Option<String>,
    require: Vec<String>,
    forbid: Vec<String>,
    require_tags: Vec<String>,
    expect_timeout: bool,
}

#[derive(Clone, Debug, Default)]
struct HostTestSpec {
    name: String,
    sources: Vec<String>,
    includes: Vec<String>,
    defines: Vec<String>,
    depends: Vec<String>,
    timeout_s: Option<u64>,
}

#[derive(Clone, Debug, Default)]
struct ScriptSpec {
    name: String,
    argv: Vec<String>,
    require: Vec<String>,
    forbid: Vec<String>,
}

#[derive(Clone, Debug, Default)]
struct GatesFile {
    run_timeout_s: Option<u64>,
    host_cc: String,
    host_cflags: Vec<String>,
    host_bin_dir: String,
    gates: Vec<GateSpec>,
    hosttests: Vec<HostTestSpec>,
    scripts: Vec<ScriptSpec>,
    build_post: Vec<String>,
    file: String,
    present: bool,
}

fn str_list(t: &toml::Table, key: &str) -> Vec<String> {
    t.get(key)
        .and_then(|v| v.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn opt_str(t: &toml::Table, key: &str) -> Option<String> {
    t.get(key).and_then(|v| v.as_str()).map(str::to_string)
}

fn load_gates(root: &Path, d: &mut Diags) -> GatesFile {
    let rel = "tests/gates.toml".to_string();
    let mut g = GatesFile {
        host_cc: "cc".to_string(),
        host_bin_dir: "build/hosttest".to_string(),
        file: rel.clone(),
        ..Default::default()
    };
    let abs = root.join(&rel);
    if !abs.is_file() {
        g.run_timeout_s = Some(5);
        return g;
    }
    g.present = true;
    let text = match fs::read_to_string(&abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("读不到 `{rel}`: {e}"), "检查路径与权限");
            return g;
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(format!("`{rel}` 不是合法 TOML: {e}"), "contract §7.4");
            return g;
        }
    };
    let Some(top) = value.as_table() else {
        d.env(format!("`{rel}` 顶层必须是表"), "contract §7.4");
        return g;
    };
    if let Some(run) = top.get("run").and_then(|v| v.as_table()) {
        g.run_timeout_s = run.get("default_timeout_s").and_then(|v| v.as_integer()).map(|v| v as u64);
    }
    if let Some(h) = top.get("host").and_then(|v| v.as_table()) {
        if let Some(cc) = opt_str(h, "cc") {
            g.host_cc = cc;
        }
        g.host_cflags = str_list(h, "cflags");
        if let Some(dir) = opt_str(h, "bin_dir") {
            g.host_bin_dir = dir;
        }
    }
    if let Some(arr) = top.get("gate").and_then(|v| v.as_array()) {
        for item in arr {
            let Some(t) = item.as_table() else { continue };
            g.gates.push(GateSpec {
                name: opt_str(t, "name").unwrap_or_default(),
                timeout_s: t.get("timeout_s").and_then(|v| v.as_integer()).map(|v| v as u64),
                log: opt_str(t, "log"),
                require: str_list(t, "require"),
                forbid: str_list(t, "forbid"),
                require_tags: str_list(t, "require_tags"),
                expect_timeout: t
                    .get("expect")
                    .and_then(|v| v.as_str())
                    .map(|s| s == "timeout")
                    .unwrap_or(true),
            });
        }
    }
    if let Some(arr) = top.get("hosttest").and_then(|v| v.as_array()) {
        for item in arr {
            let Some(t) = item.as_table() else { continue };
            g.hosttests.push(HostTestSpec {
                name: opt_str(t, "name").unwrap_or_default(),
                sources: str_list(t, "sources"),
                includes: str_list(t, "includes"),
                defines: str_list(t, "defines"),
                depends: str_list(t, "depends"),
                timeout_s: t.get("timeout_s").and_then(|v| v.as_integer()).map(|v| v as u64),
            });
        }
    }
    if let Some(arr) = top.get("script").and_then(|v| v.as_array()) {
        for item in arr {
            let Some(t) = item.as_table() else { continue };
            g.scripts.push(ScriptSpec {
                name: opt_str(t, "name").unwrap_or_default(),
                argv: str_list(t, "argv"),
                require: str_list(t, "require"),
                forbid: str_list(t, "forbid"),
            });
        }
    }
    if let Some(b) = top.get("build").and_then(|v| v.as_table()) {
        g.build_post = str_list(b, "post");
    }

    // 正则子集校验: 宁可**报形状错**, 也不让一条"永不匹配"的 require 变成假绿。
    let mut check_patterns: Vec<(String, String, String)> = Vec::new();
    for gate in &g.gates {
        for (i, pat) in gate.require.iter().enumerate() {
            check_patterns.push((gate.name.clone(), format!("gate.require[{i}]"), pat.clone()));
        }
        for (i, pat) in gate.forbid.iter().enumerate() {
            check_patterns.push((gate.name.clone(), format!("gate.forbid[{i}]"), pat.clone()));
        }
    }
    for s in &g.scripts {
        for (i, pat) in s.require.iter().enumerate() {
            check_patterns.push((s.name.clone(), format!("script.require[{i}]"), pat.clone()));
        }
        for (i, pat) in s.forbid.iter().enumerate() {
            check_patterns.push((s.name.clone(), format!("script.forbid[{i}]"), pat.clone()));
        }
    }
    for (name, span, pat) in check_patterns {
        if let Some(ch) = regex_unsupported(&pat) {
            d.shape(
                CODE_GATE_SHAPE,
                name,
                rel.clone(),
                span,
                format!("正则 `{pat}` 用了 core 正则子集不支持的 `{ch}`"),
                "支持 `.` `*` `+` `?` `[...]` 与 `\\x` 转义; 分组/交替/锚点不支持 —— 拆成多条",
            );
        }
    }
    g
}

/// 返回模式里**第一个不支持的元字符**(`None` = 在子集内)。
///
/// 为什么不引一个正则库: 判据只需要"日志里有没有这么个形状", 而这一族模式很短;
/// 引库会多一条工具依赖(§9.3 最小依赖集)。代价是**必须如实拒绝**子集外的写法。
fn regex_unsupported(pat: &str) -> Option<char> {
    let mut escaped = false;
    for ch in pat.chars() {
        if escaped {
            escaped = false;
            continue;
        }
        match ch {
            '\\' => escaped = true,
            '(' | ')' | '|' | '{' | '}' | '^' | '$' => return Some(ch),
            _ => {}
        }
    }
    None
}

// ================================================================== 命令: test

fn cmd_test(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let mut diags = tree.diags.clone();
    let g = load_gates(&root, &mut diags);
    let want = req.arg_str("name").unwrap_or_default();
    let (ctx, ctx_diags) = match load_ctx(req, &tree) {
        Ok((c, d)) => (Some(c), Some(d)),
        Err(_) => (None, None),
    };
    if let Some(d) = ctx_diags {
        diags.merge(d);
    }

    // `--list`: 只列(CI 的自检与 `--help` 之外的发现路径)。
    if req.arg_bool("list").unwrap_or(false) {
        let data = json!({
            "gates": g.gates.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
            "hosttests": g.hosttests.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
            "scripts": g.scripts.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
            "build_post": g.build_post,
            "file": g.file,
            "present": g.present,
        });
        return Response::ok(diags, data);
    }

    if !g.present {
        diags.shape(
            CODE_GATE_UNKNOWN,
            "",
            "tests/gates.toml",
            "",
            "没有 `tests/gates.toml`: 门禁没有声明面, 无从编排",
            "写下 `[[gate]]` / `[[hosttest]]` / `[[script]]`; contract §7.4",
        );
        return Response::ok(diags, json!({}));
    }
    let known: Vec<String> = g
        .gates
        .iter()
        .map(|x| x.name.clone())
        .chain(g.hosttests.iter().map(|x| x.name.clone()))
        .chain(g.scripts.iter().map(|x| x.name.clone()))
        .collect();
    if !want.is_empty() && !known.contains(&want) {
        diags.shape(
            CODE_GATE_UNKNOWN,
            want.clone(),
            &g.file.clone(),
            "",
            format!("没有名为 `{want}` 的门禁(已知: {})", known.join(", ")),
            "`brickie test --list` 可列出全部",
        );
        return Response::ok(diags, json!({}));
    }

    // 选中集合: 单个 = 它 + 它的 depends(拓扑展开); 不给 = 全跑。
    let selected: BTreeSet<String> = if want.is_empty() {
        g.hosttests
            .iter()
            .map(|x| x.name.clone())
            .chain(g.gates.iter().map(|x| x.name.clone()))
            .chain(g.scripts.iter().map(|x| x.name.clone()))
            .collect()
    } else {
        let mut sel: BTreeSet<String> = BTreeSet::new();
        fn add(sel: &mut BTreeSet<String>, g: &GatesFile, name: &str) {
            if !sel.insert(name.to_string()) {
                return;
            }
            for h in &g.hosttests {
                if h.name == name {
                    for dep in &h.depends {
                        add(sel, g, dep);
                    }
                }
            }
        }
        add(&mut sel, &g, &want);
        sel
    };

    let mut steps: Vec<Step> = Vec::new();
    let mut group: u32 = 0;

    // ---- 宿主用例: 编译 + 跑 ----
    for h in &g.hosttests {
        if !selected.contains(&h.name) {
            continue;
        }
        let mut argv = vec![g.host_cc.clone()];
        argv.extend(g.host_cflags.clone());
        argv.extend(h.defines.iter().map(|d| format!("-D{d}")));
        argv.extend(h.includes.iter().map(|i| format!("-I{i}")));
        argv.extend(h.sources.iter().cloned());
        argv.push("-o".to_string());
        let bin = format!("{}/{}", g.host_bin_dir, h.name);
        argv.push(bin.clone());
        steps.push(Step {
            label: format!("hostcc {}", h.name),
            kind: "hosttest".to_string(),
            argv,
            group,
            log: Some(format!("build/logs/{}.compile.log", h.name)),
            timeout_s: None,
            expect_timeout: false,
            judge: None,
            stdout_lines: None,
            outputs: vec![bin.clone()],
            consumes: Vec::new(),
        });
        group += 1;
        steps.push(Step {
            label: format!("run {}", h.name),
            kind: "hostrun".to_string(),
            argv: vec![format!("./{bin}")],
            group,
            log: Some(format!("build/logs/{}.log", h.name)),
            timeout_s: h.timeout_s,
            expect_timeout: false,
            judge: None,
            stdout_lines: None,
            outputs: Vec::new(),
            consumes: vec![bin.clone()],
        });
        group += 1;
    }

    // ---- QEMU 门禁 ----
    for gate in &g.gates {
        if !selected.contains(&gate.name) {
            continue;
        }
        let Some(ctx) = &ctx else { continue };
        let argv = qemu_argv(ctx);
        steps.push(Step {
            label: format!("qemu {}", gate.name),
            kind: "gate".to_string(),
            argv,
            group,
            log: Some(
                gate
                    .log
                    .clone()
                    .unwrap_or_else(|| format!("build/logs/{}.log", gate.name)),
            ),
            timeout_s: Some(
                gate
                    .timeout_s
                    .or(g.run_timeout_s)
                    .unwrap_or(5),
            ),
            expect_timeout: gate.expect_timeout,
            judge: Some(gate.name.clone()),
            stdout_lines: None,
            outputs: Vec::new(),
            consumes: Vec::new(),
        });
        group += 1;
    }

    // ---- 脚本门禁 ----
    for s in &g.scripts {
        if !selected.contains(&s.name) {
            continue;
        }
        let judge = if s.require.is_empty() && s.forbid.is_empty() {
            None
        } else {
            Some(s.name.clone())
        };
        steps.push(Step {
            label: format!("script {}", s.name),
            kind: "script".to_string(),
            argv: s.argv.clone(),
            group,
            log: Some(format!("build/logs/{}.log", s.name)),
            timeout_s: None,
            expect_timeout: false,
            judge,
            stdout_lines: None,
            outputs: Vec::new(),
            consumes: Vec::new(),
        });
        group += 1;
    }

    let plan = BuildPlan {
        todo: Vec::new(),
        stale: Vec::new(),
        state_content: String::new(),
        up_to_date: 0,
        total: steps.len(),
        plan_hash: String::new(),
    };
    let mut data = if let Some(ctx) = &ctx {
        summary_json(ctx, &plan, &steps, json!({"mode": "test", "name": want}))
    } else {
        json!({"mode": "test", "name": want, "steps": steps.iter().map(Step::to_json).collect::<Vec<_>>()})
    };
    if let Some(obj) = data.as_object_mut() {
        obj.insert(
            "gates".into(),
            json!({
                "file": g.file,
                "gates": g.gates.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
                "hosttests": g.hosttests.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
                "scripts": g.scripts.iter().map(|x| x.name.clone()).collect::<Vec<_>>(),
                "build_post": g.build_post,
            }),
        );
    }
    Response::ok(diags, data)
}

// ================================================================== 命令: judge

fn cmd_judge(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let mut diags = Diags::new();
    let g = load_gates(&root, &mut diags);
    let name = req.arg_str("name").unwrap_or_default();
    let log = req
        .context("log")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let lines: Vec<&str> = log.lines().collect();

    let gate = g.gates.iter().find(|x| x.name == name);
    let script = g.scripts.iter().find(|x| x.name == name);
    if gate.is_none() && script.is_none() {
        diags.shape(
            CODE_GATE_UNKNOWN,
            name.clone(),
            &g.file.clone(),
            "",
            format!("没有名为 `{name}` 的门禁可判"),
            "`brickie test --list` 可列出全部",
        );
        return Response::ok(diags, json!({}));
    }

    let (require, forbid, tags) = match (gate, script) {
        (Some(x), _) => (x.require.clone(), x.forbid.clone(), x.require_tags.clone()),
        (None, Some(s)) => (s.require.clone(), s.forbid.clone(), Vec::new()),
        _ => unreachable!(),
    };

    let mut checks: Vec<Value> = Vec::new();
    let mut failed = 0usize;
    for (i, pat) in require.iter().enumerate() {
        let hit = lines.iter().any(|l| regex_lite_match(pat, l));
        checks.push(json!({"kind": "require", "pattern": pat, "ok": hit}));
        if !hit {
            failed += 1;
            diags.coded(
                CODE_GATE_FAILED,
                Severity::Error,
                name.clone(),
                g.file.clone(),
                format!("gate[{i}].require"),
                format!("门禁 `{name}`: 日志里没有匹配 `{pat}` 的行"),
                "判据在 tests/gates.toml; 日志见步骤声明的 log 路径",
            );
        }
    }
    for (i, pat) in forbid.iter().enumerate() {
        let hit = lines.iter().any(|l| regex_lite_match(pat, l));
        checks.push(json!({"kind": "forbid", "pattern": pat, "ok": !hit}));
        if hit {
            failed += 1;
            let sample = lines
                .iter()
                .find(|l| regex_lite_match(pat, l))
                .copied()
                .unwrap_or_default();
            diags.coded(
                CODE_GATE_FAILED,
                Severity::Error,
                name.clone(),
                g.file.clone(),
                format!("gate[{i}].forbid"),
                format!("门禁 `{name}`: 日志命中了禁止模式 `{pat}`: {sample}"),
                "这是「不该出现的东西出现了」 —— 先看那一行日志",
            );
        }
    }
    for (i, tag) in tags.iter().enumerate() {
        let pat = format!("PASS {tag} ");
        let hit = log.contains(&pat);
        checks.push(json!({"kind": "require_tag", "pattern": pat, "ok": hit}));
        if !hit {
            failed += 1;
            diags.coded(
                CODE_GATE_FAILED,
                Severity::Error,
                name.clone(),
                g.file.clone(),
                format!("gate[{i}].require_tags"),
                format!("门禁 `{name}`: 缺用例 `{tag}` 的 PASS(用例被裁掉也算红)"),
                "用例少了和用例失败一样是红 —— 与 irq-test 同一纪律",
            );
        }
    }

    let pass_lines = lines.iter().filter(|l| l.contains("PASS ")).count();
    if failed == 0 {
        // 全绿是 **info 且无码**: 它不是缺陷, 借用失败码会让 `--json` 的读者误判。
        diags.push(
            crate::diag::Diag::new(
                Severity::Info,
                format!(
                    "门禁 `{name}` 全绿: require {} 条 / forbid {} 条 / 用例 tag {} 条 / 日志 {} 行(PASS 行 {pass_lines})",
                    require.len(),
                    forbid.len(),
                    tags.len(),
                    lines.len()
                ),
            )
            .target(name.clone())
            .file(g.file.clone()),
        );
        diags.set_exit_override(EXIT_OK);
    }

    let data = json!({
        "name": name,
        "failed": failed,
        "checks": checks,
        "log_lines": lines.len(),
        "pass_lines": pass_lines,
        "require": require.len(),
        "forbid": forbid.len(),
        "require_tags": tags.len(),
        "file": g.file,
    });
    let mut resp = Response::ok(diags, data);
    if failed > 0 {
        resp.exit_code = EXIT_RED;
    }
    resp
}

/// 极简正则: 支持 `.` `*` `+` `?` `[...]` `\(`/`\)` `|` 的**子集**判定(不编译, 线性扫描)。
///
/// 为什么不自带正则库: 判据只需要"日志里有没有这么个形状", 而这个子集覆盖
/// `tests/gates.toml` 里实际用到的全部模式(`pass=[0-9]* fail=0 `、`\[PANIC\]`、
/// `irq_ticks=[1-9]`)。**不用正则引擎**也避免了一条"工具依赖"的负债。
fn regex_lite_match(pat: &str, text: &str) -> bool {
    match regex_lite_at(pat, text) {
        Some(_) => true,
        None => false,
    }
}

/// 在 `text` 的任意位置尝试匹配 `pat`(回溯实现, 模式都很短)。
fn regex_lite_at(pat: &str, text: &str) -> Option<usize> {
    let p: Vec<char> = pat.chars().collect();
    let t: Vec<char> = text.chars().collect();
    for start in 0..=t.len() {
        if let Some(end) = regex_lite_here(&p, &t, 0, start) {
            return Some(end);
        }
    }
    None
}

/// 从 `(pi, ti)` 尝试匹配到模式结尾, 返回**结束位置**(用于 `+`/`*` 的回溯)。
fn regex_lite_here(p: &[char], t: &[char], pi: usize, ti: usize) -> Option<usize> {
    if pi >= p.len() {
        return Some(ti);
    }
    let (atom_len, atom) = match p[pi] {
        '\\' if pi + 1 < p.len() => (2usize, Some(p[pi + 1])),
        '[' => {
            // 字符类: 取到 `]`, 支持 `^` 取反与 `a-z`
            let mut j = pi + 1;
            while j < p.len() && p[j] != ']' {
                j += 1;
            }
            if j >= p.len() {
                (1, Some('['))
            } else {
                (j + 1 - pi, None)
            }
        }
        c => (1, Some(c)),
    };
    let class: Option<Vec<char>> = if p[pi] == '[' {
        let mut j = pi + 1;
        while j < p.len() && p[j] != ']' {
            j += 1;
        }
        if j >= p.len() {
            None
        } else {
            Some(p[pi + 1..j].to_vec())
        }
    } else {
        None
    };
    let quant = p.get(pi + atom_len).copied();
    let (min, max): (usize, Option<usize>) = match quant {
        Some('*') => (0, None),
        Some('+') => (1, None),
        Some('?') => (0, Some(1)),
        _ => (1, Some(1)),
    };
    let next_pi = if matches!(quant, Some('*') | Some('+') | Some('?')) {
        pi + atom_len + 1
    } else {
        pi + atom_len
    };
    let matches_atom = |c: char| -> bool {
        if p[pi] == '.' {
            return true;
        }
        if let Some(cls) = &class {
            let negate = cls.first() == Some(&'^');
            let body = if negate { &cls[1..] } else { &cls[..] };
            let mut i = 0usize;
            let mut hit = false;
            while i < body.len() {
                if i + 2 < body.len() && body[i + 1] == '-' {
                    if c >= body[i] && c <= body[i + 2] {
                        hit = true;
                    }
                    i += 3;
                } else {
                    if c == body[i] {
                        hit = true;
                    }
                    i += 1;
                }
            }
            return hit != negate;
        }
        match atom {
            Some(a) => a == c,
            None => false,
        }
    };

    // 贪心吃 atom, 失败则逐步回退(模式很短, 不需要更聪明的引擎)
    let mut count = 0usize;
    while ti + count < t.len() && matches_atom(t[ti + count]) {
        count += 1;
        if let Some(m) = max {
            if count >= m {
                break;
            }
        }
    }
    if count < min {
        return None;
    }
    let mut k = count;
    loop {
        if let Some(end) = regex_lite_here(p, t, next_pi, ti + k) {
            return Some(end);
        }
        if k == min {
            return None;
        }
        k -= 1;
    }
}

// ================================================================== 自检钩子

/// `brickie-core --selftest` 用的纯函数入口(不碰磁盘: 由 `context.files` 注入 stat)。
pub fn selftest_plan(root: &Path, req: &Request) -> (Diags, Value) {
    let tree = model::load_tree(root);
    let mut diags = tree.diags.clone();
    match load_ctx(req, &tree) {
        Ok((ctx, mut load_diags)) => {
            let plan = make_plan(&ctx);
            let steps = all_steps(&ctx);
            load_diags.merge(diags);
            (
                load_diags,
                summary_json(
                    &ctx,
                    &plan,
                    &steps,
                    json!({"selftest": true, "state_content": plan.state_content}),
                ),
            )
        }
        Err(d) => {
            diags.merge(d);
            (diags, json!({}))
        }
    }
}

/// 让 `selftest` 也能单测正则子集。
pub fn selftest_regex(pat: &str, text: &str) -> bool {
    regex_lite_match(pat, text)
}

/// `selftest`: 正则子集之外的元字符(应被拒绝)。
pub fn selftest_regex_unsupported(pat: &str) -> Option<char> {
    regex_unsupported(pat)
}

/// `selftest`: 通配匹配(`*` 不跨 `/`)。
pub fn selftest_glob(pat: &str, name: &str) -> bool {
    glob_match(pat, name)
}

/// `selftest`: 直接渲染一个后端文件(不落盘), 用来钉住生成物的关键行。
pub fn selftest_render_backend(root: &Path, req: &Request, which: &str) -> String {
    let tree = model::load_tree(root);
    match load_ctx(req, &tree) {
        Ok((ctx, _)) => {
            let steps = all_steps(&ctx);
            if which == "make" {
                render_make(&ctx, &steps)
            } else {
                render_ninja(&ctx, &steps)
            }
        }
        Err(_) => String::new(),
    }
}

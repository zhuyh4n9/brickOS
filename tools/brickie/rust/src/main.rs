//! `brickie-core` — brickie v0.1 的 L0(模型)+ L1(求解/版本/接口引擎)。
//!
//! 进程边界(contract §2): 一行 JSON 请求从 stdin 读入, 一行 JSON 响应从 stdout 写出。
//! `--selftest` **不**走信封(§5.3 / V-18a); `--version` 打印版本。
//!
//! 进程退出码: 恒为 0(**除** `internal_error` 与崩溃) —— 业务退出码在响应的
//! `exit_code` 里, 这样"工具故障"与"校验红"可分辨(BRV-D9)。

// 本 crate 有一部分 API 是**给 agent-B 的复用原语**(见 rust/README.md): 本刀
// 尚未有调用点, 但已导出并求稳定(区间/推进表/发射原语/规则谓词)。故整体豁免
// dead_code —— 不是"没用的代码", 而是"跨模块契约的一部分"。
#![allow(dead_code)]

mod check;
mod dep;
mod diag;
mod emit;
mod iface;
mod model;
mod plan;
mod proto;
mod rules;
mod selftest;
mod solver;
mod version;
mod build;

use std::io::Read;
use std::path::PathBuf;

use serde_json::json;

use crate::proto::{Request, Response};

/// 工具版本(v0.1)。
pub const VERSION: &str = "0.1.0";

/// 构建期捕获的 `rustc` 版本(取不到 = "unknown")。
pub const RUSTC_VERSION: &str = match option_env!("BRICKIE_RUSTC_VERSION") {
    Some(v) => v,
    None => "unknown",
};

fn print_help() {
    println!(
        "brickie-core {VERSION} (protocol {})\n\
         用法: brickie-core [--selftest | --version | --help]\n\
         \n\
         无参数时从 stdin 读一行 JSON 信封, 向 stdout 写一行 JSON 响应:\n\
         \x20 请求  {{protocol, command, root, args, context}}\n\
         \x20 响应  {{protocol, status, exit_code, diagnostics, files, data}}\n\
         \n\
         命令面(v0.1 全通, contract §5):\n\
         \x20 只读  version / model / check / closure / dep-tree / dep-graph / dep-why /\n\
         \x20       dep-index / ver-show / iface-list / iface-show / iface-diff / iface-status\n\
         \x20 写入  plan-new / plan-init / gen-plan / dep-add / dep-rm / ver-bump /\n\
         \x20       iface-publish / iface-freeze / iface-deprecate / iface-undeprecate /\n\
         \x20       iface-unfreeze / iface-refreeze\n\
         \n\
         业务退出码在响应的 exit_code 里(0 成功 / 1 校验红 / 2 用法或环境错)。\n",
        proto::PROTOCOL
    );
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    if !argv.is_empty() {
        for a in &argv {
            match a.as_str() {
                "--version" | "-V" => {
                    println!("brickie-core {VERSION} (protocol {})", proto::PROTOCOL);
                    return;
                }
                "--selftest" => {
                    let code = selftest::run();
                    std::process::exit(code);
                }
                "--help" | "-h" => {
                    print_help();
                    return;
                }
                other => {
                    eprintln!("brickie-core: 未知参数 `{other}`");
                    print_help();
                    std::process::exit(2);
                }
            }
        }
    }

    let mut input = String::new();
    if let Err(e) = std::io::stdin().read_to_string(&mut input) {
        let r = Response::internal_error(&format!("读 stdin 失败: {e}"), "");
        r.write_stdout();
        std::process::exit(2);
    }
    if input.trim().is_empty() {
        let r = Response::internal_error(
            "stdin 为空: 需要一个 JSON 请求对象",
            "用法: echo '{\"protocol\":1,\"command\":\"version\",\"root\":\".\"}' | brickie-core",
        );
        r.write_stdout();
        std::process::exit(2);
    }

    let req = match proto::parse_request(&input) {
        Ok(r) => r,
        Err(e) => {
            let r = Response::internal_error(&format!("请求不可解析: {e}"), "contract §2");
            r.write_stdout();
            std::process::exit(2);
        }
    };

    // 协议握手: 版本不匹配 ⇒ BRV-PROTO-0001 / exit_code 2 / status ok。
    if req.protocol != Some(proto::PROTOCOL) {
        let r = Response::proto_mismatch(req.protocol);
        r.write_stdout();
        return;
    }

    let response = dispatch(&req);
    // 业务退出码在响应里; 进程退出码保持 0(除非 internal_error)。
    let internal = response.status == "internal_error";
    response.write_stdout();
    if internal {
        std::process::exit(2);
    }
}

fn dispatch(req: &Request) -> Response {
    match req.command.as_str() {
        "version" => cmd_version(),
        "model" => cmd_model(req),
        "plan-new" => cmd_plan_new(req),
        "plan-init" => cmd_plan_init(req),
        "gen-plan" => cmd_gen_plan(req),
        "dep-add" => cmd_dep_add(req),
        "dep-rm" => cmd_dep_rm(req),
        // ---- 求解 / 校验 / 接口面(agent-B 的三条命令族) ----
        "check" => check::run(req),
        "closure" | "dep-tree" | "dep-graph" | "dep-why" | "dep-index" => solver::run(req),
        "ver-show" | "ver-bump" => iface::run(req),
        c if c.starts_with("iface-") => iface::run(req),
        // ---- 构建族(ADR-0004 / 设计 ADR-0003 的 S1–S3) ----
        "build" | "clean" | "run" | "size" | "disasm" | "test" | "judge" => build::run(req),
        other => {
            let mut d = diag::Diags::new();
            d.usage(
                format!("未知命令 `{other}`"),
                "v0.1 的命令面见 contract §5; 23 条叶子命令 + version/model 均已实现",
            );
            Response::ok(d, json!({}))
        }
    }
}

// ------------------------------------------------------------------ 已实现命令

fn cmd_version() -> Response {
    let d = diag::Diags::new();
    Response::ok(
        d,
        json!({
            "version": VERSION,
            "protocol": proto::PROTOCOL,
            "languages": [{"name": "rust", "version": RUSTC_VERSION}],
            // G-01 `--version --deps` 的指纹: 本 crate 的直接依赖(§9.3 最小依赖集)。
            "deps": [
                {"name": "toml", "version": "0.8"},
                {"name": "serde_json", "version": "1"},
                {"name": "sha2", "version": "0.10"},
            ],
        }),
    )
}

fn cmd_model(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let only = req.arg_str("plugin").filter(|s| !s.is_empty());
    let plugins: Vec<serde_json::Value> = tree
        .plugins
        .iter()
        .filter(|p| only.as_deref().map(|n| n == p.name).unwrap_or(true))
        .map(|p| p.to_json())
        .collect();
    let (cap_ram, cap_stack) = tree.platform_capacity();
    let data = json!({
        "plugins": plugins,
        "count": plugins.len(),
        "product": tree.product.as_ref().map(|p| p.to_json()),
        "has_product": tree.has_product,
        "platform_capacity": {"ram_kib": cap_ram, "stack_kib": cap_stack},
    });
    Response::ok(tree.diags, data)
}

fn cmd_plan_new(req: &Request) -> Response {
    let (d, artifacts) = plan::plan_new(&req.args, &req.context);
    Response::ok(d, plan::artifacts_data(&artifacts))
}

fn cmd_plan_init(req: &Request) -> Response {
    let (d, artifacts, lock_content) = plan::plan_init(&req.args, &req.context);
    let mut data = plan::artifacts_data(&artifacts);
    if let Some(obj) = data.as_object_mut() {
        obj.insert("lock_content".into(), json!(lock_content));
    }
    Response::ok(d, data)
}

fn cmd_gen_plan(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let (d, artifacts) = plan::gen_plan(&root, &req.args);
    Response::ok(d, plan::artifacts_data(&artifacts))
}

fn cmd_dep_add(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (d, snippet) = dep::dep_add(&tree, &req.args);
    // 树的形状诊断一并呈现(片段生成不因此中止, 但人要看得见)。
    let mut all = tree.diags.clone();
    all.merge(d);
    let data = snippet.map(|s| s.to_data()).unwrap_or_else(|| json!({}));
    Response::ok(all, data)
}

fn cmd_dep_rm(req: &Request) -> Response {
    let root = PathBuf::from(&req.root);
    let tree = model::load_tree(&root);
    let (d, snippet) = dep::dep_rm(&tree, &req.args);
    let mut all = tree.diags.clone();
    all.merge(d);
    let data = snippet.map(|s| s.to_data()).unwrap_or_else(|| json!({}));
    Response::ok(all, data)
}

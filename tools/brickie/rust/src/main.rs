//! `brickie-core` — brickie v0.1 L0 (model) + L1 (solver / version / interface engines).
//! One JSON request line on stdin, one JSON response line on stdout; `--selftest` bypasses the
//! envelope. Process exit code is always 0 except `internal_error` and crashes: business exit
//! codes live in the response's `exit_code`, so "tool failure" ≠ "validation red" (BRV-D9).

// Reusable primitives for agent-B (rust/README.md), exported ahead of their call sites.
#![allow(dead_code)]

mod audit;
mod build;
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
mod show;
mod solver;
mod srcscan;
mod version;

use std::io::Read;
use std::path::PathBuf;

use serde_json::json;

use crate::proto::{Request, Response};

pub const VERSION: &str = "0.1.0";

/// `rustc` version captured at build time; "unknown" if unavailable.
pub const RUSTC_VERSION: &str = match option_env!("BRICKIE_RUSTC_VERSION") {
    Some(v) => v,
    None => "unknown",
};

fn print_help() {
    println!(
        "brickie-core {VERSION} (protocol {})\n\
         usage: brickie-core [--selftest | --version | --help]\n\
         \n\
         With no arguments, reads one JSON envelope line from stdin and writes one JSON response line to stdout:\n\
         \x20 request   {{protocol, command, root, args, context}}\n\
         \x20 response  {{protocol, status, exit_code, diagnostics, files, data}}\n\
         \n\
         Command surface (v0.1 all pass, contract §5):\n\
         \x20 read-only  version / model / check / closure / dep-tree / dep-graph / dep-why /\n\
         \x20            dep-index / show-bricks / show-init-flow / ver-show / iface-list /\n\
         \x20            iface-show / iface-diff / iface-status\n\
         \x20 write      plan-new / plan-init / gen-plan / dep-add / dep-rm / ver-bump /\n\
         \x20            iface-publish / iface-freeze / iface-deprecate / iface-undeprecate /\n\
         \x20            iface-unfreeze / iface-refreeze\n\
         \n\
         Business exit codes live in the response's exit_code (0 success / 1 validation red / 2 usage or environment error).\n",
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
                    eprintln!("brickie-core: unknown argument `{other}`");
                    print_help();
                    std::process::exit(2);
                }
            }
        }
    }

    let mut input = String::new();
    if let Err(e) = std::io::stdin().read_to_string(&mut input) {
        let r = Response::internal_error(&format!("failed to read stdin: {e}"), "");
        r.write_stdout();
        std::process::exit(2);
    }
    if input.trim().is_empty() {
        let r = Response::internal_error(
            "stdin is empty: a JSON request object is required",
            "usage: echo '{\"protocol\":1,\"command\":\"version\",\"root\":\".\"}' | brickie-core",
        );
        r.write_stdout();
        std::process::exit(2);
    }

    let req = match proto::parse_request(&input) {
        Ok(r) => r,
        Err(e) => {
            let r = Response::internal_error(&format!("request not parseable: {e}"), "contract §2");
            r.write_stdout();
            std::process::exit(2);
        }
    };

    // Protocol handshake: version mismatch ⇒ BRV-PROTO-0001 / exit_code 2 / status ok.
    if req.protocol != Some(proto::PROTOCOL) {
        let r = Response::proto_mismatch(req.protocol);
        r.write_stdout();
        return;
    }

    let response = dispatch(&req);
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
        "check" => check::run(req),
        "closure" | "dep-tree" | "dep-graph" | "dep-why" | "dep-index" => solver::run(req),
        "show-bricks" | "show-init-flow" => show::run(req),
        "ver-show" | "ver-bump" => iface::run(req),
        c if c.starts_with("iface-") => iface::run(req),
        "build" | "clean" | "run" | "size" | "disasm" | "test" | "judge" => build::run(req),
        other => {
            let mut d = diag::Diags::new();
            d.usage(
                format!("unknown command `{other}`"),
                "see contract §5 for the v0.1 command surface; all 23 leaf commands + version/model are implemented",
            );
            Response::ok(d, json!({}))
        }
    }
}

fn cmd_version() -> Response {
    let d = diag::Diags::new();
    Response::ok(
        d,
        json!({
            "version": VERSION,
            "protocol": proto::PROTOCOL,
            "languages": [{"name": "rust", "version": RUSTC_VERSION}],
            // Fingerprint for G-01 `--version --deps`: direct crate dependencies.
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
    let (cap_ram, cap_stack) = tree.runtime_capacity();
    let data = json!({
        "plugins": plugins,
        "count": plugins.len(),
        "product": tree.product.as_ref().map(|p| p.to_json()),
        "has_product": tree.has_product,
        "runtime_capacity": {"ram_kib": cap_ram, "stack_kib": cap_stack},
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
    // Surface tree-shape diagnostics too; snippet generation does not stop for them.
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

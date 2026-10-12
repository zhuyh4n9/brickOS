// Build-time fingerprint: write the `rustc --version` number into an env var for
// the `version` command's `languages[*].version` (read via option_env!; "unknown" if absent).
//
// std only, no crates (§9.3 dependency discipline).
use std::process::Command;

fn main() {
    let version = Command::new("rustc")
        .arg("--version")
        .output()
        .ok()
        .filter(|o| o.status.success())
        .and_then(|o| String::from_utf8(o.stdout).ok())
        // "rustc 1.93.1 (hash date)" ⇒ take the second field
        .and_then(|s| s.split_whitespace().nth(1).map(|v| v.to_string()))
        .unwrap_or_else(|| "unknown".to_string());

    println!("cargo:rustc-env=BRICKIE_RUSTC_VERSION={}", version);
    println!("cargo:rerun-if-changed=build.rs");
}

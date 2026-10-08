// 构建期指纹: 把 `rustc --version` 的版本号写进环境变量, 供 `version` 命令的
// `languages[*].version` 使用(option_env! 读取; 取不到就是 "unknown")。
//
// 这里只用 std, 不引入任何 crate(§9.3 依赖纪律)。
use std::process::Command;

fn main() {
    let version = Command::new("rustc")
        .arg("--version")
        .output()
        .ok()
        .filter(|o| o.status.success())
        .and_then(|o| String::from_utf8(o.stdout).ok())
        // "rustc 1.93.1 (hash date)" ⇒ 取第二段
        .and_then(|s| s.split_whitespace().nth(1).map(|v| v.to_string()))
        .unwrap_or_else(|| "unknown".to_string());

    println!("cargo:rustc-env=BRICKIE_RUSTC_VERSION={}", version);
    println!("cargo:rerun-if-changed=build.rs");
}

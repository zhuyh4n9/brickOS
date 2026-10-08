//! 诊断模型(BRV-D8: 编号化 + 位置化 + 可机读)与退出码汇总(BRV-D9)。
//!
//! 每条诊断 = `{code, severity, target, file, span, message, hint}`。
//! - `code` = `None` ⇒ 无码诊断(用法/环境错), JSON 里写 `"code": null`。
//! - **严重度不决定退出码**(BRV-D8 注 / G-1): 退出码由"命令有没有完成"决定。
//!   `info`/`warning` 级诊断若代表命令无法完成, 用 [`Diags::shape`] / [`Diags::usage`]
//!   显式置退出码 2。
//!
//! 位置化: `file` 相对 `root`; `span` 用 TOML 路径(如 `export[0].entries[2].sig`)。
//!
//! `--json` 下诊断数组按 `(code, file, span, target)` **稳定排序**(契约 §3) ⇒ 快照可复现。

use serde_json::{json, Value};

/// 严重度。顺序即轻重, 用于需要比较的场合。
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Default)]
pub enum Severity {
    Info,
    Warning,
    #[default]
    Error,
}

impl Severity {
    pub fn as_str(self) -> &'static str {
        match self {
            Severity::Info => "info",
            Severity::Warning => "warning",
            Severity::Error => "error",
        }
    }
}

/// BRV-D9 的退出码档位。`2` 优先于 `1`(输入都没成形时谈不上"校验红")。
pub const EXIT_OK: i32 = 0;
pub const EXIT_RED: i32 = 1;
pub const EXIT_USAGE: i32 = 2;

/// 一条诊断。
#[derive(Clone, Debug, Default)]
pub struct Diag {
    pub code: Option<String>,
    pub severity: Severity,
    pub target: String,
    pub file: String,
    pub span: String,
    pub message: String,
    pub hint: String,
}

impl Diag {
    pub fn new(severity: Severity, message: impl Into<String>) -> Self {
        Diag {
            code: None,
            severity,
            target: String::new(),
            file: String::new(),
            span: String::new(),
            message: message.into(),
            hint: String::new(),
        }
    }

    pub fn code(mut self, code: impl Into<String>) -> Self {
        self.code = Some(code.into());
        self
    }

    pub fn target(mut self, t: impl Into<String>) -> Self {
        self.target = t.into();
        self
    }

    pub fn file(mut self, f: impl Into<String>) -> Self {
        self.file = f.into();
        self
    }

    pub fn span(mut self, s: impl Into<String>) -> Self {
        self.span = s.into();
        self
    }

    pub fn hint(mut self, h: impl Into<String>) -> Self {
        self.hint = h.into();
        self
    }

    pub fn to_json(&self) -> Value {
        json!({
            "code": self.code.clone().map(Value::String).unwrap_or(Value::Null),
            "severity": self.severity.as_str(),
            "target": self.target,
            "file": self.file,
            "span": self.span,
            "message": self.message,
            "hint": self.hint,
        })
    }

    /// 稳定排序键: `(code, file, span, target)`。无码诊断排在最前。
    fn sort_key(&self) -> (String, String, String, String) {
        (
            self.code.clone().unwrap_or_default(),
            self.file.clone(),
            self.span.clone(),
            self.target.clone(),
        )
    }
}

/// 诊断集合 + 退出码汇总。
#[derive(Clone, Debug, Default)]
pub struct Diags {
    items: Vec<Diag>,
    /// 显式退出码覆盖(命令未完成但只有 info/warning 时使用, 见 G-1)。
    exit_override: Option<i32>,
}

impl Diags {
    pub fn new() -> Self {
        Diags::default()
    }

    pub fn push(&mut self, d: Diag) {
        self.items.push(d);
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn items(&self) -> &[Diag] {
        &self.items
    }

    /// 带码诊断(BRV-D8 已分配码)。
    pub fn coded(
        &mut self,
        code: &str,
        severity: Severity,
        target: impl Into<String>,
        file: impl Into<String>,
        span: impl Into<String>,
        message: impl Into<String>,
        hint: impl Into<String>,
    ) {
        self.push(
            Diag::new(severity, message)
                .code(code)
                .target(target)
                .file(file)
                .span(span)
                .hint(hint),
        );
    }

    /// 声明面 / 输入**形状**错: 带码 + 显式退出码 2(命令无法完成)。
    ///
    /// 用于 contract §7 的字段表违例(缺必填 / 类型不符 / 枚举越界 / 版本串形状),
    /// 以及裁定 R-9 下"未分配码"的结构类缺陷。
    pub fn shape(
        &mut self,
        code: &str,
        target: impl Into<String>,
        file: impl Into<String>,
        span: impl Into<String>,
        message: impl Into<String>,
        hint: impl Into<String>,
    ) {
        self.coded(code, Severity::Error, target, file, span, message, hint);
        self.exit_override = Some(EXIT_USAGE);
    }

    /// 用法错(无码): 参数非法 / 未知命令 / 未知枚举。退出码 2。
    pub fn usage(&mut self, message: impl Into<String>, hint: impl Into<String>) {
        self.push(Diag::new(Severity::Error, message).hint(hint));
        self.exit_override = Some(EXIT_USAGE);
    }

    /// 环境错(无码): 文件读不到 / 不是合法 TOML / 目录不存在。退出码 2。
    pub fn env(&mut self, message: impl Into<String>, hint: impl Into<String>) {
        self.push(Diag::new(Severity::Error, message).hint(hint));
        self.exit_override = Some(EXIT_USAGE);
    }

    /// 命令未完成但只给 info/warning 时显式置退出码(G-1: `BRV-TAX-0014`)。
    pub fn set_exit_override(&mut self, code: i32) {
        self.exit_override = Some(code);
    }

    pub fn exit_override(&self) -> Option<i32> {
        self.exit_override
    }

    pub fn has_error(&self) -> bool {
        self.items.iter().any(|d| d.severity == Severity::Error)
    }

    /// BRV-D9 的汇总: 显式覆盖 > 有 error ⇒ 1 > 0。
    ///
    /// 注: `usage` / `env` / `shape` 已把覆盖置为 2, 因此"形状错 + 校验红"混合时
    /// 以 2 结尾 —— 输入都没成形时"校验红"没有意义。
    pub fn exit_code(&self) -> i32 {
        if let Some(c) = self.exit_override {
            return c;
        }
        if self.has_error() {
            EXIT_RED
        } else {
            EXIT_OK
        }
    }

    /// 按 `(code, file, span, target)` 稳定排序后的 JSON 数组。
    pub fn to_json(&self) -> Value {
        let mut sorted: Vec<&Diag> = self.items.iter().collect();
        sorted.sort_by(|a, b| a.sort_key().cmp(&b.sort_key()));
        Value::Array(sorted.into_iter().map(|d| d.to_json()).collect())
    }

    /// 统计三种严重度的条数(供 `check` 的 `summary`)。
    pub fn counts(&self) -> (usize, usize, usize) {
        let mut e = 0;
        let mut w = 0;
        let mut i = 0;
        for d in &self.items {
            match d.severity {
                Severity::Error => e += 1,
                Severity::Warning => w += 1,
                Severity::Info => i += 1,
            }
        }
        (e, w, i)
    }

    /// 合并另一个集合(退出码覆盖取更严重者: 2 > 1)。
    pub fn merge(&mut self, other: Diags) {
        self.items.extend(other.items);
        if let Some(c) = other.exit_override {
            let cur = self.exit_override.unwrap_or(EXIT_OK);
            self.exit_override = Some(cur.max(c));
        }
    }
}

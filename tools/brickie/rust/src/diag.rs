//! Diagnostic model (BRV-D8: coded + located + machine-readable) and exit-code summary (BRV-D9).
//!
//! Each diagnostic = `{code, severity, target, file, span, message, hint}`.
//! - `code` = `None` ⇒ uncoded diagnostic (usage/env error), JSON emits `"code": null`.
//! - **Severity does not decide the exit code** (BRV-D8 note / G-1): the exit code reflects
//!   whether the command completed. If `info`/`warning` diagnostics mean the command could not
//!   complete, use [`Diags::shape`] / [`Diags::usage`] to set exit code 2 explicitly.
//!
//! Location: `file` is relative to `root`; `span` is a TOML path (e.g. `export[0].entries[2].sig`).
//!
//! Under `--json` the diagnostic array is **stably sorted** by `(code, file, span, target)`
//! (contract §3) ⇒ reproducible snapshots.

use serde_json::{json, Value};

/// Severity. Order is increasing weight, for comparisons.
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

/// BRV-D9 exit-code tiers. `2` outranks `1` (no point reporting validation red when input is malformed).
pub const EXIT_OK: i32 = 0;
pub const EXIT_RED: i32 = 1;
pub const EXIT_USAGE: i32 = 2;

/// One diagnostic.
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

    /// Stable sort key: `(code, file, span, target)`. Uncoded diagnostics come first.
    fn sort_key(&self) -> (String, String, String, String) {
        (
            self.code.clone().unwrap_or_default(),
            self.file.clone(),
            self.span.clone(),
            self.target.clone(),
        )
    }
}

/// Diagnostic collection + exit-code summary.
#[derive(Clone, Debug, Default)]
pub struct Diags {
    items: Vec<Diag>,
    /// Explicit exit-code override (used when the command did not complete but only info/warning exist, see G-1).
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

    /// Coded diagnostic (code assigned by BRV-D8).
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

    /// Declaration-surface / input **shape** error: coded + explicit exit code 2 (command cannot complete).
    ///
    /// For contract §7 field-table violations (missing required / wrong type / enum out of range /
    /// version-string shape), and structural defects with no assigned code under ruling R-9.
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

    /// Usage error (uncoded): invalid argument / unknown command / unknown enum. Exit code 2.
    pub fn usage(&mut self, message: impl Into<String>, hint: impl Into<String>) {
        self.push(Diag::new(Severity::Error, message).hint(hint));
        self.exit_override = Some(EXIT_USAGE);
    }

    /// Environment error (uncoded): unreadable file / invalid TOML / missing directory. Exit code 2.
    pub fn env(&mut self, message: impl Into<String>, hint: impl Into<String>) {
        self.push(Diag::new(Severity::Error, message).hint(hint));
        self.exit_override = Some(EXIT_USAGE);
    }

    /// Explicitly set the exit code when the command did not complete but only info/warning exist (G-1: `BRV-TAX-0014`).
    pub fn set_exit_override(&mut self, code: i32) {
        self.exit_override = Some(code);
    }

    pub fn exit_override(&self) -> Option<i32> {
        self.exit_override
    }

    pub fn has_error(&self) -> bool {
        self.items.iter().any(|d| d.severity == Severity::Error)
    }

    /// BRV-D9 summary: explicit override > has error ⇒ 1 > 0.
    ///
    /// Note: `usage` / `env` / `shape` already set the override to 2, so a mix of shape errors and
    /// validation red ends as 2 — validation red is meaningless when the input is malformed.
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

    /// JSON array stably sorted by `(code, file, span, target)`.
    pub fn to_json(&self) -> Value {
        let mut sorted: Vec<&Diag> = self.items.iter().collect();
        sorted.sort_by(|a, b| a.sort_key().cmp(&b.sort_key()));
        Value::Array(sorted.into_iter().map(|d| d.to_json()).collect())
    }

    /// Count the three severities (for `check`'s `summary`).
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

    /// Merge another collection (exit-code override keeps the more severe: 2 > 1).
    pub fn merge(&mut self, other: Diags) {
        self.items.extend(other.items);
        if let Some(c) = other.exit_override {
            let cur = self.exit_override.unwrap_or(EXIT_OK);
            self.exit_override = Some(cur.max(c));
        }
    }
}

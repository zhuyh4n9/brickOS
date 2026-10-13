//! Diagnostic model (BRV-D8) and exit-code summary (BRV-D9). Each diagnostic =
//! `{code, severity, target, file, span, message, hint}`; `code = None` => uncoded (usage/env
//! error, emitted as `"code": null`). Severity does not decide the exit code -- use
//! `shape`/`usage` to force 2 when the command did not complete. `file` is relative to `root`;
//! `span` is a TOML path; `--json` entries are stably sorted by `(code, file, span, target)`.

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

/// BRV-D9 exit-code tiers. `2` outranks `1` (validation red is pointless on malformed input).
pub const EXIT_OK: i32 = 0;
pub const EXIT_RED: i32 = 1;
pub const EXIT_USAGE: i32 = 2;

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

#[derive(Clone, Debug, Default)]
pub struct Diags {
    items: Vec<Diag>,
    /// Exit-code override for commands that did not complete but only produced info/warnings (G-1).
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

    /// Shape error: coded + exit code 2 (bad field type/missing field/enum range/version string).
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

    /// Environment error (uncoded): unreadable file / invalid TOML / missing dir; exit code 2.
    pub fn env(&mut self, message: impl Into<String>, hint: impl Into<String>) {
        self.push(Diag::new(Severity::Error, message).hint(hint));
        self.exit_override = Some(EXIT_USAGE);
    }

    /// Force an exit code when only info/warning diagnostics exist (G-1: `BRV-TAX-0014`).
    pub fn set_exit_override(&mut self, code: i32) {
        self.exit_override = Some(code);
    }

    pub fn exit_override(&self) -> Option<i32> {
        self.exit_override
    }

    pub fn has_error(&self) -> bool {
        self.items.iter().any(|d| d.severity == Severity::Error)
    }

    /// BRV-D9 summary: explicit override > has-error => 1 > 0. `usage`/`env`/`shape` set the
    /// override to 2, so shape errors outrank validation red (meaningless on malformed input).
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

    pub fn to_json(&self) -> Value {
        let mut sorted: Vec<&Diag> = self.items.iter().collect();
        sorted.sort_by(|a, b| a.sort_key().cmp(&b.sort_key()));
        Value::Array(sorted.into_iter().map(|d| d.to_json()).collect())
    }

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

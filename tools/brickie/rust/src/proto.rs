//! Process-boundary envelope: one JSON request line on stdin, one response line on stdout.
//! A protocol version mismatch ⇒ `BRV-PROTO-0001`, `exit_code = 2`, `status = "ok"` (the envelope
//! itself was handled); business exit codes live in the response.

use serde_json::{json, Map, Value};

use crate::diag::{Diags, Severity, EXIT_USAGE};

pub const PROTOCOL: i64 = 1;

#[derive(Clone, Debug)]
pub struct Request {
    pub protocol: Option<i64>,
    pub command: String,
    pub root: String,
    /// Key-values in `args`; empty if not an object.
    pub args: Map<String, Value>,
    /// Key-values in `context`; empty if not an object.
    pub context: Map<String, Value>,
}

impl Request {
    pub fn arg(&self, key: &str) -> Option<&Value> {
        self.args.get(key)
    }

    pub fn arg_str(&self, key: &str) -> Option<String> {
        self.args.get(key).and_then(Value::as_str).map(str::to_string)
    }

    pub fn arg_bool(&self, key: &str) -> Option<bool> {
        self.args.get(key).and_then(Value::as_bool)
    }

    pub fn arg_u64(&self, key: &str) -> Option<u64> {
        self.args.get(key).and_then(Value::as_u64)
    }

    pub fn context(&self, key: &str) -> Option<&Value> {
        self.context.get(key)
    }
}

/// Parse the stdin request; `Err` = malformed envelope (environment error, exit code 2).
pub fn parse_request(input: &str) -> Result<Request, String> {
    let v: Value = serde_json::from_str(input).map_err(|e| format!("request is not valid JSON: {e}"))?;
    if !v.is_object() {
        return Err("request must be a JSON object".to_string());
    }
    let obj = v.as_object().expect("already verified to be an object");

    let protocol = obj.get("protocol").and_then(Value::as_i64);
    let command = obj
        .get("command")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let root = obj
        .get("root")
        .and_then(Value::as_str)
        .unwrap_or(".")
        .to_string();

    let args = obj
        .get("args")
        .and_then(Value::as_object)
        .cloned()
        .unwrap_or_default();
    let context = obj
        .get("context")
        .and_then(Value::as_object)
        .cloned()
        .unwrap_or_default();

    Ok(Request {
        protocol,
        command,
        root,
        args,
        context,
    })
}

#[derive(Clone, Debug)]
pub struct Response {
    pub protocol: i64,
    pub status: String,
    pub exit_code: i32,
    pub diagnostics: Value,
    pub files: Value,
    pub data: Value,
}

impl Response {
    /// Protocol handled successfully (even if `exit_code != 0`).
    pub fn ok(diags: Diags, data: Value) -> Self {
        Response {
            protocol: PROTOCOL,
            status: "ok".to_string(),
            exit_code: diags.exit_code(),
            diagnostics: diags.to_json(),
            files: Value::Array(Vec::new()),
            data,
        }
    }

    /// Protocol handled successfully + attached `files` (machine-owned file contents).
    pub fn ok_with_files(diags: Diags, files: Vec<Value>, data: Value) -> Self {
        Response {
            protocol: PROTOCOL,
            status: "ok".to_string(),
            exit_code: diags.exit_code(),
            diagnostics: diags.to_json(),
            files: Value::Array(files),
            data,
        }
    }

    /// Tool's own failure (environment error); process exit code is non-zero. Fallback diagnostics
    /// may use `code: null` (BRV-D8: only for unclassifiable fallback errors).
    pub fn internal_error(message: &str, hint: &str) -> Self {
        let mut d = Diags::new();
        d.push(crate::diag::Diag::new(Severity::Error, message).hint(hint));
        Response {
            protocol: PROTOCOL,
            status: "internal_error".to_string(),
            exit_code: EXIT_USAGE,
            diagnostics: d.to_json(),
            files: Value::Array(Vec::new()),
            data: json!({}),
        }
    }

    pub fn unimplemented(command: &str, owner: &str) -> Self {
        let mut d = Diags::new();
        d.usage(
            format!("command `{command}` is not implemented yet ({owner})"),
            "the v0.1 Rust core in this cut delivers only version/model/plan-new/plan-init/gen-plan/dep-add/dep-rm; other commands are in the schedule table in rust/README.md",
        );
        Response::ok(d, json!({}))
    }

    pub fn proto_mismatch(got: Option<i64>) -> Self {
        let mut d = Diags::new();
        d.push(
            crate::diag::Diag::new(Severity::Error, format!(
                "protocol version mismatch: expected protocol={PROTOCOL}, got {}",
                got.map(|v| v.to_string()).unwrap_or_else(|| "missing".to_string())
            ))
            .code("BRV-PROTO-0001")
            .hint("L5 handshake with L0/L1 failed; no automatic migration (contract §2)"),
        );
        d.set_exit_override(EXIT_USAGE);
        Response::ok(d, json!({}))
    }

    pub fn to_json(&self) -> Value {
        json!({
            "protocol": self.protocol,
            "status": self.status,
            "exit_code": self.exit_code,
            "diagnostics": self.diagnostics,
            "files": self.files,
            "data": self.data,
        })
    }

    pub fn write_stdout(&self) {
        println!("{}", serde_json::to_string(&self.to_json()).expect("JSON is always serializable"));
    }
}

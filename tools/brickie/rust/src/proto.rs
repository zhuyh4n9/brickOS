//! 进程边界信封(contract §2): 一行 JSON 从 stdin 读入, 一行 JSON 从 stdout 写出。
//!
//! 请求: `{protocol, command, root, args, context}`
//! 响应: `{protocol, status, exit_code, diagnostics, files, data}`
//!
//! 协议版本不匹配 ⇒ `BRV-PROTO-0001`、`exit_code = 2`、`status = "ok"`
//! (协议处理成功, 业务退出码在响应里)。

use serde_json::{json, Map, Value};

use crate::diag::{Diags, Severity, EXIT_USAGE};

/// 当前协议版本常量。
pub const PROTOCOL: i64 = 1;

/// 解析后的请求信封。
#[derive(Clone, Debug)]
pub struct Request {
    pub protocol: Option<i64>,
    pub command: String,
    pub root: String,
    /// `args` 里的键值(非对象时为不可变空表)。
    pub args: Map<String, Value>,
    /// `context` 里的键值(非对象时为不可变空表)。
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

/// 解析 stdin 的请求。`Err` = 连信封都不成形(环境错, 退出码 2)。
pub fn parse_request(input: &str) -> Result<Request, String> {
    let v: Value = serde_json::from_str(input).map_err(|e| format!("请求不是合法 JSON: {e}"))?;
    if !v.is_object() {
        return Err("请求必须是 JSON 对象".to_string());
    }
    let obj = v.as_object().expect("已验证是对象");

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

/// 响应信封。
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
    /// 协议处理成功(即使 `exit_code != 0`)。
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

    /// 协议处理成功 + 附带 `files`(机器拥有的文件内容, contract §4)。
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

    /// 工具自身故障(L5 抛环境错)。进程退出码非 0。
    ///
    /// 兜底诊断允许 `code: null`(BRV-D8: 仅"无法分类的兜底错误"才允许)。
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

    /// 未实现命令的统一答复: 用法错(退出码 2) + `code: null`, 见 README。
    pub fn unimplemented(command: &str, owner: &str) -> Self {
        let mut d = Diags::new();
        d.usage(
            format!("命令 `{command}` 尚未实现({owner})"),
            "v0.1 的 Rust 核心本刀只交付 version/model/plan-new/plan-init/gen-plan/dep-add/dep-rm; 其余命令见 rust/README.md 的排期表",
        );
        Response::ok(d, json!({}))
    }

    /// `BRV-PROTO-0001`: 协议版本不匹配。
    pub fn proto_mismatch(got: Option<i64>) -> Self {
        let mut d = Diags::new();
        d.push(
            crate::diag::Diag::new(Severity::Error, format!(
                "协议版本不匹配: 期望 protocol={PROTOCOL}, 收到 {}",
                got.map(|v| v.to_string()).unwrap_or_else(|| "缺失".to_string())
            ))
            .code("BRV-PROTO-0001")
            .hint("L5 与 L0/L1 的握手失败; 不自动迁移(契约 §2)"),
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

    /// 写出到 stdout: 一行 JSON。
    pub fn write_stdout(&self) {
        println!("{}", serde_json::to_string(&self.to_json()).expect("JSON 必可序列化"));
    }
}

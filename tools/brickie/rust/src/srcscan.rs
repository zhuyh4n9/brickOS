//! C source text scanner for the `src` check scope (ADR-0038). **No compiler, no preprocessor.**
//!
//! Two disciplines need a view of a single translation unit that the declaration surface cannot
//! give:
//!
//! * **R1 — a `.c` is self-contained**: every function prototype and every `extern` declaration in
//!   the file must have its definition in the *same* file (cross-TU declarations belong in a
//!   header, and a header is the declaration surface);
//! * **R2 — globals do not leave the plugin**: a file-scope object in a `.c` must be `static`, and
//!   a public header must not export an object at all.
//!
//! The scanner is deliberately textual: comments and string/char literals are blanked (line
//! continuity preserved), preprocessor lines are removed, and the file is cut into top-level
//! statements at brace depth 0. Known limits, registered in ADR-0038: a declaration inside
//! `#if 0` is still seen, a declaration produced by a macro is not seen at all, and a
//! multi-declarator statement (`int a, b;`) reports its last declarator. Those are the price of
//! "zero compilation dependency" (ruling R-13) — the precise half of the same discipline is the
//! symbol audit over the `.a` archives, which `brickie build` runs.

use std::collections::BTreeSet;

/// One declaration site (file scope, 1-based line).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Decl {
    pub name: String,
    pub line: usize,
    /// Normalized statement text (whitespace collapsed), for diagnostics.
    pub text: String,
}

/// Everything the two disciplines need from one `.c` (or one header).
#[derive(Clone, Debug, Default)]
pub struct FileScan {
    /// Function prototypes: the definition must exist in the same file (R1).
    pub prototypes: Vec<Decl>,
    /// `extern` declarations, function or object (R1: both need a definition here).
    pub externs: Vec<Decl>,
    /// The subset of `externs` that declares an **object** (R2: a header must not export one).
    pub extern_objects: Vec<Decl>,
    /// File-scope object definitions without `static` (R2).
    pub nonstatic_objects: Vec<Decl>,
    /// Names defined in this file: functions with a body, file-scope objects, extern definitions.
    pub defined: BTreeSet<String>,
    /// `#include <…>` / `#include "…"` with 1-based line numbers.
    pub includes: Vec<(usize, String)>,
}

/// C keywords that must never be taken for a declarator name.
const KEYWORDS: &[&str] = &[
    "void",
    "char",
    "short",
    "int",
    "long",
    "float",
    "double",
    "signed",
    "unsigned",
    "const",
    "volatile",
    "struct",
    "union",
    "enum",
    "static",
    "extern",
    "register",
    "auto",
    "inline",
    "restrict",
    "typedef",
    "sizeof",
    "if",
    "else",
    "for",
    "while",
    "do",
    "switch",
    "case",
    "return",
    "break",
    "continue",
    "goto",
    "default",
    "defined",
    "_Bool",
    "_Noreturn",
    "_Atomic",
    "asm",
    "__asm__",
    "__attribute__",
    "__typeof__",
    "typeof",
];

fn is_keyword(s: &str) -> bool {
    KEYWORDS.contains(&s)
}

/// Statements that declare no name this module cares about.
fn is_ignorable_head(s: &str) -> bool {
    let head = s
        .split(|c: char| c.is_whitespace() || c == '(')
        .next()
        .unwrap_or("");
    matches!(
        head,
        "typedef" | "struct" | "union" | "enum" | "asm" | "__asm__"
    ) || s.starts_with("_Static_assert")
}

/// Blank comments / string / char literals, preserving every newline and byte order so line
/// numbers stay exact. An unterminated literal recovers at the newline (C forbids a raw newline
/// inside a literal, so this only happens on broken input).
pub fn strip(text: &str) -> String {
    let b: Vec<char> = text.chars().collect();
    let mut out: Vec<char> = Vec::with_capacity(b.len());
    let n = b.len();
    let mut i = 0usize;
    // 'c' code, 'l' line comment, 'b' block comment, 's' string, 'h' char literal
    let mut st = 'c';
    while i < n {
        let c = b[i];
        match st {
            'c' => {
                if c == '/' && i + 1 < n && b[i + 1] == '/' {
                    st = 'l';
                    out.push(' ');
                    i += 2;
                } else if c == '/' && i + 1 < n && b[i + 1] == '*' {
                    st = 'b';
                    out.push(' ');
                    i += 2;
                } else if c == '"' {
                    st = 's';
                    out.push(' ');
                    i += 1;
                } else if c == '\'' {
                    st = 'h';
                    out.push(' ');
                    i += 1;
                } else {
                    out.push(c);
                    i += 1;
                }
            }
            'l' => {
                if c == '\n' {
                    st = 'c';
                    out.push('\n');
                } else {
                    out.push(' ');
                }
                i += 1;
            }
            'b' => {
                if c == '*' && i + 1 < n && b[i + 1] == '/' {
                    st = 'c';
                    out.push(' ');
                    out.push(' ');
                    i += 2;
                } else {
                    out.push(if c == '\n' { '\n' } else { ' ' });
                    i += 1;
                }
            }
            's' | 'h' => {
                if c == '\\' && i + 1 < n {
                    out.push(' ');
                    out.push(' ');
                    i += 2;
                } else if (st == 's' && c == '"') || (st == 'h' && c == '\'') {
                    st = 'c';
                    out.push(' ');
                    i += 1;
                } else if c == '\n' {
                    st = 'c';
                    out.push('\n');
                    i += 1;
                } else {
                    out.push(' ');
                    i += 1;
                }
            }
            _ => unreachable!(),
        }
    }
    out.into_iter().collect()
}

/// Drop preprocessor lines (`#…`), including `\`-continued ones. Every removed line becomes an
/// empty line so line numbers are unchanged.
fn strip_preprocessor(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut continued = false;
    for line in text.split_inclusive('\n') {
        let body = line.trim_end_matches(['\n', '\r']);
        let is_pp = continued || body.trim_start().starts_with('#');
        if is_pp {
            continued = body.trim_end().ends_with('\\');
            out.push('\n');
        } else {
            continued = false;
            out.push_str(line);
            if !line.ends_with('\n') {
                out.push('\n');
            }
        }
    }
    out
}

/// Whether a brace-opening head introduces a **type** body (so the text after `}` is a declarator
/// list belonging to the same statement).
fn is_type_head(head: &str) -> bool {
    let first = head.split_whitespace().next().unwrap_or("");
    if matches!(first, "struct" | "union" | "enum") {
        return true;
    }
    matches!(first, "typedef" | "static" | "const" | "volatile")
        && head
            .split_whitespace()
            .any(|w| matches!(w, "struct" | "union" | "enum"))
}

/// A trailing attribute macro (`} __packed;`) is not a declarator: no object starts with `__`
/// (the naming discipline forbids it, ADR-0038).
fn is_lone_attr(s: &str) -> bool {
    s.starts_with("__")
        && !s.contains(|c: char| c.is_whitespace() || c == '=' || c == '[' || c == '(')
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    /// Statement terminated with `;`.
    Semi,
    /// Statement header terminated with a `{` at depth 0 (function body, initializer, type body).
    Open,
}

struct Chunk {
    kind: Kind,
    text: String,
    line: usize,
    /// The `{`-head this declarator list belongs to said `static` (anonymous struct pools).
    static_hint: bool,
}

/// Cut text into top-level chunks: `;` at brace depth 0 ends a `Semi` chunk, `{` at depth 0 ends
/// an `Open` chunk (the braced body itself is skipped). Everything nested is dropped.
fn top_level_chunks(stripped: &str) -> Vec<Chunk> {
    let chars: Vec<char> = stripped.chars().collect();
    let n = chars.len();
    let mut out = Vec::new();
    let mut i = 0usize;
    let mut line = 1usize;
    let mut buf = String::new();
    let mut start_line = 1usize;
    let mut depth = 0i32;
    // `typedef struct { … } name_t;` declares a **type**, not an object: after the type body the
    // declarator list must not be read as a variable (ADR-0038 note). A bare `struct x { … } v;`
    // is still read as an object — only the `typedef` form sets this.
    let mut skip_declarators = false;
    // Head text of a *type* body (`struct {…}`), carried into the following declarator list so
    // `static struct { … } s_pool[N];` still reads as a `static` object (ADR-0038 note).
    let mut type_head = String::new();
    while i < n {
        let c = chars[i];
        match c {
            '\n' => {
                line += 1;
                if depth == 0 {
                    buf.push('\n');
                }
                i += 1;
            }
            '{' => {
                if depth == 0 {
                    let head = normalize(&buf);
                    skip_declarators = head.starts_with("typedef");
                    type_head = if is_type_head(&head) {
                        head.clone()
                    } else {
                        String::new()
                    };
                    out.push(Chunk {
                        kind: Kind::Open,
                        text: std::mem::take(&mut buf),
                        line: start_line,
                        static_hint: false,
                    });
                    start_line = line;
                }
                depth += 1;
                i += 1;
            }
            '}' => {
                if depth > 0 {
                    depth -= 1;
                }
                if depth == 0 {
                    buf.clear();
                    start_line = line;
                }
                i += 1;
            }
            ';' => {
                if depth == 0 {
                    let suffix = normalize(&std::mem::take(&mut buf));
                    let static_hint = type_head
                        .split_whitespace()
                        .next()
                        .map(|w| w == "static")
                        .unwrap_or(false);
                    type_head.clear();
                    if !skip_declarators && !is_lone_attr(&suffix) {
                        out.push(Chunk {
                            kind: Kind::Semi,
                            text: suffix,
                            line: start_line,
                            static_hint,
                        });
                    }
                    skip_declarators = false;
                    start_line = line;
                }
                i += 1;
            }
            _ => {
                if depth == 0 {
                    if buf.trim().is_empty() && !c.is_whitespace() {
                        start_line = line;
                    }
                    buf.push(c);
                }
                i += 1;
            }
        }
    }
    out
}

/// Collapse whitespace runs to one space.
fn normalize(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut prev_space = true;
    for c in s.chars() {
        if c.is_whitespace() {
            if !prev_space {
                out.push(' ');
            }
            prev_space = true;
        } else {
            out.push(c);
            prev_space = false;
        }
    }
    out.trim().to_string()
}

/// Identifiers in order (skipping numbers and punctuation).
fn idents(s: &str) -> Vec<&str> {
    let b: Vec<char> = s.chars().collect();
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < b.len() {
        if b[i].is_ascii_alphabetic() || b[i] == '_' {
            let start = i;
            while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == '_') {
                i += 1;
            }
            out.push(&s[start..i]);
        } else {
            i += 1;
        }
    }
    out
}

/// Declarator name of a function declarator (`... name ( args ) [attrs]`), or `None`.
///
/// Positional rule: a `[` before the first parameter list means the statement declares an object
/// (array) — C forbids a function returning an array, so this never misreads a function.
fn function_name(stmt: &str) -> Option<String> {
    let s = normalize(stmt);
    if s.is_empty() || is_ignorable_head(&s) || s.starts_with("extern") {
        return None;
    }
    let open = s.find('(')?;
    if s[..open].contains('[') {
        return None;
    }
    let chars: Vec<char> = s.chars().collect();
    let mut j = open;
    while j > 0 && chars[j - 1].is_whitespace() {
        j -= 1;
    }
    let end = j;
    while j > 0 && (chars[j - 1].is_ascii_alphanumeric() || chars[j - 1] == '_') {
        j -= 1;
    }
    if j == end {
        return None;
    }
    let name: String = chars[j..end].iter().collect();
    if is_keyword(&name) {
        return None;
    }
    let head = normalize(&chars[..j].iter().collect::<String>());
    if head.is_empty() || head.contains('=') {
        return None;
    }
    Some(name)
}

/// Declarator name of an object statement (`int x;`, `char *y[4];`, `FILE *z = NULL;`).
fn object_name(stmt: &str) -> Option<String> {
    let s = normalize(stmt);
    if s.is_empty() || is_ignorable_head(&s) {
        return None;
    }
    let cut = s.split('=').next().unwrap_or("");
    let ids: Vec<&str> = idents(cut).into_iter().filter(|t| !is_keyword(t)).collect();
    ids.last().map(|s| s.to_string())
}

fn is_static(stmt: &str) -> bool {
    let s = normalize(stmt);
    s.split_whitespace().next() == Some("static")
}

/// Scan one C file's text.
pub fn scan(text: &str) -> FileScan {
    let mut out = FileScan::default();
    // Includes come from the **original** text: `strip` blanks a `"…"` header name.
    for (idx, raw) in text.lines().enumerate() {
        let l = raw.trim_start();
        let Some(rest) = l.strip_prefix('#') else {
            continue;
        };
        let rest = rest.trim_start();
        let Some(rest) = rest.strip_prefix("include") else {
            continue;
        };
        let rest = rest.trim_start();
        let close = if rest.starts_with('<') {
            '>'
        } else if rest.starts_with('"') {
            '"'
        } else {
            continue;
        };
        if let Some(end) = rest[1..].find(close) {
            out.includes.push((idx + 1, rest[1..1 + end].to_string()));
        }
    }

    let stripped = strip_preprocessor(&strip(text));
    for ch in top_level_chunks(&stripped) {
        let text = normalize(&ch.text);
        if text.is_empty() {
            continue;
        }
        if text.starts_with("extern") {
            let rest = normalize(text.trim_start_matches("extern"));
            let as_func = function_name(&rest);
            let name = as_func.clone().or_else(|| object_name(&rest));
            if let Some(name) = name {
                let d = Decl {
                    name: name.clone(),
                    line: ch.line,
                    text: text.clone(),
                };
                out.externs.push(d.clone());
                if as_func.is_none() {
                    out.extern_objects.push(d);
                }
                // `extern int x = 5;` is a definition (initializer ⇒ definition).
                if text.contains('=') {
                    out.defined.insert(name);
                }
            }
            continue;
        }
        match ch.kind {
            Kind::Open => {
                if let Some(name) = function_name(&text) {
                    out.defined.insert(name);
                }
            }
            Kind::Semi => {
                if let Some(name) = function_name(&text) {
                    out.prototypes.push(Decl {
                        name,
                        line: ch.line,
                        text,
                    });
                } else if let Some(name) = object_name(&text) {
                    out.defined.insert(name.clone());
                    if !is_static(&text) && !ch.static_hint {
                        out.nonstatic_objects.push(Decl {
                            name,
                            line: ch.line,
                            text,
                        });
                    }
                }
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prototypes_need_definitions() {
        let s = scan("int f(void);\nint f(void) { return 0; }\n");
        assert_eq!(s.prototypes.len(), 1);
        assert!(s.defined.contains("f"));
    }

    #[test]
    fn static_forward_decl_then_def() {
        let s = scan("static int g(int);\nstatic int g(int x) { return x; }\n");
        assert_eq!(s.prototypes.len(), 1);
        assert!(s.defined.contains("g"));
    }

    #[test]
    fn extern_object_is_reported() {
        let s = scan("extern char __stack_top[];\n");
        assert_eq!(s.externs.len(), 1);
        assert!(!s.defined.contains("__stack_top"));
    }

    #[test]
    fn attribute_macro_after_array_is_not_a_function() {
        let s = scan("static br_u8 s[4] BR_ALIGN(16);\n");
        assert!(s.prototypes.is_empty());
        assert!(s.nonstatic_objects.is_empty());
    }

    #[test]
    fn nonstatic_global_is_reported() {
        let s = scan("int count;\nstatic int ok;\n");
        assert_eq!(s.nonstatic_objects.len(), 1);
        assert_eq!(s.nonstatic_objects[0].name, "count");
    }

    #[test]
    fn initializer_braces_do_not_confuse() {
        let s = scan("static const int t[] = { 1, 2 };\nint f(void) { return 0; }\n");
        assert!(s.prototypes.is_empty());
        assert!(s.nonstatic_objects.is_empty());
        assert!(s.defined.contains("f"));
    }

    #[test]
    fn comments_and_strings_are_blanked() {
        let s = scan("/* int f(void); */\n// int g(void);\nconst char *p = \"int h(void);\";\n");
        assert!(s.prototypes.is_empty());
    }

    #[test]
    fn typedef_body_is_not_an_object() {
        let s = scan("typedef struct { int a; } foo_t;\ntypedef int bar_t;\n");
        assert!(s.nonstatic_objects.is_empty(), "{:?}", s.nonstatic_objects);
    }

    #[test]
    fn struct_body_with_variable_is_an_object() {
        let s = scan("struct s { int a; } g;\n");
        assert_eq!(s.nonstatic_objects.len(), 1);
        assert_eq!(s.nonstatic_objects[0].name, "g");
    }

    #[test]
    fn anonymous_struct_pool_keeps_static() {
        let s = scan("static struct {\n int a;\n} s_pool[4];\n");
        assert!(s.nonstatic_objects.is_empty(), "{:?}", s.nonstatic_objects);
        assert!(s.defined.contains("s_pool"));
    }

    #[test]
    fn anonymous_struct_global_is_reported() {
        let s = scan("struct {\n int a;\n} g;\n");
        assert_eq!(s.nonstatic_objects.len(), 1);
        assert_eq!(s.nonstatic_objects[0].name, "g");
    }

    #[test]
    fn packed_attribute_after_struct_body_is_not_an_object() {
        let s = scan("struct x {\n int a;\n} __packed;\n");
        assert!(s.nonstatic_objects.is_empty(), "{:?}", s.nonstatic_objects);
    }

    #[test]
    fn includes_keep_quoted_names() {
        let s = scan("#include <br/core/br_log.h>\n#include \"x/y.h\"\n");
        assert_eq!(s.includes[0].1, "br/core/br_log.h");
        assert_eq!(s.includes[1].1, "x/y.h");
    }
}

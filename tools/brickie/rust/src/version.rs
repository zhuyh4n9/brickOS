//! 版本模型(§5)与区间语义(§7.4)。
//!
//! 段: `COMPAT_GEN . MAJOR . MINOR . REVISE`(四段均为非负整数)。
//! - 存储/比较: 无 `v` 前缀(`"0.1.0.0"`); **展示**加前缀(`v0.1.0.0`)。
//! - `COMPAT_GEN` 与 `MAJOR` 正交; 依赖对 `COMPAT_GEN` **精确匹配**,
//!   `range` **只比较 `MAJOR.MINOR.REVISE`**(固定 3 段, 缺段右补 0, 4 段非法)。
//! - `^` / `~` 在**解析期一次性展开**为显式区间(§7.4 的等价区间式)。
//! - 跨 `COMPAT_GEN` 比较无意义(§5.5): 字典序只用于同一代内。

use std::fmt;

/// 四段版本。
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Version {
    pub compat_gen: u64,
    pub major: u64,
    pub minor: u64,
    pub revise: u64,
}

impl Default for Version {
    fn default() -> Self {
        Version::new(0, 0, 0, 0)
    }
}

impl Version {
    pub const fn new(compat_gen: u64, major: u64, minor: u64, revise: u64) -> Self {
        Version {
            compat_gen,
            major,
            minor,
            revise,
        }
    }

    /// `MAJOR.MINOR.REVISE` 三元组。
    pub fn triple(&self) -> Triple {
        Triple(self.major, self.minor, self.revise)
    }

    /// 同代内字典序(跨代比较无意义, 见 §5.5)。
    pub fn cmp_same_gen(&self, other: &Version) -> std::cmp::Ordering {
        self.triple().cmp(&other.triple())
    }

    /// 加 `v` 前缀的展示形态。
    pub fn display_v(&self) -> String {
        format!("v{}", self)
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}.{}.{}.{}",
            self.compat_gen, self.major, self.minor, self.revise
        )
    }
}

/// 版本解析/区间解析的结构化错误: 携带 BRV-D8 的码。
#[derive(Clone, Debug)]
pub struct VerErr {
    pub code: &'static str,
    pub message: String,
}

impl VerErr {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        VerErr {
            code,
            message: message.into(),
        }
    }
}

/// 版本串段数错误 ⇒ `BRV-VER-0005`。
pub const VER_0005: &str = "BRV-VER-0005";
/// `range` 含 4 段 ⇒ `BRV-VER-0006`。
pub const VER_0006: &str = "BRV-VER-0006";
/// 版本回退 ⇒ `BRV-VER-0007`。
pub const VER_0007: &str = "BRV-VER-0007";

/// `MAJOR.MINOR.REVISE`(range 的作用域)。
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct Triple(pub u64, pub u64, pub u64);

impl fmt::Display for Triple {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.0, self.1, self.2)
    }
}

/// 解析四段版本串。**必须恰 4 段**, 缺/多/非数字均 ⇒ `BRV-VER-0005`。
pub fn parse_version(s: &str) -> Result<Version, VerErr> {
    let s = s.trim();
    let parts: Vec<&str> = s.split('.').collect();
    if parts.len() != 4 {
        return Err(VerErr::new(
            VER_0005,
            format!("版本串 `{s}` 应为 4 段 `COMPAT_GEN.MAJOR.MINOR.REVISE`, 实得 {} 段", parts.len()),
        ));
    }
    let mut n = [0u64; 4];
    for (i, p) in parts.iter().enumerate() {
        n[i] = p.parse::<u64>().map_err(|_| {
            VerErr::new(VER_0005, format!("版本串 `{s}` 第 {} 段 `{p}` 不是非负整数", i + 1))
        })?;
    }
    Ok(Version::new(n[0], n[1], n[2], n[3]))
}

/// 解析 `range` 里的一段 `M[.m[.r]]`: 缺段右补 0; 4 段 ⇒ `BRV-VER-0006`。
fn parse_triple(s: &str) -> Result<Triple, VerErr> {
    let parts: Vec<&str> = s.split('.').collect();
    match parts.len() {
        0 => Err(VerErr::new(VER_0006, format!("版本区间段 `{s}` 为空"))),
        4 => Err(VerErr::new(
            VER_0006,
            format!("版本区间段 `{s}` 含 4 段 —— range 只比较 `MAJOR.MINOR.REVISE`(§7.4)"),
        )),
        n if n > 4 => Err(VerErr::new(
            VER_0006,
            format!("版本区间段 `{s}` 含 {n} 段 —— range 只比较 `MAJOR.MINOR.REVISE`(§7.4)"),
        )),
        n => {
            let mut v = [0u64; 3];
            for (i, p) in parts.iter().enumerate() {
                v[i] = p.parse::<u64>().map_err(|_| {
                    VerErr::new(VER_0006, format!("版本区间段 `{s}` 的 `{p}` 不是非负整数"))
                })?;
            }
            let _ = n;
            Ok(Triple(v[0], v[1], v[2]))
        }
    }
}

/// 区间比较算子。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RangeOp {
    Eq,
    Ge,
    Gt,
    Le,
    Lt,
}

impl RangeOp {
    pub fn as_str(self) -> &'static str {
        match self {
            RangeOp::Eq => "=",
            RangeOp::Ge => ">=",
            RangeOp::Gt => ">",
            RangeOp::Le => "<=",
            RangeOp::Lt => "<",
        }
    }
}

/// 一条区间子句。
#[derive(Clone, Copy, Debug)]
pub struct Clause {
    pub op: RangeOp,
    pub ver: Triple,
}

/// 区间表达式: 子句的**交集**(AND)。`*` = 空子句集 = 任意。
#[derive(Clone, Debug, Default)]
pub struct Range {
    pub clauses: Vec<Clause>,
    /// 源写法是 `*`(仅用于规范化输出)。
    pub star: bool,
}

impl Range {
    pub fn any() -> Self {
        Range {
            clauses: Vec::new(),
            star: true,
        }
    }

    pub fn is_any(&self) -> bool {
        self.clauses.is_empty()
    }

    /// 解析区间。运算符: `=`(可省) / `>=` / `>` / `<=` / `<` / `~` / `^` / `*`; `,` = 交集。
    ///
    /// `^` / `~` 在此**一次性展开**为显式区间:
    /// - `~M.m.r` ⇒ `>=M.m.r,<M.(m+1).0`
    /// - `^M.m.r` ⇒ `>=M.m.r,<(M+1).0.0`
    pub fn parse(s: &str) -> Result<Range, VerErr> {
        let s = s.trim();
        if s.is_empty() || s == "*" {
            return Ok(Range::any());
        }
        let mut range = Range {
            clauses: Vec::new(),
            star: false,
        };
        for raw in s.split(',') {
            let tok = raw.trim();
            if tok.is_empty() {
                return Err(VerErr::new(VER_0006, format!("版本区间 `{s}` 含空子句")));
            }
            if tok == "*" {
                continue; // 任意子句 = 无约束
            }
            let (op, rest) = if let Some(r) = tok.strip_prefix(">=") {
                (RangeOp::Ge, r)
            } else if let Some(r) = tok.strip_prefix("<=") {
                (RangeOp::Le, r)
            } else if let Some(r) = tok.strip_prefix('>') {
                (RangeOp::Gt, r)
            } else if let Some(r) = tok.strip_prefix('<') {
                (RangeOp::Lt, r)
            } else if let Some(r) = tok.strip_prefix('=') {
                (RangeOp::Eq, r)
            } else if let Some(r) = tok.strip_prefix('~') {
                (RangeOp::Ge, r) // 展开见下(`~` 先按 >= 起, 再补上界)
            } else if let Some(r) = tok.strip_prefix('^') {
                (RangeOp::Ge, r) // 同上
            } else {
                (RangeOp::Eq, tok)
            };
            let ver = parse_triple(rest.trim())?;
            let expand_tilde = tok.starts_with('~');
            let expand_caret = tok.starts_with('^');
            range.clauses.push(Clause { op, ver });
            if expand_tilde {
                range.clauses.push(Clause {
                    op: RangeOp::Lt,
                    ver: Triple(ver.0, ver.1 + 1, 0),
                });
            } else if expand_caret {
                range.clauses.push(Clause {
                    op: RangeOp::Lt,
                    ver: Triple(ver.0 + 1, 0, 0),
                });
            }
        }
        Ok(range)
    }

    /// 三元组是否落在区间内(全部子句满足)。
    pub fn matches(&self, t: Triple) -> bool {
        self.clauses.iter().all(|c| match c.op {
            RangeOp::Eq => t == c.ver,
            RangeOp::Ge => t >= c.ver,
            RangeOp::Gt => t > c.ver,
            RangeOp::Le => t <= c.ver,
            RangeOp::Lt => t < c.ver,
        })
    }

    /// 区间交集: 子句并集(合取)。
    pub fn intersect(&self, other: &Range) -> Range {
        let mut clauses = self.clauses.clone();
        clauses.extend(other.clauses.iter().copied());
        Range {
            clauses,
            star: false,
        }
    }

    /// 区间是否可满足(存在至少一个三元组)。求解器据此判"区间冲突"。
    pub fn is_satisfiable(&self) -> bool {
        // 有 Eq 子句时, 该点必须满足全部子句。
        for c in &self.clauses {
            if c.op == RangeOp::Eq {
                return self.matches(c.ver);
            }
        }
        // 下界: 取最大者; 相等时严格者更强。
        let mut lower: Option<(Triple, bool)> = None;
        let mut upper: Option<(Triple, bool)> = None;
        for c in &self.clauses {
            match c.op {
                RangeOp::Ge => lower = stronger_lower(lower, (c.ver, false)),
                RangeOp::Gt => lower = stronger_lower(lower, (c.ver, true)),
                RangeOp::Le => upper = stronger_upper(upper, (c.ver, false)),
                RangeOp::Lt => upper = stronger_upper(upper, (c.ver, true)),
                RangeOp::Eq => unreachable!("Eq 已在上方处理"),
            }
        }
        let lo = lower.unwrap_or((Triple(0, 0, 0), false));
        let hi = match upper {
            None => return true, // 上无界
            Some(h) => h,
        };
        match lo.0.cmp(&hi.0) {
            std::cmp::Ordering::Less => true,
            std::cmp::Ordering::Greater => false,
            std::cmp::Ordering::Equal => !lo.1 && !hi.1,
        }
    }

    /// 规范化输出: **展开后**的显式区间(避免两种真值, §7.4)。
    pub fn canonical(&self) -> String {
        if self.clauses.is_empty() {
            return "*".to_string();
        }
        let mut parts = Vec::with_capacity(self.clauses.len());
        for c in &self.clauses {
            parts.push(format!("{}{}", c.op.as_str(), c.ver));
        }
        parts.join(",")
    }
}

impl fmt::Display for Range {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.canonical())
    }
}

fn stronger_lower(cur: Option<(Triple, bool)>, cand: (Triple, bool)) -> Option<(Triple, bool)> {
    match cur {
        None => Some(cand),
        Some((t, strict)) => match cand.0.cmp(&t) {
            std::cmp::Ordering::Greater => Some(cand),
            std::cmp::Ordering::Less => Some((t, strict)),
            std::cmp::Ordering::Equal => Some((t, strict || cand.1)),
        },
    }
}

fn stronger_upper(cur: Option<(Triple, bool)>, cand: (Triple, bool)) -> Option<(Triple, bool)> {
    match cur {
        None => Some(cand),
        Some((t, strict)) => match cand.0.cmp(&t) {
            std::cmp::Ordering::Less => Some(cand),
            std::cmp::Ordering::Greater => Some((t, strict)),
            std::cmp::Ordering::Equal => Some((t, strict || cand.1)),
        },
    }
}

/// `compat_gen` **精确匹配**(§5.5: 跨代比较无意义)。
pub fn compat_gen_matches(required: u64, actual: u64) -> bool {
    required == actual
}

// ------------------------------------------------------------------ §5.2 推进表

/// §5.2「事件 → 段」推进表的事件枚举。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    /// 改/删已有 `frozen` 条目(经解冻 → 重新冻结): `COMPAT_GEN+1`, `MINOR→0`, `REVISE→0`。
    CompatGenBump,
    /// 新增条目(append-only): `MINOR+1`, `REVISE→0`, `COMPAT_GEN` 不动。
    Added,
    /// 追加枚举成员 / 预留槽位填充: 同上。
    Extended,
    /// 治理状态转移(`experimental→frozen` 等): `MINOR+1`。
    StatusTransfer,
    /// 改已有全部 `experimental` 条目: `MINOR+1`。
    ChangeAllExperimental,
    /// 删已有仅 `experimental` 条目: `MINOR+1`。
    RemoveExperimental,
    /// 空解冻(未改/删任何已冻结条目): 四段全不动。
    EmptyUnfreeze,
    /// 重大产品版本: `MAJOR+1`, `MINOR→0`, `REVISE→0`, `COMPAT_GEN` 不动。
    MajorProduct,
    /// 小功能(面未动): `MINOR+1`, `REVISE→0`。
    MinorFeature,
    /// 修 bug / 改文档(面未动): `REVISE+1`。
    ReviseFix,
    /// 面完全未变(冗余重跑): 空操作。
    NoChange,
}

/// 按 §5.2 推进表计算新版本。
pub fn advance(v: Version, e: Event) -> Version {
    match e {
        Event::CompatGenBump => Version::new(v.compat_gen + 1, v.major, 0, 0),
        Event::Added | Event::Extended | Event::StatusTransfer | Event::ChangeAllExperimental
        | Event::RemoveExperimental | Event::MinorFeature => {
            Version::new(v.compat_gen, v.major, v.minor + 1, 0)
        }
        Event::EmptyUnfreeze | Event::NoChange => v,
        Event::MajorProduct => Version::new(v.compat_gen, v.major + 1, 0, 0),
        Event::ReviseFix => Version::new(v.compat_gen, v.major, v.minor, v.revise + 1),
    }
}

/// 单调不回退(§5.2): `compat_gen` 不得下降; 同代内 `(M,m,r)` 不得下降。
/// 跨代不存在"更新/更旧" ⇒ 只比 `compat_gen`。
pub fn check_no_regress(old: Version, new: Version) -> Result<(), VerErr> {
    if new.compat_gen > old.compat_gen {
        return Ok(());
    }
    if new.compat_gen < old.compat_gen {
        return Err(VerErr::new(
            VER_0007,
            format!("版本回退: compat_gen 由 {} 降到 {}", old.compat_gen, new.compat_gen),
        ));
    }
    if new.triple() < old.triple() {
        return Err(VerErr::new(
            VER_0007,
            format!(
                "版本回退: 同代内 {} 低于 {}",
                new.triple(),
                old.triple()
            ),
        ));
    }
    Ok(())
}

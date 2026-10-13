//! Version model: `COMPAT_GEN.MAJOR.MINOR.REVISE`, all non-negative integers.
//! Storage/compare omit the `v` prefix; display adds it (`v0.1.0.0`). Dependencies require an
//! exact `COMPAT_GEN` match; `range` compares only `MAJOR.MINOR.REVISE` (3 segments, missing ones
//! zero-filled on the right, 4 illegal), and `^`/`~` expand once at parse time into explicit
//! intervals. Order is meaningful only within one generation.

use std::fmt;

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

    pub fn triple(&self) -> Triple {
        Triple(self.major, self.minor, self.revise)
    }

    pub fn cmp_same_gen(&self, other: &Version) -> std::cmp::Ordering {
        self.triple().cmp(&other.triple())
    }

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

/// Structured version/range parse error: carries a BRV-D8 code.
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

/// Wrong number of version-string segments ⇒ `BRV-VER-0005`.
pub const VER_0005: &str = "BRV-VER-0005";
/// `range` contains 4 segments ⇒ `BRV-VER-0006`.
pub const VER_0006: &str = "BRV-VER-0006";
/// Version regression ⇒ `BRV-VER-0007`.
pub const VER_0007: &str = "BRV-VER-0007";

/// `MAJOR.MINOR.REVISE` (the scope of `range`).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct Triple(pub u64, pub u64, pub u64);

impl fmt::Display for Triple {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.0, self.1, self.2)
    }
}

pub fn parse_version(s: &str) -> Result<Version, VerErr> {
    let s = s.trim();
    let parts: Vec<&str> = s.split('.').collect();
    if parts.len() != 4 {
        return Err(VerErr::new(
            VER_0005,
            format!("version string `{s}` must have 4 segments `COMPAT_GEN.MAJOR.MINOR.REVISE`, got {} segments", parts.len()),
        ));
    }
    let mut n = [0u64; 4];
    for (i, p) in parts.iter().enumerate() {
        n[i] = p.parse::<u64>().map_err(|_| {
            VerErr::new(VER_0005, format!("version string `{s}` segment {} `{p}` is not a non-negative integer", i + 1))
        })?;
    }
    Ok(Version::new(n[0], n[1], n[2], n[3]))
}

fn parse_triple(s: &str) -> Result<Triple, VerErr> {
    let parts: Vec<&str> = s.split('.').collect();
    match parts.len() {
        0 => Err(VerErr::new(VER_0006, format!("version range segment `{s}` is empty"))),
        4 => Err(VerErr::new(
            VER_0006,
            format!("version range segment `{s}` has 4 segments — range compares only `MAJOR.MINOR.REVISE` (§7.4)"),
        )),
        n if n > 4 => Err(VerErr::new(
            VER_0006,
            format!("version range segment `{s}` has {n} segments — range compares only `MAJOR.MINOR.REVISE` (§7.4)"),
        )),
        n => {
            let mut v = [0u64; 3];
            for (i, p) in parts.iter().enumerate() {
                v[i] = p.parse::<u64>().map_err(|_| {
                    VerErr::new(VER_0006, format!("version range segment `{s}`: `{p}` is not a non-negative integer"))
                })?;
            }
            let _ = n;
            Ok(Triple(v[0], v[1], v[2]))
        }
    }
}

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

/// One range clause.
#[derive(Clone, Copy, Debug)]
pub struct Clause {
    pub op: RangeOp,
    pub ver: Triple,
}

/// Range expression: **intersection** (AND) of clauses. `*` = empty clause set = anything.
#[derive(Clone, Debug, Default)]
pub struct Range {
    pub clauses: Vec<Clause>,
    /// Source was `*` (used only for canonical output).
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

    /// Parse a range. Operators: `=` (may be omitted) / `>=` / `>` / `<=` / `<` / `~` / `^` / `*`; `,` = intersection.
    ///
    /// `^` / `~` are **expanded once here** into explicit intervals:
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
                return Err(VerErr::new(VER_0006, format!("version range `{s}` contains an empty clause")));
            }
            if tok == "*" {
                continue; // "any" clause = no constraint
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
                (RangeOp::Ge, r) // expansion below (`~` starts as >=, then the upper bound is added)
            } else if let Some(r) = tok.strip_prefix('^') {
                (RangeOp::Ge, r) // same as above
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

    /// Whether the triple falls inside the range (all clauses satisfied).
    pub fn matches(&self, t: Triple) -> bool {
        self.clauses.iter().all(|c| match c.op {
            RangeOp::Eq => t == c.ver,
            RangeOp::Ge => t >= c.ver,
            RangeOp::Gt => t > c.ver,
            RangeOp::Le => t <= c.ver,
            RangeOp::Lt => t < c.ver,
        })
    }

    /// Range intersection: union of clauses (conjunction).
    pub fn intersect(&self, other: &Range) -> Range {
        let mut clauses = self.clauses.clone();
        clauses.extend(other.clauses.iter().copied());
        Range {
            clauses,
            star: false,
        }
    }

    /// Whether the range is satisfiable (at least one triple exists). The solver uses this to detect "range conflicts".
    pub fn is_satisfiable(&self) -> bool {
        // With an Eq clause, that point must satisfy all clauses.
        for c in &self.clauses {
            if c.op == RangeOp::Eq {
                return self.matches(c.ver);
            }
        }
        // Lower bound: take the largest; on ties the strict one is stronger.
        let mut lower: Option<(Triple, bool)> = None;
        let mut upper: Option<(Triple, bool)> = None;
        for c in &self.clauses {
            match c.op {
                RangeOp::Ge => lower = stronger_lower(lower, (c.ver, false)),
                RangeOp::Gt => lower = stronger_lower(lower, (c.ver, true)),
                RangeOp::Le => upper = stronger_upper(upper, (c.ver, false)),
                RangeOp::Lt => upper = stronger_upper(upper, (c.ver, true)),
                RangeOp::Eq => unreachable!("Eq handled above"),
            }
        }
        let lo = lower.unwrap_or((Triple(0, 0, 0), false));
        let hi = match upper {
            None => return true, // no upper bound
            Some(h) => h,
        };
        match lo.0.cmp(&hi.0) {
            std::cmp::Ordering::Less => true,
            std::cmp::Ordering::Greater => false,
            std::cmp::Ordering::Equal => !lo.1 && !hi.1,
        }
    }

    /// Canonical output: the **expanded** explicit interval (avoids two truths, §7.4).
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

pub fn compat_gen_matches(required: u64, actual: u64) -> bool {
    required == actual
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    CompatGenBump,
    Added,
    Extended,
    StatusTransfer,
    ChangeAllExperimental,
    RemoveExperimental,
    EmptyUnfreeze,
    MajorProduct,
    MinorFeature,
    ReviseFix,
    NoChange,
}

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

/// Monotonic: `compat_gen` must not decrease; within a generation `(M,m,r)` must not either.
pub fn check_no_regress(old: Version, new: Version) -> Result<(), VerErr> {
    if new.compat_gen > old.compat_gen {
        return Ok(());
    }
    if new.compat_gen < old.compat_gen {
        return Err(VerErr::new(
            VER_0007,
            format!("version regression: compat_gen dropped from {} to {}", old.compat_gen, new.compat_gen),
        ));
    }
    if new.triple() < old.triple() {
        return Err(VerErr::new(
            VER_0007,
            format!(
                "version regression: within the generation {} is lower than {}",
                new.triple(),
                old.triple()
            ),
        ));
    }
    Ok(())
}

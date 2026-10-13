//! `brickie-core --selftest`: domain self-test without Python (V-18a).
//!
//! Covers the version/range/name/phase/privilege/dependency/export rules, TOML model loading,
//! planning and emission, plus the check/iface/build command families.
//! All green => `ok <n> cases`; any failure => per-case FAIL lines, exit code 1.

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Map, Value};

use crate::build;
use crate::diag::Diags;
use crate::model;
use crate::plan;
use crate::rules;
use crate::version::{self, Event, Range, Triple, Version};

struct Harness {
    passed: usize,
    failures: Vec<String>,
}

impl Harness {
    fn new() -> Self {
        Harness {
            passed: 0,
            failures: Vec::new(),
        }
    }

    fn check(&mut self, cond: bool, what: impl Into<String>) {
        if cond {
            self.passed += 1;
        } else {
            self.failures.push(what.into());
        }
    }

    fn eq<T: PartialEq + std::fmt::Debug>(&mut self, got: T, want: T, what: &str) {
        if got == want {
            self.passed += 1;
        } else {
            self.failures
                .push(format!("{what}: got {got:?}, want {want:?}"));
        }
    }

    fn has_code(&mut self, d: &Diags, code: &str, what: &str) {
        let ok = d.items().iter().any(|x| x.code.as_deref() == Some(code));
        if ok {
            self.passed += 1;
        } else {
            self.failures.push(format!(
                "{what}: expected diagnostic `{code}`, got {:?}",
                d.items()
                    .iter()
                    .map(|x| x.code.clone().unwrap_or_else(|| "<null>".into()))
                    .collect::<Vec<_>>()
            ));
        }
    }

    /// Compare a JSON `Value` (serde_json's Index returns `&Value`, so compare by reference).
    fn j(&mut self, got: &Value, want: Value, what: &str) {
        if *got == want {
            self.passed += 1;
        } else {
            self.failures
                .push(format!("{what}: got {got:?}, want {want:?}"));
        }
    }

    fn exit_is(&mut self, d: &Diags, code: i32, what: &str) {
        if d.exit_code() == code {
            self.passed += 1;
        } else {
            self.failures
                .push(format!("{what}: exit_code = {}, want {code}", d.exit_code()));
        }
    }
}

fn args(v: Value) -> Map<String, Value> {
    v.as_object().cloned().unwrap_or_default()
}

fn ctx(v: Value) -> Map<String, Value> {
    v.as_object().cloned().unwrap_or_default()
}

fn tmpdir(tag: &str) -> PathBuf {
    let mut p = std::env::temp_dir();
    p.push(format!("brickie-core-selftest-{}-{tag}", std::process::id()));
    let _ = fs::remove_dir_all(&p);
    fs::create_dir_all(&p).expect("create temp dir");
    p
}

fn write(root: &Path, rel: &str, content: &str) {
    let p = root.join(rel);
    if let Some(parent) = p.parent() {
        fs::create_dir_all(parent).expect("create parent dir");
    }
    fs::write(p, content).expect("write file");
}

fn version_matrix(h: &mut Harness) {
    let v = Version::new(0, 1, 2, 3);
    let a = version::advance(v, Event::CompatGenBump);
    h.eq(
        a,
        Version::new(1, 1, 0, 0),
        "V-4(a) unfreeze + change frozen iface ⇒ COMPAT_GEN+1 only (minor/revise cleared)",
    );
    h.eq(a.major, v.major, "V-4(a) COMPAT_GEN and MAJOR are independent");
    let b = version::advance(v, Event::Added);
    h.eq(b, Version::new(0, 1, 3, 0), "V-4(b) added entry ⇒ COMPAT_GEN unchanged, MINOR+1");
    h.eq(version::advance(v, Event::Extended), Version::new(0, 1, 3, 0), "V-4 append enum member ⇒ MINOR+1");
    h.eq(
        version::advance(v, Event::StatusTransfer),
        Version::new(0, 1, 3, 0),
        "V-4 governance status transfer ⇒ MINOR+1",
    );
    h.eq(
        version::advance(v, Event::ChangeAllExperimental),
        Version::new(0, 1, 3, 0),
        "V-4 change all experimental entries ⇒ MINOR+1",
    );
    h.eq(
        version::advance(v, Event::RemoveExperimental),
        Version::new(0, 1, 3, 0),
        "V-4 remove experimental-only entry ⇒ MINOR+1",
    );
    let c = version::advance(v, Event::MajorProduct);
    h.eq(c, Version::new(0, 2, 0, 0), "V-4(c) major product version ⇒ MAJOR+1 only");
    h.eq(c.compat_gen, v.compat_gen, "V-4(c) MAJOR and COMPAT_GEN are independent");
    h.eq(version::advance(v, Event::MinorFeature), Version::new(0, 1, 3, 0), "V-4(d) minor feature ⇒ MINOR+1");
    h.eq(version::advance(v, Event::ReviseFix), Version::new(0, 1, 2, 4), "V-4(e) bug fix ⇒ REVISE+1");
    h.eq(version::advance(v, Event::EmptyUnfreeze), v, "V-4(f) empty unfreeze ⇒ all four segments unchanged");
    h.eq(version::advance(v, Event::NoChange), v, "surface fully unchanged ⇒ no-op");

    h.eq(
        version::parse_version("0.1.0.0").unwrap(),
        Version::new(0, 1, 0, 0),
        "four-segment version parsing",
    );
    h.eq(Version::new(1, 2, 3, 4).to_string(), "1.2.3.4".to_string(), "version serialization (no v)");
    h.eq(Version::new(1, 2, 3, 4).display_v(), "v1.2.3.4".to_string(), "version display (with v)");
    h.eq(
        version::parse_version("1.2.3").unwrap_err().code,
        "BRV-VER-0005",
        "3-segment version string ⇒ BRV-VER-0005",
    );
    h.check(version::parse_version("1.2.3.4.5").is_err(), "5-segment version string is illegal");
    h.check(version::parse_version("a.b.c.d").is_err(), "non-numeric version string is illegal");

    h.check(
        version::check_no_regress(Version::new(1, 0, 0, 0), Version::new(1, 0, 0, 1)).is_ok(),
        "increase within a generation is not a regression",
    );
    h.check(
        version::check_no_regress(Version::new(1, 0, 0, 0), Version::new(2, 0, 0, 0)).is_ok(),
        "cross-generation (COMPAT_GEN+1) is not a regression",
    );
    h.eq(
        version::check_no_regress(Version::new(1, 5, 0, 0), Version::new(1, 4, 9, 9))
            .unwrap_err()
            .code,
        "BRV-VER-0007",
        "regression within a generation ⇒ BRV-VER-0007",
    );
    h.eq(
        version::check_no_regress(Version::new(1, 5, 0, 0), Version::new(0, 9, 9, 9))
            .unwrap_err()
            .code,
        "BRV-VER-0007",
        "compat_gen decrease ⇒ BRV-VER-0007",
    );
}

fn range_cases(h: &mut Harness) {
    let ge = Range::parse(">=1.0.0").unwrap();
    h.check(
        ge.matches(Triple(1, 0, 0)) && ge.matches(Triple(2, 3, 4)) && !ge.matches(Triple(0, 9, 9)),
        ">=1.0.0 lexicographic comparison",
    );
    let tilde = Range::parse("~1.2.3").unwrap();
    h.eq(tilde.canonical(), ">=1.2.3,<1.3.0".to_string(), "~ expands to an explicit interval");
    h.check(tilde.matches(Triple(1, 2, 9)) && !tilde.matches(Triple(1, 3, 0)), "~ does not cross MINOR");
    let caret = Range::parse("^1.2.3").unwrap();
    h.eq(caret.canonical(), ">=1.2.3,<2.0.0".to_string(), "^ expands to an explicit interval");
    h.check(caret.matches(Triple(1, 9, 9)) && !caret.matches(Triple(2, 0, 0)), "^ does not cross MAJOR");
    let pad = Range::parse(">=1.0").unwrap();
    h.eq(pad.canonical(), ">=1.0.0".to_string(), "range pads missing segments with 0");
    h.eq(
        Range::parse(">=1.2.3.4").unwrap_err().code,
        "BRV-VER-0006",
        "4-segment range ⇒ BRV-VER-0006",
    );
    h.check(Range::parse("*").unwrap().matches(Triple(9, 9, 9)), "* = anything");
    h.check(Range::parse("=1.2.3").unwrap().matches(Triple(1, 2, 3)), "= exact");

    let a = Range::parse(">=1.0.0,<2.0.0").unwrap();
    let b = Range::parse(">=1.5.0").unwrap();
    let i = a.intersect(&b);
    h.check(
        i.is_satisfiable() && i.matches(Triple(1, 5, 0)) && !i.matches(Triple(2, 0, 0)),
        "interval intersection (AND)",
    );
    h.check(!Range::parse(">=2.0.0,<1.0.0").unwrap().is_satisfiable(), "empty intersection is unsatisfiable");
    h.check(
        !Range::parse(">=1.0.0,<1.0.0").unwrap().is_satisfiable(),
        "half-open interval with equal bounds is unsatisfiable",
    );
    h.check(
        Range::parse(">=1.0.0,<=1.0.0").unwrap().is_satisfiable(),
        "closed interval with equal bounds is satisfiable",
    );
    h.check(
        version::compat_gen_matches(3, 3) && !version::compat_gen_matches(3, 4),
        "compat_gen exact match (cross-generation comparison is meaningless)",
    );
}

fn name_and_subkind(h: &mut Harness) {
    for ok in [
        "service/crypto",
        "sched-coop",
        "sched/sched-coop",
        "iface-min",
        "app/hsm",
        "platform/qemu-aarch64",
        "framework/cdev-core",
        "driver/virtio-hsm",
        "fs/tmpfs",
    ] {
        h.check(rules::valid_plugin_name(ok), format!("name contract accepts `{ok}`"));
    }
    for bad in ["Service/Crypto", "service//crypto", "service/", "1crypto", "", "service/-x", "-x"] {
        h.check(!rules::valid_plugin_name(bad), format!("name contract rejects `{bad}`"));
    }
    h.eq(rules::name_namespace("service/crypto"), "service", "namespace extraction");
    h.eq(rules::name_short("service/crypto"), "crypto", "short extraction");
    h.eq(rules::name_namespace("sched-coop"), "", "bare name has empty namespace");
    h.check(rules::is_recommended_form("service/crypto"), "recommended form");
    h.check(!rules::is_recommended_form("sched-coop"), "bare name is not the recommended form");
    h.check(rules::is_recommended_form("ability/x") == false, "`ability/` is not a recommended namespace");
    h.check(rules::is_recommended_form("runtime/posix"), "recommended form: runtime/posix (ADR-0015)");

    h.eq(rules::derive_subkind("service/crypto"), Some("service"), "subkind: service/⇒service");
    h.eq(rules::derive_subkind("sched/coop"), Some("scheduler"), "subkind: sched/⇒scheduler");
    h.eq(rules::derive_subkind("framework/dev-core"), Some("framework"), "subkind: framework/");
    h.eq(rules::derive_subkind("driver/virtio-hsm"), Some("io"), "subkind: driver/⇒io (ADR-0032)");
    h.eq(rules::derive_subkind("fs/tmpfs"), Some("fs"), "subkind: fs/");
    h.eq(rules::derive_subkind("sched-coop"), None, "subkind: bare name cannot be derived");
    h.eq(rules::derive_subkind("iface/min"), None, "subkind: iface/ has no subkind");
    h.eq(rules::derive_subkind("runtime/posix"), Some("service"), "subkind: runtime/⇒service");
    h.check(rules::is_recommended_form("driver/uart-pl011"), "recommended form: driver/uart-pl011 (ADR-0032)");
    h.check(!rules::is_recommended_form("io/uart-pl011"), "the old `io/` namespace is no longer recommended (ADR-0032)");
    h.eq(rules::namespace_plugin_type("driver"), Some("ability"), "driver/⇒ability (ADR-0032)");

    h.eq(rules::namespace_plugin_type("iface"), Some("interface"), "namespace: iface⇒interface");
    h.eq(rules::namespace_plugin_type("sched"), Some("ability"), "namespace: sched⇒ability");
    h.eq(rules::namespace_plugin_type("platform"), Some("platform"), "namespace: platform");
    h.eq(rules::namespace_plugin_type("runtime"), Some("ability"), "namespace: runtime⇒ability");
    h.eq(rules::namespace_subkind("runtime"), Some("service"), "namespace: runtime⇒subkind service");
    h.eq(rules::namespace_plugin_type("nope"), None, "namespace: unknown");
}

fn phase_cases(h: &mut Harness) {
    h.eq(rules::phase_rank("early"), Some(0), "EARLY=0");
    h.eq(rules::phase_rank("core"), Some(1), "CORE=1");
    h.eq(rules::phase_rank("late"), Some(2), "LATE=2");
    h.eq(rules::phase_rank("app"), Some(3), "APP=3");

    h.eq(rules::derive_phase("platform", None, true), "core", "platform ⇒ core");
    h.eq(rules::derive_phase("ability", Some("scheduler"), true), "core", "scheduler ⇒ core");
    h.eq(rules::derive_phase("ability", Some("service"), true), "late", "service ⇒ late");
    h.eq(rules::derive_phase("ability", Some("fs"), true), "core", "fs ⇒ core");
    h.eq(rules::derive_phase("interface", None, true), "late", "interface ⇒ late");
    h.eq(rules::derive_phase("app", None, true), "app", "app ⇒ app");
    h.eq(rules::derive_phase("ability", Some("service"), false), "early", "no init hook ⇒ early");

    h.check(rules::phase_reason("interface", None, "late").is_none(), "interface+late ok");
    h.check(rules::phase_reason("interface", None, "core").is_some(), "interface+core ⇒ wrong");
    h.check(rules::phase_reason("ability", Some("service"), "late").is_none(), "service+late ok");
    h.check(rules::phase_reason("ability", Some("service"), "core").is_some(), "service+core ⇒ wrong");
    h.check(rules::phase_reason("platform", None, "core").is_none(), "platform+core ok");
    h.check(rules::phase_reason("platform", None, "late").is_some(), "platform+late ⇒ wrong");
    h.check(rules::phase_reason("ability", Some("scheduler"), "core").is_none(), "scheduler+core ok");
    h.check(rules::phase_reason("app", None, "app").is_none(), "app+app ok (M0 app/hello case)");
    h.check(rules::phase_reason("app", None, "core").is_none(), "app+core ok");
    h.check(rules::phase_reason("app", None, "late").is_some(), "app+late ⇒ wrong");
    h.check(rules::phase_reason("app", None, "early").is_none(), "early is always legal");
}

fn privilege_cases(h: &mut Harness) {
    h.eq(rules::max_privilege_level("app", None), "P0", "app ≤ P0");
    h.eq(rules::max_privilege_level("interface", None), "P0", "interface ≤ P0");
    h.eq(rules::max_privilege_level("ability", Some("service")), "P2", "ability ≤ P2");
    h.eq(rules::max_privilege_level("ability", Some("scheduler")), "P3", "scheduler ≤ P3");
    h.eq(rules::max_privilege_level("platform", None), "P4", "platform ≤ P4");

    h.check(rules::memory_combo_legal("alloc", "pool", "contig"), "pool lifecycle: alloc×pool×contig");
    h.check(rules::memory_combo_legal("free", "pool", "page"), "pool lifecycle: free×pool×page");
    h.check(!rules::memory_combo_legal("alloc", "pool", "heap"), "pool with heap is illegal");
    h.check(rules::memory_combo_legal("alloc", "page", "heap"), "alloc×page×heap");
    h.check(rules::memory_combo_legal("map", "page", "mmio"), "map/protect belong to P4 (machine)");
    h.check(!rules::memory_combo_legal("map", "page", "dma"), "map×dma is illegal");
    h.check(rules::memory_combo_legal("protect", "region", "reserved"), "protect×region×reserved");
    h.check(rules::memory_combo_legal("flush", "byte", "dma"), "flush×byte×dma");
    h.check(rules::memory_combo_legal("unmap", "region", "heap"), "unmap×region×heap");
    h.check(!rules::memory_combo_legal("nope", "page", "heap"), "unknown op is illegal");

    let ok_mem = vec![rules::MemoryEntry {
        granularity: "pool".into(),
        ops: vec!["alloc".into(), "free".into()],
        regions: vec!["contig".into(), "page".into()],
    }];
    let ok_input = rules::PrivInput {
        plugin_type: "ability",
        subkind: Some("service"),
        target: "service/crypto",
        file: "service/crypto/brick.toml",
        level: Some("P2"),
        memory: &ok_mem,
        has_resources: false,
    };
    h.check(
        rules::check_privileged(&ok_input).is_empty(),
        "positive: ability P2 + pool lifecycle has no diagnostics",
    );

    let bad_mem = vec![rules::MemoryEntry {
        granularity: "page".into(),
        ops: vec!["map".into()],
        regions: vec!["mmio".into()],
    }];
    let bad_input = rules::PrivInput {
        plugin_type: "ability",
        subkind: Some("service"),
        target: "service/crypto",
        file: "service/crypto/brick.toml",
        level: Some("P2"),
        memory: &bad_mem,
        has_resources: false,
    };
    let diags = rules::check_privileged(&bad_input);
    h.check(
        diags.iter().any(|d| d.code.as_deref() == Some("BRV-PRIV-0001")),
        "negative 1: ability declares map ⇒ BRV-PRIV-0001",
    );

    let app_input = rules::PrivInput {
        plugin_type: "app",
        subkind: None,
        target: "app/hsm",
        file: "app/hsm/brick.toml",
        level: Some("P1"),
        memory: &[],
        has_resources: true,
    };
    let diags = rules::check_privileged(&app_input);
    h.check(
        diags.iter().any(|d| d.code.as_deref() == Some("BRV-PRIV-0001")),
        "negative 2: app declares P1/resource ⇒ BRV-PRIV-0001",
    );

    let illegal = vec![rules::MemoryEntry {
        granularity: "pool".into(),
        ops: vec!["alloc".into()],
        regions: vec!["heap".into()],
    }];
    let illegal_input = rules::PrivInput {
        plugin_type: "ability",
        subkind: Some("service"),
        target: "service/crypto",
        file: "service/crypto/brick.toml",
        level: Some("P2"),
        memory: &illegal,
        has_resources: false,
    };
    let diags = rules::check_privileged(&illegal_input);
    h.check(
        diags.iter().any(|d| d.code.as_deref() == Some("BRV-PRIV-0002")),
        "negative 3: alloc×pool×heap ⇒ BRV-PRIV-0002",
    );
}

fn dep_direction_cases(h: &mut Harness) {
    h.check(!rules::dep_direction_ok("app", "ability", false), "prohibition (1): app must not depend on ability");
    h.check(!rules::dep_direction_ok("app", "platform", false), "prohibition (1): app must not depend on platform");
    h.check(rules::dep_direction_ok("app", "interface", false), "app only via interface");
    h.check(!rules::dep_direction_ok("platform", "ability", false), "platform depends on no plugin");
    h.check(!rules::dep_direction_ok("interface", "third_party", true), "interface does not consume third-party");
    h.check(rules::dep_direction_ok("ability", "third_party", true), "ability is the only legal third-party consumer");
    h.check(!rules::dep_direction_ok("platform", "interface", false), "prohibition (3): interface is a strict leaf");
    h.check(rules::dep_direction_ok("interface", "ability", false), "interface may re-export ability");
    h.check(rules::dep_direction_ok("ability", "platform", false), "ability may depend on platform");

    h.check(
        rules::api_type_edge_reason("ability", "native", "runtime_adapter", "runtime").is_some(),
        "prohibition (2): native ↛ runtime_adapter",
    );
    h.check(
        rules::api_type_edge_reason("ability", "runtime_adapter", "third_party", "type").is_none(),
        "runtime_adapter→third_party type edge is legal (§3.1 exception)",
    );
    h.check(
        rules::api_type_edge_reason("ability", "runtime_adapter", "third_party", "runtime").is_some(),
        "runtime_adapter→third_party runtime edge is illegal",
    );
    h.check(
        rules::api_type_edge_reason("ability", "native", "third_party", "type").is_some(),
        "native must not depend on third_party",
    );
    h.check(
        rules::api_type_edge_reason("app", "native", "runtime_adapter", "runtime").is_none(),
        "pure consumer (app) is outside the api_type prohibitions",
    );

    let edges = vec![("platform/soc".to_string(), "ability/helper".to_string())];
    h.check(
        rules::dep_edge_violation(
            "platform/soc", "platform", "native", "ability/helper", "ability", "native", "init", &edges,
        )
        .is_none(),
        "allow_edges whitelist exempts the direction table",
    );
    h.check(
        rules::dep_edge_violation(
            "platform/soc", "platform", "native", "ability/helper", "ability", "native", "init", &[],
        )
        .is_some(),
        "without whitelist platform→ability violates",
    );
    h.check(
        rules::dep_edge_violation(
            "ability/a", "ability", "native", "svc/posix", "ability", "runtime_adapter", "runtime", &edges,
        )
        .is_some(),
        "allow_edges does not exempt hard api_type prohibitions",
    );
}

fn export_invariant_cases(h: &mut Harness) {
    // invariant 1: api_iface must equal api_type
    let v = vec![rules::ExportView {
        api_iface: "runtime_adapter",
        form: "api",
        reexport_of: &[],
        symbols: &[],
    }];
    h.check(
        rules::check_export_invariants("native", &v)
            .iter()
            .any(|(_, c, _)| *c == "BRV-TAX-0016"),
        "invariant 1 ⇒ BRV-TAX-0016",
    );
    // invariant 2: third_party may not expose a native surface
    h.check(
        rules::check_export_invariants("third_party", &v)
            .iter()
            .any(|(_, c, _)| *c == "BRV-TAX-0017"),
        "invariant 2 ⇒ BRV-TAX-0017",
    );
    // invariant 3: a skin must declare reexport_of
    let skin = vec![rules::ExportView {
        api_iface: "native",
        form: "skin",
        reexport_of: &[],
        symbols: &[],
    }];
    h.check(
        rules::check_export_invariants("native", &skin)
            .iter()
            .any(|(_, c, _)| *c == "BRV-TAX-0018"),
        "invariant 3 ⇒ BRV-TAX-0018",
    );
    let rr: Vec<String> = vec![
        "service/crypto#crypto".into(),
        "service/keyring#keyring".into(),
    ];
    let skin_ok = vec![rules::ExportView {
        api_iface: "native",
        form: "skin",
        reexport_of: &rr,
        symbols: &[],
    }];
    h.check(
        rules::check_export_invariants("native", &skin_ok).is_empty(),
        "invariant 3 positive (multiple providers)",
    );
    // invariant 4: only a skin may declare reexport_of
    let bad4 = vec![rules::ExportView {
        api_iface: "native",
        form: "api",
        reexport_of: &rr,
        symbols: &[],
    }];
    h.check(
        rules::check_export_invariants("native", &bad4)
            .iter()
            .any(|(_, c, _)| *c == "BRV-TAX-0019"),
        "invariant 4 ⇒ BRV-TAX-0019",
    );
    h.check(rules::valid_unit_ref("service/crypto#crypto"), "unit ref shape");
    h.check(!rules::valid_unit_ref("service/crypto"), "unit ref missing #");
}

/// `driver/` namespace ⇔ `api_type = "native"` (`BRV-TAX-0021`, ADR-0032).
fn driver_namespace_cases(h: &mut Harness) {
    let ok = tmpdir("drv-ok");
    write(&ok, "driver/uart/brick.toml", &ability_toml("driver/uart", "io", "core", ""));
    let tree = model::load_tree(&ok);
    h.check(
        !tree.diags.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0021")),
        "driver/: a native driver is not reported",
    );

    let bad = tmpdir("drv-third-party");
    write(
        &bad,
        "driver/uart/brick.toml",
        "schema = 1\n[plugin]\nname = \"driver/uart\"\nplugin_type = \"ability\"\n\
         api_type = \"third_party\"\nsubkind = \"io\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n",
    );
    let tree = model::load_tree(&bad);
    h.check(
        tree.diags.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0021")),
        "driver/: api_type = third_party ⇒ BRV-TAX-0021 (the namespace pins the surface)",
    );

    // The rule is about the `driver/` namespace only: a third_party plugin elsewhere is untouched.
    // It exports a surface, so BRV-TAX-0022 does not apply either (ADR-0033).
    let elsewhere = tmpdir("drv-elsewhere-ok");
    write(
        &elsewhere,
        "fs/port/brick.toml",
        "schema = 1\n[plugin]\nname = \"fs/port\"\nplugin_type = \"ability\"\n\
         api_type = \"third_party\"\nsubkind = \"fs\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n\
         [[export]]\napi_iface = \"third_party\"\nform = \"api\"\nname = \"port\"\n\
         version = \"0.1.0.0\"\ncompat_gen = 0\nfrozen_state = \"unfrozen\"\n\
         freeze_state = \"unfrozen\"\nstatus = \"experimental\"\n",
    );
    let tree = model::load_tree(&elsewhere);
    h.check(
        !tree.diags.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0021")),
        "driver/: a third_party plugin outside driver/ is untouched by BRV-TAX-0021",
    );
}

/// `api_type` is the *surface* axis (`BRV-TAX-0022`) and provenance has its own guard
/// (`BRV-TAX-0023`) -- ADR-0033.
fn vendor_provenance_cases(h: &mut Harness) {
    // Rule A: a surface class needs a surface (`[[export]]`).
    let bare = tmpdir("surface-bare");
    write(
        &bare,
        "fs/port/brick.toml",
        "schema = 1\n[plugin]\nname = \"fs/port\"\nplugin_type = \"ability\"\n\
         api_type = \"third_party\"\nsubkind = \"fs\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n",
    );
    let tree = model::load_tree(&bare);
    h.check(
        tree.diags.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0022")),
        "surface: api_type=third_party with no [[export]] => BRV-TAX-0022",
    );

    let with_export = tmpdir("surface-export");
    write(
        &with_export,
        "fs/port/brick.toml",
        "schema = 1\n[plugin]\nname = \"fs/port\"\nplugin_type = \"ability\"\n\
         api_type = \"third_party\"\nsubkind = \"fs\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n\
         [[export]]\napi_iface = \"third_party\"\nform = \"api\"\nname = \"port\"\n\
         version = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nstatus = \"experimental\"\n",
    );
    let tree = model::load_tree(&with_export);
    h.check(
        !tree.diags.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0022")),
        "surface: third_party WITH an export is not reported",
    );

    // `native` is the neutral value and is never reported, exports or not.
    let native = tmpdir("surface-native");
    write(&native, "fs/port/brick.toml", &ability_toml("fs/port", "fs", "core", ""));
    let tree = model::load_tree(&native);
    h.check(
        !tree.diags.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0022")),
        "surface: api_type=native needs no surface",
    );

    // Rule B decision table.
    h.check(
        rules::vendor_edge_reason(true, true).is_none(),
        "vendor: vendor-carrying -> vendor-carrying is legal",
    );
    h.check(
        rules::vendor_edge_reason(false, false).is_none(),
        "vendor: no vendor code on either side is legal",
    );
    h.check(
        rules::vendor_edge_reason(false, true).is_some(),
        "vendor: non-vendor -> vendor is a violation",
    );

    // Rule B end-to-end through `check`.
    let tree_bad = tmpdir("vendor-edge-bad");
    write(
        &tree_bad,
        "driver/port/brick.toml",
        "schema = 1\n[plugin]\nname = \"driver/port\"\nplugin_type = \"ability\"\n\
         api_type = \"native\"\nsubkind = \"io\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n\
         [build]\nsources = [\"../../third_party/vendor/x.c\"]\n",
    );
    write(
        &tree_bad,
        "framework/fw/brick.toml",
        &ability_toml(
            "framework/fw",
            "framework",
            "core",
            "[[dep]]\nname = \"driver/port\"\nrange = \">=0.1.0\"\nkind = \"init\"\n",
        ),
    );
    let resp = check::run(&mk_req("check", &tree_bad, json!({"profile": "dev"})));
    h.check(
        has_code(&resp, "BRV-TAX-0023"),
        "vendor: a plugin without `third_party/` in [build] must not depend on one with it",
    );

    // ... and the same edge is fine when the consumer carries vendor code too.
    let tree_ok = tmpdir("vendor-edge-ok");
    write(
        &tree_ok,
        "driver/port/brick.toml",
        "schema = 1\n[plugin]\nname = \"driver/port\"\nplugin_type = \"ability\"\n\
         api_type = \"native\"\nsubkind = \"io\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n\
         [build]\nsources = [\"../../third_party/vendor/x.c\"]\n",
    );
    write(
        &tree_ok,
        "fs/port2/brick.toml",
        "schema = 1\n[plugin]\nname = \"fs/port2\"\nplugin_type = \"ability\"\n\
         api_type = \"native\"\nsubkind = \"fs\"\nlang = \"c\"\nphase = \"core\"\n\
         version = \"0.1.0.0\"\n\
         [build]\nsources = [\"../../third_party/vendor/y.c\"]\n\
         [[dep]]\nname = \"driver/port\"\nrange = \">=0.1.0\"\nkind = \"init\"\n",
    );
    let resp = check::run(&mk_req("check", &tree_ok, json!({"profile": "dev"})));
    h.check(
        !has_code(&resp, "BRV-TAX-0023"),
        "vendor: vendor-carrying -> vendor-carrying is not reported",
    );
}

fn model_cases(h: &mut Harness) {
    let root = tmpdir("model-ok");
    write(
        &root,
        "service/crypto/brick.toml",
        r#"schema = 1

[plugin]
name        = "service/crypto"
plugin_type = "ability"
api_type    = "native"
subkind     = "service"
lang        = "c"
phase       = "late"
version     = "0.1.0.0"

[[res]]
kind = "ram"
used_kib = 48

[[export]]
api_iface    = "native"
form         = "service"
name         = "crypto"
version      = "0.1.0.0"
compat_gen   = 0
freeze_state = "unfrozen"
status       = "experimental"

[[export.entries]]
kind   = "macro"
name   = "BR_MAX"
value  = ["16", "32"]
status = "experimental"

[privileged]
level = "P2"
[[privileged.memory]]
granularity = "pool"
ops         = ["alloc", "free"]
regions     = ["contig", "page"]
[[privileged.resources]]
irq          = [32]
dma_channels = [3]
device_names = ["hsm0"]
"#,
    );
    let tree = model::load_tree(&root);
    h.eq(tree.diags.len(), 0, "valid model has no diagnostics");
    h.eq(tree.plugins.len(), 1, "valid model loads 1 plugin");
    if let Some(p) = tree.plugins.first() {
        h.eq(p.name.clone(), "service/crypto".to_string(), "plugin name");
        h.eq(p.phase.clone(), "late".to_string(), "phase");
        h.eq(p.version, Version::new(0, 1, 0, 0), "four-segment version");
        h.eq(p.res_totals(), (48, 0), "[[res]] sum");
        h.eq(p.unit_ids(), vec!["service/crypto#crypto".to_string()], "interface unit id");
        let privd = p.privileged.as_ref().expect("privileged should exist");
        h.eq(privd.resources.irq.clone(), vec![32], "[[privileged.resources]] is an array of tables");
        h.eq(privd.resources.dma_channels.clone(), vec![3], "dma_channels accumulate");
        h.eq(
            p.exports[0].entries[0].value.clone(),
            Some("16,32".to_string()),
            "string-array value ⇒ normalized",
        );
    } else {
        h.check(false, "valid model is missing the plugin");
    }
    h.check(!tree.has_product, "no product.toml ⇒ has_product = false");
    h.check(!tree.product_level_constraints_apply(), "R-1: no product ⇒ skip product-level constraints");

    let root_cap = tmpdir("model-cap");
    write(
        &root_cap,
        "platform/soc/brick.toml",
        r#"schema = 1
[plugin]
name = "platform/soc"
plugin_type = "platform"
api_type = "native"
lang = "c"
phase = "core"
version = "0.1.0.0"
[[res]]
kind = "ram"
used_kib = 512
[[res]]
kind = "stack"
used_kib = 16
"#,
    );
    let tree_cap = model::load_tree(&root_cap);
    h.eq(tree_cap.diags.len(), 0, "valid platform has no diagnostics");
    h.eq(tree_cap.platform_capacity(), (512, 16), "R-6: platform [[res]] is the platform capacity");

    let root2 = tmpdir("model-noname");
    write(
        &root2,
        "a/brick.toml",
        r#"schema = 1
[plugin]
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
"#,
    );
    let tree2 = model::load_tree(&root2);
    h.has_code(&tree2.diags, "BRV-MF-0001", "missing required name ⇒ BRV-MF-0001");
    h.exit_is(&tree2.diags, 2, "shape error ⇒ exit code 2");

    let root3 = tmpdir("model-ver");
    write(
        &root3,
        "b/brick.toml",
        r#"schema = 1
[plugin]
name = "service/b"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0"
"#,
    );
    let tree3 = model::load_tree(&root3);
    h.has_code(&tree3.diags, "BRV-VER-0005", "3-segment version string ⇒ BRV-VER-0005");
    h.exit_is(&tree3.diags, 2, "bad version string shape ⇒ exit code 2");

    let root4 = tmpdir("model-range");
    write(
        &root4,
        "c/brick.toml",
        r#"schema = 1
[plugin]
name = "service/c"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[compat]
core = ">=1.2.3.4"
"#,
    );
    let tree4 = model::load_tree(&root4);
    h.has_code(&tree4.diags, "BRV-VER-0006", "4-segment range ⇒ BRV-VER-0006");
    h.exit_is(&tree4.diags, 2, "bad range shape ⇒ exit code 2");

    let root5 = tmpdir("model-phase");
    write(
        &root5,
        "d/brick.toml",
        r#"schema = 1
[plugin]
name = "service/d"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "core"
version = "0.1.0.0"
"#,
    );
    let tree5 = model::load_tree(&root5);
    h.has_code(&tree5.diags, "BRV-MF-0001", "service declares core ⇒ MF-0001");

    let root6 = tmpdir("model-subkind");
    write(
        &root6,
        "service/e/brick.toml",
        r#"schema = 1
[plugin]
name = "service/e"
plugin_type = "ability"
api_type = "native"
subkind = "fs"
lang = "c"
phase = "core"
version = "0.1.0.0"
"#,
    );
    let tree6 = model::load_tree(&root6);
    h.has_code(&tree6.diags, "BRV-TAX-0015", "subkind conflict ⇒ BRV-TAX-0015");
    h.exit_is(&tree6.diags, 1, "TAX-0015 is a validation error ⇒ exit code 1");

    let root7 = tmpdir("model-dup");
    write(
        &root7,
        "service/x/brick.toml",
        r#"schema = 1
[plugin]
name = "service/x"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
"#,
    );
    write(
        &root7,
        "service/x2/brick.toml",
        r#"schema = 1
[plugin]
name = "service/x"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
"#,
    );
    let tree7 = model::load_tree(&root7);
    h.has_code(&tree7.diags, "BRV-MF-0001", "duplicate plugin name ⇒ BRV-MF-0001");

    let root8 = tmpdir("model-toml");
    write(&root8, "e/brick.toml", "schema = = 1\n");
    let tree8 = model::load_tree(&root8);
    h.check(
        tree8.diags.items().iter().any(|d| d.code.is_none()),
        "invalid TOML ⇒ codeless environment error",
    );
    h.exit_is(&tree8.diags, 2, "invalid TOML ⇒ exit code 2");

    let root9 = tmpdir("model-skip");
    write(
        &root9,
        "build/ignored/brick.toml",
        "schema = 1\n",
    );
    write(&root9, ".hidden/ignored/brick.toml", "schema = 1\n");
    let tree9 = model::load_tree(&root9);
    h.eq(tree9.plugins.len(), 0, "scan skips build/ and dot-directories");

    let root10 = tmpdir("model-compat");
    write(
        &root10,
        "service/g/brick.toml",
        r#"schema = 1
[plugin]
name = "service/g"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[compat]
api_rev = 1
"#,
    );
    let tree10 = model::load_tree(&root10);
    h.has_code(&tree10.diags, "BRV-MF-0001", "`[compat]` missing core ⇒ MF-0001");

    let root11 = tmpdir("model-export");
    write(
        &root11,
        "service/h/brick.toml",
        r#"schema = 1
[plugin]
name = "service/h"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[[export]]
api_iface  = "native"
form       = "service"
name       = "h"
version    = "0.1.0.0"
compat_gen = 0
status     = "experimental"
"#,
    );
    let tree11 = model::load_tree(&root11);
    h.has_code(&tree11.diags, "BRV-MF-0001", "export missing freeze_state ⇒ MF-0001");

    let root12 = tmpdir("model-priv");
    write(
        &root12,
        "service/i/brick.toml",
        r#"schema = 1
[plugin]
name = "service/i"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[privileged]
[[privileged.memory]]
granularity = "pool"
ops = ["alloc"]
regions = ["contig"]
"#,
    );
    let tree12 = model::load_tree(&root12);
    h.has_code(&tree12.diags, "BRV-PRIV-0001", "missing privileged.level ⇒ PRIV-0001");
}

fn snapshot_and_lock_cases(h: &mut Harness) {
    let root = tmpdir("snap");
    write(
        &root,
        "api/iface/service/crypto/crypto.toml",
        r#"schema = 1
unit = "service/crypto#crypto"

[unit_meta]
api_iface   = "native"
form        = "service"
version     = "0.1.0.0"
compat_gen  = 0
freeze_state = "unfrozen"
status      = "experimental"
hash        = "sha256:abc"
hash_scope  = "decl"
truth       = "decl"

[[entry]]
kind = "func"
name = "br_crypto_hash"
sig = "int (const uint8_t*, size_t, uint8_t*)"
status = "experimental"
"#,
    );
    let mut d = Diags::new();
    let snaps = model::load_snapshots(&root, &mut d);
    h.eq(snaps.len(), 1, "load 1 snapshot");
    if let Some(s) = snaps.first() {
        h.eq(s.id.clone(), "service/crypto#crypto".to_string(), "snapshot id");
        h.eq(s.unit.clone(), "crypto".to_string(), "snapshot unit");
        h.eq(s.entries.len(), 1, "snapshot entry count");
    }

    let empty = tmpdir("snap-empty");
    let mut d2 = Diags::new();
    h.eq(model::load_snapshots(&empty, &mut d2).len(), 0, "no api/iface ⇒ empty (unpublished)");
    h.check(d2.is_empty(), "no api/iface reports no diagnostics");
    let mut d3 = Diags::new();
    h.check(model::load_lock(&empty, &mut d3).is_none(), "no brickie.lock ⇒ None");

    let lock = model::Lock {
        schema: 1,
        profile: "dev".into(),
        product: Some(model::LockProduct {
            name: "hsm".into(),
            version: "0.1.0.0".into(),
            stage: "dev".into(),
        }),
        plugins: vec![model::LockPlugin {
            name: "service/crypto".into(),
            version: "0.1.0.0".into(),
            compat_gen: 0,
        }],
        units: vec![model::LockUnit {
            id: "service/crypto#crypto".into(),
            provider: "service/crypto".into(),
            unit: "crypto".into(),
            api_iface: "native".into(),
            version: "0.1.0.0".into(),
            compat_gen: 0,
            hash: "sha256:abc".into(),
            hash_scope: "decl".into(),
            truth: "decl".into(),
        }],
    };
    let text1 = crate::emit::render_lock(&lock);
    let text2 = crate::emit::render_lock(&lock);
    h.eq(text1.clone(), text2, "lock serialization is idempotent");
    h.check(text1.contains("[[lock.plugins]]"), "lock shape: [[lock.plugins]]");
    h.check(text1.contains("[[lock.units]]"), "lock shape: [[lock.units]]");
    h.check(text1.parse::<toml::Value>().is_ok(), "lock text parses back as toml");
    write(&root, "brickie.lock", &text1);
    let mut d4 = Diags::new();
    let back = model::load_lock(&root, &mut d4);
    h.check(back.is_some(), "lock can be read back");
    if let Some(l) = back {
        h.eq(l.plugins.len(), 1, "lock plugin count");
        h.eq(l.units.len(), 1, "lock unit count");
        h.eq(l.profile.clone(), "dev".to_string(), "lock profile");
        h.eq(crate::emit::render_lock(&l), text1, "lock round-trip is byte-identical");
    }

    let idx = model::DependentsIndex {
        schema: 1,
        dependents: {
            let mut m = std::collections::BTreeMap::new();
            m.insert(
                "service/crypto".to_string(),
                vec![model::DependentEdge {
                    from: "app/hsm".into(),
                    kind: "runtime".into(),
                }],
            );
            m
        },
    };
    let js1 = crate::emit::render_dependents(&idx);
    let js2 = crate::emit::render_dependents(&idx);
    h.eq(js1.clone(), js2, "dependents index serialization is idempotent");
    h.check(js1.contains("\"from\": \"app/hsm\""), "dependents contains the edge");
}

fn plan_cases(h: &mut Harness) {
    // four plan classes x native x c
    let cases = [
        ("app", "app/hsm", "app", "native/app/c/brick.toml.tmpl"),
        ("interface", "iface/posix", "late", "native/interface/c/brick.toml.tmpl"),
        ("ability", "service/crypto", "late", "native/ability/c/brick.toml.tmpl"),
        ("platform", "platform/qemu-aarch64", "core", "native/platform/c/brick.toml.tmpl"),
    ];
    for (pt, name, want_phase, want_tpl) in cases {
        let a = args(json!({
            "plugin_type": pt, "name": name, "api_type": "native", "lang": "c",
        }));
        let (d, arts) = plan::plan_new(&a, &Map::new());
        h.exit_is(&d, 0, &format!("plan-new {pt} exit code 0"));
        h.eq(arts.len(), 6, &format!("plan-new {pt} yields 6 planned artifacts"));
        let manifest = arts.iter().find(|x| x.path.ends_with("/brick.toml"));
        match manifest {
            Some(m) => {
                h.eq(m.kind.clone(), "human".to_string(), "brick.toml is human");
                h.eq(m.template.clone(), want_tpl.to_string(), "skeleton template path");
                h.eq(
                    m.vars.get("phase").cloned().unwrap_or_default(),
                    want_phase.to_string(),
                    &format!("{pt} phase derivation"),
                );
                h.eq(
                    m.vars.get("name").cloned().unwrap_or_default(),
                    name.to_string(),
                    "lowercase placeholder name",
                );
                h.eq(
                    m.vars.get("PLUGIN_NAME").cloned().unwrap_or_default(),
                    name.to_string(),
                    "uppercase placeholder PLUGIN_NAME",
                );
            }
            None => h.check(false, "plan-new is missing the brick.toml artifact"),
        }
        h.check(
            arts.iter().any(|x| {
                x.path == format!("build/gen/{name}/plugin_desc.c")
                    && x.kind == "generated"
                    && x.template == "descriptor/native/c/plugin_desc.c.tmpl"
            }),
            &format!("plan-new {pt} descriptor artifact"),
        );
    }

    let a = args(json!({
        "plugin_type": "ability", "name": "sched-coop", "api_type": "native",
        "lang": "c", "subkind": "scheduler",
    }));
    let (d, arts) = plan::plan_new(&a, &Map::new());
    h.exit_is(&d, 0, "bare-name ability + --subkind passes");
    h.check(d.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0013")), "bare name ⇒ TAX-0013 warning");
    h.eq(arts.len(), 6, "bare-name ability artifact count");

    let a = args(json!({"plugin_type": "ability", "name": "sched-coop", "api_type": "native", "lang": "c"}));
    let (d, arts) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-MF-0001", "ability bare name missing subkind ⇒ MF-0001");
    h.exit_is(&d, 2, "missing subkind ⇒ exit code 2");
    h.eq(arts.len(), 0, "shape error ⇒ no artifacts produced");

    let a = args(json!({
        "plugin_type": "ability", "name": "service/crypto", "api_type": "native",
        "lang": "c", "subkind": "fs",
    }));
    let (d, _) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-TAX-0015", "subkind conflict ⇒ TAX-0015");
    h.exit_is(&d, 1, "TAX-0015 ⇒ exit code 1");

    let a = args(json!({
        "plugin_type": "ability", "name": "service/crypto", "api_type": "runtime_adapter", "lang": "c",
    }));
    let (d, arts) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-TAX-0014", "undelivered template ⇒ TAX-0014");
    h.exit_is(&d, 2, "template not delivered ⇒ command incomplete, exit code 2 (G-1)");
    h.eq(arts.len(), 0, "template not delivered ⇒ no artifacts produced");

    let a = args(json!({"plugin_type": "app", "name": "App/Hsm", "api_type": "native", "lang": "c"}));
    let (d, _) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-MF-0001", "name contract violation ⇒ MF-0001");

    let a = args(json!({"plugin_type": "app", "name": "app/hsm", "api_type": "native", "lang": "c"}));
    let c = ctx(json!({"existing": [{"path": "app/hsm/brick.toml", "first_line": "# human-written"}]}));
    let (d, arts) = plan::plan_new(&a, &c);
    h.has_code(&d, "BRV-GEN-0002", "human-written file already exists ⇒ BRV-GEN-0002");
    h.exit_is(&d, 1, "BRV-GEN-0002 ⇒ exit code 1");
    h.eq(arts.len(), 0, "conflict ⇒ empty plan");

    let c = ctx(json!({"existing": [{"path": "build/gen/app/hsm/plugin_desc.c", "first_line": "int x;"}]}));
    let a = args(json!({"plugin_type": "app", "name": "app/hsm", "api_type": "native", "lang": "c"}));
    let (d, _) = plan::plan_new(&a, &c);
    h.has_code(&d, "BRV-GEN-0002", "generated artifact not produced by this tool ⇒ BRV-GEN-0002");
    let a2 = args(json!({"plugin_type": "app", "name": "app/hsm", "api_type": "native", "lang": "c", "force": true}));
    let (d2, arts2) = plan::plan_new(&a2, &c);
    h.exit_is(&d2, 0, "--force overrides generated-artifact conflict");
    h.eq(arts2.len(), 6, "still 6 planned artifacts after --force");

    let c = ctx(json!({"existing": [{"path": "build/gen/app/hsm/plugin_desc.c", "first_line": "/* brickie:generated */"}]}));
    let (d3, arts3) = plan::plan_new(&a, &c);
    h.exit_is(&d3, 0, "already a generated artifact ⇒ normal rebuild");
    h.eq(arts3.len(), 6, "generated-artifact rebuild plan count");

    let a = args(json!({"name": "hsm"}));
    let (d, arts, lock) = plan::plan_init(&a, &Map::new());
    h.exit_is(&d, 0, "plan-init exit code 0");
    h.check(
        arts.iter().any(|x| x.path == "product.toml" && x.template == "product/c/product.toml.tmpl"),
        "plan-init produces product.toml",
    );
    h.check(
        arts.iter().any(|x| x.path == "app/hsm/brick.toml"),
        "plan-init produces the app/<name>/ skeleton",
    );
    h.check(
        arts.iter().any(|x| x.path == "brickie.lock" && x.kind == "machine"),
        "plan-init produces brickie.lock (machine)",
    );
    h.check(
        lock.as_deref()
            .map(|s| s.contains("[lock]") && s.contains("[lock.product]"))
            .unwrap_or(false),
        "plan-init carries lock text",
    );

    let empty = tmpdir("gen-plan");
    let (d, arts) = plan::gen_plan(&empty, &Map::new());
    h.exit_is(&d, 0, "gen-plan empty tree exit code 0");
    h.eq(arts.len(), 0, "gen-plan empty tree has no artifacts");
}

fn emit_cases(h: &mut Harness) {
    use crate::emit::{TDoc, TTable, TValue};
    let doc = TDoc::new()
        .set("schema", TValue::Int(1))
        .set("note", TValue::Str("a\"b\\c\n".into()))
        .push(
            TTable::array_table(&["item"])
                .set("name", TValue::Str("a".into()))
                .set("tags", TValue::StrArray(vec!["x".into(), "y".into()])),
        );
    let text = doc.render();
    let text2 = doc.render();
    h.eq(text.clone(), text2, "TOML emission is idempotent");
    h.check(text.contains("note = \"a\\\"b\\\\c\\n\""), "TOML string escaping");
    h.check(text.contains("[\"x\", \"y\"]"), "TOML string array");
    h.check(text.starts_with("schema = 1\n"), "root key-values precede tables");
    let parsed: Result<toml::Value, _> = text.parse();
    h.check(parsed.is_ok(), "emitted TOML can be re-parsed");
}

use crate::check;
use crate::iface;
use crate::proto::{Request, Response};
use crate::solver;

fn mk_req(command: &str, root: &Path, args: Value) -> Request {
    Request {
        protocol: Some(1),
        command: command.to_string(),
        root: root.to_string_lossy().to_string(),
        args: args.as_object().cloned().unwrap_or_default(),
        context: Map::new(),
    }
}

fn diag_triples(resp: &Response) -> Vec<(String, String, String)> {
    resp.diagnostics
        .as_array()
        .map(|a| {
            a.iter()
                .map(|d| {
                    (
                        d.get("code").and_then(Value::as_str).unwrap_or("").to_string(),
                        d.get("severity")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_string(),
                        d.get("message").and_then(Value::as_str).unwrap_or("").to_string(),
                    )
                })
                .collect()
        })
        .unwrap_or_default()
}

fn has_code(resp: &Response, code: &str) -> bool {
    diag_triples(resp).iter().any(|(c, _, _)| c == code)
}

fn has_code_sev(resp: &Response, code: &str, sev: &str) -> bool {
    diag_triples(resp)
        .iter()
        .any(|(c, s, _)| c == code && s == sev)
}

fn msg_of(resp: &Response, code: &str) -> String {
    diag_triples(resp)
        .iter()
        .filter(|(c, _, _)| c == code)
        .map(|(_, _, m)| m.clone())
        .collect::<Vec<_>>()
        .join(" | ")
}

fn msg_of2(resp: &Response) -> String {
    diag_triples(resp)
        .iter()
        .map(|(c, _, m)| format!("{c} {m}"))
        .collect::<Vec<_>>()
        .join(" | ")
}

fn apply_files(root: &Path, resp: &Response) {
    if let Some(arr) = resp.files.as_array() {
        for f in arr {
            let p = f.get("path").and_then(Value::as_str).unwrap_or("");
            let c = f.get("content").and_then(Value::as_str).unwrap_or("");
            if !p.is_empty() {
                write(root, p, c);
            }
        }
    }
}

fn scoped_has(sol: &solver::SolveResult, code: &str) -> bool {
    sol.diags.iter().any(|(_, d)| d.code.as_deref() == Some(code))
}

fn scoped_msg(sol: &solver::SolveResult, code: &str) -> String {
    sol.diags
        .iter()
        .filter(|(_, d)| d.code.as_deref() == Some(code))
        .map(|(_, d)| d.message.clone())
        .collect::<Vec<_>>()
        .join(" | ")
}

/// Minimal valid ability plugin text.
fn ability_toml(name: &str, subkind: &str, phase: &str, extra: &str) -> String {
    format!(
        "schema = 1\n[plugin]\nname = \"{name}\"\nplugin_type = \"ability\"\napi_type = \"native\"\n\
         subkind = \"{subkind}\"\nlang = \"c\"\nphase = \"{phase}\"\nversion = \"0.1.0.0\"\n\
         [compat]\ncore = \">=1.0.0\"\n{extra}"
    )
}

fn platform_toml(name: &str, phase: &str, extra: &str) -> String {
    format!(
        "schema = 1\n[plugin]\nname = \"{name}\"\nplugin_type = \"platform\"\napi_type = \"native\"\n\
         lang = \"c\"\nphase = \"{phase}\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n{extra}"
    )
}

fn core_toml(name: &str, extra: &str) -> String {
    format!(
        "schema = 1\n[plugin]\nname = \"{name}\"\nplugin_type = \"core\"\napi_type = \"native\"\n\
         lang = \"c\"\nphase = \"early\"\nversion = \"0.1.0.0\"\n{extra}"
    )
}

fn product_with_core(app: &str, plugins: &str) -> String {
    format!(
        "schema = 1\n[product]\nname = \"p\"\nversion = \"1.0.0.0\"\napp = \"{app}\"\n\
         core = \">=1.0.0\"\nstage = \"dev\"\n[select]\nplugins = [{plugins}]\n"
    )
}

/// `plugin_type = "core"` (ADR-0031): mandatory + singular, no lifecycle, no `[compat]` self-pin.
fn core_type_cases(h: &mut Harness) {
    // Positive: exactly one core in the product closure ⇒ no BRV-TAX-0020.
    let ok = tmpdir("core-ok");
    write(&ok, "core/c_core/brick.toml", &core_toml("core/c_core", ""));
    write(&ok, "app/m/brick.toml", &ability_toml("app/m", "service", "late", ""));
    write(&ok, "product.toml", &product_with_core("app/m", "\"core/c_core\""));
    let resp = check::run(&mk_req("check", &ok, json!({"profile": "dev"})));
    h.check(!has_code(&resp, "BRV-TAX-0020"), "core: one selected core is not reported");

    // Negative ①: a product whose closure contains no core ⇒ red, message says "found 0".
    let miss = tmpdir("core-missing");
    write(&miss, "app/m/brick.toml", &ability_toml("app/m", "service", "late", ""));
    write(&miss, "product.toml", &product_with_core("app/m", ""));
    let resp = check::run(&mk_req("check", &miss, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "core: a product without a core ⇒ exit code 1");
    h.check(has_code(&resp, "BRV-TAX-0020"), "core: missing core ⇒ BRV-TAX-0020");
    h.check(
        msg_of(&resp, "BRV-TAX-0020").contains("found 0"),
        "core: the message counts what it found",
    );

    // Negative ②: two selected cores ⇒ red, message says "found 2".
    let two = tmpdir("core-two");
    write(&two, "core/c_core/brick.toml", &core_toml("core/c_core", ""));
    write(&two, "core/r_core/brick.toml", &core_toml("core/r_core", ""));
    write(&two, "app/m/brick.toml", &ability_toml("app/m", "service", "late", ""));
    write(
        &two,
        "product.toml",
        &product_with_core("app/m", "\"core/c_core\", \"core/r_core\""),
    );
    let resp = check::run(&mk_req("check", &two, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "core: two cores ⇒ exit code 1");
    h.check(
        msg_of(&resp, "BRV-TAX-0020").contains("found 2"),
        "core: two cores are reported as found 2",
    );

    // R-1: no product.toml ⇒ product-level counts do not apply.
    let bare = tmpdir("core-bare");
    write(&bare, "app/m/brick.toml", &ability_toml("app/m", "service", "late", ""));
    let resp = check::run(&mk_req("check", &bare, json!({"profile": "dev"})));
    h.check(!has_code(&resp, "BRV-TAX-0020"), "core: R-1 — a bare plugin tree has no product-level count");

    // Lifecycle: `core` has no phase other than `early`.
    h.check(
        rules::phase_reason("core", None, "core").is_some(),
        "core: phase=core is rejected (no lifecycle, hence no init completion point)",
    );
    h.check(
        rules::phase_reason("core", None, "early").is_none(),
        "core: phase=early is legal (means no init hook)",
    );
    // Direction: everyone may depend on core; core may not depend on core.
    h.check(dep_direction_ok_public("app", "core"), "core: app → core is legal");
    h.check(dep_direction_ok_public("interface", "core"), "core: interface → core is legal");
    h.check(dep_direction_ok_public("ability", "core"), "core: ability → core is legal");
    h.check(dep_direction_ok_public("platform", "core"), "core: platform → core is legal (start.S enters br_core_main)");
    h.check(!dep_direction_ok_public("core", "core"), "core: core → core is illegal (exactly one core)");
    h.check(dep_direction_ok_public("core", "platform"), "core: core → platform is legal (contract-level call)");
    h.check(!dep_direction_ok_public("core", "app"), "core: core → app is illegal");
}

/// Thin wrapper so the case reads as a rule assertion (the table itself is `rules::dep_direction_ok`).
fn dep_direction_ok_public(consumer_pt: &str, provider_pt: &str) -> bool {
    rules::dep_direction_ok(consumer_pt, provider_pt, false)
}

fn cycle_cases(h: &mut Harness) {
    let root = tmpdir("cycle");
    write(&root, "p-one/brick.toml", &ability_toml("p-one", "service", "late", "[[dep]]\nname = \"p-two\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    write(&root, "p-two/brick.toml", &ability_toml("p-two", "service", "late", "[[dep]]\nname = \"p-three\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    write(&root, "p-three/brick.toml", &ability_toml("p-three", "service", "late", "[[dep]]\nname = \"p-one\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    let tree = model::load_tree(&root);
    let sol = solver::solve(&tree, "dev", false);
    h.eq(sol.closure.cycles.len(), 1, "V-3: one init cycle");
    h.eq(
        sol.closure.cycles[0].path.clone(),
        vec![
            "p-one".to_string(),
            "p-two".to_string(),
            "p-three".to_string(),
            "p-one".to_string(),
        ],
        "V-3: full cycle path A→B→C→A",
    );
    h.eq(sol.closure.cycles[0].edges.len(), 3, "V-3: structured edge list has 3 entries");
    h.check(scoped_has(&sol, "BRV-MF-0001"), "V-3: cycle reports BRV-MF-0001 (gap stand-in)");
    h.check(
        scoped_msg(&sol, "BRV-MF-0001").contains("p-one → p-two → p-three → p-one"),
        "V-3: message contains the full cycle path",
    );
    let resp = solver::run(&mk_req("closure", &root, json!({})));
    h.eq(resp.exit_code, 1, "V-3: cycle ⇒ exit code 1");
    h.j(&resp.data["cycles"][0]["path"], json!(["p-one", "p-two", "p-three", "p-one"]), "V-3: data.cycles[0].path");
    h.eq(
        resp.data["cycles"][0]["edges"].as_array().map(|a| a.len()),
        Some(3),
        "V-3: data.cycles[0].edges is structured",
    );
    h.eq(
        resp.data["topo_order"].as_array().map(|a| a.len()),
        Some(3),
        "V-3: with a cycle topo_order still covers every node",
    );

    let root2 = tmpdir("cycle-rt");
    write(&root2, "r-one/brick.toml", &ability_toml("r-one", "service", "late", "[[dep]]\nname = \"r-two\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    write(&root2, "r-two/brick.toml", &ability_toml("r-two", "service", "late", "[[dep]]\nname = \"r-one\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    let sol2 = solver::solve(&model::load_tree(&root2), "dev", false);
    h.check(
        !sol2
            .diags
            .iter()
            .any(|(_, d)| d.code.as_deref() == Some("BRV-MF-0001")
                && d.severity == crate::diag::Severity::Error),
        "§7.1: runtime cycle does not report error",
    );
    h.check(
        sol2.diags
            .iter()
            .any(|(_, d)| d.severity == crate::diag::Severity::Info),
        "§7.1: runtime cycle reports info only",
    );
}

fn phase_check_cases(h: &mut Harness) {
    let bad = tmpdir("phase-bad2");
    write(&bad, "phase-a/brick.toml", &ability_toml("phase-a", "io", "core", "[[dep]]\nname = \"phase-b\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    write(&bad, "phase-b/brick.toml", &ability_toml("phase-b", "service", "late", ""));
    let resp = check::run(&mk_req("check", &bad, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "V-8: phase monotonicity negative ⇒ exit code 1");
    h.check(has_code(&resp, "BRV-DEP-0009"), "V-8(R1): reports BRV-DEP-0009");
    h.check(
        msg_of(&resp, "BRV-DEP-0009").contains("phase-b") && msg_of(&resp, "BRV-DEP-0009").contains("phase-a"),
        "V-8(R1): message names both sides",
    );

    let ok = tmpdir("phase-ok2");
    write(&ok, "sched-coop/brick.toml", &ability_toml("sched-coop", "scheduler", "core", "[[dep]]\nname = \"platform/qemu-aarch64\"\nrange = \">=0.1.0\"\nkind = \"init\"\nphase = \"core\"\n"));
    write(&ok, "platform/qemu-aarch64/brick.toml", &platform_toml("platform/qemu-aarch64", "core", ""));
    let resp = check::run(&mk_req("check", &ok, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 0, "V-8: sched-coop→platform is no false positive ⇒ exit code 0");
    h.check(!has_code(&resp, "BRV-DEP-0009"), "V-8: does not report BRV-DEP-0009");
    let sol_ok = solver::solve(&model::load_tree(&ok), "dev", false);
    h.eq(
        sol_ok.closure.topo_order.clone(),
        vec!["platform/qemu-aarch64".to_string(), "sched-coop".to_string()],
        "§7.2: topo_order is the init order (provider first)",
    );

    let r2 = tmpdir("phase-r2");
    write(&r2, "phase-c/brick.toml", &ability_toml("phase-c", "io", "core", "[[dep]]\nname = \"phase-d\"\nrange = \">=0.1.0\"\nkind = \"init\"\nphase = \"early\"\n"));
    write(&r2, "phase-d/brick.toml", &ability_toml("phase-d", "io", "core", ""));
    let resp = check::run(&mk_req("check", &r2, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-DEP-0010"), "V-8(R2): assertion conflicts with provider's own phase ⇒ BRV-DEP-0010");

    let r2ok = tmpdir("phase-r2ok");
    write(&r2ok, "phase-e/brick.toml", &ability_toml("phase-e", "service", "late", "[[dep]]\nname = \"phase-f\"\nrange = \">=0.1.0\"\nkind = \"init\"\nphase = \"late\"\n"));
    write(&r2ok, "phase-f/brick.toml", &ability_toml("phase-f", "io", "core", ""));
    let resp = check::run(&mk_req("check", &r2ok, json!({"profile": "dev"})));
    h.check(!has_code(&resp, "BRV-DEP-0010"), "V-8(R2): assertion later than the provider is not reported");
    h.check(!has_code(&resp, "BRV-DEP-0009"), "V-8(R1): core ≤ late is legal");
}

fn version_solver_cases(h: &mut Harness) {
    let root = tmpdir("ver-range");
    write(&root, "ver-a/brick.toml", &ability_toml("ver-a", "service", "late", "[[dep]]\nname = \"ver-b\"\nrange = \">=2.0.0\"\nkind = \"runtime\"\n"));
    write(&root, "ver-b/brick.toml", &ability_toml("ver-b", "service", "late", ""));
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "V-7: out-of-range range ⇒ exit code 1");
    h.check(has_code(&resp, "BRV-VER-0002"), "V-7: out-of-range range ⇒ BRV-VER-0002");

    let root = tmpdir("ver-conflict2");
    write(&root, "ver-c/brick.toml", &ability_toml("ver-c", "service", "late", "[[dep]]\nname = \"ver-d\"\nrange = \">=2.0.0\"\nkind = \"runtime\"\n"));
    write(&root, "ver-d/brick.toml", &ability_toml("ver-d", "service", "late", ""));
    write(&root, "ver-e/brick.toml", &ability_toml("ver-e", "service", "late", "[[dep]]\nname = \"ver-d\"\nrange = \"<1.0.0\"\nkind = \"runtime\"\n"));
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-DEP-0011"), "V-7: mutually exclusive ranges ⇒ BRV-DEP-0011 (single-version policy)");

    let miss = tmpdir("ver-miss");
    write(&miss, "ver-f/brick.toml", &ability_toml("ver-f", "service", "late", "[[dep]]\nname = \"no/such\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    let resp = check::run(&mk_req("check", &miss, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-MF-0001"), "V-7: missing dependency ⇒ BRV-MF-0001 (gap stand-in)");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("no/such"),
        "V-7: missing message names the dependency",
    );

    let bud = tmpdir("budget2");
    write(&bud, "platform/soc/brick.toml", &platform_toml("platform/soc", "core", "[[res]]\nkind = \"ram\"\nused_kib = 64\n"));
    write(&bud, "service/mem/brick.toml", &ability_toml("service/mem", "service", "late", "[[res]]\nkind = \"ram\"\nused_kib = 200\n"));
    write(&bud, "product.toml", "schema = 1\n[product]\nname = \"p\"\nversion = \"0.1.0.0\"\napp = \"\"\ncore = \">=1.0.0\"\nstage = \"dev\"\n[select]\nplugins = [\"platform/soc\", \"service/mem\"]\n[budget]\nram_kib = 100\nstack_kib = 8\n");
    let resp = check::run(&mk_req("check", &bud, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "V-19③: budget overrun ⇒ exit code 1");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("delta 100 KiB"),
        "V-19③: budget message carries the delta (200-100)",
    );
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("exceeds the platform capacity 64"),
        "V-7: physical limit (platform capacity) reported separately",
    );
    h.j(&resp.data["summary"]["closure"]["ram_kib"], json!(200), "S-14: platform [[res]] is capacity, not part of the consumer Σ");
}

fn iface_crossfile_cases(h: &mut Harness) {
    let root = tmpdir("skin-miss");
    write(&root, "iface/skin/brick.toml", "schema = 1\n[plugin]\nname = \"iface/skin\"\nplugin_type = \"interface\"\napi_type = \"native\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n[[export]]\napi_iface = \"native\"\nform = \"skin\"\nname = \"skin\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\nreexport_of = [\"service/nope#u\"]\n");
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-TAX-0018"), "V-12: skin points at a nonexistent unit ⇒ BRV-TAX-0018");

    let root2 = tmpdir("skin-cls");
    write(&root2, "service/back/brick.toml", &ability_toml("service/back", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    write(&root2, "iface/skin/brick.toml", "schema = 1\n[plugin]\nname = \"iface/skin\"\nplugin_type = \"interface\"\napi_type = \"runtime_adapter\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n[[export]]\napi_iface = \"runtime_adapter\"\nform = \"skin\"\nname = \"skin\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\nreexport_of = [\"service/back#u\"]\n");
    let resp = check::run(&mk_req("check", &root2, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-TAX-0018"), "V-12: skin classification mismatch ⇒ BRV-TAX-0018");

    let root3 = tmpdir("collide");
    write(&root3, "service/one/brick.toml", &ability_toml("service/one", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"dup\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\nsymbols = [\"symA\"]\n"));
    write(&root3, "service/two/brick.toml", &ability_toml("service/two", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"dup\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    let resp = check::run(&mk_req("check", &root3, json!({"profile": "dev"})));
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("unit name `dup`"),
        "§2 item 4: unit-level collision ⇒ reported",
    );

    let root4 = tmpdir("profile14");
    write(&root4, "service/prov/brick.toml", &ability_toml("service/prov", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    write(&root4, "service/cons/brick.toml", &ability_toml("service/cons", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"c\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n[[dep]]\nname = \"service/prov\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n[[compat.requires_iface]]\nid = \"service/prov#u\"\napi_iface = \"native\"\ncompat_gen = 0\nrange = \">=1.0.0\"\nmode = \"decl\"\n"));
    let dev = check::run(&mk_req("check", &root4, json!({"profile": "dev"})));
    let rel = check::run(&mk_req("check", &root4, json!({"profile": "release"})));
    h.eq(dev.exit_code, 0, "V-14: dev ⇒ exit code 0");
    h.eq(rel.exit_code, 1, "V-14: release ⇒ exit code 1");
    h.check(
        has_code_sev(&dev, "BRV-VER-0004", "info"),
        "V-14: under dev VER-0004 is info",
    );
    h.check(
        has_code_sev(&rel, "BRV-VER-0004", "error"),
        "V-14: under release VER-0004 is error",
    );

    let with_iface = solver::run(&mk_req("closure", &root4, json!({"profile": "dev"})));
    let root5 = tmpdir("profile-noiface");
    write(&root5, "service/prov/brick.toml", &ability_toml("service/prov", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    write(&root5, "service/cons/brick.toml", &ability_toml("service/cons", "service", "late", "[[dep]]\nname = \"service/prov\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    let without = solver::run(&mk_req("closure", &root5, json!({"profile": "dev"})));
    h.eq(
        serde_json::to_string(&with_iface.data.clone()).unwrap(),
        serde_json::to_string(&without.data.clone()).unwrap(),
        "V-10②: requires_iface does not affect closure data",
    );

    let show = iface::run(&mk_req("iface-show", &root4, json!({"id": "service/cons#c"})));
    h.check(
        has_code_sev(&show, "BRV-MF-0001", "info"),
        "V-10③: iface show reports info only for requires_iface",
    );

    h.j(&show.data["not_abi"], json!(true), "V-11①: not_abi=true");
    h.j(&show.data["truth"], json!("decl"), "V-11①: truth=decl");
    h.j(&show.data["hash_scope"], json!("decl"), "V-11①: hash_scope=decl");
}

fn priv_and_sched_cases(h: &mut Harness) {
    let root = tmpdir("res-conflict2");
    write(&root, "service/a/brick.toml", &ability_toml("service/a", "service", "late", "[[privileged.resources]]\nirq = [32]\ndma_channels = [3]\n"));
    write(&root, "service/b/brick.toml", &ability_toml("service/b", "service", "late", "[[privileged.resources]]\nirq = [32]\ndma_channels = [7]\n"));
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev", "scopes": ["priv"]})));
    h.eq(resp.exit_code, 1, "V-19④: IRQ exclusivity conflict ⇒ exit code 1");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("service/a") && msg_of(&resp, "BRV-MF-0001").contains("service/b"),
        "V-19④: reports both conflicting parties",
    );
    h.check(msg_of(&resp, "BRV-MF-0001").contains("irq `32`"), "V-19④: names the resource");

    let privroot = tmpdir("priv19");
    write(&privroot, "service/p/brick.toml", &ability_toml("service/p", "service", "late", "[privileged]\nlevel = \"P2\"\n[[privileged.memory]]\ngranularity = \"page\"\nops = [\"map\"]\nregions = [\"mmio\"]\n"));
    let resp = check::run(&mk_req("check", &privroot, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-PRIV-0001"), "V-19②: ability declares P2 yet uses map ⇒ BRV-PRIV-0001");
    h.check(
        !has_code(&resp, "BRV-PRIV-0002"),
        "§3.4: page×map×mmio is a legal combination (only the level is too low ⇒ PRIV-0001 only)",
    );
    let bad2 = tmpdir("priv19b2");
    write(&bad2, "service/pp/brick.toml", &ability_toml("service/pp", "service", "late", "[privileged]\nlevel = \"P4\"\n[[privileged.memory]]\ngranularity = \"pool\"\nops = [\"map\"]\nregions = [\"page\"]\n"));
    let resp2 = check::run(&mk_req("check", &bad2, json!({"profile": "dev"})));
    h.check(has_code(&resp2, "BRV-PRIV-0002"), "V-19①: illegal (ops×granularity×region) ⇒ BRV-PRIV-0002");
    h.check(
        !diag_triples(&resp)
            .iter()
            .filter(|(c, _, _)| c == "BRV-PRIV-0001")
            .collect::<Vec<_>>()
            .is_empty(),
        "V-19①: at least one PRIV-0001",
    );

    let pool = tmpdir("priv-pool");
    write(&pool, "service/q/brick.toml", &ability_toml("service/q", "service", "late", "[privileged]\nlevel = \"P2\"\n[[privileged.memory]]\ngranularity = \"pool\"\nops = [\"alloc\", \"free\"]\nregions = [\"contig\", \"page\"]\n"));
    let resp = check::run(&mk_req("check", &pool, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 0, "V-19①: P2 pool lifecycle is legal ⇒ exit code 0");

    let ttr = tmpdir("sched-tt");
    write(&ttr, "service/t/brick.toml", &ability_toml("service/t", "service", "late", ""));
    let p = ttr.join("service/t/brick.toml");
    let s = fs::read_to_string(&p).unwrap().replace("version = \"0.1.0.0\"", "version = \"0.1.0.0\"\nsched_class = \"TT_SAFE\"");
    fs::write(&p, s).unwrap();
    let resp = check::run(&mk_req("check", &ttr, json!({"profile": "dev", "scopes": ["tax"]})));
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("TT_SAFE"),
        "§2 item 3: TT_SAFE missing [sched.tt] ⇒ error",
    );

    let coopbad = tmpdir("sched-coopbad");
    write(&coopbad, "sched-preempt/brick.toml", &ability_toml("sched-preempt", "scheduler", "core", ""));
    write(&coopbad, "service/c/brick.toml", &ability_toml("service/c", "service", "late", ""));
    let p = coopbad.join("service/c/brick.toml");
    let s = fs::read_to_string(&p).unwrap().replace("version = \"0.1.0.0\"", "version = \"0.1.0.0\"\nsched_class = \"COOP_ONLY\"");
    fs::write(&p, s).unwrap();
    let resp = check::run(&mk_req("check", &coopbad, json!({"profile": "dev", "scopes": ["tax"]})));
    h.eq(resp.exit_code, 1, "§2 item 3: COOP_ONLY×preempt ⇒ exit code 1");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("sched-preempt"),
        "§2 item 3: names the scheduler",
    );

    let coopok = tmpdir("sched-coopok");
    write(&coopok, "sched-coop/brick.toml", &ability_toml("sched-coop", "scheduler", "core", ""));
    write(&coopok, "service/c/brick.toml", &ability_toml("service/c", "service", "late", ""));
    let p = coopok.join("service/c/brick.toml");
    let s = fs::read_to_string(&p).unwrap().replace("version = \"0.1.0.0\"", "version = \"0.1.0.0\"\nsched_class = \"COOP_ONLY\"");
    fs::write(&p, s).unwrap();
    let resp = check::run(&mk_req("check", &coopok, json!({"profile": "dev", "scopes": ["tax"]})));
    h.eq(resp.exit_code, 0, "§2 item 3: COOP_ONLY×coop ⇒ green (ruling R-5 name inference)");
}

fn advance_plan_cases(h: &mut Harness) {
    let hard = vec![iface::Change {
        kind: "CHANGED",
        name: "f".into(),
        from: Some("a".into()),
        to: Some("b".into()),
        old_status: "frozen".into(),
        note: String::new(),
    }];
    h.check(hard[0].is_hard(), "V-15: CHANGED/frozen ⇒ unfreeze required");
    let soft = vec![iface::Change {
        kind: "CHANGED",
        name: "f".into(),
        from: Some("a".into()),
        to: Some("b".into()),
        old_status: "experimental".into(),
        note: String::new(),
    }];
    h.check(!soft[0].is_hard(), "V-15: CHANGED/experimental ⇒ no unfreeze needed");
    let removed_frozen = vec![iface::Change {
        kind: "REMOVED",
        name: "f".into(),
        from: Some("f".into()),
        to: None,
        old_status: "frozen".into(),
        note: String::new(),
    }];
    h.check(removed_frozen[0].is_hard(), "V-15: REMOVED/frozen ⇒ unfreeze required");

    let a = version::advance(Version::new(0, 1, 4, 7), version::Event::CompatGenBump);
    h.eq(a, Version::new(1, 1, 0, 0), "V-4(a): change frozen ⇒ compat_gen+1, minor/revise→0");
    h.eq(a.major, 1, "V-4(a): major unchanged");
    let c = version::advance(Version::new(3, 1, 4, 7), version::Event::MajorProduct);
    h.eq(c, Version::new(3, 2, 0, 0), "V-4(c): major ⇒ MAJOR+1 only, compat_gen unchanged");
    h.eq(c.compat_gen, 3, "V-4(c): compat_gen and major are independent");
    let b = version::advance(Version::new(2, 5, 1, 9), version::Event::Added);
    h.eq(b, Version::new(2, 5, 2, 0), "V-4(b): added ⇒ compat_gen unchanged, MINOR+1");
    h.eq(
        version::advance(Version::new(0, 1, 2, 3), version::Event::MinorFeature),
        Version::new(0, 1, 3, 0),
        "V-4(d): minor feature ⇒ MINOR+1",
    );
    h.eq(
        version::advance(Version::new(0, 1, 2, 3), version::Event::ReviseFix),
        Version::new(0, 1, 2, 4),
        "V-4(e): bug fix ⇒ REVISE+1",
    );
    h.eq(
        version::advance(Version::new(2, 3, 4, 5), version::Event::EmptyUnfreeze),
        Version::new(2, 3, 4, 5),
        "V-4(f): empty unfreeze ⇒ all four segments unchanged",
    );
}

fn iface_ir_cases(h: &mut Harness) {
    let mk = |kind: &str, name: &str, sig: Option<&str>, layout: Option<&str>, value: Option<&str>, ops: Vec<&str>| iface::Entry {
        kind: kind.into(),
        name: name.into(),
        sig: sig.map(str::to_string),
        layout: layout.map(str::to_string),
        value: value.map(str::to_string),
        ops: ops.into_iter().map(str::to_string).collect(),
        status: "frozen".into(),
    };

    let e1 = mk("enum", "color", None, None, Some("RED,GREEN"), vec![]);
    let e2 = mk("enum", "color", None, None, Some("RED,GREEN,BLUE"), vec![]);
    let e3 = mk("enum", "color", None, None, Some("GREEN,RED,BLUE"), vec![]);
    h.eq(iface::classify_modify(&e1, &e2), "EXTENDED", "V-15: append at end of enum ⇒ EXTENDED");
    h.eq(iface::classify_modify(&e1, &e3), "CHANGED", "V-15: enum reorder ⇒ CHANGED");
    let t1 = mk("type", "ctx", None, Some("struct { u32 a; }"), None, vec![]);
    let t2 = mk("type", "ctx", None, Some("struct { u32 a; u32 b; }"), None, vec![]);
    h.eq(iface::classify_modify(&t1, &t2), "CHANGED", "V-15: add field to frozen struct ⇒ CHANGED");
    let s1 = mk("service", "svc", None, None, None, vec!["open"]);
    let s2 = mk("service", "svc", None, None, None, vec!["open", "close"]);
    h.eq(iface::classify_modify(&s1, &s2), "CHANGED", "V-15: adding a service op slot ⇒ CHANGED");
    let f1 = mk("func", "f", Some("int(int)"), None, None, vec![]);
    let f2 = mk("func", "f", Some("long(int)"), None, None, vec![]);
    h.eq(iface::classify_modify(&f1, &f2), "CHANGED", "V-15: func signature change ⇒ CHANGED");
    let m1 = mk("macro", "M", None, None, Some("16"), vec![]);
    let m2 = mk("macro", "M", None, None, Some("4096"), vec![]);
    h.eq(iface::classify_modify(&m1, &m2), "CHANGED", "V-15: macro value change ⇒ CHANGED");

    let base = vec![
        mk("macro", "BR_MAX", None, None, Some("16"), vec![]),
        mk("service", "svc", None, None, None, vec!["open"]),
        mk("enum", "color", None, None, Some("RED,GREEN"), vec![]),
    ];
    let h_base = iface::surface_hash("p#u", &base, &[], false);
    let mut macro_ch = base.clone();
    macro_ch[0].value = Some("4096".into());
    h.check(
        iface::surface_hash("p#u", &macro_ch, &[], false) != h_base,
        "V-17: BR_MAX 16→4096 yields a different hash",
    );
    let mut ops_ch = base.clone();
    ops_ch[1].ops = vec!["open".into(), "close".into()];
    h.check(
        iface::surface_hash("p#u", &ops_ch, &[], false) != h_base,
        "V-17: adding a service op yields a different hash",
    );
    let mut enum_re = base.clone();
    enum_re[2].value = Some("GREEN,RED".into());
    h.check(
        iface::surface_hash("p#u", &enum_re, &[], false) != h_base,
        "V-17: enum reorder is seen by the hash",
    );
    let mut enum_app = base.clone();
    enum_app[2].value = Some("RED,GREEN,BLUE".into());
    h.check(
        iface::surface_hash("p#u", &enum_app, &[], false) != h_base,
        "V-17: enum append is seen by the hash",
    );
    let mut st_ch = base.clone();
    st_ch[0].status = "deprecated".into();
    h.eq(
        iface::surface_hash("p#u", &st_ch, &[], false),
        h_base.clone(),
        "§6.2 rule 6: status is not part of the hash",
    );
    let mut ordered = base.clone();
    ordered.reverse();
    h.eq(
        iface::surface_hash("p#u", &ordered, &[], false),
        h_base.clone(),
        "§6.2 rule 1: declaration order does not affect the hash",
    );
    h.check(
        iface::surface_hash("p#v", &base, &[], false) != h_base,
        "§6.2 rule 10: domain separation (unit id) prevents cross-unit collisions",
    );
    let mut sig_a = vec![mk("func", "f", Some("int(const u32 * buf, size_t len)"), None, None, vec![])];
    let mut sig_b = vec![mk("func", "f", Some("int (const u32 *buf, size_t n)"), None, None, vec![])];
    h.eq(
        iface::surface_hash("p#u", &sig_a, &[], false),
        iface::surface_hash("p#u", &sig_b, &[], false),
        "§6.2 rule 3: param names/layout are not hashed (non-strict)",
    );
    h.check(
        iface::surface_hash("p#u", &sig_a, &[], true) != iface::surface_hash("p#u", &sig_b, &[], true),
        "§6.2 rule 3: with --strict-params param names enter the hash",
    );
    sig_a[0].sig = Some("int(const uint32_t* buf, size_t len)".into());
    sig_b[0].sig = Some("int(const u32* buf, size_t len)".into());
    let td = vec![("u32".to_string(), "uint32_t".to_string())];
    h.eq(
        iface::surface_hash("p#u", &sig_a, &td, false),
        iface::surface_hash("p#u", &sig_b, &td, false),
        "§6.2 rule 4: after typedef expansion both spellings of one type hash the same",
    );
    h.check(
        h_base.starts_with("sha256:") && h_base.len() == 7 + 64,
        "§6.2 rule 8: hash = sha256:<64 hex>",
    );
    h.eq(iface::short_hash(Some(&h_base)).len(), 12, "§5.1: display takes 12 hex");
    h.eq(iface::normalize_ws("  int   a  "), "int a".to_string(), "§6.2 rule 2: whitespace normalization");
    h.check(
        iface::expand_typedefs("u32 x", &td) == "uint32_t x",
        "§6.2 rule 4: aliases expand on identifier boundaries",
    );
    h.check(
        iface::expand_typedefs("my_u32 x", &td) == "my_u32 x",
        "§6.2 rule 4: no substring replacement",
    );

    let old = iface::Surface {
        id: "p#u".into(),
        entries: vec![
            mk("func", "keep", Some("void(void)"), None, None, vec![]),
            mk("func", "gone", Some("void(void)"), None, None, vec![]),
            mk("enum", "e", None, None, Some("A"), vec![]),
        ],
        ..Default::default()
    };
    let mut new = old.clone();
    new.entries = vec![
        mk("func", "keep", Some("int(void)"), None, None, vec![]),
        mk("func", "brand_new", Some("void(void)"), None, None, vec![]),
        mk("enum", "e", None, None, Some("A,B"), vec![]),
    ];
    let cs = iface::change_set(&old, &new);
    let kinds: Vec<&str> = cs.iter().map(|c| c.kind).collect();
    h.check(kinds.contains(&"CHANGED"), "V-15: change_set contains CHANGED (sig)");
    h.check(kinds.contains(&"ADDED"), "V-15: change_set contains ADDED");
    h.check(kinds.contains(&"REMOVED"), "V-15: change_set contains REMOVED");
    h.check(kinds.contains(&"EXTENDED"), "V-15: change_set contains EXTENDED (enum end)");
}

const PUB_UNIT: &str = "service/pub/brick.toml";
fn pub_toml(macro_value: &str) -> String {
    format!(
        "schema = 1\n[plugin]\nname = \"service/pub\"\nplugin_type = \"ability\"\napi_type = \"native\"\n\
         subkind = \"service\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n\
         [compat]\ncore = \">=1.0.0\"\n\
         [[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\n\
         compat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n\
         [[export.entries]]\nkind = \"macro\"\nname = \"BR_MAX\"\nvalue = \"{macro_value}\"\nstatus = \"experimental\"\n"
    )
}

fn publish_cases(h: &mut Harness) {
    let root = tmpdir("publish");
    write(&root, PUB_UNIT, &pub_toml("16"));
    let r1 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/pub#u"})));
    h.eq(r1.exit_code, 0, "V-5: first publish exit code 0");
    h.eq(r1.files.as_array().map(|a| a.len()), Some(3), "V-5: first run yields 3 machine files");
    let paths: Vec<String> = r1
        .files
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f["path"].as_str().unwrap_or("").to_string())
        .collect();
    h.check(
        paths.contains(&"api/iface/service/pub/u.toml".to_string()),
        "V-5: snapshot path = api/iface/<provider path segments>/<unit>.toml (R-11)",
    );
    h.check(
        paths.contains(&"api/iface/CHANGELOG.md".to_string()),
        "V-5: produces CHANGELOG.md",
    );
    h.check(paths.contains(&"brickie.lock".to_string()), "V-5: produces brickie.lock");
    h.check(
        r1.files.as_array().unwrap().iter().all(|f| f["kind"] == "machine"),
        "§4: all kind=machine",
    );
    let snapshot_text = r1.files.as_array().unwrap()[0]["content"].as_str().unwrap_or("");
    h.check(snapshot_text.contains("NOT_ABI"), "V-11: snapshot header carries NOT_ABI");
    h.check(snapshot_text.contains("hash_scope = \"decl\""), "V-11: snapshot carries hash_scope");
    h.check(snapshot_text.contains("truth = \"decl\""), "V-11: snapshot carries truth");
    h.check(snapshot_text.contains("strict_params = false"), "§6.2 rule 10: strict_params stored in the snapshot");
    apply_files(&root, &r1);

    let r2 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/pub#u"})));
    h.eq(r2.exit_code, 0, "V-5: second publish exit code 0");
    h.eq(r2.files.as_array().map(|a| a.len()), Some(0), "V-5: surface unchanged ⇒ no files (no-op)");
    h.j(&r2.data["noop"], json!(true), "V-5: data.noop = true");
    h.j(&r2.data["version"], json!("0.1.0.0"), "V-5: no-op does not advance segments");

    let root_b = tmpdir("publish-b");
    write(&root_b, PUB_UNIT, &pub_toml("16"));
    let rb = iface::run(&mk_req("iface-publish", &root_b, json!({"id": "service/pub#u"})));
    h.eq(
        serde_json::to_string(&r1.files.clone()).unwrap(),
        serde_json::to_string(&rb.files.clone()).unwrap(),
        "§4: two publishes of the same input are byte-identical",
    );

    let st0 = iface::run(&mk_req("iface-status", &root, json!({"id": "service/pub#u", "check": true})));
    h.j(&st0.data["consistent"], json!(true), "V-5: after publish status --check is consistent");
    h.eq(st0.exit_code, 0, "V-5: status --check exit code 0");
    write(&root, PUB_UNIT, &pub_toml("4096"));
    let st1 = iface::run(&mk_req("iface-status", &root, json!({"id": "service/pub#u", "check": true})));
    h.j(&st1.data["consistent"], json!(false), "V-17: after the BR_MAX change status is inconsistent");
    h.eq(st1.exit_code, 1, "V-17: inconsistent ⇒ exit code 1");
    let d = iface::run(&mk_req("iface-diff", &root, json!({"id": "service/pub#u"})));
    h.j(&d.data["changes"][0]["kind"], json!("CHANGED"), "V-15: diff reports CHANGED");
    h.j(&d.data["version"]["to"], json!("0.1.1.0"), "V-4: experimental-only change ⇒ MINOR+1");
    let r3 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/pub#u"})));
    h.eq(r3.exit_code, 0, "V-17: publish after the change succeeds");
    h.j(&r3.data["to"], json!("0.1.1.0"), "V-4(b): add/change ⇒ COMPAT_GEN unchanged");
    h.j(&r3.data["compat_gen_changed"], json!(false), "V-4(b): compat_gen_changed=false");
    apply_files(&root, &r3);
    let st2 = iface::run(&mk_req("iface-status", &root, json!({"id": "service/pub#u", "check": true})));
    h.j(&st2.data["consistent"], json!(true), "V-5: after republishing status is consistent");
}

fn freeze_window_cases(h: &mut Harness) {
    let root = tmpdir("unfreeze");
    let toml_frozen = "schema = 1\n[plugin]\nname = \"service/w\"\nplugin_type = \"ability\"\napi_type = \"native\"\n\
        subkind = \"service\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n\
        [[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\n\
        compat_gen = 0\nfreeze_state = \"frozen\"\nhash = \"\"\nstatus = \"frozen\"\n\
        [[export.entries]]\nkind = \"type\"\nname = \"ctx\"\nlayout = \"struct { u32 a; }\"\nstatus = \"frozen\"\n";
    write(&root, "service/w/brick.toml", toml_frozen);
    write(&root, "NOTE.md", "decision record\n");
    let r0 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/w#u"})));
    h.eq(r0.exit_code, 0, "V-16: initial publish succeeds");
    apply_files(&root, &r0);
    let ver0 = r0.data["to"].clone();

    let r1 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u"})));
    h.eq(r1.exit_code, 1, "V-16: unfreeze without --note ⇒ error");

    let r2 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    h.eq(r2.exit_code, 0, "V-16: unfreeze succeeds");
    h.j(&r2.data["freeze_state"], json!("unfreezing"), "V-16: enters the unfreeze window");
    let snap_text = r2.files.as_array().unwrap()[0]["content"].as_str().unwrap_or("");
    h.check(snap_text.contains("baseline_hash"), "ruling R-7: baseline recorded in the snapshot");
    apply_files(&root, &r2);

    let rel = check::run(&mk_req("check", &root, json!({"profile": "release"})));
    h.eq(rel.exit_code, 1, "V-16: release inside the window ⇒ exit code 1");
    h.check(has_code(&rel, "BRV-IFACE-0009"), "V-16: release inside the window ⇒ BRV-IFACE-0009");
    let dev = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.eq(dev.exit_code, 0, "V-16: dev inside the window ⇒ exit code 0");

    let r3 = iface::run(&mk_req("iface-refreeze", &root, json!({"id": "service/w#u"})));
    h.eq(r3.exit_code, 0, "V-16: refreeze succeeds");
    h.j(&r3.data["empty_unfreeze"], json!(true), "V-16: detected as an empty unfreeze");
    h.j(&r3.data["from"], ver0.clone(), "V-16: empty unfreeze ⇒ original version");
    h.j(&r3.data["to"], ver0.clone(), "V-16: empty unfreeze ⇒ all four segments unchanged");
    h.j(&r3.data["compat_gen_changed"], json!(false), "V-16: empty unfreeze does not bump COMPAT_GEN");
    apply_files(&root, &r3);

    let r4 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    apply_files(&root, &r4);
    write(&root, "service/w/brick.toml", &toml_frozen.replace("struct { u32 a; }", "struct { u32 a; u32 b; }"));
    let r5 = iface::run(&mk_req("iface-refreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    h.eq(r5.exit_code, 0, "V-4(a): refreeze changing a frozen entry succeeds");
    h.j(&r5.data["compat_gen_changed"], json!(true), "V-4(a): a frozen entry really changed ⇒ COMPAT_GEN+1");
    h.j(&r5.data["compat_gen"], json!(1), "V-4(a): compat_gen goes from 0 to 1");
    h.check(
        r5.data["changes"]
            .as_array()
            .map(|a| a.iter().any(|c| c["kind"] == json!("CHANGED")))
            .unwrap_or(false),
        "V-4(a): change set contains CHANGED",
    );
    apply_files(&root, &r5);
    let r6 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    h.eq(r6.exit_code, 0, "unfreeze requires frozen (just refrozen back to frozen)");
    apply_files(&root, &r6);

    write(
        &root,
        "service/w/brick.toml",
        &toml_frozen
            .replace("struct { u32 a; }", "struct { u32 a; u32 b; u32 c; }"),
    );
    let p1 = iface::run(&mk_req(
        "iface-publish",
        &root,
        json!({"id": "service/w#u", "note": "NOTE.md"}),
    ));
    h.eq(p1.exit_code, 0, "S-19: in-window publish of a hard change (with note) succeeds");
    h.j(&p1.data["compat_gen_changed"], json!(true), "S-19: publish records COMPAT_GEN+1");
    let cg1 = p1.data["compat_gen"].clone();
    h.j(&p1.data["freeze_state"], json!("unfreezing"), "S-19: publish does not erase the unfreeze window");
    apply_files(&root, &p1);
    let p2 = iface::run(&mk_req("iface-refreeze", &root, json!({"id": "service/w#u"})));
    h.eq(p2.exit_code, 0, "S-19: refreeze closes the window");
    h.j(&p2.data["empty_unfreeze"], json!(true), "S-19: publish already consumed the baseline ⇒ empty unfreeze");
    h.j(&p2.data["compat_gen"], cg1, "S-19: COMPAT_GEN is not bumped twice");
}

fn gate_cases(h: &mut Harness) {
    let root = tmpdir("removed-gate");
    let toml_gone = "schema = 1\n[plugin]\nname = \"service/g\"\nplugin_type = \"ability\"\napi_type = \"native\"\n\
        subkind = \"service\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n\
        [[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\n\
        compat_gen = 0\nfreeze_state = \"frozen\"\nhash = \"\"\nstatus = \"frozen\"\n\
        [[export.entries]]\nkind = \"func\"\nname = \"gone\"\nsig = \"void(void)\"\nstatus = \"frozen\"\n\
        [[export.entries]]\nkind = \"func\"\nname = \"keep\"\nsig = \"void(void)\"\nstatus = \"frozen\"\n";
    write(&root, "service/g/brick.toml", toml_gone);
    write(&root, "NOTE.md", "n\n");
    let r0 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/g#u"})));
    apply_files(&root, &r0);
    let s2 = toml_gone.replace(
        "[[export.entries]]\nkind = \"func\"\nname = \"gone\"\nsig = \"void(void)\"\nstatus = \"frozen\"\n",
        "",
    );
    write(&root, "service/g/brick.toml", &s2);

    let r1 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/g#u", "note": "NOTE.md"})));
    h.eq(r1.exit_code, 1, "§6.3: REMOVED not deprecated ⇒ error");
    h.check(
        msg_of2(&r1).contains("REMOVED"),
        "§6.3: message says REMOVED must be deprecated first",
    );

    let f = iface::run(&mk_req(
        "iface-freeze",
        &root,
        json!({"id": "service/g#u", "entry": "keep", "note": "NOTE.md"}),
    ));
    h.eq(f.exit_code, 0, "iface freeze: entry filtering succeeds");
    let content = f.files[0]["content"].as_str().unwrap_or("");
    h.check(content.contains("requested_entry = \"keep\""), "iface freeze: records the entry");
    h.check(
        !content.contains("name = \"gone\""),
        "iface freeze: proposal contains only the named entry",
    );
}

fn dep_command_cases(h: &mut Harness) {
    let root = tmpdir("depcmds");
    write(&root, "service/consumer/brick.toml", &ability_toml("service/consumer", "service", "late", "[[dep]]\nname = \"service/provider\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    write(&root, "service/provider/brick.toml", &ability_toml("service/provider", "service", "late", ""));

    let g = solver::run(&mk_req("dep-graph", &root, json!({"format": "json"})));
    h.j(&g.data["format"], json!("json"), "dep graph: format");
    let text = g.data["text"].as_str().unwrap_or("");
    let parsed: Result<Value, _> = serde_json::from_str(text);
    h.check(parsed.is_ok(), "dep graph(json): data.text is a canonical JSON string");
    h.eq(
        parsed.map(|v| v["edges"].as_array().map(|a| a.len())).unwrap_or(None),
        Some(1),
        "dep graph(json): 1 edge",
    );
    let dot = solver::run(&mk_req("dep-graph", &root, json!({"format": "dot"})));
    h.check(
        dot.data["text"].as_str().unwrap_or("").contains("digraph"),
        "dep graph(dot): produces dot",
    );
    let mer = solver::run(&mk_req("dep-graph", &root, json!({"format": "mermaid"})));
    h.check(
        mer.data["text"].as_str().unwrap_or("").contains("graph LR"),
        "dep graph(mermaid): produces mermaid",
    );
    let badfmt = solver::run(&mk_req("dep-graph", &root, json!({"format": "svg"})));
    h.eq(badfmt.exit_code, 2, "dep graph: unknown format ⇒ usage error 2");

    let w = solver::run(&mk_req("dep-why", &root, json!({"from": "service/consumer", "to": "service/provider"})));
    h.j(&w.data["found"], json!(true), "dep why: path found");
    h.j(&w.data["path"], json!(["service/consumer", "service/provider"]), "dep why: shortest path");
    h.j(&w.data["edges"][0]["kind"], json!("runtime"), "dep why: edge carries kind");
    let w2 = solver::run(&mk_req("dep-why", &root, json!({"from": "service/provider", "to": "service/consumer"})));
    h.j(&w2.data["found"], json!(false), "dep why: reverse direction not found ⇒ found=false");
    h.eq(w2.exit_code, 0, "dep why: not found is not an error (exit 0)");

    let t = solver::run(&mk_req("dep-tree", &root, json!({"kind": "init"})));
    h.j(&t.data["kind"], json!("init"), "dep tree: kind=init");
    let t2 = solver::run(&mk_req("dep-tree", &root, json!({"kind": "bogus"})));
    h.eq(t2.exit_code, 2, "dep tree: unknown kind ⇒ usage error 2");

    let idx = solver::run(&mk_req("dep-index", &root, json!({})));
    h.eq(idx.files.as_array().map(|a| a.len()), Some(1), "dep index: one machine file");
    h.j(&idx.files[0]["path"], json!("build/index/dependents.json"), "dep index: path");
    let content = idx.files[0]["content"].as_str().unwrap_or("");
    h.check(content.contains("\"schema\": 1"), "dep index: JSON contains schema");
    h.j(&idx.data["dependents"]["service/provider"][0]["from"], json!("service/consumer"), "dep index: reverse edge");
    let idx2 = solver::run(&mk_req("dep-index", &root, json!({})));
    h.eq(
        idx.files[0]["content"].clone(),
        idx2.files[0]["content"].clone(),
        "dep index: byte-for-byte reproducible",
    );
}

fn ver_command_cases(h: &mut Harness) {
    let root = tmpdir("vercmds");
    write(&root, "service/v/brick.toml", &ability_toml("service/v", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"1.2.3.4\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    let s = iface::run(&mk_req("ver-show", &root, json!({"id": "service/v#u"})));
    h.eq(s.exit_code, 0, "ver show: exit code 0");
    h.j(&s.data["version"], json!("1.2.3.4"), "ver show: four-segment version");
    h.j(&s.data["segments"], json!({"compat_gen": 1, "major": 2, "minor": 3, "revise": 4}), "ver show: segments");
    h.j(&s.data["source"], json!("declaration"), "ver show: unpublished ⇒ source=declaration");
    h.j(&s.data["compat_gen_source"]["kind"], json!("never-frozen"), "ver show: compat_gen source");

    let b = iface::run(&mk_req("ver-bump", &root, json!({"id": "service/v#u", "rule": "revise"})));
    h.eq(b.exit_code, 0, "ver bump: exit code 0");
    h.j(&b.data["from"], json!("1.2.3.4"), "ver bump: from");
    h.j(&b.data["to"], json!("1.2.3.5"), "V-4(e): ver bump revise ⇒ REVISE+1");
    h.eq(b.files.as_array().map(|a| a.len()), Some(2), "ver bump: writes snapshot + CHANGELOG");
    h.check(
        b.files
            .as_array()
            .unwrap()
            .iter()
            .any(|f| f["path"] == json!("api/iface/service/v/u.toml")),
        "ver bump: writes the unit snapshot",
    );
    apply_files(&root, &b);
    let s2 = iface::run(&mk_req("ver-show", &root, json!({"id": "service/v#u"})));
    h.j(&s2.data["source"], json!("snapshot"), "ver show: after publish source=snapshot");
    h.j(&s2.data["published"], json!(true), "ver show: published=true");

    let maj = iface::run(&mk_req("ver-bump", &root, json!({"id": "service/v#u", "rule": "major"})));
    h.j(&maj.data["to"], json!("1.3.0.0"), "V-4(c): ver bump major ⇒ MAJOR+1, compat_gen unchanged");
    h.j(&maj.data["compat_gen"], json!(1), "V-4(c): compat_gen unchanged");

    let bad = iface::run(&mk_req("ver-bump", &root, json!({"id": "service/v#u", "rule": "compat_gen"})));
    h.eq(bad.exit_code, 2, "ver bump: compat_gen is not a rule ⇒ usage error 2");

    let root2 = tmpdir("vermulti");
    write(&root2, "service/m/brick.toml", &ability_toml("service/m", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"a\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"b\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    let amb = iface::run(&mk_req("ver-show", &root2, json!({"id": "service/m"})));
    h.eq(amb.exit_code, 2, "R-3: plugin name with multiple exports ⇒ usage error 2");

    let list = iface::run(&mk_req("iface-list", &root, json!({})));
    h.j(&list.data["count"], json!(1), "iface list: count");
    h.j(&list.data["units"][0]["id"], json!("service/v#u"), "iface list: unit id");
    h.j(&list.data["units"][0]["published"], json!(true), "iface list: published");

    let f0 = iface::run(&mk_req("iface-freeze", &root, json!({"id": "service/v#u"})));
    h.eq(f0.exit_code, 1, "A-26: freeze without --note ⇒ error");
    write(&root, "NOTE.md", "note\n");
    let f1 = iface::run(&mk_req("iface-freeze", &root, json!({"id": "service/v#u", "note": "NOTE.md"})));
    h.eq(f1.exit_code, 0, "A-26: freeze produces a proposal");
    h.j(&f1.data["proposal"], json!("build/gen/proposals/u.toml"), "A-26: proposal path build/gen/proposals/<unit>.toml");
    h.eq(
        f1.files
            .as_array()
            .unwrap()
            .iter()
            .filter(|f| f["path"].as_str().unwrap_or("").starts_with("api/iface/"))
            .count(),
        0,
        "A-26: no frozen snapshot written",
    );
    let proposal = f1.files[0]["content"].as_str().unwrap_or("");
    h.check(
        proposal.contains("v01_no_frozen_snapshot = true"),
        "A-26: proposal explicitly states v0.1 writes no frozen snapshot",
    );
}

pub fn run() -> i32 {
    let mut h = Harness::new();
    version_matrix(&mut h);
    range_cases(&mut h);
    name_and_subkind(&mut h);
    phase_cases(&mut h);
    privilege_cases(&mut h);
    dep_direction_cases(&mut h);
    export_invariant_cases(&mut h);
    model_cases(&mut h);
    driver_namespace_cases(&mut h);
    vendor_provenance_cases(&mut h);
    snapshot_and_lock_cases(&mut h);
    plan_cases(&mut h);
    emit_cases(&mut h);
    cycle_cases(&mut h);
    core_type_cases(&mut h);
    phase_check_cases(&mut h);
    version_solver_cases(&mut h);
    iface_crossfile_cases(&mut h);
    priv_and_sched_cases(&mut h);
    advance_plan_cases(&mut h);
    iface_ir_cases(&mut h);
    publish_cases(&mut h);
    freeze_window_cases(&mut h);
    dep_command_cases(&mut h);
    ver_command_cases(&mut h);
    gate_cases(&mut h);
    build_cases(&mut h);

    if h.failures.is_empty() {
        println!("ok {} cases", h.passed);
        0
    } else {
        for f in &h.failures {
            println!("FAIL {f}");
        }
        println!(
            "brickie-core selftest: {} failed / {} passed",
            h.failures.len(),
            h.passed
        );
        1
    }
}

/// Whether `Diags` contains a code (unlike the Response `has_code`: planning returns Diags).
fn diags_code(d: &Diags, code: &str) -> bool {
    d.items().iter().any(|x| x.code.as_deref() == Some(code))
}

fn mk_req_ctx(command: &str, root: &Path, args: Value, context: Value) -> Request {
    let mut r = mk_req(command, root, args);
    r.context = ctx(context);
    r
}

/// Synthetic buildable tree: product.toml + platform (with [build.target]) + ability + app, plus
/// source files so globs match; the plan is computed from the declaration surface, not mocked. Tool
/// resolution is off via `context.path = ""`, so any machine replays it.
fn write_build_tree(root: &Path, extra_build: &str, target: &str) {
    write(root, "core/c_core/src/core.c", "int core_fn(void){return 0;}\n");
    write(root, "platform/fake/src/plat.c", "int plat_fn(void){return 0;}\n");
    write(root, "platform/fake/src/start.S", "nop\n");
    write(root, "platform/fake/src/link.ld", "SECTIONS { }\n");
    write(root, "app/m/src/main.c", "int app_fn(void){return 0;}\n");
    write(
        root,
        "product.toml",
        &format!(
            "schema = 1\n\
             [product]\nname = \"p\"\nversion = \"1.0.0.0\"\napp = \"app/m\"\ncore = \">=0.1.0\"\nstage = \"dev\"\n\
             [select]\nplugins = [\"core/c_core\", \"platform/fake\"]\n\
             [build]\n\
             cflags = [\"-std=c11\", \"-O2\"]\nasflags = [\"-g3\"]\nldflags = [\"-nostdlib\"]\n{extra_build}"
        ),
    );
    write(
        root,
        "core/c_core/brick.toml",
        "schema = 1\n[plugin]\nname = \"core/c_core\"\nplugin_type = \"core\"\napi_type = \"native\"\n\
         lang = \"c\"\nphase = \"early\"\nversion = \"1.0.0.0\"\n\
         [build]\nsources = [\"src/*.c\"]\nincludes = [\"include\"]\n",
    );
    write(
        root,
        "platform/fake/brick.toml",
        &format!(
            "schema = 1\n[plugin]\nname = \"platform/fake\"\nplugin_type = \"platform\"\napi_type = \"native\"\n\
             lang = \"c\"\nphase = \"early\"\nversion = \"1.0.0.0\"\n\
             [build]\nsources = [\"src/*.c\", \"src/*.S\"]\nincludes = [\"include\"]\n{target}"
        ),
    );
    write(
        root,
        "app/m/brick.toml",
        "schema = 1\n[plugin]\nname = \"app/m\"\nplugin_type = \"app\"\napi_type = \"native\"\n\
         lang = \"c\"\nphase = \"app\"\nversion = \"1.0.0.0\"\n\
         [build]\nsources = [\"src/*.c\"]\nincludes = [\"include\"]\n",
    );
}

const TARGET_OK: &str = "[build.target]\narch = \"aarch64\"\ncross = \"aarch64-linux-gnu-\"\n\
     arch_flags = [\"-march=armv8-a\", \"-mstrict-align\"]\nlinker_script = \"src/link.ld\"\n\
     [build.target.qemu]\nbinary = \"qemu-system-aarch64\"\nmachine = \"virt,gic-version=3\"\n\
     cpu = \"cortex-a53\"\nmemory = \"128M\"\nextra = [\"-nographic\"]\n";

fn build_cases(h: &mut Harness) {
    h.check(build::selftest_glob("*.c", "a.c"), "glob_match: *.c matches a.c");
    h.check(!build::selftest_glob("*.c", "a.h"), "glob_match: *.c does not match a.h");
    h.check(build::selftest_glob("a?c", "abc"), "glob_match: a?c matches abc");
    h.check(
        build::selftest_glob("*_test.c", "mem_test.c"),
        "glob_match: *_test.c matches mem_test.c",
    );
    h.check(!build::selftest_glob("*_test.c", "test_mem.c"), "glob_match: different prefix does not match");

    h.check(
        build::selftest_regex("pass=[0-9]* fail=0 ", "x pass=35 fail=0 y"),
        "regex: [0-9]* matches",
    );
    h.check(
        !build::selftest_regex("pass=[0-9]+ fail=0 ", "pass= fail=0 "),
        "regex: + requires at least one",
    );
    h.check(
        build::selftest_regex(r"\[PANIC\]", "x [PANIC] y"),
        "regex: \\[ is a literal square bracket",
    );
    h.check(
        !build::selftest_regex(r"\[PANIC\]", "x PANIC y"),
        "regex: after escaping it no longer matches the bare word",
    );
    h.check(
        build::selftest_regex("irq_ticks=[1-9]", "irq_ticks=7"),
        "regex: character-class range matches",
    );
    h.check(
        !build::selftest_regex("irq_ticks=[1-9]", "irq_ticks=0"),
        "regex: outside the range does not match",
    );
    h.check(
        build::selftest_regex("[^0-9]x", "ax"),
        "regex: negated character class matches",
    );
    h.check(
        build::selftest_regex_unsupported("(a|b)").is_some(),
        "regex: grouping/alternation is **rejected** (no silent non-match)",
    );
    h.check(
        build::selftest_regex_unsupported("^a$").is_some(),
        "regex: anchors are rejected",
    );
    h.check(
        build::selftest_regex_unsupported(r"\[A\] pass=[0-9]*").is_none(),
        "regex: every form used in gates.toml is inside the subset",
    );

    let root = tmpdir("build-ok");
    write_build_tree(&root, "", TARGET_OK);
    let req = mk_req_ctx(
        "build",
        &root,
        json!({"dry_run": true}),
        json!({"path": ""}),
    );
    let (d, data) = build::selftest_plan(&root, &req);
    h.exit_is(&d, 0, "build: synthetic-tree plan has no diagnostics");
    h.j(&data["unit_count"], json!(3), "build: unit count = core + platform + app");
    h.j(&data["source_count"], json!(4), "build: source count = core 1 + plat 2 + app 1");
    let steps = data["steps"].as_array().cloned().unwrap_or_default();
    h.j(&json!(steps.len()), json!(6), "build: step count = 4 compiles + link + objcopy");
    let all: Vec<String> = steps
        .iter()
        .map(|s| {
            s["argv"]
                .as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|x| x.as_str())
                        .collect::<Vec<_>>()
                        .join(" ")
                })
                .unwrap_or_default()
        })
        .collect();
    h.check(
        all.iter().any(|s| s.contains("-march=armv8-a") && s.contains("-mstrict-align")),
        "build: arch_flags enter the compile command (from platform's [build.target])",
    );
    h.check(
        all.iter().any(|s| s.contains("-Wl,-T,platform/fake/src/link.ld")),
        "build: linker script comes from platform and resolves against the plugin root",
    );
    h.check(
        all.iter().any(|s| s.contains("-MF build/obj/core/c_core/src/core.o.d")),
        "build: compile carries -MMD/-MF (the only source of header deps)",
    );
    h.check(
        all.iter().any(|s| s.contains("-nostdlib") && s.contains("build/brick.elf")),
        "build: link command carries product ldflags and the artifact location",
    );
    h.check(
        data["target"]["owner"] == json!("platform/fake"),
        "build: target facts are owned by the single platform",
    );
    h.check(
        data["outputs"]["obj_dir"] == json!("build/obj"),
        "build: object output defaults to build/obj",
    );
    h.check(
        data["tools"]
            .as_array()
            .map(|a| a.iter().any(|t| t["name"] == json!("cc")
                && t["candidates"]
                    .as_array()
                    .map(|c| c.iter().any(|x| x.as_str() == Some("aarch64-linux-gnu-gcc-16")))
                    .unwrap_or(false)))
            .unwrap_or(false),
        "build: cc candidates include versioned variants (Ubuntu with only gcc-16 installed)",
    );
    h.check(
        data["state_content"]
            .as_str()
            .map(|s| s.contains("[[jobs]]") && s.contains("argv_hash"))
            .unwrap_or(false),
        "build: incremental state is TOML produced by core (argv fingerprint + input table)",
    );
    h.check(
        data["plan_hash"].as_str().map(|s| s.starts_with("sha256:")).unwrap_or(false),
        "build: plan fingerprint is sha256:...",
    );

    let root2 = tmpdir("build-nobuild");
    write_build_tree(&root2, "", TARGET_OK);
    write(
        &root2,
        "product.toml",
        "schema = 1\n[product]\nname = \"p\"\napp = \"app/m\"\n",
    );
    let (d2, _) = build::selftest_plan(&root2, &mk_req_ctx("build", &root2, json!({}), json!({"path": ""})));
    h.check(diags_code(&d2, build::CODE_NO_BUILD), "build: missing [build] ⇒ BRV-BLD-0001");
    h.exit_is(&d2, 2, "build: missing [build] is a shape error (exit code 2)");

    let root3 = tmpdir("build-notarget");
    write_build_tree(&root3, "", "");
    let (d3, _) = build::selftest_plan(&root3, &mk_req_ctx("build", &root3, json!({}), json!({"path": ""})));
    h.check(diags_code(&d3, build::CODE_NO_TARGET), "build: no [build.target] ⇒ BRV-BLD-0003");

    let root4 = tmpdir("build-ambiguous");
    write_build_tree(&root4, "", TARGET_OK);
    write(
        &root4,
        "platform/other/brick.toml",
        "schema = 1\n[plugin]\nname = \"platform/other\"\nplugin_type = \"platform\"\n\
         api_type = \"native\"\nlang = \"c\"\nphase = \"early\"\nversion = \"1.0.0.0\"\n\
         [build]\nsources = [\"src/*.c\"]\n[build.target]\narch = \"aarch64\"\n",
    );
    let (d4, _) = build::selftest_plan(&root4, &mk_req_ctx("build", &root4, json!({}), json!({"path": ""})));
    h.check(
        diags_code(&d4, build::CODE_TARGET_AMBIGUOUS),
        "build: two [build.target] ⇒ BRV-BLD-0004 (source of truth unclear)",
    );

    let root5 = tmpdir("build-missing-src");
    write_build_tree(&root5, "", TARGET_OK);
    write(
        &root5,
        "app/m/brick.toml",
        "schema = 1\n[plugin]\nname = \"app/m\"\nplugin_type = \"app\"\napi_type = \"native\"\n\
         lang = \"c\"\nphase = \"app\"\nversion = \"1.0.0.0\"\n\
         [build]\nsources = [\"src/nope.c\"]\n",
    );
    let (d5, _) = build::selftest_plan(&root5, &mk_req_ctx("build", &root5, json!({}), json!({"path": ""})));
    h.check(
        diags_code(&d5, build::CODE_MISSING_SOURCE),
        "build: literal source path does not exist ⇒ BRV-BLD-0007",
    );

    // gen_sources: `**` must be closure-filtered; a stale/non-closure descriptor would link twice.
    let root8 = tmpdir("build-gen-src");
    write_build_tree(
        &root8,
        "gen_sources = [\"build/gen/**/plugin_desc.c\", \"generated/hand.c\"]\n",
        TARGET_OK,
    );
    write(&root8, "build/gen/app/m/plugin_desc.c", "int app_desc;\n");
    write(&root8, "build/gen/service/out/plugin_desc.c", "int out_desc;\n");
    write(
        &root8,
        "build/gen/third_party/uboot-mmc/plugin_desc.c",
        "int ghost_desc;\n",
    );
    write(&root8, "generated/hand.c", "int hand;\n");
    write(
        &root8,
        "service/out/brick.toml",
        "schema = 1\n[plugin]\nname = \"service/out\"\nplugin_type = \"ability\"\n\
         api_type = \"native\"\nsubkind = \"service\"\nlang = \"c\"\nphase = \"late\"\n\
         version = \"1.0.0.0\"\n[build]\nsources = [\"src/*.c\"]\n",
    );
    let (d9, data9) = build::selftest_plan(
        &root8,
        &mk_req_ctx("build", &root8, json!({}), json!({"path": ""})),
    );
    h.exit_is(&d9, 0, "gen_sources: skipping non-closure/stale artifacts is info only (no exit-code change)");
    h.check(
        diags_code(&d9, build::CODE_GEN_NOT_SELECTED),
        "gen_sources: a non-closure plugin's artifact is skipped and **recorded** (BRV-BLD-0013)",
    );
    h.check(
        diags_code(&d9, build::CODE_GEN_ORPHAN),
        "gen_sources: a stale artifact whose plugin is gone is skipped and **recorded** (BRV-BLD-0014)",
    );
    let gen_srcs: Vec<String> = data9["units"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter(|u| u["name"] == json!("build/gen"))
                .flat_map(|u| {
                    u["sources_list"]
                        .as_array()
                        .map(|s| {
                            s.iter()
                                .filter_map(|x| x.as_str().map(str::to_string))
                                .collect::<Vec<_>>()
                        })
                        .unwrap_or_default()
                })
                .collect()
        })
        .unwrap_or_default();
    h.check(
        gen_srcs.iter().any(|s| s == "build/gen/app/m/plugin_desc.c"),
        "gen_sources: `**` matched an in-closure plugin's artifact",
    );
    h.check(
        !gen_srcs.iter().any(|s| s.contains("service/out")),
        "gen_sources: a non-closure plugin's artifact does **not** enter the image",
    );
    h.check(
        !gen_srcs.iter().any(|s| s.contains("third_party/uboot-mmc")),
        "gen_sources: a stale artifact (ghost plugin) does not enter the image -- else the same symbol is registered twice",
    );
    h.check(
        gen_srcs.iter().any(|s| s == "generated/hand.c"),
        "gen_sources: a path outside `build/gen/` is kept even when ownership is unknown",
    );

    let root6 = tmpdir("build-gates");
    write_build_tree(&root6, "", TARGET_OK);
    write(
        &root6,
        "tests/gates.toml",
        "schema = 1\n[run]\ndefault_timeout_s = 3\n\
         [host]\ncc = \"cc\"\ncflags = [\"-O2\"]\n\
         [[hosttest]]\nname = \"t1\"\nsources = [\"a.c\"]\n\
         [[gate]]\nname = \"g1\"\ntimeout_s = 3\nlog = \"build/logs/g1.log\"\n\
         require = [\"banner\", \"pass=[0-9]* fail=0 \"]\nforbid = [\"\\\\[PANIC\\\\]\"]\n\
         require_tags = [\"TC-X-001\"]\n\
         [[script]]\nname = \"s1\"\nargv = [\"bash\", \"tools/x.sh\"]\n\
         [build]\npost = [\"s1\"]\n",
    );
    let list = build::run(&mk_req("test", &root6, json!({"list": true})));
    h.j(&list.data["gates"], json!(["g1"]), "gates: gate name list");
    h.j(&list.data["hosttests"], json!(["t1"]), "gates: host test name list");
    h.j(&list.data["scripts"], json!(["s1"]), "gates: script gate name list");
    h.j(&list.data["build_post"], json!(["s1"]), "gates: [build].post made it into the data");

    let good = "banner\nPASS TC-X-001 ok\npass=3 fail=0 \n";
    let r_good = build::run(&mk_req_ctx(
        "judge",
        &root6,
        json!({"name": "g1"}),
        json!({"log": good}),
    ));
    h.exit_is(&Diags::new(), 0, "judge: baseline exit code 0");
    h.check(r_good.exit_code == 0, "judge: require/forbid/tag all satisfied ⇒ exit code 0");
    h.check(
        r_good.data["failed"] == json!(0),
        "judge: failed count is 0",
    );

    let bad = "banner only\n[PANIC] boom\n";
    let r_bad = build::run(&mk_req_ctx(
        "judge",
        &root6,
        json!({"name": "g1"}),
        json!({"log": bad}),
    ));
    h.check(r_bad.exit_code == 1, "judge: failures ⇒ exit code 1 (validation error)");
    h.j(&r_bad.data["failed"], json!(3), "judge: each of the three criteria fails once");
    let codes: Vec<String> = r_bad
        .diagnostics
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|d| d.get("code").and_then(Value::as_str).map(str::to_string))
                .collect()
        })
        .unwrap_or_default();
    h.check(
        codes.iter().all(|c| c == build::CODE_GATE_FAILED),
        "judge: failure diagnostics all carry BRV-BLD-0009",
    );

    let r_unknown = build::run(&mk_req_ctx(
        "judge",
        &root6,
        json!({"name": "nope"}),
        json!({"log": ""}),
    ));
    h.check(
        has_code(&r_unknown, build::CODE_GATE_UNKNOWN),
        "judge: unknown gate ⇒ BRV-BLD-0008",
    );
    h.check(r_unknown.exit_code == 2, "judge: unknown gate ⇒ usage error (exit code 2)");

    let root7 = tmpdir("build-badregex");
    write_build_tree(&root7, "", TARGET_OK);
    write(
        &root7,
        "tests/gates.toml",
        "schema = 1\n[[gate]]\nname = \"g\"\nrequire = [\"(a|b)\"]\n",
    );
    let bad_re = build::run(&mk_req("test", &root7, json!({"name": "g"})));
    h.check(
        has_code(&bad_re, build::CODE_GATE_SHAPE),
        "gates: regex outside the subset ⇒ BRV-BLD-0012",
    );
    h.check(bad_re.exit_code == 2, "gates: regex outside the subset is a shape error (exit code 2)");

    let (d8, data8) = build::selftest_plan(&root, &mk_req_ctx("build", &root, json!({}), json!({"path": ""})));
    h.exit_is(&d8, 0, "build: backend-render tree has no diagnostics");
    let mk = build::selftest_render_backend(&root, &req, "make");
    let nj = build::selftest_render_backend(&root, &req, "ninja");
    h.check(
        mk.contains("brickie:generated") && mk.contains(".DEFAULT_GOAL := all") && mk.contains("-include $(DEPS)"),
        "backend/make: generated marker + default goal + header-dep include",
    );
    h.check(
        nj.contains("rule cc") && nj.contains("depfile = $out.d") && nj.contains("deps = gcc"),
        "backend/ninja: rule/depfile/deps all present",
    );
    h.check(
        nj.contains("cc = "),
        "backend/ninja: the compiler variable must be defined first ($cc expands empty otherwise -- hit before)",
    );
    h.check(
        nj.contains("builddir = build/gen/ninja"),
        "backend/ninja: pin the ninja state file under build/ (it lands in the repo root otherwise -- hit before)",
    );
    h.check(
        nj.contains("rule asm") && nj.contains("rule cc") && nj.contains(": asm "),
        "backend/ninja: one rule each for C and .S (using the C template for assembly is wrong)",
    );
    h.check(
        mk.contains("build/obj/core/c_core/src/core.o") && nj.contains("build/obj/app/m/src/main.o"),
        "backend: both backends list the objects",
    );
    h.check(
        data8["steps_total"] == json!(6),
        "build: backend rendering does not change the plan (still 6 steps)",
    );
    let _ = fs::remove_dir_all(&root7);
}

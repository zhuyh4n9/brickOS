//! `brickie-core --selftest`: 不经 Python 的领域自检(V-18a)。
//!
//! 覆盖: 版本矩阵逐行 / range 展开与交集 / 名字契约与 subkind 推导 / 相位推导 /
//! 特权正交表 / 依赖方向禁则 / 导出面不变量 / TOML 模型加载正反例 /
//! `plan-new` 四类 / 机器文件确定性。
//!
//! 输出: 全绿 ⇒ `ok <n> cases`; 有失败 ⇒ 逐条 FAIL 明细, 退出码 1。

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
                "{what}: 期望诊断 `{code}`, 实得 {:?}",
                d.items()
                    .iter()
                    .map(|x| x.code.clone().unwrap_or_else(|| "<null>".into()))
                    .collect::<Vec<_>>()
            ));
        }
    }

    /// 比较一个 JSON `Value`(serde_json 的 Index 返回 `&Value`, 故按引用比)。
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

// ------------------------------------------------------------------ 工具

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
    fs::create_dir_all(&p).expect("建临时目录");
    p
}

fn write(root: &Path, rel: &str, content: &str) {
    let p = root.join(rel);
    if let Some(parent) = p.parent() {
        fs::create_dir_all(parent).expect("建父目录");
    }
    fs::write(p, content).expect("写文件");
}

// ------------------------------------------------------------------ 各领域

fn version_matrix(h: &mut Harness) {
    let v = Version::new(0, 1, 2, 3);
    let a = version::advance(v, Event::CompatGenBump);
    h.eq(
        a,
        Version::new(1, 1, 0, 0),
        "V-4(a) 解冻+改已冻结接口 ⇒ 仅 COMPAT_GEN+1(minor/revise 清零)",
    );
    h.eq(a.major, v.major, "V-4(a) COMPAT_GEN 与 MAJOR 互不牵连");
    let b = version::advance(v, Event::Added);
    h.eq(b, Version::new(0, 1, 3, 0), "V-4(b) 新增条目 ⇒ COMPAT_GEN 不动, MINOR+1");
    h.eq(version::advance(v, Event::Extended), Version::new(0, 1, 3, 0), "V-4 追加枚举成员 ⇒ MINOR+1");
    h.eq(
        version::advance(v, Event::StatusTransfer),
        Version::new(0, 1, 3, 0),
        "V-4 治理状态转移 ⇒ MINOR+1",
    );
    h.eq(
        version::advance(v, Event::ChangeAllExperimental),
        Version::new(0, 1, 3, 0),
        "V-4 改全部 experimental 条目 ⇒ MINOR+1",
    );
    h.eq(
        version::advance(v, Event::RemoveExperimental),
        Version::new(0, 1, 3, 0),
        "V-4 删仅 experimental 条目 ⇒ MINOR+1",
    );
    let c = version::advance(v, Event::MajorProduct);
    h.eq(c, Version::new(0, 2, 0, 0), "V-4(c) 重大产品版本 ⇒ 仅 MAJOR+1");
    h.eq(c.compat_gen, v.compat_gen, "V-4(c) MAJOR 与 COMPAT_GEN 互不牵连");
    h.eq(version::advance(v, Event::MinorFeature), Version::new(0, 1, 3, 0), "V-4(d) 小功能 ⇒ MINOR+1");
    h.eq(version::advance(v, Event::ReviseFix), Version::new(0, 1, 2, 4), "V-4(e) 修 bug ⇒ REVISE+1");
    h.eq(version::advance(v, Event::EmptyUnfreeze), v, "V-4(f) 空解冻 ⇒ 四段全不动");
    h.eq(version::advance(v, Event::NoChange), v, "面完全未变 ⇒ 空操作");

    h.eq(
        version::parse_version("0.1.0.0").unwrap(),
        Version::new(0, 1, 0, 0),
        "四段版本解析",
    );
    h.eq(Version::new(1, 2, 3, 4).to_string(), "1.2.3.4".to_string(), "版本序列化(无 v)");
    h.eq(Version::new(1, 2, 3, 4).display_v(), "v1.2.3.4".to_string(), "版本显示(带 v)");
    h.eq(
        version::parse_version("1.2.3").unwrap_err().code,
        "BRV-VER-0005",
        "3 段版本串 ⇒ BRV-VER-0005",
    );
    h.check(version::parse_version("1.2.3.4.5").is_err(), "5 段版本串非法");
    h.check(version::parse_version("a.b.c.d").is_err(), "非数字版本串非法");

    h.check(
        version::check_no_regress(Version::new(1, 0, 0, 0), Version::new(1, 0, 0, 1)).is_ok(),
        "同代内递增不回退",
    );
    h.check(
        version::check_no_regress(Version::new(1, 0, 0, 0), Version::new(2, 0, 0, 0)).is_ok(),
        "跨代(COMPAT_GEN+1)不算回退",
    );
    h.eq(
        version::check_no_regress(Version::new(1, 5, 0, 0), Version::new(1, 4, 9, 9))
            .unwrap_err()
            .code,
        "BRV-VER-0007",
        "同代内回退 ⇒ BRV-VER-0007",
    );
    h.eq(
        version::check_no_regress(Version::new(1, 5, 0, 0), Version::new(0, 9, 9, 9))
            .unwrap_err()
            .code,
        "BRV-VER-0007",
        "compat_gen 下降 ⇒ BRV-VER-0007",
    );
}

fn range_cases(h: &mut Harness) {
    let ge = Range::parse(">=1.0.0").unwrap();
    h.check(
        ge.matches(Triple(1, 0, 0)) && ge.matches(Triple(2, 3, 4)) && !ge.matches(Triple(0, 9, 9)),
        ">=1.0.0 字典序比较",
    );
    let tilde = Range::parse("~1.2.3").unwrap();
    h.eq(tilde.canonical(), ">=1.2.3,<1.3.0".to_string(), "~ 展开为显式区间");
    h.check(tilde.matches(Triple(1, 2, 9)) && !tilde.matches(Triple(1, 3, 0)), "~ 不跨 MINOR");
    let caret = Range::parse("^1.2.3").unwrap();
    h.eq(caret.canonical(), ">=1.2.3,<2.0.0".to_string(), "^ 展开为显式区间");
    h.check(caret.matches(Triple(1, 9, 9)) && !caret.matches(Triple(2, 0, 0)), "^ 不跨 MAJOR");
    let pad = Range::parse(">=1.0").unwrap();
    h.eq(pad.canonical(), ">=1.0.0".to_string(), "range 缺段右补 0");
    h.eq(
        Range::parse(">=1.2.3.4").unwrap_err().code,
        "BRV-VER-0006",
        "4 段 range ⇒ BRV-VER-0006",
    );
    h.check(Range::parse("*").unwrap().matches(Triple(9, 9, 9)), "* = 任意");
    h.check(Range::parse("=1.2.3").unwrap().matches(Triple(1, 2, 3)), "= 精确");

    let a = Range::parse(">=1.0.0,<2.0.0").unwrap();
    let b = Range::parse(">=1.5.0").unwrap();
    let i = a.intersect(&b);
    h.check(
        i.is_satisfiable() && i.matches(Triple(1, 5, 0)) && !i.matches(Triple(2, 0, 0)),
        "区间交集(AND)",
    );
    h.check(!Range::parse(">=2.0.0,<1.0.0").unwrap().is_satisfiable(), "空交集不可满足");
    h.check(
        !Range::parse(">=1.0.0,<1.0.0").unwrap().is_satisfiable(),
        "开区间同点不可满足",
    );
    h.check(
        Range::parse(">=1.0.0,<=1.0.0").unwrap().is_satisfiable(),
        "闭区间同点可满足",
    );
    h.check(
        version::compat_gen_matches(3, 3) && !version::compat_gen_matches(3, 4),
        "compat_gen 精确匹配(跨代比较无意义)",
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
        "io/virtio-hsm",
        "fs/tmpfs",
    ] {
        h.check(rules::valid_plugin_name(ok), format!("名字契约接受 `{ok}`"));
    }
    for bad in ["Service/Crypto", "service//crypto", "service/", "1crypto", "", "service/-x", "-x"] {
        h.check(!rules::valid_plugin_name(bad), format!("名字契约拒绝 `{bad}`"));
    }
    h.eq(rules::name_namespace("service/crypto"), "service", "namespace 提取");
    h.eq(rules::name_short("service/crypto"), "crypto", "short 提取");
    h.eq(rules::name_namespace("sched-coop"), "", "裸名 namespace 为空");
    h.check(rules::is_recommended_form("service/crypto"), "推荐形态");
    h.check(!rules::is_recommended_form("sched-coop"), "裸名不是推荐形态");
    h.check(rules::is_recommended_form("ability/x") == false, "`ability/` 不是推荐 namespace");

    h.eq(rules::derive_subkind("service/crypto"), Some("service"), "subkind: service/⇒service");
    h.eq(rules::derive_subkind("sched/coop"), Some("scheduler"), "subkind: sched/⇒scheduler");
    h.eq(rules::derive_subkind("framework/dev-core"), Some("framework"), "subkind: framework/");
    h.eq(rules::derive_subkind("io/virtio-hsm"), Some("io"), "subkind: io/");
    h.eq(rules::derive_subkind("fs/tmpfs"), Some("fs"), "subkind: fs/");
    h.eq(rules::derive_subkind("sched-coop"), None, "subkind: 裸名不可推导");
    h.eq(rules::derive_subkind("iface/min"), None, "subkind: iface/ 无 subkind");

    h.eq(rules::namespace_plugin_type("iface"), Some("interface"), "namespace: iface⇒interface");
    h.eq(rules::namespace_plugin_type("sched"), Some("ability"), "namespace: sched⇒ability");
    h.eq(rules::namespace_plugin_type("platform"), Some("platform"), "namespace: platform");
    h.eq(rules::namespace_plugin_type("nope"), None, "namespace: 未知");
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
    h.eq(rules::derive_phase("ability", Some("service"), false), "early", "无 init 钩子 ⇒ early");

    // 父 agent 澄清口径的允许集
    h.check(rules::phase_reason("interface", None, "late").is_none(), "interface+late ok");
    h.check(rules::phase_reason("interface", None, "core").is_some(), "interface+core ⇒ 错");
    h.check(rules::phase_reason("ability", Some("service"), "late").is_none(), "service+late ok");
    h.check(rules::phase_reason("ability", Some("service"), "core").is_some(), "service+core ⇒ 错");
    h.check(rules::phase_reason("platform", None, "core").is_none(), "platform+core ok");
    h.check(rules::phase_reason("platform", None, "late").is_some(), "platform+late ⇒ 错");
    h.check(rules::phase_reason("ability", Some("scheduler"), "core").is_none(), "scheduler+core ok");
    h.check(rules::phase_reason("app", None, "app").is_none(), "app+app ok(M0 app/hello 判例)");
    h.check(rules::phase_reason("app", None, "core").is_none(), "app+core ok");
    h.check(rules::phase_reason("app", None, "late").is_some(), "app+late ⇒ 错");
    h.check(rules::phase_reason("app", None, "early").is_none(), "early 恒合法");
}

fn privilege_cases(h: &mut Harness) {
    h.eq(rules::max_privilege_level("app", None), "P0", "app ≤ P0");
    h.eq(rules::max_privilege_level("interface", None), "P0", "interface ≤ P0");
    h.eq(rules::max_privilege_level("ability", Some("service")), "P2", "ability ≤ P2");
    h.eq(rules::max_privilege_level("ability", Some("scheduler")), "P3", "scheduler ≤ P3");
    h.eq(rules::max_privilege_level("platform", None), "P4", "platform ≤ P4");

    h.check(rules::memory_combo_legal("alloc", "pool", "contig"), "池生命周期: alloc×pool×contig");
    h.check(rules::memory_combo_legal("free", "pool", "page"), "池生命周期: free×pool×page");
    h.check(!rules::memory_combo_legal("alloc", "pool", "heap"), "pool 对 heap 非法");
    h.check(rules::memory_combo_legal("alloc", "page", "heap"), "alloc×page×heap");
    h.check(rules::memory_combo_legal("map", "page", "mmio"), "map/protect 属 P4(machine)");
    h.check(!rules::memory_combo_legal("map", "page", "dma"), "map×dma 非法");
    h.check(rules::memory_combo_legal("protect", "region", "reserved"), "protect×region×reserved");
    h.check(rules::memory_combo_legal("flush", "byte", "dma"), "flush×byte×dma");
    h.check(rules::memory_combo_legal("unmap", "region", "heap"), "unmap×region×heap");
    h.check(!rules::memory_combo_legal("nope", "page", "heap"), "未知 op 非法");

    // 正例: ability P2 + 池生命周期
    let ok_mem = vec![rules::MemoryEntry {
        granularity: "pool".into(),
        ops: vec!["alloc".into(), "free".into()],
        regions: vec!["contig".into(), "page".into()],
    }];
    let ok_input = rules::PrivInput {
        plugin_type: "ability",
        subkind: Some("service"),
        target: "service/crypto",
        file: "service/crypto/plugin.toml",
        level: Some("P2"),
        memory: &ok_mem,
        has_resources: false,
    };
    h.check(
        rules::check_privileged(&ok_input).is_empty(),
        "正例: ability P2 + 池生命周期无诊断",
    );

    // 反例 ①: ability P2 声明 map/page/mmio ⇒ PRIV-0001(正交表该行最低 = P4)
    let bad_mem = vec![rules::MemoryEntry {
        granularity: "page".into(),
        ops: vec!["map".into()],
        regions: vec!["mmio".into()],
    }];
    let bad_input = rules::PrivInput {
        plugin_type: "ability",
        subkind: Some("service"),
        target: "service/crypto",
        file: "service/crypto/plugin.toml",
        level: Some("P2"),
        memory: &bad_mem,
        has_resources: false,
    };
    let diags = rules::check_privileged(&bad_input);
    h.check(
        diags.iter().any(|d| d.code.as_deref() == Some("BRV-PRIV-0001")),
        "反例①: ability 声明 map ⇒ BRV-PRIV-0001",
    );

    // 反例 ②: app 声明 P1 ⇒ PRIV-0001
    let app_input = rules::PrivInput {
        plugin_type: "app",
        subkind: None,
        target: "app/hsm",
        file: "app/hsm/plugin.toml",
        level: Some("P1"),
        memory: &[],
        has_resources: true,
    };
    let diags = rules::check_privileged(&app_input);
    h.check(
        diags.iter().any(|d| d.code.as_deref() == Some("BRV-PRIV-0001")),
        "反例②: app 声明 P1/resource ⇒ BRV-PRIV-0001",
    );

    // 反例 ③: alloc × pool × heap ⇒ PRIV-0002(组合非法)
    let illegal = vec![rules::MemoryEntry {
        granularity: "pool".into(),
        ops: vec!["alloc".into()],
        regions: vec!["heap".into()],
    }];
    let illegal_input = rules::PrivInput {
        plugin_type: "ability",
        subkind: Some("service"),
        target: "service/crypto",
        file: "service/crypto/plugin.toml",
        level: Some("P2"),
        memory: &illegal,
        has_resources: false,
    };
    let diags = rules::check_privileged(&illegal_input);
    h.check(
        diags.iter().any(|d| d.code.as_deref() == Some("BRV-PRIV-0002")),
        "反例③: alloc×pool×heap ⇒ BRV-PRIV-0002",
    );
}

fn dep_direction_cases(h: &mut Harness) {
    h.check(!rules::dep_direction_ok("app", "ability", false), "禁则(1): app 不得依赖 ability");
    h.check(!rules::dep_direction_ok("app", "platform", false), "禁则(1): app 不得依赖 platform");
    h.check(rules::dep_direction_ok("app", "interface", false), "app 仅经 interface");
    h.check(!rules::dep_direction_ok("platform", "ability", false), "platform 不依赖任何插件");
    h.check(!rules::dep_direction_ok("interface", "third_party", true), "interface 不消费三方件");
    h.check(rules::dep_direction_ok("ability", "third_party", true), "ability 是三方件唯一合法消费者");
    h.check(!rules::dep_direction_ok("platform", "interface", false), "禁则(3): interface 严格叶子");
    h.check(rules::dep_direction_ok("interface", "ability", false), "interface 可再导出 ability");
    h.check(rules::dep_direction_ok("ability", "platform", false), "ability 可依赖 platform");

    h.check(
        rules::api_type_edge_reason("ability", "native", "runtime_adapter", "runtime").is_some(),
        "禁则(2): native ↛ runtime_adapter",
    );
    h.check(
        rules::api_type_edge_reason("ability", "runtime_adapter", "third_party", "type").is_none(),
        "runtime_adapter 对 third_party 的 type 边合法(§3.1 例外)",
    );
    h.check(
        rules::api_type_edge_reason("ability", "runtime_adapter", "third_party", "runtime").is_some(),
        "runtime_adapter 对 third_party 的 runtime 边非法",
    );
    h.check(
        rules::api_type_edge_reason("ability", "native", "third_party", "type").is_some(),
        "native 不得依赖 third_party",
    );
    h.check(
        rules::api_type_edge_reason("app", "native", "runtime_adapter", "runtime").is_none(),
        "纯消费者(app)不参与 api_type 禁则",
    );

    // allow_edges 只豁免 plugin_type 方向, 不豁免 api_type 硬禁则
    let edges = vec![("platform/soc".to_string(), "ability/helper".to_string())];
    h.check(
        rules::dep_edge_violation(
            "platform/soc", "platform", "native", "ability/helper", "ability", "native", "init", &edges,
        )
        .is_none(),
        "allow_edges 白名单豁免方向表",
    );
    h.check(
        rules::dep_edge_violation(
            "platform/soc", "platform", "native", "ability/helper", "ability", "native", "init", &[],
        )
        .is_some(),
        "无白名单时 platform→ability 违规",
    );
    h.check(
        rules::dep_edge_violation(
            "ability/a", "ability", "native", "svc/posix", "ability", "runtime_adapter", "runtime", &edges,
        )
        .is_some(),
        "allow_edges 不豁免 api_type 硬禁则",
    );
}

fn export_invariant_cases(h: &mut Harness) {
    // 不变量 1: api_iface ≠ api_type
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
        "不变量 1 ⇒ BRV-TAX-0016",
    );
    // 不变量 2: third_party 抛 native 面
    h.check(
        rules::check_export_invariants("third_party", &v)
            .iter()
            .any(|(_, c, _)| *c == "BRV-TAX-0017"),
        "不变量 2 ⇒ BRV-TAX-0017",
    );
    // 不变量 3: skin 缺 reexport_of
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
        "不变量 3 ⇒ BRV-TAX-0018",
    );
    // 不变量 3 正例: 多提供者 + 分类相等形状
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
        "不变量 3 正例(多提供者)",
    );
    // 不变量 4: 非 skin 声明 reexport_of
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
        "不变量 4 ⇒ BRV-TAX-0019",
    );
    h.check(rules::valid_unit_ref("service/crypto#crypto"), "unit ref 形状");
    h.check(!rules::valid_unit_ref("service/crypto"), "unit ref 缺 #");
}

fn model_cases(h: &mut Harness) {
    // ---- 正例 ----
    let root = tmpdir("model-ok");
    write(
        &root,
        "service/crypto/plugin.toml",
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
    h.eq(tree.diags.len(), 0, "模型正例无诊断");
    h.eq(tree.plugins.len(), 1, "模型正例载入 1 个插件");
    if let Some(p) = tree.plugins.first() {
        h.eq(p.name.clone(), "service/crypto".to_string(), "插件名");
        h.eq(p.phase.clone(), "late".to_string(), "phase");
        h.eq(p.version, Version::new(0, 1, 0, 0), "四段版本");
        h.eq(p.res_totals(), (48, 0), "[[res]] 求和");
        h.eq(p.unit_ids(), vec!["service/crypto#crypto".to_string()], "接口单元 id");
        let privd = p.privileged.as_ref().expect("privileged 应存在");
        h.eq(privd.resources.irq.clone(), vec![32], "[[privileged.resources]] 是数组表");
        h.eq(privd.resources.dma_channels.clone(), vec![3], "dma_channels 累加");
        h.eq(
            p.exports[0].entries[0].value.clone(),
            Some("16,32".to_string()),
            "value 字符串数组 ⇒ 规范化",
        );
    } else {
        h.check(false, "模型正例缺少插件");
    }
    h.check(!tree.has_product, "无 product.toml ⇒ has_product = false");
    h.check(!tree.product_level_constraints_apply(), "R-1: 无 product ⇒ 跳过产品级约束");

    // ---- R-6: platform 的 [[res]] 是容量 ----
    let root_cap = tmpdir("model-cap");
    write(
        &root_cap,
        "platform/soc/plugin.toml",
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
    h.eq(tree_cap.diags.len(), 0, "platform 正例无诊断");
    h.eq(tree_cap.platform_capacity(), (512, 16), "R-6: platform [[res]] 即平台容量");

    // ---- 反例: 缺 name ⇒ MF-0001 + exit 2 ----
    let root2 = tmpdir("model-noname");
    write(
        &root2,
        "a/plugin.toml",
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
    h.has_code(&tree2.diags, "BRV-MF-0001", "缺必填 name ⇒ BRV-MF-0001");
    h.exit_is(&tree2.diags, 2, "形状错 ⇒ 退出码 2");

    // ---- 反例: 3 段版本串 ⇒ VER-0005 ----
    let root3 = tmpdir("model-ver");
    write(
        &root3,
        "b/plugin.toml",
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
    h.has_code(&tree3.diags, "BRV-VER-0005", "3 段版本串 ⇒ BRV-VER-0005");
    h.exit_is(&tree3.diags, 2, "版本串形状错 ⇒ 退出码 2");

    // ---- 反例: 4 段 range ⇒ VER-0006 ----
    let root4 = tmpdir("model-range");
    write(
        &root4,
        "c/plugin.toml",
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
    h.has_code(&tree4.diags, "BRV-VER-0006", "4 段 range ⇒ BRV-VER-0006");
    h.exit_is(&tree4.diags, 2, "range 形状错 ⇒ 退出码 2");

    // ---- 反例: phase 与 plugin_type 不相容 ⇒ MF-0001 ----
    let root5 = tmpdir("model-phase");
    write(
        &root5,
        "d/plugin.toml",
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
    h.has_code(&tree5.diags, "BRV-MF-0001", "service 声明 core ⇒ MF-0001");

    // ---- 反例: subkind 推导与显式冲突 ⇒ TAX-0015(exit 1) ----
    let root6 = tmpdir("model-subkind");
    write(
        &root6,
        "service/e/plugin.toml",
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
    h.has_code(&tree6.diags, "BRV-TAX-0015", "subkind 冲突 ⇒ BRV-TAX-0015");
    h.exit_is(&tree6.diags, 1, "TAX-0015 是校验红 ⇒ 退出码 1");

    // ---- 反例: 名字重复 ⇒ MF-0001 ----
    let root7 = tmpdir("model-dup");
    write(
        &root7,
        "service/x/plugin.toml",
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
        "service/x2/plugin.toml",
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
    h.has_code(&tree7.diags, "BRV-MF-0001", "插件名重复 ⇒ BRV-MF-0001");

    // ---- 反例: 非法 TOML ⇒ 环境错(无码) + 退出码 2 ----
    let root8 = tmpdir("model-toml");
    write(&root8, "e/plugin.toml", "schema = = 1\n");
    let tree8 = model::load_tree(&root8);
    h.check(
        tree8.diags.items().iter().any(|d| d.code.is_none()),
        "非法 TOML ⇒ 无码环境错",
    );
    h.exit_is(&tree8.diags, 2, "非法 TOML ⇒ 退出码 2");

    // ---- 扫描跳过 build/ 与 .git/ ----
    let root9 = tmpdir("model-skip");
    write(
        &root9,
        "build/ignored/plugin.toml",
        "schema = 1\n",
    );
    write(&root9, ".hidden/ignored/plugin.toml", "schema = 1\n");
    let tree9 = model::load_tree(&root9);
    h.eq(tree9.plugins.len(), 0, "扫描跳过 build/ 与 . 开头目录");

    // ---- schema 对齐: `[compat]` 存在但缺 `core` ⇒ MF-0001 ----
    let root10 = tmpdir("model-compat");
    write(
        &root10,
        "service/g/plugin.toml",
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
    h.has_code(&tree10.diags, "BRV-MF-0001", "`[compat]` 缺 core ⇒ MF-0001");

    // ---- schema 对齐: `[[export]]` 缺 `freeze_state` ⇒ MF-0001 ----
    let root11 = tmpdir("model-export");
    write(
        &root11,
        "service/h/plugin.toml",
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
    h.has_code(&tree11.diags, "BRV-MF-0001", "export 缺 freeze_state ⇒ MF-0001");

    // ---- schema 对齐: `[privileged]` 缺 level 但声明了 memory ⇒ PRIV-0001 ----
    let root12 = tmpdir("model-priv");
    write(
        &root12,
        "service/i/plugin.toml",
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
    h.has_code(&tree12.diags, "BRV-PRIV-0001", "缺 privileged.level ⇒ PRIV-0001");
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
    h.eq(snaps.len(), 1, "读取 1 份快照");
    if let Some(s) = snaps.first() {
        h.eq(s.id.clone(), "service/crypto#crypto".to_string(), "快照 id");
        h.eq(s.unit.clone(), "crypto".to_string(), "快照 unit");
        h.eq(s.entries.len(), 1, "快照条目数");
    }

    // 未发布 ⇒ 空表, 不是错误
    let empty = tmpdir("snap-empty");
    let mut d2 = Diags::new();
    h.eq(model::load_snapshots(&empty, &mut d2).len(), 0, "无 api/iface ⇒ 空(未发布)");
    h.check(d2.is_empty(), "无 api/iface 不报诊断");
    let mut d3 = Diags::new();
    h.check(model::load_lock(&empty, &mut d3).is_none(), "无 brickie.lock ⇒ None");

    // lock 往返(形状 = schema/lock.schema.json)
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
    h.eq(text1.clone(), text2, "lock 序列化幂等");
    h.check(text1.contains("[[lock.plugins]]"), "lock 形状: [[lock.plugins]]");
    h.check(text1.contains("[[lock.units]]"), "lock 形状: [[lock.units]]");
    h.check(text1.parse::<toml::Value>().is_ok(), "lock 文本可被 toml 回解析");
    write(&root, "brickie.lock", &text1);
    let mut d4 = Diags::new();
    let back = model::load_lock(&root, &mut d4);
    h.check(back.is_some(), "lock 可回读");
    if let Some(l) = back {
        h.eq(l.plugins.len(), 1, "lock 插件数");
        h.eq(l.units.len(), 1, "lock 单元数");
        h.eq(l.profile.clone(), "dev".to_string(), "lock profile");
        h.eq(crate::emit::render_lock(&l), text1, "lock 往返逐字节一致");
    }

    // 反向依赖索引
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
    h.eq(js1.clone(), js2, "dependents 索引序列化幂等");
    h.check(js1.contains("\"from\": \"app/hsm\""), "dependents 含边");
}

fn plan_cases(h: &mut Harness) {
    // ---- 四类 × native × c ----
    let cases = [
        ("app", "app/hsm", "app", "native/app/c/plugin.toml.tmpl"),
        ("interface", "iface/posix", "late", "native/interface/c/plugin.toml.tmpl"),
        ("ability", "service/crypto", "late", "native/ability/c/plugin.toml.tmpl"),
        ("platform", "platform/qemu-aarch64", "core", "native/platform/c/plugin.toml.tmpl"),
    ];
    for (pt, name, want_phase, want_tpl) in cases {
        let a = args(json!({
            "plugin_type": pt, "name": name, "api_type": "native", "lang": "c",
        }));
        let (d, arts) = plan::plan_new(&a, &Map::new());
        h.exit_is(&d, 0, &format!("plan-new {pt} 退出码 0"));
        h.eq(arts.len(), 6, &format!("plan-new {pt} 产 6 件计划"));
        let manifest = arts.iter().find(|x| x.path.ends_with("/plugin.toml"));
        match manifest {
            Some(m) => {
                h.eq(m.kind.clone(), "human".to_string(), "plugin.toml 是 human");
                h.eq(m.template.clone(), want_tpl.to_string(), "骨架模板路径");
                h.eq(
                    m.vars.get("phase").cloned().unwrap_or_default(),
                    want_phase.to_string(),
                    &format!("{pt} 相位推导"),
                );
                h.eq(
                    m.vars.get("name").cloned().unwrap_or_default(),
                    name.to_string(),
                    "小写占位符 name",
                );
                h.eq(
                    m.vars.get("PLUGIN_NAME").cloned().unwrap_or_default(),
                    name.to_string(),
                    "大写占位符 PLUGIN_NAME",
                );
            }
            None => h.check(false, "plan-new 缺 plugin.toml 计划"),
        }
        h.check(
            arts.iter().any(|x| {
                x.path == format!("build/gen/{name}/plugin_desc.c")
                    && x.kind == "generated"
                    && x.template == "descriptor/native/c/plugin_desc.c.tmpl"
            }),
            &format!("plan-new {pt} 描述符计划"),
        );
    }

    // ability 裸名 + 显式 subkind
    let a = args(json!({
        "plugin_type": "ability", "name": "sched-coop", "api_type": "native",
        "lang": "c", "subkind": "scheduler",
    }));
    let (d, arts) = plan::plan_new(&a, &Map::new());
    h.exit_is(&d, 0, "裸名 ability + --subkind 通过");
    h.check(d.items().iter().any(|x| x.code.as_deref() == Some("BRV-TAX-0013")), "裸名 ⇒ TAX-0013 warning");
    h.eq(arts.len(), 6, "裸名 ability 计划件数");

    // ability 裸名缺 subkind ⇒ MF-0001 exit 2
    let a = args(json!({"plugin_type": "ability", "name": "sched-coop", "api_type": "native", "lang": "c"}));
    let (d, arts) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-MF-0001", "ability 裸名缺 subkind ⇒ MF-0001");
    h.exit_is(&d, 2, "缺 subkind ⇒ 退出码 2");
    h.eq(arts.len(), 0, "有形状错 ⇒ 不产出计划");

    // subkind 冲突 ⇒ TAX-0015 exit 1
    let a = args(json!({
        "plugin_type": "ability", "name": "service/crypto", "api_type": "native",
        "lang": "c", "subkind": "fs",
    }));
    let (d, _) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-TAX-0015", "subkind 冲突 ⇒ TAX-0015");
    h.exit_is(&d, 1, "TAX-0015 ⇒ 退出码 1");

    // runtime_adapter 模板未交付 ⇒ TAX-0014(info) 但 exit 2(G-1)
    let a = args(json!({
        "plugin_type": "ability", "name": "service/crypto", "api_type": "runtime_adapter", "lang": "c",
    }));
    let (d, arts) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-TAX-0014", "未交付模板 ⇒ TAX-0014");
    h.exit_is(&d, 2, "模板未交付 ⇒ 命令未完成, 退出码 2(G-1)");
    h.eq(arts.len(), 0, "模板未交付 ⇒ 不产出计划");

    // 名字契约 ⇒ MF-0001 exit 2
    let a = args(json!({"plugin_type": "app", "name": "App/Hsm", "api_type": "native", "lang": "c"}));
    let (d, _) = plan::plan_new(&a, &Map::new());
    h.has_code(&d, "BRV-MF-0001", "名字契约违例 ⇒ MF-0001");

    // 已存在人写文件 ⇒ BRV-GEN-0002 exit 1(两段式的第二段)
    let a = args(json!({"plugin_type": "app", "name": "app/hsm", "api_type": "native", "lang": "c"}));
    let c = ctx(json!({"existing": [{"path": "app/hsm/plugin.toml", "first_line": "# 人写"}]}));
    let (d, arts) = plan::plan_new(&a, &c);
    h.has_code(&d, "BRV-GEN-0002", "已存在人写文件 ⇒ BRV-GEN-0002");
    h.exit_is(&d, 1, "BRV-GEN-0002 ⇒ 退出码 1");
    h.eq(arts.len(), 0, "冲突 ⇒ 空计划");

    // 生成物冲突: 非生成物 + 无 force ⇒ 红; 有 force ⇒ 越过
    let c = ctx(json!({"existing": [{"path": "build/gen/app/hsm/plugin_desc.c", "first_line": "int x;"}]}));
    let a = args(json!({"plugin_type": "app", "name": "app/hsm", "api_type": "native", "lang": "c"}));
    let (d, _) = plan::plan_new(&a, &c);
    h.has_code(&d, "BRV-GEN-0002", "生成物非本工具产物 ⇒ BRV-GEN-0002");
    let a2 = args(json!({"plugin_type": "app", "name": "app/hsm", "api_type": "native", "lang": "c", "force": true}));
    let (d2, arts2) = plan::plan_new(&a2, &c);
    h.exit_is(&d2, 0, "--force 越过生成物冲突");
    h.eq(arts2.len(), 6, "--force 后仍有 6 件计划");

    // 生成物已是本工具产物 ⇒ 正常重建(无需 force)
    let c = ctx(json!({"existing": [{"path": "build/gen/app/hsm/plugin_desc.c", "first_line": "/* brickie:generated */"}]}));
    let (d3, arts3) = plan::plan_new(&a, &c);
    h.exit_is(&d3, 0, "已是生成物 ⇒ 正常重建");
    h.eq(arts3.len(), 6, "生成物重建计划件数");

    // ---- plan-init ----
    let a = args(json!({"name": "hsm"}));
    let (d, arts, lock) = plan::plan_init(&a, &Map::new());
    h.exit_is(&d, 0, "plan-init 退出码 0");
    h.check(
        arts.iter().any(|x| x.path == "product.toml" && x.template == "product/c/product.toml.tmpl"),
        "plan-init 产 product.toml",
    );
    h.check(
        arts.iter().any(|x| x.path == "app/hsm/plugin.toml"),
        "plan-init 产 app/<name>/ 骨架",
    );
    h.check(
        arts.iter().any(|x| x.path == "brickie.lock" && x.kind == "machine"),
        "plan-init 产 brickie.lock(machine)",
    );
    h.check(
        lock.as_deref()
            .map(|s| s.contains("[lock]") && s.contains("[lock.product]"))
            .unwrap_or(false),
        "plan-init 带 lock 文本",
    );

    // ---- gen-plan(空树 ⇒ 空计划) ----
    let empty = tmpdir("gen-plan");
    let (d, arts) = plan::gen_plan(&empty, &Map::new());
    h.exit_is(&d, 0, "gen-plan 空树退出码 0");
    h.eq(arts.len(), 0, "gen-plan 空树无计划");
}

fn emit_cases(h: &mut Harness) {
    use crate::emit::{TDoc, TTable, TValue};
    let doc = TDoc::new()
        .set("schema", TValue::Int(1))
        .set("note", TValue::Str("中\"文\\转义\n".into()))
        .push(
            TTable::array_table(&["item"])
                .set("name", TValue::Str("a".into()))
                .set("tags", TValue::StrArray(vec!["x".into(), "y".into()])),
        );
    let text = doc.render();
    let text2 = doc.render();
    h.eq(text.clone(), text2, "TOML 发射幂等");
    h.check(text.contains("note = \"中\\\"文\\\\转义\\n\""), "TOML 字符串转义");
    h.check(text.contains("[\"x\", \"y\"]"), "TOML 字符串数组");
    h.check(text.starts_with("schema = 1\n"), "根键值先于表");
    let parsed: Result<toml::Value, _> = text.parse();
    h.check(parsed.is_ok(), "发射的 TOML 可被重新解析");
}

// ------------------------------------------------------------------ 入口

// ================================================================== agent-B: 新增自检
//
// 覆盖: 闭包 / 环路径(V-3)/ 相位单调不误杀(V-8)/ 版本推进表逐行(V-4)/
// append-vs-modify 三类(V-15)/ hash 输入完整性(V-17)/ publish 幂等(V-5)/
// 空解冻 refreeze(V-16)/ dev vs release(V-14)/ 预算差值(V-7)/ IRQ 独占(V-19④)/
// PRIV-0001(V-19①②)/ requires_iface 不参与闭包(V-10)。

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

/// 最小合法 ability 插件文本。
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

// ------------------------------------------------------------------ V-3 环路径

fn cycle_cases(h: &mut Harness) {
    let root = tmpdir("cycle");
    write(&root, "p-one/plugin.toml", &ability_toml("p-one", "service", "late", "[[dep]]\nname = \"p-two\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    write(&root, "p-two/plugin.toml", &ability_toml("p-two", "service", "late", "[[dep]]\nname = \"p-three\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    write(&root, "p-three/plugin.toml", &ability_toml("p-three", "service", "late", "[[dep]]\nname = \"p-one\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    let tree = model::load_tree(&root);
    let sol = solver::solve(&tree, "dev", false);
    h.eq(sol.closure.cycles.len(), 1, "V-3: 一个 init 环");
    h.eq(
        sol.closure.cycles[0].path.clone(),
        vec![
            "p-one".to_string(),
            "p-two".to_string(),
            "p-three".to_string(),
            "p-one".to_string(),
        ],
        "V-3: 完整环路径 A→B→C→A",
    );
    h.eq(sol.closure.cycles[0].edges.len(), 3, "V-3: 结构化边列表 3 条");
    h.check(scoped_has(&sol, "BRV-MF-0001"), "V-3: 环报 BRV-MF-0001(缺口代用)");
    h.check(
        scoped_msg(&sol, "BRV-MF-0001").contains("p-one → p-two → p-three → p-one"),
        "V-3: 消息含完整环路径",
    );
    let resp = solver::run(&mk_req("closure", &root, json!({})));
    h.eq(resp.exit_code, 1, "V-3: 环 ⇒ 退出码 1");
    h.j(&resp.data["cycles"][0]["path"], json!(["p-one", "p-two", "p-three", "p-one"]), "V-3: data.cycles[0].path");
    h.eq(
        resp.data["cycles"][0]["edges"].as_array().map(|a| a.len()),
        Some(3),
        "V-3: data.cycles[0].edges 结构化",
    );
    h.eq(
        resp.data["topo_order"].as_array().map(|a| a.len()),
        Some(3),
        "V-3: 有环时 topo_order 仍覆盖全部节点",
    );

    // runtime/type 环只报 info
    let root2 = tmpdir("cycle-rt");
    write(&root2, "r-one/plugin.toml", &ability_toml("r-one", "service", "late", "[[dep]]\nname = \"r-two\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    write(&root2, "r-two/plugin.toml", &ability_toml("r-two", "service", "late", "[[dep]]\nname = \"r-one\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    let sol2 = solver::solve(&model::load_tree(&root2), "dev", false);
    h.check(
        !sol2
            .diags
            .iter()
            .any(|(_, d)| d.code.as_deref() == Some("BRV-MF-0001")
                && d.severity == crate::diag::Severity::Error),
        "§7.1: runtime 环不报 error",
    );
    h.check(
        sol2.diags
            .iter()
            .any(|(_, d)| d.severity == crate::diag::Severity::Info),
        "§7.1: runtime 环只报 info",
    );
}

// ------------------------------------------------------------------ V-8 相位

fn phase_check_cases(h: &mut Harness) {
    // 反例: A(core, io) init-> B(late, service)
    let bad = tmpdir("phase-bad2");
    write(&bad, "phase-a/plugin.toml", &ability_toml("phase-a", "io", "core", "[[dep]]\nname = \"phase-b\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    write(&bad, "phase-b/plugin.toml", &ability_toml("phase-b", "service", "late", ""));
    let resp = check::run(&mk_req("check", &bad, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "V-8: 相位单调反例 ⇒ 退出码 1");
    h.check(has_code(&resp, "BRV-DEP-0009"), "V-8(R1): 报 BRV-DEP-0009");
    h.check(
        msg_of(&resp, "BRV-DEP-0009").contains("phase-b") && msg_of(&resp, "BRV-DEP-0009").contains("phase-a"),
        "V-8(R1): 消息点名双方",
    );

    // 正例(不误杀): sched-coop(core) → platform/qemu-aarch64(core)
    let ok = tmpdir("phase-ok2");
    write(&ok, "sched-coop/plugin.toml", &ability_toml("sched-coop", "scheduler", "core", "[[dep]]\nname = \"platform/qemu-aarch64\"\nrange = \">=0.1.0\"\nkind = \"init\"\nphase = \"core\"\n"));
    write(&ok, "platform/qemu-aarch64/plugin.toml", &platform_toml("platform/qemu-aarch64", "core", ""));
    let resp = check::run(&mk_req("check", &ok, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 0, "V-8: sched-coop→platform 不误杀 ⇒ 退出码 0");
    h.check(!has_code(&resp, "BRV-DEP-0009"), "V-8: 不报 BRV-DEP-0009");
    // 初始化拓扑序: 提供方(platform)先于消费者(sched-coop)
    let sol_ok = solver::solve(&model::load_tree(&ok), "dev", false);
    h.eq(
        sol_ok.closure.topo_order.clone(),
        vec!["platform/qemu-aarch64".to_string(), "sched-coop".to_string()],
        "§7.2: topo_order 是初始化序(提供方在前)",
    );

    // R2: 断言早于提供方自述 ⇒ DEP-0010
    let r2 = tmpdir("phase-r2");
    write(&r2, "phase-c/plugin.toml", &ability_toml("phase-c", "io", "core", "[[dep]]\nname = \"phase-d\"\nrange = \">=0.1.0\"\nkind = \"init\"\nphase = \"early\"\n"));
    write(&r2, "phase-d/plugin.toml", &ability_toml("phase-d", "io", "core", ""));
    let resp = check::run(&mk_req("check", &r2, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-DEP-0010"), "V-8(R2): 断言与提供方自述冲突 ⇒ BRV-DEP-0010");

    // R2 正例: 断言晚于提供方 ⇒ 不报
    let r2ok = tmpdir("phase-r2ok");
    write(&r2ok, "phase-e/plugin.toml", &ability_toml("phase-e", "service", "late", "[[dep]]\nname = \"phase-f\"\nrange = \">=0.1.0\"\nkind = \"init\"\nphase = \"late\"\n"));
    write(&r2ok, "phase-f/plugin.toml", &ability_toml("phase-f", "io", "core", ""));
    let resp = check::run(&mk_req("check", &r2ok, json!({"profile": "dev"})));
    h.check(!has_code(&resp, "BRV-DEP-0010"), "V-8(R2): 断言晚于提供方不报");
    h.check(!has_code(&resp, "BRV-DEP-0009"), "V-8(R1): core ≤ late 合法");
}

// ------------------------------------------------------------------ V-7 版本 / 预算

fn version_solver_cases(h: &mut Harness) {
    // range 越界 ⇒ VER-0002
    let root = tmpdir("ver-range");
    write(&root, "ver-a/plugin.toml", &ability_toml("ver-a", "service", "late", "[[dep]]\nname = \"ver-b\"\nrange = \">=2.0.0\"\nkind = \"runtime\"\n"));
    write(&root, "ver-b/plugin.toml", &ability_toml("ver-b", "service", "late", ""));
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "V-7: range 越界 ⇒ 退出码 1");
    h.check(has_code(&resp, "BRV-VER-0002"), "V-7: range 越界 ⇒ BRV-VER-0002");

    // 互斥区间 ⇒ DEP-0011(单版本政策)
    let root = tmpdir("ver-conflict2");
    write(&root, "ver-c/plugin.toml", &ability_toml("ver-c", "service", "late", "[[dep]]\nname = \"ver-d\"\nrange = \">=2.0.0\"\nkind = \"runtime\"\n"));
    write(&root, "ver-d/plugin.toml", &ability_toml("ver-d", "service", "late", ""));
    write(&root, "ver-e/plugin.toml", &ability_toml("ver-e", "service", "late", "[[dep]]\nname = \"ver-d\"\nrange = \"<1.0.0\"\nkind = \"runtime\"\n"));
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-DEP-0011"), "V-7: 互斥区间 ⇒ BRV-DEP-0011(单版本政策)");

    // 缺失依赖 ⇒ MF-0001
    let miss = tmpdir("ver-miss");
    write(&miss, "ver-f/plugin.toml", &ability_toml("ver-f", "service", "late", "[[dep]]\nname = \"no/such\"\nrange = \">=0.1.0\"\nkind = \"init\"\n"));
    let resp = check::run(&mk_req("check", &miss, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-MF-0001"), "V-7: 缺失依赖 ⇒ BRV-MF-0001(缺口代用)");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("no/such"),
        "V-7: 缺失消息点名依赖名",
    );

    // 预算超限 ⇒ 差值(V-19③)
    let bud = tmpdir("budget2");
    write(&bud, "platform/soc/plugin.toml", &platform_toml("platform/soc", "core", "[[res]]\nkind = \"ram\"\nused_kib = 64\n"));
    write(&bud, "service/mem/plugin.toml", &ability_toml("service/mem", "service", "late", "[[res]]\nkind = \"ram\"\nused_kib = 200\n"));
    write(&bud, "product.toml", "schema = 1\n[product]\nname = \"p\"\nversion = \"0.1.0.0\"\napp = \"\"\ncore = \">=1.0.0\"\nstage = \"dev\"\n[select]\nplugins = [\"platform/soc\", \"service/mem\"]\n[budget]\nram_kib = 100\nstack_kib = 8\n");
    let resp = check::run(&mk_req("check", &bud, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 1, "V-19③: 预算超限 ⇒ 退出码 1");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("差值 100 KiB"),
        "V-19③: 预算消息带差值(200-100)",
    );
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("超出 platform 容量 64"),
        "V-7: 物理上限(platform 容量)单独报",
    );
    h.j(&resp.data["summary"]["closure"]["ram_kib"], json!(200), "S-14: platform 的 [[res]] 作容量, 不进消费者 Σ");
}

// ------------------------------------------------------------------ V-12 跨文件 / V-14 profile

fn iface_crossfile_cases(h: &mut Harness) {
    // skin 指向不存在的单元 ⇒ TAX-0018
    let root = tmpdir("skin-miss");
    write(&root, "iface/skin/plugin.toml", "schema = 1\n[plugin]\nname = \"iface/skin\"\nplugin_type = \"interface\"\napi_type = \"native\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n[[export]]\napi_iface = \"native\"\nform = \"skin\"\nname = \"skin\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\nreexport_of = [\"service/nope#u\"]\n");
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-TAX-0018"), "V-12: skin 指向不存在单元 ⇒ BRV-TAX-0018");

    // skin 分类不等 ⇒ TAX-0018
    let root2 = tmpdir("skin-cls");
    write(&root2, "service/back/plugin.toml", &ability_toml("service/back", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    write(&root2, "iface/skin/plugin.toml", "schema = 1\n[plugin]\nname = \"iface/skin\"\nplugin_type = \"interface\"\napi_type = \"runtime_adapter\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n[[export]]\napi_iface = \"runtime_adapter\"\nform = \"skin\"\nname = \"skin\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\nreexport_of = [\"service/back#u\"]\n");
    let resp = check::run(&mk_req("check", &root2, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-TAX-0018"), "V-12: skin 分类不等 ⇒ BRV-TAX-0018");

    // 单元名碰撞 + 符号族碰撞
    let root3 = tmpdir("collide");
    write(&root3, "service/one/plugin.toml", &ability_toml("service/one", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"dup\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\nsymbols = [\"symA\"]\n"));
    write(&root3, "service/two/plugin.toml", &ability_toml("service/two", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"dup\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    let resp = check::run(&mk_req("check", &root3, json!({"profile": "dev"})));
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("单元名 `dup`"),
        "§2 第 4 项: 单元级碰撞 ⇒ 报",
    );

    // V-14: 同一输入, profile dev 绿 / release 红
    let root4 = tmpdir("profile14");
    write(&root4, "service/prov/plugin.toml", &ability_toml("service/prov", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    write(&root4, "service/cons/plugin.toml", &ability_toml("service/cons", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"c\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n[[dep]]\nname = \"service/prov\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n[[compat.requires_iface]]\nid = \"service/prov#u\"\napi_iface = \"native\"\ncompat_gen = 0\nrange = \">=1.0.0\"\nmode = \"decl\"\n"));
    let dev = check::run(&mk_req("check", &root4, json!({"profile": "dev"})));
    let rel = check::run(&mk_req("check", &root4, json!({"profile": "release"})));
    h.eq(dev.exit_code, 0, "V-14: dev ⇒ 退出码 0");
    h.eq(rel.exit_code, 1, "V-14: release ⇒ 退出码 1");
    h.check(
        has_code_sev(&dev, "BRV-VER-0004", "info"),
        "V-14: dev 下 VER-0004 是 info",
    );
    h.check(
        has_code_sev(&rel, "BRV-VER-0004", "error"),
        "V-14: release 下 VER-0004 是 error",
    );

    // V-10②: 加/去 requires_iface, 闭包 JSON 逐字节相同
    let with_iface = solver::run(&mk_req("closure", &root4, json!({"profile": "dev"})));
    let root5 = tmpdir("profile-noiface");
    write(&root5, "service/prov/plugin.toml", &ability_toml("service/prov", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    write(&root5, "service/cons/plugin.toml", &ability_toml("service/cons", "service", "late", "[[dep]]\nname = \"service/prov\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    let without = solver::run(&mk_req("closure", &root5, json!({"profile": "dev"})));
    h.eq(
        serde_json::to_string(&with_iface.data.clone()).unwrap(),
        serde_json::to_string(&without.data.clone()).unwrap(),
        "V-10②: requires_iface 不影响闭包 data",
    );

    // V-10③: iface show 对 requires_iface 只报 info
    let show = iface::run(&mk_req("iface-show", &root4, json!({"id": "service/cons#c"})));
    h.check(
        has_code_sev(&show, "BRV-MF-0001", "info"),
        "V-10③: iface show 对 requires_iface 只报 info",
    );

    // V-11: iface show 带 not_abi / truth / hash_scope
    h.j(&show.data["not_abi"], json!(true), "V-11①: not_abi=true");
    h.j(&show.data["truth"], json!("decl"), "V-11①: truth=decl");
    h.j(&show.data["hash_scope"], json!("decl"), "V-11①: hash_scope=decl");
}

// ------------------------------------------------------------------ V-19 特权独占 / 调度

fn priv_and_sched_cases(h: &mut Harness) {
    let root = tmpdir("res-conflict2");
    write(&root, "service/a/plugin.toml", &ability_toml("service/a", "service", "late", "[[privileged.resources]]\nirq = [32]\ndma_channels = [3]\n"));
    write(&root, "service/b/plugin.toml", &ability_toml("service/b", "service", "late", "[[privileged.resources]]\nirq = [32]\ndma_channels = [7]\n"));
    let resp = check::run(&mk_req("check", &root, json!({"profile": "dev", "scopes": ["priv"]})));
    h.eq(resp.exit_code, 1, "V-19④: IRQ 独占冲突 ⇒ 退出码 1");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("service/a") && msg_of(&resp, "BRV-MF-0001").contains("service/b"),
        "V-19④: 报冲突双方",
    );
    h.check(msg_of(&resp, "BRV-MF-0001").contains("irq `32`"), "V-19④: 点名资源");

    // V-19①②: PRIV 规则(加载期已报, check 不重复)
    let privroot = tmpdir("priv19");
    write(&privroot, "service/p/plugin.toml", &ability_toml("service/p", "service", "late", "[privileged]\nlevel = \"P2\"\n[[privileged.memory]]\ngranularity = \"page\"\nops = [\"map\"]\nregions = [\"mmio\"]\n"));
    let resp = check::run(&mk_req("check", &privroot, json!({"profile": "dev"})));
    h.check(has_code(&resp, "BRV-PRIV-0001"), "V-19②: ability 声明 P2 却用 map ⇒ BRV-PRIV-0001");
    h.check(
        !has_code(&resp, "BRV-PRIV-0002"),
        "§3.4: page×map×mmio 是合法组合(只是级别不够 ⇒ 只报 PRIV-0001)",
    );
    // 真正非法的组合: granularity=pool 的 map ⇒ PRIV-0002
    let bad2 = tmpdir("priv19b2");
    write(&bad2, "service/pp/plugin.toml", &ability_toml("service/pp", "service", "late", "[privileged]\nlevel = \"P4\"\n[[privileged.memory]]\ngranularity = \"pool\"\nops = [\"map\"]\nregions = [\"page\"]\n"));
    let resp2 = check::run(&mk_req("check", &bad2, json!({"profile": "dev"})));
    h.check(has_code(&resp2, "BRV-PRIV-0002"), "V-19①: 非法 (ops×granularity×region) ⇒ BRV-PRIV-0002");
    h.check(
        !diag_triples(&resp)
            .iter()
            .filter(|(c, _, _)| c == "BRV-PRIV-0001")
            .collect::<Vec<_>>()
            .is_empty(),
        "V-19①: 至少一条 PRIV-0001",
    );

    // 池生命周期合法(P2)
    let pool = tmpdir("priv-pool");
    write(&pool, "service/q/plugin.toml", &ability_toml("service/q", "service", "late", "[privileged]\nlevel = \"P2\"\n[[privileged.memory]]\ngranularity = \"pool\"\nops = [\"alloc\", \"free\"]\nregions = [\"contig\", \"page\"]\n"));
    let resp = check::run(&mk_req("check", &pool, json!({"profile": "dev"})));
    h.eq(resp.exit_code, 0, "V-19①: P2 池生命周期合法 ⇒ 退出码 0");

    // 调度: TT_SAFE 缺 [sched.tt] ⇒ 红
    let ttr = tmpdir("sched-tt");
    write(&ttr, "service/t/plugin.toml", &ability_toml("service/t", "service", "late", ""));
    let p = ttr.join("service/t/plugin.toml");
    let s = fs::read_to_string(&p).unwrap().replace("version = \"0.1.0.0\"", "version = \"0.1.0.0\"\nsched_class = \"TT_SAFE\"");
    fs::write(&p, s).unwrap();
    let resp = check::run(&mk_req("check", &ttr, json!({"profile": "dev", "scopes": ["tax"]})));
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("TT_SAFE"),
        "§2 第 3 项: TT_SAFE 缺 [sched.tt] ⇒ 红",
    );

    // COOP_ONLY × sched-preempt ⇒ 红; × sched-coop ⇒ 绿
    let coopbad = tmpdir("sched-coopbad");
    write(&coopbad, "sched-preempt/plugin.toml", &ability_toml("sched-preempt", "scheduler", "core", ""));
    write(&coopbad, "service/c/plugin.toml", &ability_toml("service/c", "service", "late", ""));
    let p = coopbad.join("service/c/plugin.toml");
    let s = fs::read_to_string(&p).unwrap().replace("version = \"0.1.0.0\"", "version = \"0.1.0.0\"\nsched_class = \"COOP_ONLY\"");
    fs::write(&p, s).unwrap();
    let resp = check::run(&mk_req("check", &coopbad, json!({"profile": "dev", "scopes": ["tax"]})));
    h.eq(resp.exit_code, 1, "§2 第 3 项: COOP_ONLY×preempt ⇒ 退出码 1");
    h.check(
        msg_of(&resp, "BRV-MF-0001").contains("sched-preempt"),
        "§2 第 3 项: 点名调度器",
    );

    let coopok = tmpdir("sched-coopok");
    write(&coopok, "sched-coop/plugin.toml", &ability_toml("sched-coop", "scheduler", "core", ""));
    write(&coopok, "service/c/plugin.toml", &ability_toml("service/c", "service", "late", ""));
    let p = coopok.join("service/c/plugin.toml");
    let s = fs::read_to_string(&p).unwrap().replace("version = \"0.1.0.0\"", "version = \"0.1.0.0\"\nsched_class = \"COOP_ONLY\"");
    fs::write(&p, s).unwrap();
    let resp = check::run(&mk_req("check", &coopok, json!({"profile": "dev", "scopes": ["tax"]})));
    h.eq(resp.exit_code, 0, "§2 第 3 项: COOP_ONLY×coop ⇒ 绿(裁定 R-5 名字推断)");
}

// ------------------------------------------------------------------ V-4 推进表

fn advance_plan_cases(h: &mut Harness) {
    // version::advance 逐行(既有 version_matrix 已覆盖大部分; 这里补 plan_version 的组合判定)
    let hard = vec![iface::Change {
        kind: "CHANGED",
        name: "f".into(),
        from: Some("a".into()),
        to: Some("b".into()),
        old_status: "frozen".into(),
        note: String::new(),
    }];
    h.check(hard[0].is_hard(), "V-15: CHANGED/frozen ⇒ 须解冻");
    let soft = vec![iface::Change {
        kind: "CHANGED",
        name: "f".into(),
        from: Some("a".into()),
        to: Some("b".into()),
        old_status: "experimental".into(),
        note: String::new(),
    }];
    h.check(!soft[0].is_hard(), "V-15: CHANGED/experimental ⇒ 免解冻");
    let removed_frozen = vec![iface::Change {
        kind: "REMOVED",
        name: "f".into(),
        from: Some("f".into()),
        to: None,
        old_status: "frozen".into(),
        note: String::new(),
    }];
    h.check(removed_frozen[0].is_hard(), "V-15: REMOVED/frozen ⇒ 须解冻");

    // (a) 改已冻结 ⇒ 仅 compat_gen+1, major 不动, minor/revise 归零
    let a = version::advance(Version::new(0, 1, 4, 7), version::Event::CompatGenBump);
    h.eq(a, Version::new(1, 1, 0, 0), "V-4(a): 改已冻结 ⇒ compat_gen+1, minor/revise→0");
    h.eq(a.major, 1, "V-4(a): major 不动");
    // (c) major ⇒ 仅 MAJOR+1, compat_gen 不动
    let c = version::advance(Version::new(3, 1, 4, 7), version::Event::MajorProduct);
    h.eq(c, Version::new(3, 2, 0, 0), "V-4(c): major ⇒ 仅 MAJOR+1, compat_gen 不动");
    h.eq(c.compat_gen, 3, "V-4(c): compat_gen 与 major 互不牵连");
    // (b) 新增 ⇒ compat_gen 不动, minor+1
    let b = version::advance(Version::new(2, 5, 1, 9), version::Event::Added);
    h.eq(b, Version::new(2, 5, 2, 0), "V-4(b): 新增 ⇒ compat_gen 不动, MINOR+1");
    // (d) minor
    h.eq(
        version::advance(Version::new(0, 1, 2, 3), version::Event::MinorFeature),
        Version::new(0, 1, 3, 0),
        "V-4(d): 小功能 ⇒ MINOR+1",
    );
    // (e) revise
    h.eq(
        version::advance(Version::new(0, 1, 2, 3), version::Event::ReviseFix),
        Version::new(0, 1, 2, 4),
        "V-4(e): 修 bug ⇒ REVISE+1",
    );
    // (f) 空解冻 ⇒ 四段不动
    h.eq(
        version::advance(Version::new(2, 3, 4, 5), version::Event::EmptyUnfreeze),
        Version::new(2, 3, 4, 5),
        "V-4(f): 空解冻 ⇒ 四段全不动",
    );
}

// ------------------------------------------------------------------ V-15 append vs modify + IFACE-IR

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

    // V-15: 三类 append vs modify
    let e1 = mk("enum", "color", None, None, Some("RED,GREEN"), vec![]);
    let e2 = mk("enum", "color", None, None, Some("RED,GREEN,BLUE"), vec![]);
    let e3 = mk("enum", "color", None, None, Some("GREEN,RED,BLUE"), vec![]);
    h.eq(iface::classify_modify(&e1, &e2), "EXTENDED", "V-15: 枚举末尾追加 ⇒ EXTENDED");
    h.eq(iface::classify_modify(&e1, &e3), "CHANGED", "V-15: 枚举重排 ⇒ CHANGED");
    let t1 = mk("type", "ctx", None, Some("struct { u32 a; }"), None, vec![]);
    let t2 = mk("type", "ctx", None, Some("struct { u32 a; u32 b; }"), None, vec![]);
    h.eq(iface::classify_modify(&t1, &t2), "CHANGED", "V-15: 给已冻结结构体加字段 ⇒ CHANGED");
    let s1 = mk("service", "svc", None, None, None, vec!["open"]);
    let s2 = mk("service", "svc", None, None, None, vec!["open", "close"]);
    h.eq(iface::classify_modify(&s1, &s2), "CHANGED", "V-15: service ops 加槽 ⇒ CHANGED");
    let f1 = mk("func", "f", Some("int(int)"), None, None, vec![]);
    let f2 = mk("func", "f", Some("long(int)"), None, None, vec![]);
    h.eq(iface::classify_modify(&f1, &f2), "CHANGED", "V-15: func 签名变更 ⇒ CHANGED");
    let m1 = mk("macro", "M", None, None, Some("16"), vec![]);
    let m2 = mk("macro", "M", None, None, Some("4096"), vec![]);
    h.eq(iface::classify_modify(&m1, &m2), "CHANGED", "V-15: macro 值变更 ⇒ CHANGED");

    // V-17: hash 输入完整性
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
        "V-17: BR_MAX 16→4096 产生不同 hash",
    );
    let mut ops_ch = base.clone();
    ops_ch[1].ops = vec!["open".into(), "close".into()];
    h.check(
        iface::surface_hash("p#u", &ops_ch, &[], false) != h_base,
        "V-17: service ops 加槽产生不同 hash",
    );
    let mut enum_re = base.clone();
    enum_re[2].value = Some("GREEN,RED".into());
    h.check(
        iface::surface_hash("p#u", &enum_re, &[], false) != h_base,
        "V-17: 枚举重排被 hash 感知",
    );
    let mut enum_app = base.clone();
    enum_app[2].value = Some("RED,GREEN,BLUE".into());
    h.check(
        iface::surface_hash("p#u", &enum_app, &[], false) != h_base,
        "V-17: 枚举追加被 hash 感知",
    );
    let mut st_ch = base.clone();
    st_ch[0].status = "deprecated".into();
    h.eq(
        iface::surface_hash("p#u", &st_ch, &[], false),
        h_base.clone(),
        "§6.2 规则 6: status 不进 hash",
    );
    let mut ordered = base.clone();
    ordered.reverse();
    h.eq(
        iface::surface_hash("p#u", &ordered, &[], false),
        h_base.clone(),
        "§6.2 规则 1: 声明顺序不影响 hash",
    );
    h.check(
        iface::surface_hash("p#v", &base, &[], false) != h_base,
        "§6.2 规则 10: 域分隔(unit id)防跨单元碰撞",
    );
    let mut sig_a = vec![mk("func", "f", Some("int(const u32 * buf, size_t len)"), None, None, vec![])];
    let mut sig_b = vec![mk("func", "f", Some("int (const u32 *buf, size_t n)"), None, None, vec![])];
    h.eq(
        iface::surface_hash("p#u", &sig_a, &[], false),
        iface::surface_hash("p#u", &sig_b, &[], false),
        "§6.2 规则 3: 形参名/排版不进 hash(非 strict)",
    );
    h.check(
        iface::surface_hash("p#u", &sig_a, &[], true) != iface::surface_hash("p#u", &sig_b, &[], true),
        "§6.2 规则 3: --strict-params 下形参名进 hash",
    );
    sig_a[0].sig = Some("int(const uint32_t* buf, size_t len)".into());
    sig_b[0].sig = Some("int(const u32* buf, size_t len)".into());
    let td = vec![("u32".to_string(), "uint32_t".to_string())];
    h.eq(
        iface::surface_hash("p#u", &sig_a, &td, false),
        iface::surface_hash("p#u", &sig_b, &td, false),
        "§6.2 规则 4: typedef 展开后同一类型两种写法 hash 相同",
    );
    h.check(
        h_base.starts_with("sha256:") && h_base.len() == 7 + 64,
        "§6.2 规则 8: hash = sha256:<64 hex>",
    );
    h.eq(iface::short_hash(Some(&h_base)).len(), 12, "§5.1: 展示取 12 hex");
    h.eq(iface::normalize_ws("  int   a  "), "int a".to_string(), "§6.2 规则 2: 空白归一");
    h.check(
        iface::expand_typedefs("u32 x", &td) == "uint32_t x",
        "§6.2 规则 4: 别名按标识符边界展开",
    );
    h.check(
        iface::expand_typedefs("my_u32 x", &td) == "my_u32 x",
        "§6.2 规则 4: 不做子串替换",
    );

    // change_set 的类别
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
    h.check(kinds.contains(&"CHANGED"), "V-15: change_set 含 CHANGED(sig)");
    h.check(kinds.contains(&"ADDED"), "V-15: change_set 含 ADDED");
    h.check(kinds.contains(&"REMOVED"), "V-15: change_set 含 REMOVED");
    h.check(kinds.contains(&"EXTENDED"), "V-15: change_set 含 EXTENDED(枚举末尾)");
}

// ------------------------------------------------------------------ V-5 publish 幂等 + V-11

const PUB_UNIT: &str = "service/pub/plugin.toml";
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
    h.eq(r1.exit_code, 0, "V-5: 首次 publish 退出码 0");
    h.eq(r1.files.as_array().map(|a| a.len()), Some(3), "V-5: 首次产出 3 类机器文件");
    let paths: Vec<String> = r1
        .files
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f["path"].as_str().unwrap_or("").to_string())
        .collect();
    h.check(
        paths.contains(&"api/iface/service/pub/u.toml".to_string()),
        "V-5: 快照路径 = api/iface/<provider 路径段>/<unit>.toml(R-11)",
    );
    h.check(
        paths.contains(&"api/iface/CHANGELOG.md".to_string()),
        "V-5: 产 CHANGELOG.md",
    );
    h.check(paths.contains(&"brickie.lock".to_string()), "V-5: 产 brickie.lock");
    h.check(
        r1.files.as_array().unwrap().iter().all(|f| f["kind"] == "machine"),
        "§4: 全部 kind=machine",
    );
    let snapshot_text = r1.files.as_array().unwrap()[0]["content"].as_str().unwrap_or("");
    h.check(snapshot_text.contains("NOT_ABI"), "V-11: 快照头带 NOT_ABI");
    h.check(snapshot_text.contains("hash_scope = \"decl\""), "V-11: 快照带 hash_scope");
    h.check(snapshot_text.contains("truth = \"decl\""), "V-11: 快照带 truth");
    h.check(snapshot_text.contains("strict_params = false"), "§6.2 规则 10: strict_params 存进快照");
    apply_files(&root, &r1);

    let r2 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/pub#u"})));
    h.eq(r2.exit_code, 0, "V-5: 第二次 publish 退出码 0");
    h.eq(r2.files.as_array().map(|a| a.len()), Some(0), "V-5: 面未变 ⇒ 无 files(空操作)");
    h.j(&r2.data["noop"], json!(true), "V-5: data.noop = true");
    h.j(&r2.data["version"], json!("0.1.0.0"), "V-5: 空操作不推进段");

    // 两次运行逐字节相同(新树 + 同一输入)
    let root_b = tmpdir("publish-b");
    write(&root_b, PUB_UNIT, &pub_toml("16"));
    let rb = iface::run(&mk_req("iface-publish", &root_b, json!({"id": "service/pub#u"})));
    h.eq(
        serde_json::to_string(&r1.files.clone()).unwrap(),
        serde_json::to_string(&rb.files.clone()).unwrap(),
        "§4: 同一输入两次 publish 逐字节相同",
    );

    // V-17 + V-5: 改 BR_MAX 后 status 重算不一致, publish 后一致
    let st0 = iface::run(&mk_req("iface-status", &root, json!({"id": "service/pub#u", "check": true})));
    h.j(&st0.data["consistent"], json!(true), "V-5: 发布后 status --check 一致");
    h.eq(st0.exit_code, 0, "V-5: status --check 退出码 0");
    write(&root, PUB_UNIT, &pub_toml("4096"));
    let st1 = iface::run(&mk_req("iface-status", &root, json!({"id": "service/pub#u", "check": true})));
    h.j(&st1.data["consistent"], json!(false), "V-17: BR_MAX 改动后 status 不一致");
    h.eq(st1.exit_code, 1, "V-17: 不一致 ⇒ 退出码 1");
    let d = iface::run(&mk_req("iface-diff", &root, json!({"id": "service/pub#u"})));
    h.j(&d.data["changes"][0]["kind"], json!("CHANGED"), "V-15: diff 报 CHANGED");
    h.j(&d.data["version"]["to"], json!("0.1.1.0"), "V-4: 仅 experimental 改动 ⇒ MINOR+1");
    let r3 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/pub#u"})));
    h.eq(r3.exit_code, 0, "V-17: 改后再 publish 成功");
    h.j(&r3.data["to"], json!("0.1.1.0"), "V-4(b): 新增/改动 ⇒ COMPAT_GEN 不动");
    h.j(&r3.data["compat_gen_changed"], json!(false), "V-4(b): compat_gen_changed=false");
    apply_files(&root, &r3);
    let st2 = iface::run(&mk_req("iface-status", &root, json!({"id": "service/pub#u", "check": true})));
    h.j(&st2.data["consistent"], json!(true), "V-5: 再发布后 status 一致");
}

// ------------------------------------------------------------------ V-16 解冻窗口

fn freeze_window_cases(h: &mut Harness) {
    let root = tmpdir("unfreeze");
    let toml_frozen = "schema = 1\n[plugin]\nname = \"service/w\"\nplugin_type = \"ability\"\napi_type = \"native\"\n\
        subkind = \"service\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n\
        [[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\n\
        compat_gen = 0\nfreeze_state = \"frozen\"\nhash = \"\"\nstatus = \"frozen\"\n\
        [[export.entries]]\nkind = \"type\"\nname = \"ctx\"\nlayout = \"struct { u32 a; }\"\nstatus = \"frozen\"\n";
    write(&root, "service/w/plugin.toml", toml_frozen);
    write(&root, "NOTE.md", "决策记录\n");
    let r0 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/w#u"})));
    h.eq(r0.exit_code, 0, "V-16: 建档发布成功");
    apply_files(&root, &r0);
    let ver0 = r0.data["to"].clone();

    // 无 note ⇒ 红
    let r1 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u"})));
    h.eq(r1.exit_code, 1, "V-16: unfreeze 无 --note ⇒ 红");

    // frozen ⇒ unfreezing
    let r2 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    h.eq(r2.exit_code, 0, "V-16: unfreeze 成功");
    h.j(&r2.data["freeze_state"], json!("unfreezing"), "V-16: 进入解冻窗口");
    let snap_text = r2.files.as_array().unwrap()[0]["content"].as_str().unwrap_or("");
    h.check(snap_text.contains("baseline_hash"), "裁定 R-7: 快照里记 baseline");
    apply_files(&root, &r2);

    // 窗口内 release ⇒ IFACE-0009
    let rel = check::run(&mk_req("check", &root, json!({"profile": "release"})));
    h.eq(rel.exit_code, 1, "V-16: 窗口内 release ⇒ 退出码 1");
    h.check(has_code(&rel, "BRV-IFACE-0009"), "V-16: 窗口内 release ⇒ BRV-IFACE-0009");
    let dev = check::run(&mk_req("check", &root, json!({"profile": "dev"})));
    h.eq(dev.exit_code, 0, "V-16: 窗口内 dev ⇒ 退出码 0");

    // 空解冻 refreeze ⇒ 四段全不动
    let r3 = iface::run(&mk_req("iface-refreeze", &root, json!({"id": "service/w#u"})));
    h.eq(r3.exit_code, 0, "V-16: refreeze 成功");
    h.j(&r3.data["empty_unfreeze"], json!(true), "V-16: 识别为空解冻");
    h.j(&r3.data["from"], ver0.clone(), "V-16: 空解冻 ⇒ 原版本");
    h.j(&r3.data["to"], ver0.clone(), "V-16: 空解冻 ⇒ 四段全不动");
    h.j(&r3.data["compat_gen_changed"], json!(false), "V-16: 空解冻不 bump COMPAT_GEN");
    apply_files(&root, &r3);

    // 再解冻 → 改已冻结结构体 → refreeze ⇒ compat_gen+1
    let r4 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    apply_files(&root, &r4);
    write(&root, "service/w/plugin.toml", &toml_frozen.replace("struct { u32 a; }", "struct { u32 a; u32 b; }"));
    let r5 = iface::run(&mk_req("iface-refreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    h.eq(r5.exit_code, 0, "V-4(a): refreeze 改已冻结条目成功");
    h.j(&r5.data["compat_gen_changed"], json!(true), "V-4(a): 确有改已冻结条目 ⇒ COMPAT_GEN+1");
    h.j(&r5.data["compat_gen"], json!(1), "V-4(a): compat_gen 由 0 变 1");
    h.check(
        r5.data["changes"]
            .as_array()
            .map(|a| a.iter().any(|c| c["kind"] == json!("CHANGED")))
            .unwrap_or(false),
        "V-4(a): 变更集含 CHANGED",
    );
    apply_files(&root, &r5);
    // 非 frozen 不能解冻(刚 refreeze 回 frozen ⇒ 可解冻, 但这里只验证"能进窗口")
    let r6 = iface::run(&mk_req("iface-unfreeze", &root, json!({"id": "service/w#u", "note": "NOTE.md"})));
    h.eq(r6.exit_code, 0, "解冻需要 frozen(刚 refreeze 回 frozen)");
    apply_files(&root, &r6);

    // S-19: 窗口内用 `publish --note` 记录硬变更 ⇒ 只 bump 一次; 随后的 refreeze 只关窗口。
    write(
        &root,
        "service/w/plugin.toml",
        &toml_frozen
            .replace("struct { u32 a; }", "struct { u32 a; u32 b; u32 c; }"),
    );
    let p1 = iface::run(&mk_req(
        "iface-publish",
        &root,
        json!({"id": "service/w#u", "note": "NOTE.md"}),
    ));
    h.eq(p1.exit_code, 0, "S-19: 窗口内 publish 硬变更(有 note)成功");
    h.j(&p1.data["compat_gen_changed"], json!(true), "S-19: publish 记录 COMPAT_GEN+1");
    let cg1 = p1.data["compat_gen"].clone();
    h.j(&p1.data["freeze_state"], json!("unfreezing"), "S-19: publish 不抹掉解冻窗口");
    apply_files(&root, &p1);
    let p2 = iface::run(&mk_req("iface-refreeze", &root, json!({"id": "service/w#u"})));
    h.eq(p2.exit_code, 0, "S-19: refreeze 关窗口");
    h.j(&p2.data["empty_unfreeze"], json!(true), "S-19: publish 已消费基线 ⇒ 空解冻");
    h.j(&p2.data["compat_gen"], cg1, "S-19: 不重复 bump COMPAT_GEN");
}

// ------------------------------------------------------------------ 其它门钩

fn gate_cases(h: &mut Harness) {
    // REMOVED(仍是 frozen)⇒ 红(§6.3)
    let root = tmpdir("removed-gate");
    let toml_gone = "schema = 1\n[plugin]\nname = \"service/g\"\nplugin_type = \"ability\"\napi_type = \"native\"\n\
        subkind = \"service\"\nlang = \"c\"\nphase = \"late\"\nversion = \"0.1.0.0\"\n[compat]\ncore = \">=1.0.0\"\n\
        [[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"0.1.0.0\"\n\
        compat_gen = 0\nfreeze_state = \"frozen\"\nhash = \"\"\nstatus = \"frozen\"\n\
        [[export.entries]]\nkind = \"func\"\nname = \"gone\"\nsig = \"void(void)\"\nstatus = \"frozen\"\n\
        [[export.entries]]\nkind = \"func\"\nname = \"keep\"\nsig = \"void(void)\"\nstatus = \"frozen\"\n";
    write(&root, "service/g/plugin.toml", toml_gone);
    write(&root, "NOTE.md", "n\n");
    let r0 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/g#u"})));
    apply_files(&root, &r0);
    let s2 = toml_gone.replace(
        "[[export.entries]]\nkind = \"func\"\nname = \"gone\"\nsig = \"void(void)\"\nstatus = \"frozen\"\n",
        "",
    );
    write(&root, "service/g/plugin.toml", &s2);

    let r1 = iface::run(&mk_req("iface-publish", &root, json!({"id": "service/g#u", "note": "NOTE.md"})));
    h.eq(r1.exit_code, 1, "§6.3: REMOVED 未 deprecated ⇒ 红");
    h.check(
        msg_of2(&r1).contains("REMOVED"),
        "§6.3: 消息点名 REMOVED 需先 deprecated",
    );

    // iface freeze 的 entry 过滤: 提案只含指定条目
    let f = iface::run(&mk_req(
        "iface-freeze",
        &root,
        json!({"id": "service/g#u", "entry": "keep", "note": "NOTE.md"}),
    ));
    h.eq(f.exit_code, 0, "iface freeze: entry 过滤成功");
    let content = f.files[0]["content"].as_str().unwrap_or("");
    h.check(content.contains("requested_entry = \"keep\""), "iface freeze: 记下 entry");
    h.check(
        !content.contains("name = \"gone\""),
        "iface freeze: 提案只含指定条目",
    );
}

// ------------------------------------------------------------------ dep 命令面

fn dep_command_cases(h: &mut Harness) {
    let root = tmpdir("depcmds");
    write(&root, "service/consumer/plugin.toml", &ability_toml("service/consumer", "service", "late", "[[dep]]\nname = \"service/provider\"\nrange = \">=0.1.0\"\nkind = \"runtime\"\n"));
    write(&root, "service/provider/plugin.toml", &ability_toml("service/provider", "service", "late", ""));

    let g = solver::run(&mk_req("dep-graph", &root, json!({"format": "json"})));
    h.j(&g.data["format"], json!("json"), "dep graph: format");
    let text = g.data["text"].as_str().unwrap_or("");
    let parsed: Result<Value, _> = serde_json::from_str(text);
    h.check(parsed.is_ok(), "dep graph(json): data.text 是规范化 JSON 串");
    h.eq(
        parsed.map(|v| v["edges"].as_array().map(|a| a.len())).unwrap_or(None),
        Some(1),
        "dep graph(json): 1 条边",
    );
    let dot = solver::run(&mk_req("dep-graph", &root, json!({"format": "dot"})));
    h.check(
        dot.data["text"].as_str().unwrap_or("").contains("digraph"),
        "dep graph(dot): 产 dot",
    );
    let mer = solver::run(&mk_req("dep-graph", &root, json!({"format": "mermaid"})));
    h.check(
        mer.data["text"].as_str().unwrap_or("").contains("graph LR"),
        "dep graph(mermaid): 产 mermaid",
    );
    let badfmt = solver::run(&mk_req("dep-graph", &root, json!({"format": "svg"})));
    h.eq(badfmt.exit_code, 2, "dep graph: 未知 format ⇒ 用法错 2");

    let w = solver::run(&mk_req("dep-why", &root, json!({"from": "service/consumer", "to": "service/provider"})));
    h.j(&w.data["found"], json!(true), "dep why: 找到路径");
    h.j(&w.data["path"], json!(["service/consumer", "service/provider"]), "dep why: 最短路径");
    h.j(&w.data["edges"][0]["kind"], json!("runtime"), "dep why: 边带 kind");
    let w2 = solver::run(&mk_req("dep-why", &root, json!({"from": "service/provider", "to": "service/consumer"})));
    h.j(&w2.data["found"], json!(false), "dep why: 反向找不到 ⇒ found=false");
    h.eq(w2.exit_code, 0, "dep why: 找不到不是错误(exit 0)");

    let t = solver::run(&mk_req("dep-tree", &root, json!({"kind": "init"})));
    h.j(&t.data["kind"], json!("init"), "dep tree: kind=init");
    let t2 = solver::run(&mk_req("dep-tree", &root, json!({"kind": "bogus"})));
    h.eq(t2.exit_code, 2, "dep tree: 未知 kind ⇒ 用法错 2");

    let idx = solver::run(&mk_req("dep-index", &root, json!({})));
    h.eq(idx.files.as_array().map(|a| a.len()), Some(1), "dep index: 一个机器文件");
    h.j(&idx.files[0]["path"], json!("build/index/dependents.json"), "dep index: 路径");
    let content = idx.files[0]["content"].as_str().unwrap_or("");
    h.check(content.contains("\"schema\": 1"), "dep index: JSON 含 schema");
    h.j(&idx.data["dependents"]["service/provider"][0]["from"], json!("service/consumer"), "dep index: 反向边");
    // 幂等
    let idx2 = solver::run(&mk_req("dep-index", &root, json!({})));
    h.eq(
        idx.files[0]["content"].clone(),
        idx2.files[0]["content"].clone(),
        "dep index: 逐字节可复现",
    );
}

// ------------------------------------------------------------------ 版本面命令

fn ver_command_cases(h: &mut Harness) {
    let root = tmpdir("vercmds");
    write(&root, "service/v/plugin.toml", &ability_toml("service/v", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"u\"\nversion = \"1.2.3.4\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    let s = iface::run(&mk_req("ver-show", &root, json!({"id": "service/v#u"})));
    h.eq(s.exit_code, 0, "ver show: 退出码 0");
    h.j(&s.data["version"], json!("1.2.3.4"), "ver show: 四段版本");
    h.j(&s.data["segments"], json!({"compat_gen": 1, "major": 2, "minor": 3, "revise": 4}), "ver show: segments");
    h.j(&s.data["source"], json!("declaration"), "ver show: 未发布 ⇒ source=declaration");
    h.j(&s.data["compat_gen_source"]["kind"], json!("never-frozen"), "ver show: compat_gen 来源");

    let b = iface::run(&mk_req("ver-bump", &root, json!({"id": "service/v#u", "rule": "revise"})));
    h.eq(b.exit_code, 0, "ver bump: 退出码 0");
    h.j(&b.data["from"], json!("1.2.3.4"), "ver bump: from");
    h.j(&b.data["to"], json!("1.2.3.5"), "V-4(e): ver bump revise ⇒ REVISE+1");
    h.eq(b.files.as_array().map(|a| a.len()), Some(2), "ver bump: 写快照 + CHANGELOG");
    h.check(
        b.files
            .as_array()
            .unwrap()
            .iter()
            .any(|f| f["path"] == json!("api/iface/service/v/u.toml")),
        "ver bump: 落单元快照",
    );
    apply_files(&root, &b);
    let s2 = iface::run(&mk_req("ver-show", &root, json!({"id": "service/v#u"})));
    h.j(&s2.data["source"], json!("snapshot"), "ver show: 发布后 source=snapshot");
    h.j(&s2.data["published"], json!(true), "ver show: published=true");

    let maj = iface::run(&mk_req("ver-bump", &root, json!({"id": "service/v#u", "rule": "major"})));
    h.j(&maj.data["to"], json!("1.3.0.0"), "V-4(c): ver bump major ⇒ MAJOR+1, compat_gen 不动");
    h.j(&maj.data["compat_gen"], json!(1), "V-4(c): compat_gen 不动");

    let bad = iface::run(&mk_req("ver-bump", &root, json!({"id": "service/v#u", "rule": "compat_gen"})));
    h.eq(bad.exit_code, 2, "ver bump: compat_gen 不在 rule 里 ⇒ 用法错 2");

    // 多 export 插件用插件名 ⇒ 用法错
    let root2 = tmpdir("vermulti");
    write(&root2, "service/m/plugin.toml", &ability_toml("service/m", "service", "late", "[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"a\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n[[export]]\napi_iface = \"native\"\nform = \"api\"\nname = \"b\"\nversion = \"0.1.0.0\"\ncompat_gen = 0\nfreeze_state = \"unfrozen\"\nhash = \"\"\nstatus = \"experimental\"\n"));
    let amb = iface::run(&mk_req("ver-show", &root2, json!({"id": "service/m"})));
    h.eq(amb.exit_code, 2, "R-3: 插件名多 export ⇒ 用法错 2");

    // iface list / show
    let list = iface::run(&mk_req("iface-list", &root, json!({})));
    h.j(&list.data["count"], json!(1), "iface list: 计数");
    h.j(&list.data["units"][0]["id"], json!("service/v#u"), "iface list: 单元 id");
    h.j(&list.data["units"][0]["published"], json!(true), "iface list: published");

    // iface freeze: 只出提案, 不落快照 / 不 bump
    let f0 = iface::run(&mk_req("iface-freeze", &root, json!({"id": "service/v#u"})));
    h.eq(f0.exit_code, 1, "A-26: freeze 无 --note ⇒ 红");
    write(&root, "NOTE.md", "note\n");
    let f1 = iface::run(&mk_req("iface-freeze", &root, json!({"id": "service/v#u", "note": "NOTE.md"})));
    h.eq(f1.exit_code, 0, "A-26: freeze 出提案");
    h.j(&f1.data["proposal"], json!("build/gen/proposals/u.toml"), "A-26: 提案路径 build/gen/proposals/<unit>.toml");
    h.eq(
        f1.files
            .as_array()
            .unwrap()
            .iter()
            .filter(|f| f["path"].as_str().unwrap_or("").starts_with("api/iface/"))
            .count(),
        0,
        "A-26: 不落 frozen 快照",
    );
    let proposal = f1.files[0]["content"].as_str().unwrap_or("");
    h.check(
        proposal.contains("v01_no_frozen_snapshot = true"),
        "A-26: 提案显式声明 v0.1 不落 frozen 快照",
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
    snapshot_and_lock_cases(&mut h);
    plan_cases(&mut h);
    emit_cases(&mut h);
    // agent-B: 三条命令族的领域用例
    cycle_cases(&mut h);
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
            "brickie-core selftest: {} 失败 / {} 通过",
            h.failures.len(),
            h.passed
        );
        1
    }
}

// ================================================================== 构建族(ADR-0004)

/// 极简请求(带 context)。
/// `Diags` 里有没有某条码(与 `has_code` 的 Response 版分工: 计划类命令回 Diags)。
fn diags_code(d: &Diags, code: &str) -> bool {
    d.items().iter().any(|x| x.code.as_deref() == Some(code))
}

fn mk_req_ctx(command: &str, root: &Path, args: Value, context: Value) -> Request {
    let mut r = mk_req(command, root, args);
    r.context = ctx(context);
    r
}

/// 一个最小可构建的合成树: product.toml + platform(带 [build.target]) + ability + app。
///
/// 真源文件也写出来(通配要匹配得到) —— 这样"计划"是**真的**从声明面算出来的,
/// 而不是靠 mock。工具解析用 `context.path = ""` 关掉, 于是用例在任何机器上都可复现。
fn write_build_tree(root: &Path, extra_build: &str, target: &str) {
    write(root, "core/src/core.c", "int core_fn(void){return 0;}\n");
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
             [select]\nplugins = [\"platform/fake\"]\n\
             [build]\ncore_sources = [\"core/src/*.c\"]\ncore_includes = [\"core/include\"]\n\
             cflags = [\"-std=c11\", \"-O2\"]\nasflags = [\"-g3\"]\nldflags = [\"-nostdlib\"]\n{extra_build}"
        ),
    );
    write(
        root,
        "platform/fake/plugin.toml",
        &format!(
            "schema = 1\n[plugin]\nname = \"platform/fake\"\nplugin_type = \"platform\"\napi_type = \"native\"\n\
             lang = \"c\"\nphase = \"early\"\nversion = \"1.0.0.0\"\n\
             [build]\nsources = [\"src/*.c\", \"src/*.S\"]\nincludes = [\"include\"]\n{target}"
        ),
    );
    write(
        root,
        "app/m/plugin.toml",
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
    // ---- 纯函数: 通配 ----
    h.check(build::selftest_glob("*.c", "a.c"), "glob_match: *.c 匹配 a.c");
    h.check(!build::selftest_glob("*.c", "a.h"), "glob_match: *.c 不匹配 a.h");
    h.check(build::selftest_glob("a?c", "abc"), "glob_match: a?c 匹配 abc");
    h.check(
        build::selftest_glob("*_test.c", "mem_test.c"),
        "glob_match: *_test.c 匹配 mem_test.c",
    );
    h.check(!build::selftest_glob("*_test.c", "test_mem.c"), "glob_match: 前缀不同不匹配");

    // ---- 纯函数: 正则子集 ----
    h.check(
        build::selftest_regex("pass=[0-9]* fail=0 ", "x pass=35 fail=0 y"),
        "regex: [0-9]* 命中",
    );
    h.check(
        !build::selftest_regex("pass=[0-9]+ fail=0 ", "pass= fail=0 "),
        "regex: + 要求至少一次",
    );
    h.check(
        build::selftest_regex(r"\[PANIC\]", "x [PANIC] y"),
        "regex: \\[ 是字面方括号",
    );
    h.check(
        !build::selftest_regex(r"\[PANIC\]", "x PANIC y"),
        "regex: 转义后不再匹配裸词",
    );
    h.check(
        build::selftest_regex("irq_ticks=[1-9]", "irq_ticks=7"),
        "regex: 字符类区间命中",
    );
    h.check(
        !build::selftest_regex("irq_ticks=[1-9]", "irq_ticks=0"),
        "regex: 区间外不命中",
    );
    h.check(
        build::selftest_regex("[^0-9]x", "ax"),
        "regex: 字符类取反命中",
    );
    h.check(
        build::selftest_regex_unsupported("(a|b)").is_some(),
        "regex: 分组/交替被**拒绝**(不静默不匹配)",
    );
    h.check(
        build::selftest_regex_unsupported("^a$").is_some(),
        "regex: 锚点被拒绝",
    );
    h.check(
        build::selftest_regex_unsupported(r"\[A\] pass=[0-9]*").is_none(),
        "regex: gates.toml 里用到的形态都在子集内",
    );

    // ---- 计划: 正常树 ----
    let root = tmpdir("build-ok");
    write_build_tree(&root, "", TARGET_OK);
    let req = mk_req_ctx(
        "build",
        &root,
        json!({"dry_run": true}),
        json!({"path": ""}),
    );
    let (d, data) = build::selftest_plan(&root, &req);
    h.exit_is(&d, 0, "build: 合成树计划无诊断");
    h.j(&data["unit_count"], json!(3), "build: 单元数 = core + platform + app");
    h.j(&data["source_count"], json!(4), "build: 源文件数 = core 1 + plat 2 + app 1");
    let steps = data["steps"].as_array().cloned().unwrap_or_default();
    h.j(&json!(steps.len()), json!(6), "build: 步骤数 = 4 编译 + 链接 + objcopy");
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
        "build: arch_flags 进了编译命令(来自 platform 的 [build.target])",
    );
    h.check(
        all.iter().any(|s| s.contains("-Wl,-T,platform/fake/src/link.ld")),
        "build: 链接脚本来自 platform 且路径按插件根解析",
    );
    h.check(
        all.iter().any(|s| s.contains("-MF build/obj/core/src/core.o.d")),
        "build: 编译带 -MMD/-MF(头依赖的唯一来源)",
    );
    h.check(
        all.iter().any(|s| s.contains("-nostdlib") && s.contains("build/brick.elf")),
        "build: 链接命令含产品 ldflags 与产物落点",
    );
    h.check(
        data["target"]["owner"] == json!("platform/fake"),
        "build: 目标事实的 owner = 唯一的 platform",
    );
    h.check(
        data["outputs"]["obj_dir"] == json!("build/obj"),
        "build: 对象落点缺省 = build/obj",
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
        "build: cc 的候选序里含带版本号的变体(Ubuntu 只装 gcc-16 的情况)",
    );
    h.check(
        data["state_content"]
            .as_str()
            .map(|s| s.contains("[[jobs]]") && s.contains("argv_hash"))
            .unwrap_or(false),
        "build: 增量状态是 core 产出的 TOML(含 argv 指纹与输入表)",
    );
    h.check(
        data["plan_hash"].as_str().map(|s| s.starts_with("sha256:")).unwrap_or(false),
        "build: 计划指纹是 sha256:…",
    );

    // ---- 计划: 缺 [build] ----
    let root2 = tmpdir("build-nobuild");
    write_build_tree(&root2, "", TARGET_OK);
    write(
        &root2,
        "product.toml",
        "schema = 1\n[product]\nname = \"p\"\napp = \"app/m\"\n",
    );
    let (d2, _) = build::selftest_plan(&root2, &mk_req_ctx("build", &root2, json!({}), json!({"path": ""})));
    h.check(diags_code(&d2, build::CODE_NO_BUILD), "build: 缺 [build] ⇒ BRV-BLD-0001");
    h.exit_is(&d2, 2, "build: 缺 [build] 是形状错(退出码 2)");

    // ---- 计划: 缺 [build.target] ----
    let root3 = tmpdir("build-notarget");
    write_build_tree(&root3, "", "");
    let (d3, _) = build::selftest_plan(&root3, &mk_req_ctx("build", &root3, json!({}), json!({"path": ""})));
    h.check(diags_code(&d3, build::CODE_NO_TARGET), "build: 无 [build.target] ⇒ BRV-BLD-0003");

    // ---- 计划: 两个 platform 都声明 target ----
    let root4 = tmpdir("build-ambiguous");
    write_build_tree(&root4, "", TARGET_OK);
    write(
        &root4,
        "platform/other/plugin.toml",
        "schema = 1\n[plugin]\nname = \"platform/other\"\nplugin_type = \"platform\"\n\
         api_type = \"native\"\nlang = \"c\"\nphase = \"early\"\nversion = \"1.0.0.0\"\n\
         [build]\nsources = [\"src/*.c\"]\n[build.target]\narch = \"aarch64\"\n",
    );
    let (d4, _) = build::selftest_plan(&root4, &mk_req_ctx("build", &root4, json!({}), json!({"path": ""})));
    h.check(
        diags_code(&d4, build::CODE_TARGET_AMBIGUOUS),
        "build: 两个 [build.target] ⇒ BRV-BLD-0004(谁是真值不明)",
    );

    // ---- 计划: 字面源路径不存在 ----
    let root5 = tmpdir("build-missing-src");
    write_build_tree(&root5, "", TARGET_OK);
    write(
        &root5,
        "app/m/plugin.toml",
        "schema = 1\n[plugin]\nname = \"app/m\"\nplugin_type = \"app\"\napi_type = \"native\"\n\
         lang = \"c\"\nphase = \"app\"\nversion = \"1.0.0.0\"\n\
         [build]\nsources = [\"src/nope.c\"]\n",
    );
    let (d5, _) = build::selftest_plan(&root5, &mk_req_ctx("build", &root5, json!({}), json!({"path": ""})));
    h.check(
        diags_code(&d5, build::CODE_MISSING_SOURCE),
        "build: 字面源路径不存在 ⇒ BRV-BLD-0007",
    );

    // ---- `**` 通配 + `gen_sources` 的闭包过滤(踩到过: 非闭包插件的描述符被编进镜像,
    //      而它的 .c 不编 ⇒ 链接期未定义符号) ----
    let root8 = tmpdir("build-gen-src");
    write_build_tree(
        &root8,
        "gen_sources = [\"build/gen/**/plugin_desc.c\"]\n",
        TARGET_OK,
    );
    // 闭包内插件(app/m)的生成物 + **不在闭包内**的插件(service/out)的生成物
    write(&root8, "build/gen/app/m/plugin_desc.c", "int app_desc;\n");
    write(&root8, "build/gen/service/out/plugin_desc.c", "int out_desc;\n");
    write(
        &root8,
        "service/out/plugin.toml",
        "schema = 1\n[plugin]\nname = \"service/out\"\nplugin_type = \"ability\"\n\
         api_type = \"native\"\nsubkind = \"service\"\nlang = \"c\"\nphase = \"late\"\n\
         version = \"1.0.0.0\"\n[build]\nsources = [\"src/*.c\"]\n",
    );
    let (d9, data9) = build::selftest_plan(
        &root8,
        &mk_req_ctx("build", &root8, json!({}), json!({"path": ""})),
    );
    h.exit_is(&d9, 0, "gen_sources: 跳过非闭包生成物只是 info(不改退出码)");
    h.check(
        diags_code(&d9, build::CODE_GEN_NOT_SELECTED),
        "gen_sources: 非闭包插件的生成物被跳过且**留痕**(BRV-BLD-0013)",
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
        "gen_sources: `**` 匹配到闭包内插件的生成物",
    );
    h.check(
        !gen_srcs.iter().any(|s| s.contains("service/out")),
        "gen_sources: 非闭包插件的生成物**不进**镜像",
    );

    // ---- 门禁: 声明面与判据 ----
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
    h.j(&list.data["gates"], json!(["g1"]), "gates: gate 名字表");
    h.j(&list.data["hosttests"], json!(["t1"]), "gates: 宿主用例名字表");
    h.j(&list.data["scripts"], json!(["s1"]), "gates: 脚本门禁名字表");
    h.j(&list.data["build_post"], json!(["s1"]), "gates: [build].post 进了数据");

    // 判据: 全绿
    let good = "banner\nPASS TC-X-001 ok\npass=3 fail=0 \n";
    let r_good = build::run(&mk_req_ctx(
        "judge",
        &root6,
        json!({"name": "g1"}),
        json!({"log": good}),
    ));
    h.exit_is(&Diags::new(), 0, "judge: 基线退出码 0");
    h.check(r_good.exit_code == 0, "judge: require/forbid/tag 全满足 ⇒ 退出码 0");
    h.check(
        r_good.data["failed"] == json!(0),
        "judge: failed 计数为 0",
    );

    // 判据: 缺 require + 命中 forbid + 缺用例 tag
    let bad = "banner only\n[PANIC] boom\n";
    let r_bad = build::run(&mk_req_ctx(
        "judge",
        &root6,
        json!({"name": "g1"}),
        json!({"log": bad}),
    ));
    h.check(r_bad.exit_code == 1, "judge: 有失败 ⇒ 退出码 1(校验红)");
    h.j(&r_bad.data["failed"], json!(3), "judge: 三类判据各失败一次");
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
        "judge: 失败诊断统一带 BRV-BLD-0009",
    );

    // 判据: 未知门禁名
    let r_unknown = build::run(&mk_req_ctx(
        "judge",
        &root6,
        json!({"name": "nope"}),
        json!({"log": ""}),
    ));
    h.check(
        has_code(&r_unknown, build::CODE_GATE_UNKNOWN),
        "judge: 未知门禁 ⇒ BRV-BLD-0008",
    );
    h.check(r_unknown.exit_code == 2, "judge: 未知门禁 ⇒ 用法错(退出码 2)");

    // 判据: 正则超出子集 ⇒ 形状错(不静默不匹配)
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
        "gates: 正则超子集 ⇒ BRV-BLD-0012",
    );
    h.check(bad_re.exit_code == 2, "gates: 正则超子集是形状错(退出码 2)");

    // ---- 后端文件(生成物): 关键行都得在 ----
    let (d8, data8) = build::selftest_plan(&root, &mk_req_ctx("build", &root, json!({}), json!({"path": ""})));
    h.exit_is(&d8, 0, "build: 后端渲染用树无诊断");
    let mk = build::selftest_render_backend(&root, &req, "make");
    let nj = build::selftest_render_backend(&root, &req, "ninja");
    h.check(
        mk.contains("brickie:generated") && mk.contains(".DEFAULT_GOAL := all") && mk.contains("-include $(DEPS)"),
        "backend/make: 生成物标记 + 缺省目标 + 头依赖 include",
    );
    h.check(
        nj.contains("rule cc") && nj.contains("depfile = $out.d") && nj.contains("deps = gcc"),
        "backend/ninja: rule/depfile/deps 三件齐",
    );
    h.check(
        nj.contains("cc = "),
        "backend/ninja: 编译器变量必须先定义($cc 否则展开为空 —— 踩过)",
    );
    h.check(
        nj.contains("builddir = build/gen/ninja"),
        "backend/ninja: ninja 状态文件钉到 build/ 下(否则落到仓库根 —— 踩过)",
    );
    h.check(
        nj.contains("rule asm") && nj.contains("rule cc") && nj.contains(": asm "),
        "backend/ninja: C 与 .S 各一条 rule(用 C 的模板编汇编是错的)",
    );
    h.check(
        mk.contains("build/obj/core/src/core.o") && nj.contains("build/obj/app/m/src/main.o"),
        "backend: 两个后端都列出对象",
    );
    h.check(
        data8["steps_total"] == json!(6),
        "build: 后端渲染不改计划(仍 6 步)",
    );
    let _ = fs::remove_dir_all(&root7);
}

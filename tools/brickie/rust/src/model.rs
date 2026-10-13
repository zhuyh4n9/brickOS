//! Declaration-surface model + loader for `brick.toml` / `product.toml`.
//! Scan skips `.git` / `build` / `prebuilts` / `tools` / `api` / `docs` / `Design` and dot-dirs;
//! `product.toml` is optional. Shape errors (incl. 4-segment version/range) exit 2 with
//! `BRV-MF-0001` / `BRV-VER-0005/0006`; per-record invariants (taxonomy, phase, privilege) are
//! reported here for V-18a -- check.rs must not re-report them. `Snapshot` / `Lock` are read-only;
//! parsing is via `toml::Value` (no serde derive).

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use crate::diag::{Diags, Severity};
use crate::rules;
use crate::version::{self, Range, Version};

pub const SKIP_DIRS: &[&str] = &[".git", "build", "prebuilts", "tools", "api", "docs", "Design"];

/// One `[[res]]` entry.
#[derive(Clone, Debug, Default)]
pub struct Res {
    pub kind: String,
    pub used_kib: i64,
    pub notes: Option<String>,
}

/// `[sched.tt]` (only when `sched_class = "TT_SAFE"`).
#[derive(Clone, Debug, Default)]
pub struct SchedTt {
    pub period_us: i64,
    pub deadline_us: i64,
    pub wcet_us: i64,
}

/// `[[compat.requires_iface]]` (v0.1 validates schema only, not used in solving — V-10).
#[derive(Clone, Debug, Default)]
pub struct RequiresIface {
    pub id: String,
    pub api_iface: String,
    pub compat_gen: u64,
    pub range: Range,
    pub range_raw: String,
    pub mode: String,
}

#[derive(Clone, Debug, Default)]
pub struct Compat {
    pub core: Option<Range>,
    pub core_raw: Option<String>,
    pub api_rev: Option<i64>,
    pub hash_scope: Option<String>,
    pub truth: Option<String>,
    pub abi_id: Option<String>,
    pub requires_iface: Vec<RequiresIface>,
}

/// `[[dep]]` entry (structural dependency; never **pins** `compat_gen`, per F3).
#[derive(Clone, Debug, Default)]
pub struct Dep {
    pub name: String,
    pub range: Range,
    pub range_raw: String,
    pub kind: String,
    /// Phase assertion (only `kind = "init"`; default = consumer's own phase).
    pub phase: Option<String>,
    pub symbol: Option<String>,
}

#[derive(Clone, Debug, Default)]
pub struct ExportEntry {
    pub kind: String,
    pub name: String,
    pub sig: Option<String>,
    pub layout: Option<String>,
    pub value: Option<String>,
    pub ops: Option<Vec<String>>,
    pub status: String,
}

/// One `[[export]]` (interface unit = basic granularity of freezing and versioning, F1).
#[derive(Clone, Debug, Default)]
pub struct Export {
    pub api_iface: String,
    pub form: String,
    pub name: String,
    pub version: Option<Version>,
    pub version_raw: Option<String>,
    pub compat_gen: Option<u64>,
    pub freeze_state: Option<String>,
    pub hash: Option<String>,
    pub status: Option<String>,
    pub reexport_of: Vec<String>,
    pub symbols: Vec<String>,
    pub entries: Vec<ExportEntry>,
}

impl Export {
    /// Interface unit id = `<provider>#<unit>`.
    pub fn unit_id(&self, plugin_name: &str) -> String {
        format!("{}#{}", plugin_name, self.name)
    }
}

#[derive(Clone, Debug, Default)]
pub struct PrivilegedResources {
    pub irq: Vec<i64>,
    pub dma_channels: Vec<i64>,
    pub pins: Vec<i64>,
    pub device_names: Vec<String>,
}

impl PrivilegedResources {
    pub fn is_empty(&self) -> bool {
        self.irq.is_empty()
            && self.dma_channels.is_empty()
            && self.pins.is_empty()
            && self.device_names.is_empty()
    }
}

#[derive(Clone, Debug, Default)]
pub struct PrivilegedDecl {
    pub level: Option<String>,
    pub memory: Vec<rules::MemoryEntry>,
    pub resources: PrivilegedResources,
}

/// One `[[mount]]` entry: the APP declares what to mount where; FS plugins only register drivers.
#[derive(Clone, Debug, Default)]
pub struct MountDecl {
    /// POSIX `mount()` arg 1: source device name (media FS only; empty when no media).
    pub source: Option<String>,
    /// POSIX `mount()` arg 2: mount point (absolute path).
    pub target: String,
    /// POSIX `mount()` arg 3: driver name (`"tmpfs"`/`"devfs"`/`"fat"`/`"erofs"`).
    pub fstype: String,
    /// Build-time sort key (APP must mount in ascending order: "/" before sub-mounts).
    pub order: i64,
}

/// `[build]` (consumed by `brickie build`).
#[derive(Clone, Debug, Default)]
pub struct BuildDecl {
    /// Globs relative to the plugin root (`src/*.c`; expansion rules in `build.rs`).
    pub sources: Vec<String>,
    /// Include directories relative to the plugin root.
    pub includes: Vec<String>,
    /// Extra `-D<name>` (optional; the product-level `[build]` has the same key).
    pub defines: Vec<String>,
    /// Plugin-local flags, appended after the product-level `cflags`; can disable warnings.
    pub cflags: Vec<String>,
}

/// Platform `[build.target]`: target facts; ownership is the platform (one platform per image).
#[derive(Clone, Debug, Default)]
pub struct BuildTarget {
    pub arch: String,
    /// Cross prefix (`aarch64-linux-gnu-`) or a path-qualified prefix.
    pub cross: String,
    /// Flags **determined by the target**, such as `-march=…` / `-mstrict-align`.
    pub arch_flags: Vec<String>,
    /// Linker script path relative to the **plugin root**.
    pub linker_script: String,
    pub qemu_binary: String,
    pub qemu_machine: String,
    pub qemu_cpu: String,
    pub qemu_memory: String,
    pub qemu_extra: Vec<String>,
}

/// `product.toml [build]`: **product-level build policy** (flag tables + output location). Source
/// sets belong to the plugins themselves (`brick.toml [build]`), including the core plugin's.
#[derive(Clone, Debug, Default)]
pub struct ProductBuild {
    /// Generated-source set (`build/gen/**`, produced by `brickie gen`), relative to the repo root.
    pub gen_sources: Vec<String>,
    /// Selftest sources appended to the **core plugin's** unit (repo-root relative). Product-level
    /// because the selftest master switch is product policy: off ⇒ this suite is not compiled.
    pub selftest_sources: Vec<String>,
    pub cflags: Vec<String>,
    pub asflags: Vec<String>,
    pub ldflags: Vec<String>,
    /// Flags **appended** under `--profile release` (default = same as dev).
    pub release_cflags_extra: Vec<String>,
    pub release_asflags_extra: Vec<String>,
    pub release_ldflags_extra: Vec<String>,
    pub obj_dir: Option<String>,
}

#[derive(Clone, Debug, Default)]
pub struct Plugin {
    /// `brick.toml` path relative to `root`.
    pub file: String,
    /// Plugin directory relative to `root`.
    pub dir: String,
    pub schema: i64,
    pub name: String,
    pub plugin_type: String,
    pub api_type: String,
    pub subkind: Option<String>,
    /// `[plugin].symbol_prefix`: hook/descriptor symbol prefix override. Default derives from the
    /// short name, so same-named plugins in different namespaces collide at link time.
    pub symbol_prefix: Option<String>,
    pub lang: String,
    pub phase: String,
    pub sched_class: String,
    pub sched_kind: Option<String>,
    /// Declares `[selftest]`: emit the `.selftest` symbol reference only then (not derivable).
    pub has_selftest: bool,
    /// `[selftest].cases`: self-declared case count (**presentation/diagnostics only**).
    pub selftest_cases: Option<i64>,
    pub version: Version,
    pub version_raw: String,
    pub summary: Option<String>,
    pub license: Option<String>,
    pub res: Vec<Res>,
    pub sched_tt: Option<SchedTt>,
    pub compat: Compat,
    pub typedefs: Vec<(String, String)>,
    pub deps: Vec<Dep>,
    pub exports: Vec<Export>,
    pub privileged: Option<PrivilegedDecl>,
    pub mounts: Vec<MountDecl>,
    pub build: Option<BuildDecl>,
    pub build_target: Option<BuildTarget>,
}

impl Plugin {
    /// `[[res]]` totals: `(ram_kib, stack_kib)`.
    pub fn res_totals(&self) -> (i64, i64) {
        let mut ram = 0;
        let mut stack = 0;
        for r in &self.res {
            match r.kind.as_str() {
                "ram" => ram += r.used_kib,
                "stack" => stack += r.used_kib,
                _ => {}
            }
        }
        (ram, stack)
    }

    pub fn unit_ids(&self) -> Vec<String> {
        self.exports.iter().map(|e| e.unit_id(&self.name)).collect()
    }

    pub fn find_export(&self, unit: &str) -> Option<&Export> {
        self.exports.iter().find(|e| e.name == unit)
    }

    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let res: Vec<_> = self
            .res
            .iter()
            .map(|r| json!({"kind": r.kind, "used_kib": r.used_kib, "notes": r.notes}))
            .collect();
        let deps: Vec<_> = self
            .deps
            .iter()
            .map(|d| {
                json!({
                    "name": d.name,
                    "range": d.range.canonical(),
                    "range_raw": d.range_raw,
                    "kind": d.kind,
                    "phase": d.phase,
                    "symbol": d.symbol,
                })
            })
            .collect();
        let exports: Vec<_> = self
            .exports
            .iter()
            .map(|e| {
                let entries: Vec<_> = e
                    .entries
                    .iter()
                    .map(|en| {
                        json!({
                            "kind": en.kind,
                            "name": en.name,
                            "sig": en.sig,
                            "layout": en.layout,
                            "value": en.value,
                            "ops": en.ops,
                            "status": en.status,
                        })
                    })
                    .collect();
                json!({
                    "id": e.unit_id(&self.name),
                    "api_iface": e.api_iface,
                    "form": e.form,
                    "name": e.name,
                    "version": e.version.map(|v| v.to_string()),
                    "compat_gen": e.compat_gen,
                    "freeze_state": e.freeze_state,
                    "hash": e.hash,
                    "status": e.status,
                    "reexport_of": e.reexport_of,
                    "symbols": e.symbols,
                    "entries": entries,
                })
            })
            .collect();
        let requires: Vec<_> = self
            .compat
            .requires_iface
            .iter()
            .map(|r| {
                json!({
                    "id": r.id,
                    "api_iface": r.api_iface,
                    "compat_gen": r.compat_gen,
                    "range": r.range.canonical(),
                    "mode": r.mode,
                })
            })
            .collect();
        let typedefs: serde_json::Map<String, serde_json::Value> = self
            .typedefs
            .iter()
            .map(|(k, v)| (k.clone(), serde_json::Value::String(v.clone())))
            .collect();
        let privileged = self.privileged.as_ref().map(|p| {
            let mem: Vec<_> = p
                .memory
                .iter()
                .map(|m| {
                    json!({
                        "granularity": m.granularity,
                        "ops": m.ops,
                        "regions": m.regions,
                    })
                })
                .collect();
            json!({
                "level": p.level,
                "memory": mem,
                "resources": {
                    "irq": p.resources.irq,
                    "dma_channels": p.resources.dma_channels,
                    "pins": p.resources.pins,
                    "device_names": p.resources.device_names,
                },
            })
        });
        json!({
            "file": self.file,
            "dir": self.dir,
            "schema": self.schema,
            "name": self.name,
            "plugin_type": self.plugin_type,
            "api_type": self.api_type,
            "subkind": self.subkind,
            "lang": self.lang,
            "phase": self.phase,
            "sched_class": self.sched_class,
            "sched_kind": self.sched_kind,
            "version": self.version.to_string(),
            "summary": self.summary,
            "license": self.license,
            "res": res,
            "res_totals": {"ram_kib": self.res_totals().0, "stack_kib": self.res_totals().1},
            "sched_tt": self.sched_tt.as_ref().map(|t| json!({
                "period_us": t.period_us, "deadline_us": t.deadline_us, "wcet_us": t.wcet_us,
            })),
            "compat": {
                "core": self.compat.core.as_ref().map(|r| r.canonical()),
                "api_rev": self.compat.api_rev,
                "hash_scope": self.compat.hash_scope,
                "truth": self.compat.truth,
                "abi_id": self.compat.abi_id,
                "requires_iface": requires,
            },
            "iface": {"typedefs": serde_json::Value::Object(typedefs)},
            "dep": deps,
            "export": exports,
            "privileged": privileged,
            "build": self.build.as_ref().map(|b| json!({
                "sources": b.sources, "includes": b.includes,
            })),
            "unit_ids": self.unit_ids(),
        })
    }
}

#[derive(Clone, Debug, Default)]
pub struct Product {
    pub file: String,
    pub schema: i64,
    pub name: String,
    pub version: Version,
    pub app: String,
    pub core: Option<Range>,
    pub stage: String,
    pub select: Vec<String>,
    pub budget_ram_kib: Option<i64>,
    pub budget_stack_kib: Option<i64>,
    /// `[kernel].hz`: tick frequency in Hz. `None` = undeclared ⇒ default 200 (5 ms/tick);
    /// passed to all compilation units as `-DBR_CFG_TICK_HZ=<hz>` (single source of truth).
    pub kernel_hz: Option<i64>,
    pub lint_frozen_deps: String,
    pub allow_edges: Vec<(String, String)>,
    /// `[build]`: product-level build policy (consumed by `brickie build`).
    pub build: Option<ProductBuild>,
    /// `[selftest].enabled`: master selftest switch; `None` = undeclared ⇒ follow stage.
    pub selftest_enabled: Option<bool>,
    /// `[selftest].plugins`: run only the listed plugins (empty = all declaring `[selftest]`).
    pub selftest_plugins: Vec<String>,
    /// Whether the `[selftest]` table is present (distinguishes absent from empty table).
    pub has_selftest_table: bool,
}

impl Product {
    /// Selftest master switch: explicit true/false, else follow `stage` (dev on, release off).
    pub fn selftest_on(&self) -> bool {
        match self.selftest_enabled {
            Some(v) => v,
            None => self.stage != "release",
        }
    }

    /// Whether this plugin emits the `selftest` hook now (switch ∧ list); empty = no filtering.
    pub fn selftest_for(&self, plugin: &str) -> bool {
        if !self.selftest_on() {
            return false;
        }
        self.selftest_plugins.is_empty() || self.selftest_plugins.iter().any(|n| n == plugin)
    }

    /// `[lint].frozen_deps` overrides the `stage` default.
    pub fn release_strict(&self) -> bool {
        match self.lint_frozen_deps.as_str() {
            "deny" => true,
            "allow" => false,
            _ => self.stage == "release",
        }
    }

    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let edges: Vec<_> = self
            .allow_edges
            .iter()
            .map(|(a, b)| json!([a, b]))
            .collect();
        json!({
            "file": self.file,
            "schema": self.schema,
            "name": self.name,
            "version": self.version.to_string(),
            "app": self.app,
            "core": self.core.as_ref().map(|r| r.canonical()),
            "stage": self.stage,
            "select": self.select,
            "budget": {"ram_kib": self.budget_ram_kib, "stack_kib": self.budget_stack_kib},
            "kernel": {"hz": self.kernel_hz},
            "lint": {"frozen_deps": self.lint_frozen_deps, "allow_edges": edges},
            "selftest": {
                "enabled": self.selftest_on(),
                "declared": self.selftest_enabled,
                "plugins": self.selftest_plugins,
            },
            "build": self.build.as_ref().map(|b| json!({
                "gen_sources": b.gen_sources,
                "selftest_sources": b.selftest_sources,
                "cflags": b.cflags,
                "asflags": b.asflags,
                "ldflags": b.ldflags,
                "release": {
                    "cflags_extra": b.release_cflags_extra,
                    "asflags_extra": b.release_asflags_extra,
                    "ldflags_extra": b.release_ldflags_extra,
                },
                "obj_dir": b.obj_dir,
            })),
        })
    }
}

#[derive(Clone, Debug, Default)]
pub struct TreeLoad {
    pub diags: Diags,
    pub plugins: Vec<Plugin>,
    pub product: Option<Product>,
    /// Whether `product.toml` was found (R-1 branch criterion).
    pub has_product: bool,
}

impl TreeLoad {
    pub fn by_name(&self, name: &str) -> Option<&Plugin> {
        self.plugins.iter().find(|p| p.name == name)
    }

    pub fn allow_edges(&self) -> Vec<(String, String)> {
        self.product
            .as_ref()
            .map(|p| p.allow_edges.clone())
            .unwrap_or_default()
    }

    /// Ruling R-6: platform `[[res]]` is also platform capacity; `(ram_kib, stack_kib)`, summed.
    pub fn platform_capacity(&self) -> (i64, i64) {
        let mut ram = 0;
        let mut stack = 0;
        for p in &self.plugins {
            if p.plugin_type == "platform" {
                let (r, s) = p.res_totals();
                ram += r;
                stack += s;
            }
        }
        (ram, stack)
    }

    /// Ruling R-1: no `product.toml` ⇒ validate the whole plugin tree, skip product-level counts.
    pub fn product_level_constraints_apply(&self) -> bool {
        self.has_product
    }
}

fn req_str(
    t: &toml::Table,
    key: &str,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<String> {
    match t.get(key) {
        None => {
            d.shape(
                "BRV-MF-0001",
                target,
                file,
                format!("{span}.{key}"),
                format!("missing required field `{key}`"),
                "contract §7 field table",
            );
            None
        }
        Some(v) => match v.as_str() {
            Some(s) => Some(s.to_string()),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("field `{key}` has wrong shape: expected string"),
                    "contract §7 field table",
                );
                None
            }
        },
    }
}

fn opt_str(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Option<String> {
    match t.get(key) {
        None => None,
        Some(v) => match v.as_str() {
            Some(s) => Some(s.to_string()),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("field `{key}` has wrong shape: expected string"),
                    "contract §7 field table",
                );
                None
            }
        },
    }
}

fn req_int(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Option<i64> {
    match t.get(key) {
        None => {
            d.shape(
                "BRV-MF-0001",
                target,
                file,
                format!("{span}.{key}"),
                format!("missing required field `{key}`"),
                "contract §7 field table",
            );
            None
        }
        Some(v) => match v.as_integer() {
            Some(i) => Some(i),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("field `{key}` has wrong shape: expected int"),
                    "contract §7 field table",
                );
                None
            }
        },
    }
}

fn opt_int(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Option<i64> {
    match t.get(key) {
        None => None,
        Some(v) => match v.as_integer() {
            Some(i) => Some(i),
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("field `{key}` has wrong shape: expected int"),
                    "contract §7 field table",
                );
                None
            }
        },
    }
}

fn str_array(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Vec<String> {
    match t.get(key) {
        None => Vec::new(),
        Some(v) => match v.as_array() {
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("field `{key}` has wrong shape: expected string array"),
                    "contract §7 field table",
                );
                Vec::new()
            }
            Some(arr) => {
                let mut out = Vec::new();
                for (i, item) in arr.iter().enumerate() {
                    match item.as_str() {
                        Some(s) => out.push(s.to_string()),
                        None => d.shape(
                            "BRV-MF-0001",
                            target,
                            file,
                            format!("{span}.{key}[{i}]"),
                            format!("field `{key}[{i}]` has wrong shape: expected string"),
                            "contract §7 field table",
                        ),
                    }
                }
                out
            }
        },
    }
}

fn int_array(t: &toml::Table, key: &str, target: &str, file: &str, span: &str, d: &mut Diags) -> Vec<i64> {
    match t.get(key) {
        None => Vec::new(),
        Some(v) => match v.as_array() {
            None => {
                d.shape(
                    "BRV-MF-0001",
                    target,
                    file,
                    format!("{span}.{key}"),
                    format!("field `{key}` has wrong shape: expected int array"),
                    "contract §7 field table",
                );
                Vec::new()
            }
            Some(arr) => {
                let mut out = Vec::new();
                for (i, item) in arr.iter().enumerate() {
                    match item.as_integer() {
                        Some(n) => out.push(n),
                        None => d.shape(
                            "BRV-MF-0001",
                            target,
                            file,
                            format!("{span}.{key}[{i}]"),
                            format!("field `{key}[{i}]` has wrong shape: expected int"),
                            "contract §7 field table",
                        ),
                    }
                }
                out
            }
        },
    }
}

/// Parse `[[mount]]`: `target` / `fstype` required, `source` optional (media-less FS), `order`
/// optional (default 0, ties keep declaration order). Only `plugin_type = "app"` may declare it.
fn parse_mounts(
    top: &toml::Table,
    name: &str,
    file: &str,
    plugin_type: &str,
    d: &mut Diags,
) -> Vec<MountDecl> {
    let Some(arr) = top.get("mount") else {
        return Vec::new();
    };
    let Some(items) = arr.as_array() else {
        d.shape(
            "BRV-MF-0001",
            name,
            file,
            "mount",
            "`[[mount]]` has wrong shape: expected array of tables".to_string(),
            "write `[[mount]]` followed by source/target/fstype/order",
        );
        return Vec::new();
    };
    if plugin_type != "app" && !items.is_empty() {
        d.shape(
            "BRV-MF-0001",
            name,
            file,
            "mount",
            format!(
                "`[[mount]]` may only be declared by `plugin_type = \"app\"` (this one is `{plugin_type}`)"
            ),
            "ADR-0030: the mount plan belongs to the **consumer** (APP); FS plugins only register drivers (fstype)",
        );
        return Vec::new();
    }
    let mut out = Vec::new();
    for (i, it) in items.iter().enumerate() {
        let Some(tb) = it.as_table() else {
            d.shape(
                "BRV-MF-0001",
                name,
                file,
                format!("mount[{i}]"),
                "`[[mount]]` element is not a table".to_string(),
                "contract §7 field table",
            );
            continue;
        };
        let span = format!("mount[{i}]");
        let target = opt_str(tb, "target", name, file, &span, d);
        let fstype = opt_str(tb, "fstype", name, file, &span, d);
        let (Some(target), Some(fstype)) = (target, fstype) else {
            d.shape(
                "BRV-MF-0001",
                name,
                file,
                span,
                "`[[mount]]` is missing required field(s) (target / fstype)".to_string(),
                "target = mount point (absolute path), fstype = driver name (e.g. \"fat\")",
            );
            continue;
        };
        out.push(MountDecl {
            source: opt_str(tb, "source", name, file, &span, d),
            target,
            fstype,
            order: opt_int(tb, "order", name, file, &span, d).unwrap_or(0),
        });
    }
    out
}

fn opt_table<'a>(t: &'a toml::Table, key: &str) -> Option<&'a toml::Table> {
    t.get(key).and_then(|v| v.as_table())
}

/// Fetch a nested value by path (`[sched.tt]` ⇒ `["sched","tt"]`).
fn dig<'a>(t: &'a toml::Table, path: &[&str]) -> Option<&'a toml::Value> {
    let (first, rest) = path.split_first()?;
    let mut cur = t.get(*first)?;
    for k in rest {
        cur = cur.as_table()?.get(*k)?;
    }
    Some(cur)
}

fn opt_sub_table<'a>(t: &'a toml::Table, path: &[&str]) -> Option<&'a toml::Table> {
    dig(t, path).and_then(|v| v.as_table())
}

fn array_of_tables<'a>(t: &'a toml::Table, key: &str) -> Vec<&'a toml::Table> {
    match t.get(key).and_then(|v| v.as_array()) {
        None => Vec::new(),
        Some(arr) => arr.iter().filter_map(|v| v.as_table()).collect(),
    }
}

/// `[[export.entries]].value`: string or string array (R-4). The array form is joined with `,`
/// so the hash input is one deterministic string.
fn load_entry_value(
    t: &toml::Table,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<String> {
    let v = t.get("value")?;
    if let Some(s) = v.as_str() {
        return Some(s.to_string());
    }
    if let Some(arr) = v.as_array() {
        let mut parts = Vec::with_capacity(arr.len());
        let mut ok = true;
        for (i, item) in arr.iter().enumerate() {
            match item.as_str() {
                Some(s) => parts.push(s.to_string()),
                None => {
                    ok = false;
                    d.shape(
                        "BRV-MF-0001",
                        target,
                        file,
                        format!("{span}.value[{i}]"),
                        "elements of the `value` array must be strings (ruling R-4)",
                        "contract §7.1 / §6.2 rule 9",
                    );
                }
            }
        }
        return if ok { Some(parts.join(",")) } else { None };
    }
    d.shape(
        "BRV-MF-0001",
        target,
        file,
        format!("{span}.value"),
        "`value` has wrong shape: expected string or string array (ruling R-4)",
        "contract §7.1",
    );
    None
}

fn shape_enum(
    d: &mut Diags,
    target: &str,
    file: &str,
    span: &str,
    key: &str,
    val: &str,
    allowed: &[&str],
) {
    d.shape(
        "BRV-MF-0001",
        target,
        file,
        format!("{span}.{key}"),
        format!(
            "field `{key}` value `{val}` out of range; allowed: {}",
            rules::list_of(allowed)
        ),
        "contract §7 field table",
    );
}

fn check_enum(
    d: &mut Diags,
    target: &str,
    file: &str,
    span: &str,
    key: &str,
    val: &str,
    allowed: &[&str],
) -> bool {
    if rules::in_list(allowed, val) {
        true
    } else {
        shape_enum(d, target, file, span, key, val, allowed);
        false
    }
}

/// Parse the `version` field (four segments); shape error ⇒ `BRV-VER-0005` + exit code 2.
fn parse_version_field(
    raw: &str,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<Version> {
    match version::parse_version(raw) {
        Ok(v) => Some(v),
        Err(e) => {
            d.shape(e.code, target, file, span, e.message, "§5.1: four segments `COMPAT_GEN.MAJOR.MINOR.REVISE`");
            None
        }
    }
}

/// Parse the `range` field: 4 segments ⇒ `BRV-VER-0006`, else `BRV-MF-0001` (both exit 2).
fn parse_range_field(
    raw: &str,
    target: &str,
    file: &str,
    span: &str,
    d: &mut Diags,
) -> Option<Range> {
    match Range::parse(raw) {
        Ok(r) => Some(r),
        Err(e) => {
            let code = if e.code == version::VER_0006 {
                version::VER_0006
            } else {
                "BRV-MF-0001"
            };
            d.shape(code, target, file, span, e.message, "§7.4: range is exactly 3 segments, missing ones right-padded 0, 4 illegal");
            None
        }
    }
}

fn rel_path(root: &Path, p: &Path) -> String {
    p.strip_prefix(root)
        .unwrap_or(p)
        .to_string_lossy()
        .replace('\\', "/")
}

/// Parse one `brick.toml`. Returns `None` only when even the TOML cannot be read.
pub fn load_plugin_file(root: &Path, abs: &Path, d: &mut Diags) -> Option<Plugin> {
    let file = rel_path(root, abs);
    let dir = abs
        .parent()
        .map(|p| rel_path(root, p))
        .unwrap_or_default();
    let text = match fs::read_to_string(abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("cannot read `{file}`: {e}"), "check path and permissions");
            return None;
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(
                format!("`{file}` is not valid TOML: {e}"),
                "BRV-D9 exit code 2 (environment/usage error, no code)",
            );
            return None;
        }
    };
    let top = match value.as_table() {
        Some(t) => t,
        None => {
            d.env(format!("top level of `{file}` must be a TOML table"), "contract §7.1");
            return None;
        }
    };
    let target = file.as_str();

    let mut p = Plugin {
        file: file.clone(),
        dir,
        schema: 0,
        lang: "c".to_string(), // default is c per §8.1; still required explicitly (see below)
        sched_class: "SAFE_PREEMPT".to_string(),
        version: Version::new(0, 0, 0, 0),
        ..Default::default()
    };

    match req_int(top, "schema", target, &file, "", d) {
        Some(1) => p.schema = 1,
        Some(other) => d.shape(
            "BRV-MF-0001",
            target,
            &file,
            "schema",
            format!("`schema` supports only 1, got {other}"),
            "contract §7.1",
        ),
        None => {}
    }

    let Some(plugin) = opt_table(top, "plugin") else {
        d.shape(
            "BRV-MF-0001",
            target,
            &file,
            "plugin",
            "missing required table `[plugin]`",
            "contract §7.1",
        );
        return Some(p);
    };

    p.name = req_str(plugin, "name", target, &file, "plugin", d).unwrap_or_default();
    if !p.name.is_empty() && !rules::valid_plugin_name(&p.name) {
        d.shape(
            "BRV-MF-0001",
            &p.name,
            &file,
            "plugin.name",
            format!("plugin name `{}` violates the name contract", p.name),
            "contract: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)",
        );
    }
    p.plugin_type =
        req_str(plugin, "plugin_type", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.plugin_type.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "plugin_type", &p.plugin_type, rules::PLUGIN_TYPES);
    }
    p.api_type = req_str(plugin, "api_type", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.api_type.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "api_type", &p.api_type, rules::API_TYPES);
    }
    p.subkind = opt_str(plugin, "subkind", &p.name, &file, "plugin", d);
    if let Some(sk) = &p.subkind {
        check_enum(d, &p.name, &file, "plugin", "subkind", sk, rules::SUBKINDS);
    }
    p.symbol_prefix = opt_str(plugin, "symbol_prefix", &p.name, &file, "plugin", d);
    if let Some(sp) = &p.symbol_prefix {
        if sp.is_empty() || !sp.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                "plugin.symbol_prefix",
                format!("`symbol_prefix` allows only [A-Za-z0-9_] and must be non-empty (got `{sp}`)"),
                "it becomes the C symbol prefix (`<prefix>early_init` etc.), so it must be an identifier fragment",
            );
        }
    }
    p.lang = req_str(plugin, "lang", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.lang.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "lang", &p.lang, rules::LANGS);
    }
    p.phase = req_str(plugin, "phase", &p.name, &file, "plugin", d).unwrap_or_default();
    p.sched_class = opt_str(plugin, "sched_class", &p.name, &file, "plugin", d)
        .unwrap_or_else(|| "SAFE_PREEMPT".to_string());
    // `[selftest]` present = has selftests; `cases` is a self-reported count (presentation only).
    if let Some(st) = opt_table(top, "selftest") {
        p.has_selftest = true;
        p.selftest_cases = opt_int(st, "cases", &p.name, &file, "selftest", d);
    }
    if !p.sched_class.is_empty() {
        check_enum(d, &p.name, &file, "plugin", "sched_class", &p.sched_class, rules::SCHED_CLASSES);
    }
    p.sched_kind = opt_str(plugin, "sched_kind", &p.name, &file, "plugin", d);
    if let Some(k) = &p.sched_kind {
        check_enum(d, &p.name, &file, "plugin", "sched_kind", k, rules::SCHED_KINDS);
        if p.subkind.as_deref() != Some("scheduler") {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                "plugin.sched_kind",
                "`sched_kind` is only meaningful for `subkind = \"scheduler\"`",
                "contract §7.1 / ruling R-5",
            );
        }
    }
    p.version_raw = req_str(plugin, "version", &p.name, &file, "plugin", d).unwrap_or_default();
    if !p.version_raw.is_empty() {
        if let Some(v) = parse_version_field(&p.version_raw, &p.name, &file, "plugin.version", d) {
            p.version = v;
        }
    }
    p.summary = opt_str(plugin, "summary", &p.name, &file, "plugin", d);
    p.license = opt_str(plugin, "license", &p.name, &file, "plugin", d);

    // subkind derivation and conflicts (BRV-TAX-0015)
    validate_subkind(&mut p, d);

    if !p.phase.is_empty() && !p.plugin_type.is_empty() {
        if let Some(reason) = rules::phase_reason(&p.plugin_type, p.subkind.as_deref(), &p.phase) {
            d.shape("BRV-MF-0001", &p.name, &file, "plugin.phase", reason, "§7.2");
        }
    }

    for (i, t) in array_of_tables(top, "res").into_iter().enumerate() {
        let span = format!("res[{i}]");
        let kind = req_str(t, "kind", &p.name, &file, &span, d).unwrap_or_default();
        if !kind.is_empty() {
            check_enum(d, &p.name, &file, &span, "kind", &kind, rules::RES_KINDS);
        }
        let used = req_int(t, "used_kib", &p.name, &file, &span, d).unwrap_or(0);
        let notes = opt_str(t, "notes", &p.name, &file, &span, d);
        p.res.push(Res {
            kind,
            used_kib: used,
            notes,
        });
    }

    if let Some(tt) = opt_sub_table(top, &["sched", "tt"]) {
        p.sched_tt = Some(SchedTt {
            period_us: req_int(tt, "period_us", &p.name, &file, "sched.tt", d).unwrap_or(0),
            deadline_us: req_int(tt, "deadline_us", &p.name, &file, "sched.tt", d).unwrap_or(0),
            wcet_us: req_int(tt, "wcet_us", &p.name, &file, "sched.tt", d).unwrap_or(0),
        });
    } else if p.sched_class == "TT_SAFE" {
        d.shape(
            "BRV-MF-0001",
            &p.name,
            &file,
            "sched.tt",
            "`sched_class = \"TT_SAFE\"` requires `[sched.tt]` period metadata",
            "§8.1",
        );
    }

    load_compat(top, &mut p, d);

    if let Some(td) = opt_sub_table(top, &["iface", "typedefs"]) {
        for (k, v) in td {
            match v.as_str() {
                Some(s) => p.typedefs.push((k.clone(), s.to_string())),
                None => d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    format!("iface.typedefs.{k}"),
                    format!("target of type alias `{k}` must be a string (canonical name)"),
                    "contract §7.1 / IFACE-IR rule 4",
                ),
            }
        }
        p.typedefs.sort();
    }

    for (i, t) in array_of_tables(top, "dep").into_iter().enumerate() {
        let span = format!("dep[{i}]");
        let name = req_str(t, "name", &p.name, &file, &span, d).unwrap_or_default();
        if !name.is_empty() && !rules::valid_plugin_name(&name) {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                format!("{span}.name"),
                format!("dependency name `{name}` violates the name contract"),
                "contract: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)",
            );
        }
        if t.contains_key("compat_gen") {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                format!("{span}.compat_gen"),
                "structural dependencies (`[[dep]]`) never pin `compat_gen` (per F3)",
                "§7.1: pinning a generation belongs to interface deps (`requires_iface`) only",
            );
        }
        let range_raw = opt_str(t, "range", &p.name, &file, &span, d).unwrap_or_else(|| "*".to_string());
        let range = parse_range_field(&range_raw, &p.name, &file, format!("{span}.range").as_str(), d)
            .unwrap_or_else(Range::any);
        let kind = req_str(t, "kind", &p.name, &file, &span, d).unwrap_or_default();
        if !kind.is_empty() {
            check_enum(d, &p.name, &file, &span, "kind", &kind, rules::DEP_KINDS);
        }
        let phase = opt_str(t, "phase", &p.name, &file, &span, d);
        if let Some(ph) = &phase {
            if !rules::in_list(rules::PHASES, ph) {
                shape_enum(d, &p.name, &file, &span, "phase", ph, rules::PHASES);
            }
            if kind != "init" {
                d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    format!("{span}.phase"),
                    "`[[dep]].phase` is only meaningful for `kind = \"init\"`",
                    "§7.2: the assertion describes only the init completion point",
                );
            }
        }
        let symbol = opt_str(t, "symbol", &p.name, &file, &span, d);
        if symbol.is_some() && kind != "runtime" {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &file,
                format!("{span}.symbol"),
                "`[[dep]].symbol` is only meaningful for `kind = \"runtime\"`",
                "contract §7.1",
            );
        }
        p.deps.push(Dep {
            name,
            range,
            range_raw,
            kind,
            phase,
            symbol,
        });
    }

    for (i, t) in array_of_tables(top, "export").into_iter().enumerate() {
        let span = format!("export[{i}]");
        let mut e = Export::default();
        e.api_iface = req_str(t, "api_iface", &p.name, &file, &span, d).unwrap_or_default();
        if !e.api_iface.is_empty() {
            check_enum(d, &p.name, &file, &span, "api_iface", &e.api_iface, rules::API_TYPES);
        }
        e.form = req_str(t, "form", &p.name, &file, &span, d).unwrap_or_default();
        if !e.form.is_empty() {
            check_enum(d, &p.name, &file, &span, "form", &e.form, rules::EXPORT_FORMS);
        }
        e.name = req_str(t, "name", &p.name, &file, &span, d).unwrap_or_default();
        let vraw = req_str(t, "version", &p.name, &file, &span, d);
        e.version_raw = vraw.clone();
        if let Some(raw) = &vraw {
            e.version = parse_version_field(raw, &p.name, &file, format!("{span}.version").as_str(), d);
        }
        e.compat_gen = req_int(t, "compat_gen", &p.name, &file, &span, d).map(|v| v.max(0) as u64);
        e.freeze_state = req_str(t, "freeze_state", &p.name, &file, &span, d);
        if let Some(fs) = &e.freeze_state {
            check_enum(d, &p.name, &file, &span, "freeze_state", fs, rules::FREEZE_STATES);
        }
        e.hash = opt_str(t, "hash", &p.name, &file, &span, d);
        e.status = req_str(t, "status", &p.name, &file, &span, d);
        if let Some(st) = &e.status {
            check_enum(d, &p.name, &file, &span, "status", st, rules::ENTRY_STATUSES);
        }
        e.reexport_of = str_array(t, "reexport_of", &p.name, &file, &span, d);
        e.symbols = str_array(t, "symbols", &p.name, &file, &span, d);
        for (j, et) in array_of_tables(t, "entries").into_iter().enumerate() {
            let espan = format!("{span}.entries[{j}]");
            let mut en = ExportEntry::default();
            en.kind = req_str(et, "kind", &p.name, &file, &espan, d).unwrap_or_default();
            if !en.kind.is_empty() {
                check_enum(d, &p.name, &file, &espan, "kind", &en.kind, rules::ENTRY_KINDS);
            }
            en.name = req_str(et, "name", &p.name, &file, &espan, d).unwrap_or_default();
            en.sig = opt_str(et, "sig", &p.name, &file, &espan, d);
            en.layout = opt_str(et, "layout", &p.name, &file, &espan, d);
            en.value = load_entry_value(et, &p.name, &file, &espan, d);
            en.ops = et.get("ops").map(|_| str_array(et, "ops", &p.name, &file, &espan, d));
            en.status = opt_str(et, "status", &p.name, &file, &espan, d)
                .unwrap_or_else(|| "experimental".to_string());
            if !rules::in_list(rules::ENTRY_STATUSES, &en.status) {
                shape_enum(d, &p.name, &file, &espan, "status", &en.status, rules::ENTRY_STATUSES);
            }
            e.entries.push(en);
        }
        p.exports.push(e);
    }

    check_export_invariants(&p, d);

    if let Some(priv_t) = opt_table(top, "privileged") {
        let level = opt_str(priv_t, "level", &p.name, &file, "privileged", d);
        if let Some(l) = &level {
            if !rules::in_list(rules::PRIV_LEVELS, l) {
                shape_enum(d, &p.name, &file, "privileged", "level", l, rules::PRIV_LEVELS);
            }
        }
        let mut mem = Vec::new();
        for (i, mt) in array_of_tables(priv_t, "memory").into_iter().enumerate() {
            let span = format!("privileged.memory[{i}]");
            let granularity = req_str(mt, "granularity", &p.name, &file, &span, d).unwrap_or_default();
            // `ops` is required (an empty table is handled by BRV-PRIV-0002).
            if !mt.contains_key("ops") {
                d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    format!("{span}.ops"),
                    "missing required field `ops`",
                    "contract §7.1 / schema/plugin.schema.json",
                );
            }
            mem.push(rules::MemoryEntry {
                granularity,
                ops: str_array(mt, "ops", &p.name, &file, &span, d),
                regions: str_array(mt, "regions", &p.name, &file, &span, d),
            });
        }
        // `[[privileged.resources]]` is an array of tables; entries accumulate per field.
        let mut resources = PrivilegedResources::default();
        for rt in array_of_tables(priv_t, "resources") {
            resources
                .irq
                .extend(int_array(rt, "irq", &p.name, &file, "privileged.resources", d));
            resources.dma_channels.extend(int_array(
                rt,
                "dma_channels",
                &p.name,
                &file,
                "privileged.resources",
                d,
            ));
            resources
                .pins
                .extend(int_array(rt, "pins", &p.name, &file, "privileged.resources", d));
            resources.device_names.extend(str_array(
                rt,
                "device_names",
                &p.name,
                &file,
                "privileged.resources",
                d,
            ));
        }
        let has_resources = !resources.is_empty();
        let input = rules::PrivInput {
            plugin_type: &p.plugin_type,
            subkind: p.subkind.as_deref(),
            target: &p.name,
            file: &file,
            level: level.as_deref(),
            memory: &mem,
            has_resources,
        };
        for diag in rules::check_privileged(&input) {
            d.push(diag);
        }
        p.privileged = Some(PrivilegedDecl {
            level,
            memory: mem,
            resources,
        });
    }

    p.mounts = parse_mounts(top, &p.name, &file, &p.plugin_type, d);

    if let Some(bt) = opt_table(top, "build") {
        p.build = Some(BuildDecl {
            sources: str_array(bt, "sources", &p.name, &file, "build", d),
            includes: str_array(bt, "includes", &p.name, &file, "build", d),
            defines: str_array(bt, "defines", &p.name, &file, "build", d),
            cflags: str_array(bt, "cflags", &p.name, &file, "build", d),
        });
        if let Some(tt) = opt_table(bt, "target") {
            if p.plugin_type != "platform" {
                d.shape(
                    "BRV-MF-0001",
                    &p.name,
                    &file,
                    "build.target",
                    format!(
                        "`[build.target]` may only be declared by `plugin_type = \"platform\"`, this plugin is `{}`",
                        p.plugin_type
                    ),
                    "target facts are part of \"what the platform is\"; exactly one platform per image (§8.3 ④)",
                );
            }
            let qemu = opt_table(tt, "qemu");
            p.build_target = Some(BuildTarget {
                arch: opt_str(tt, "arch", &p.name, &file, "build.target", d).unwrap_or_default(),
                cross: opt_str(tt, "cross", &p.name, &file, "build.target", d).unwrap_or_default(),
                arch_flags: str_array(tt, "arch_flags", &p.name, &file, "build.target", d),
                linker_script: opt_str(tt, "linker_script", &p.name, &file, "build.target", d)
                    .unwrap_or_default(),
                qemu_binary: qemu
                    .and_then(|q| opt_str(q, "binary", &p.name, &file, "build.target.qemu", d))
                    .unwrap_or_default(),
                qemu_machine: qemu
                    .and_then(|q| opt_str(q, "machine", &p.name, &file, "build.target.qemu", d))
                    .unwrap_or_default(),
                qemu_cpu: qemu
                    .and_then(|q| opt_str(q, "cpu", &p.name, &file, "build.target.qemu", d))
                    .unwrap_or_default(),
                qemu_memory: qemu
                    .and_then(|q| opt_str(q, "memory", &p.name, &file, "build.target.qemu", d))
                    .unwrap_or_default(),
                qemu_extra: qemu
                    .map(|q| str_array(q, "extra", &p.name, &file, "build.target.qemu", d))
                    .unwrap_or_default(),
            });
        }
    }

    Some(p)
}

/// `subkind` derivation and conflicts (§8.4 / `BRV-TAX-0015`; error ⇒ exit code 1).
fn validate_subkind(p: &mut Plugin, d: &mut Diags) {
    if p.plugin_type.is_empty() {
        return;
    }
    if p.plugin_type != "ability" {
        if p.subkind.is_some() {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &p.file,
                "plugin.subkind",
                format!("`subkind` is only meaningful for `plugin_type = \"ability\"`, `{}` does not accept it", p.plugin_type),
                "§8.1",
            );
        }
        return;
    }
    let derived = rules::derive_subkind(&p.name);
    match (derived, &p.subkind) {
        (Some(der), Some(explicit)) => {
            if der != explicit {
                d.coded(
                    "BRV-TAX-0015",
                    Severity::Error,
                    &p.name,
                    &p.file,
                    "plugin.subkind",
                    format!(
                        "subkind derivation conflicts with the explicit declaration: derived from name namespace = `{der}`, explicit = `{explicit}`"
                    ),
                    "drop the explicit `subkind`, or use a name matching its namespace (§8.4)",
                );
            }
        }
        (Some(der), None) => {
            // Derivable from the name; not written back to the file.
            p.subkind = Some(der.to_string());
        }
        (None, None) => {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &p.file,
                "plugin.subkind",
                format!(
                    "ability plugin `{}` has no name-derivable `subkind`; it must be declared explicitly",
                    p.name
                ),
                format!("allowed values: {}; or use a derivable name (service/ sched/ framework/ io/ fs/)", rules::list_of(rules::SUBKINDS)),
            );
        }
        (None, Some(_)) => {}
    }
}

/// Export-surface taxonomy invariants (`BRV-TAX-0016/0017/0018/0019`); invariant 3 is cross-file.
fn check_export_invariants(p: &Plugin, d: &mut Diags) {
    let views: Vec<rules::ExportView<'_>> = p
        .exports
        .iter()
        .map(|e| rules::ExportView {
            api_iface: &e.api_iface,
            form: &e.form,
            reexport_of: &e.reexport_of,
            symbols: &e.symbols,
        })
        .collect();
    for (i, code, msg) in rules::check_export_invariants(&p.api_type, &views) {
        d.coded(
            code,
            Severity::Error,
            &p.name,
            &p.file,
            format!("export[{i}]"),
            msg,
            "§3.5 export-surface taxonomy invariants (V-12)",
        );
    }
}

fn load_compat(top: &toml::Table, p: &mut Plugin, d: &mut Diags) {
    let Some(c) = opt_table(top, "compat") else {
        return;
    };
    if c.contains_key("core") {
        if let Some(raw) = opt_str(c, "core", &p.name, &p.file, "compat", d) {
            p.compat.core = parse_range_field(&raw, &p.name, &p.file, "compat.core", d);
            p.compat.core_raw = Some(raw);
        }
    } else {
        d.shape(
            "BRV-MF-0001",
            &p.name,
            &p.file,
            "compat.core",
            "missing required field `core` (required when `[compat]` exists)",
            "contract §7.1 / schema/plugin.schema.json",
        );
    }
    p.compat.api_rev = opt_int(c, "api_rev", &p.name, &p.file, "compat", d);
    p.compat.hash_scope = opt_str(c, "hash_scope", &p.name, &p.file, "compat", d);
    p.compat.truth = opt_str(c, "truth", &p.name, &p.file, "compat", d);
    p.compat.abi_id = opt_str(c, "abi_id", &p.name, &p.file, "compat", d);

    // v0.1 allows only hash_scope/truth both = "decl" (BRV-IFACE-0011).
    let hs = p.compat.hash_scope.clone().unwrap_or_else(|| "decl".to_string());
    let tr = p.compat.truth.clone().unwrap_or_else(|| "decl".to_string());
    if hs != "decl" || tr != "decl" {
        d.coded(
            "BRV-IFACE-0011",
            Severity::Error,
            &p.name,
            &p.file,
            "compat",
            format!(
                "`truth`/`hash_scope` must be paired, and v0.1 allows `decl` only: hash_scope=`{hs}`, truth=`{tr}`"
            ),
            "§6.6/§10 V-11②: sym/header are v0.x; no silent migration",
        );
    }

    for (i, t) in array_of_tables(c, "requires_iface").into_iter().enumerate() {
        let span = format!("compat.requires_iface[{i}]");
        let id = req_str(t, "id", &p.name, &p.file, &span, d).unwrap_or_default();
        if !id.is_empty() && !id.contains('#') {
            d.shape(
                "BRV-MF-0001",
                &p.name,
                &p.file,
                format!("{span}.id"),
                format!("interface unit id `{id}` has wrong shape: expected `<provider>#<unit>`"),
                "§6.1",
            );
        }
        let api_iface = req_str(t, "api_iface", &p.name, &p.file, &span, d).unwrap_or_default();
        if !api_iface.is_empty() {
            check_enum(d, &p.name, &p.file, &span, "api_iface", &api_iface, rules::API_TYPES);
        }
        // compat_gen and range are separate required fields.
        let compat_gen = req_int(t, "compat_gen", &p.name, &p.file, &span, d)
            .map(|v| v.max(0) as u64)
            .unwrap_or(0);
        let range_raw = req_str(t, "range", &p.name, &p.file, &span, d).unwrap_or_default();
        let range = if range_raw.is_empty() {
            Range::any()
        } else {
            parse_range_field(&range_raw, &p.name, &p.file, format!("{span}.range").as_str(), d)
                .unwrap_or_else(Range::any)
        };
        let mode = opt_str(t, "mode", &p.name, &p.file, &span, d)
            .unwrap_or_else(|| "decl".to_string());
        if !rules::in_list(&["decl", "sym"], &mode) {
            shape_enum(d, &p.name, &p.file, &span, "mode", &mode, &["decl", "sym"]);
        }
        p.compat.requires_iface.push(RequiresIface {
            id,
            api_iface,
            compat_gen,
            range,
            range_raw,
            mode,
        });
    }

    // Taxonomy compatibility: info only in v0.1 (V-10).
    for r in &p.compat.requires_iface {
        if p.api_type == "native" && r.api_iface == "runtime_adapter" {
            d.coded(
                "BRV-TAX-0016",
                Severity::Info,
                &p.name,
                &p.file,
                "compat.requires_iface",
                format!(
                    "`native` plugin references a `runtime_adapter` unit `{}` (v0.1 does not scan iface deps, info only; red from v0.2)",
                    r.id
                ),
                "§3.5 consumer 2 / V-10③",
            );
        }
    }
}

/// Load `root/product.toml` (optional). Returns `(file present, model)`.
pub fn load_product(root: &Path, d: &mut Diags) -> (bool, Option<Product>) {
    let abs = root.join("product.toml");
    if !abs.is_file() {
        return (false, None);
    }
    let file = "product.toml".to_string();
    let text = match fs::read_to_string(&abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("cannot read `{file}`: {e}"), "check path and permissions");
            return (true, None);
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(format!("`{file}` is not valid TOML: {e}"), "BRV-D9 exit code 2");
            return (true, None);
        }
    };
    let Some(top) = value.as_table() else {
        d.env(format!("top level of `{file}` must be a TOML table"), "contract §7.2");
        return (true, None);
    };
    let mut prod = Product {
        file: file.clone(),
        stage: "dev".to_string(),
        lint_frozen_deps: "inherit".to_string(),
        ..Default::default()
    };
    match req_int(top, "schema", &file, &file, "", d) {
        Some(1) => prod.schema = 1,
        Some(other) => d.shape(
            "BRV-MF-0001",
            &file,
            &file,
            "schema",
            format!("`schema` supports only 1, got {other}"),
            "contract §7.2",
        ),
        None => {}
    }

    let Some(product) = opt_table(top, "product") else {
        d.shape(
            "BRV-MF-0001",
            &file,
            &file,
            "product",
            "missing required table `[product]`",
            "contract §7.2",
        );
        return (true, Some(prod));
    };
    prod.name = req_str(product, "name", &file, &file, "product", d).unwrap_or_default();
    // `version` is not required inside `[product]`; default 0.0.0.0.
    if let Some(vraw) = opt_str(product, "version", &file, &file, "product", d) {
        if let Some(v) = parse_version_field(&vraw, &file, &file, "product.version", d) {
            prod.version = v;
        }
    }
    prod.app = req_str(product, "app", &file, &file, "product", d).unwrap_or_default();
    if !prod.app.is_empty() && !rules::valid_plugin_name(&prod.app) {
        d.shape(
            "BRV-MF-0001",
            &file,
            &file,
            "product.app",
            format!("`app` name `{}` violates the name contract", prod.app),
            "§8.3",
        );
    }
    if let Some(core) = opt_str(product, "core", &file, &file, "product", d) {
        prod.core = parse_range_field(&core, &file, &file, "product.core", d);
    }
    prod.stage = opt_str(product, "stage", &file, &file, "product", d)
        .unwrap_or_else(|| "dev".to_string());
    if !rules::in_list(&["dev", "release"], &prod.stage) {
        shape_enum(d, &file, &file, "product", "stage", &prod.stage, &["dev", "release"]);
    }

    if let Some(sel) = opt_table(top, "select") {
        prod.select = str_array(sel, "plugins", &file, &file, "select", d);
    }
    if let Some(st) = opt_table(top, "selftest") {
        prod.has_selftest_table = true;
        match st.get("enabled") {
            None => {}
            Some(toml::Value::Boolean(v)) => prod.selftest_enabled = Some(*v),
            Some(toml::Value::String(v)) if v == "inherit" => prod.selftest_enabled = None,
            Some(other) => d.shape(
                "BRV-MF-0001",
                &file,
                &file,
                "selftest.enabled",
                format!(
                    "`enabled` must be a boolean or the string \"inherit\", got {}",
                    other.type_str()
                ),
                "ADR-0010 §2.3: inherit = follow [product].stage (dev on / release off)",
            ),
        }
        prod.selftest_plugins = str_array(st, "plugins", &file, &file, "selftest", d);
    }
    if let Some(b) = opt_table(top, "budget") {
        prod.budget_ram_kib = opt_int(b, "ram_kib", &file, &file, "budget", d);
        prod.budget_stack_kib = opt_int(b, "stack_kib", &file, &file, "budget", d);
    }
    if let Some(k) = opt_table(top, "kernel") {
        prod.kernel_hz = opt_int(k, "hz", &file, &file, "kernel", d);
    }
    if let Some(l) = opt_table(top, "lint") {
        prod.lint_frozen_deps = opt_str(l, "frozen_deps", &file, &file, "lint", d)
            .unwrap_or_else(|| "inherit".to_string());
        if !rules::in_list(&["inherit", "allow", "deny"], &prod.lint_frozen_deps) {
            shape_enum(
                d,
                &file,
                &file,
                "lint",
                "frozen_deps",
                &prod.lint_frozen_deps,
                &["inherit", "allow", "deny"],
            );
        }
        if let Some(arr) = l.get("allow_edges").and_then(|v| v.as_array()) {
            for (i, item) in arr.iter().enumerate() {
                let pair = item.as_array();
                let ok = match pair {
                    Some(pair) if pair.len() == 2 => {
                        let a = pair[0].as_str();
                        let b = pair[1].as_str();
                        if let (Some(a), Some(b)) = (a, b) {
                            prod.allow_edges.push((a.to_string(), b.to_string()));
                            true
                        } else {
                            false
                        }
                    }
                    _ => false,
                };
                if !ok {
                    d.shape(
                        "BRV-MF-0001",
                        &file,
                        &file,
                        format!("lint.allow_edges[{i}]"),
                        "each `allow_edges` entry must be two strings `[<consumer>, <provider>]`",
                        "§7.3 / §8.2",
                    );
                }
            }
        }
    }

    if let Some(b) = opt_table(top, "build") {
        let rel = opt_table(b, "release");
        prod.build = Some(ProductBuild {
            gen_sources: str_array(b, "gen_sources", &file, &file, "build", d),
            selftest_sources: str_array(b, "selftest_sources", &file, &file, "build", d),
            cflags: str_array(b, "cflags", &file, &file, "build", d),
            asflags: str_array(b, "asflags", &file, &file, "build", d),
            ldflags: str_array(b, "ldflags", &file, &file, "build", d),
            release_cflags_extra: rel
                .map(|r| str_array(r, "cflags_extra", &file, &file, "build.release", d))
                .unwrap_or_default(),
            release_asflags_extra: rel
                .map(|r| str_array(r, "asflags_extra", &file, &file, "build.release", d))
                .unwrap_or_default(),
            release_ldflags_extra: rel
                .map(|r| str_array(r, "ldflags_extra", &file, &file, "build.release", d))
                .unwrap_or_default(),
            obj_dir: opt_str(b, "obj_dir", &file, &file, "build", d),
        });
    }

    (true, Some(prod))
}

fn scan_plugin_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    let mut items: Vec<(String, PathBuf, bool)> = Vec::new();
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
        items.push((name, e.path(), is_dir));
    }
    items.sort_by(|a, b| a.0.cmp(&b.0));
    for (name, path, is_dir) in items {
        if is_dir {
            if name.starts_with('.') || rules::in_list(SKIP_DIRS, &name) {
                continue;
            }
            scan_plugin_files(&path, out);
        } else if name == "brick.toml" {
            out.push(path);
        }
    }
}

/// Transitive closure of a plugin's declared dependencies (by name; excludes itself).
pub fn dep_closure(tree: &TreeLoad, name: &str) -> std::collections::BTreeSet<String> {
    let mut seen: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();
    let mut stack: Vec<String> = vec![name.to_string()];
    while let Some(cur) = stack.pop() {
        let Some(p) = tree.by_name(&cur) else { continue };
        for d in &p.deps {
            if seen.insert(d.name.clone()) {
                stack.push(d.name.clone());
            }
        }
    }
    seen
}

/// One cross-plugin `#include` (a header owned by another selected plugin, used in consumer source).
#[derive(Clone, Debug)]
pub struct IncludeUse {
    pub consumer: String,
    pub provider: String,
    pub header: String,
    /// Source file path relative to `root` (diagnostic location).
    pub file: String,
}

/// Scan `#include`s in selected plugins' sources (`src/` + `include/`) and map each to the header's
/// owner across the whole tree. Narrowing `-I` is not enough: a common name like `<unistd.h>` is
/// silently satisfied by the toolchain's glibc headers. Limitation: comments are not parsed.
pub fn cross_plugin_includes(
    root: &Path,
    tree: &TreeLoad,
    selected: &std::collections::BTreeSet<String>,
) -> Vec<IncludeUse> {
    // (include dir, owning plugin) for the whole tree: header ownership is closure-independent.
    let mut inc_owner: Vec<(String, String)> = Vec::new();
    for p in &tree.plugins {
        if let Some(b) = &p.build {
            for inc in &b.includes {
                inc_owner.push((format!("{}/{}", p.dir, inc), p.name.clone()));
            }
        }
    }

    let mut out: Vec<IncludeUse> = Vec::new();
    for p in &tree.plugins {
        if !selected.contains(&p.name) {
            continue;
        }
        for f in plugin_source_files(root, p) {
            let Ok(text) = fs::read_to_string(root.join(&f)) else { continue };
            for h in scan_include_names(&text) {
                // core headers: core is not a plugin, so record synthetic provider `"core"`;
                // the caller judges by consumer class (direct core includes are forbidden, A-2).
                if h.starts_with("br/core/") {
                    out.push(IncludeUse {
                        consumer: p.name.clone(),
                        provider: "core".to_string(),
                        header: h.clone(),
                        file: f.clone(),
                    });
                    continue;
                }
                for (dir, owner) in &inc_owner {
                    if owner == &p.name {
                        continue; // own header
                    }
                    if root.join(dir).join(&h).is_file() {
                        out.push(IncludeUse {
                            consumer: p.name.clone(),
                            provider: owner.clone(),
                            header: h.clone(),
                            file: f.clone(),
                        });
                    }
                }
            }
        }
    }
    out
}

/// Plugin source files (`.c/.h/.S` under `src/**` and `include/**`), relative to `root`.
fn plugin_source_files(root: &Path, p: &Plugin) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for base in ["src", "include"] {
        collect_source_files(&root.join(&p.dir).join(base), root, &mut out);
    }
    out.sort();
    out.dedup();
    out
}

fn collect_source_files(dir: &Path, root: &Path, out: &mut Vec<String>) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    let mut items: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
    items.sort();
    for path in items {
        if path.is_dir() {
            collect_source_files(&path, root, out);
        } else {
            let ext = path.extension().and_then(|s| s.to_str()).unwrap_or("");
            if matches!(ext, "c" | "h" | "S") {
                if let Ok(rel) = path.strip_prefix(root) {
                    out.push(rel.to_string_lossy().replace('\\', "/"));
                }
            }
        }
    }
}

/// Extract header names from `#include <…>` / `#include "…"` in a piece of C source.
fn scan_include_names(text: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for line in text.lines() {
        let l = line.trim_start();
        let Some(rest) = l.strip_prefix('#') else { continue };
        let rest = rest.trim_start();
        let Some(rest) = rest.strip_prefix("include") else { continue };
        let rest = rest.trim_start();
        let close = if rest.starts_with('<') {
            '>'
        } else if rest.starts_with('"') {
            '"'
        } else {
            continue;
        };
        if let Some(end) = rest[1..].find(close) {
            out.push(rest[1..1 + end].to_string());
        }
    }
    out
}

/// Load the whole plugin tree (ruling R-1: validate the whole tree when there is no `product.toml`).
pub fn load_tree(root: &Path) -> TreeLoad {
    let mut load = TreeLoad::default();
    let mut files = Vec::new();
    scan_plugin_files(root, &mut files);
    files.sort();
    for f in &files {
        if let Some(p) = load_plugin_file(root, f, &mut load.diags) {
            load.plugins.push(p);
        }
    }
    // Globally unique names (BRV-D4 single source of truth / §8.3): duplicates ⇒ shape error.
    let mut seen: BTreeMap<String, String> = BTreeMap::new();
    let mut dups: Vec<(String, String, String)> = Vec::new();
    for p in &load.plugins {
        if p.name.is_empty() {
            continue;
        }
        if let Some(first) = seen.get(&p.name) {
            dups.push((p.name.clone(), first.clone(), p.file.clone()));
        } else {
            seen.insert(p.name.clone(), p.file.clone());
        }
    }
    for (name, first, second) in dups {
        load.diags.shape(
            "BRV-MF-0001",
            &name,
            &second,
            "plugin.name",
            format!("plugin name `{name}` is duplicated in the tree: {first} and {second}"),
            "§8.3: plugin names are globally unique",
        );
    }

    let (has_product, product) = load_product(root, &mut load.diags);
    load.has_product = has_product;
    load.product = product;
    load.plugins.sort_by(|a, b| a.name.cmp(&b.name));
    load
}

/// One interface-surface snapshot entry (read side; same shape as [`ExportEntry`]).
#[derive(Clone, Debug, Default)]
pub struct SnapshotEntry {
    pub kind: String,
    pub name: String,
    pub sig: Option<String>,
    pub layout: Option<String>,
    pub value: Option<String>,
    pub ops: Option<Vec<String>>,
    pub status: String,
}

/// A snapshot of `api/iface/<provider>/<unit>.toml` (**read-only**).
#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    /// Relative to `root`.
    pub file: String,
    /// `<provider>#<unit>`.
    pub id: String,
    pub provider: String,
    pub unit: String,
    pub api_iface: Option<String>,
    pub form: Option<String>,
    pub version: Option<Version>,
    pub compat_gen: Option<u64>,
    pub freeze_state: Option<String>,
    pub status: Option<String>,
    pub hash: Option<String>,
    pub hash_scope: Option<String>,
    pub truth: Option<String>,
    pub not_abi: bool,
    /// `strict_params` (IFACE-IR rule 10: must be stored in the snapshot, else cross-env hash jitter).
    pub strict_params: Option<bool>,
    /// Baseline of the unfreeze window (ruling R-7): written on `unfreeze`, compared on `refreeze`.
    pub baseline_hash: Option<String>,
    pub baseline_entries: Vec<SnapshotEntry>,
    pub entries: Vec<SnapshotEntry>,
}

/// Read all snapshots under `api/iface/**` (unreadable ⇒ empty = "unpublished").
pub fn load_snapshots(root: &Path, d: &mut Diags) -> Vec<Snapshot> {
    let base = root.join("api").join("iface");
    if !base.is_dir() {
        return Vec::new();
    }
    let mut files = Vec::new();
    collect_files(&base, &mut files);
    files.sort();
    let mut out = Vec::new();
    for f in files {
        if f.extension().and_then(|e| e.to_str()) != Some("toml") {
            continue;
        }
        let file = rel_path(root, &f);
        let text = match fs::read_to_string(&f) {
            Ok(t) => t,
            Err(e) => {
                d.env(format!("cannot read snapshot `{file}`: {e}"), "api/iface/** is read-only");
                continue;
            }
        };
        let value: toml::Value = match text.parse() {
            Ok(v) => v,
            Err(e) => {
                d.env(format!("snapshot `{file}` is not valid TOML: {e}"), "written by agent-B; read-only here");
                continue;
            }
        };
        let Some(top) = value.as_table() else { continue };
        out.push(snapshot_from_table(&file, &base, top));
    }
    out
}

fn collect_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    for e in entries.flatten() {
        let path = e.path();
        let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
        if is_dir {
            collect_files(&path, out);
        } else {
            out.push(path);
        }
    }
}

fn snapshot_from_table(file: &str, _base: &Path, top: &toml::Table) -> Snapshot {
    let unit = std::path::Path::new(file)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    // provider = path segment between api/iface and the file name
    let provider = {
        let p = std::path::Path::new(file);
        let mut comps: Vec<String> = p
            .components()
            .map(|c| c.as_os_str().to_string_lossy().to_string())
            .collect();
        // drop "api" / "iface" segments and the file name
        if comps.len() >= 3 {
            comps.drain(0..2);
            comps.pop();
            comps.join("/")
        } else {
            String::new()
        }
    };

    // Fields may be flat or inside the `[unit_meta]` sub-table (B's shape is TBD ⇒ read both).
    let meta = opt_table(top, "unit_meta");
    let get_str = |key: &str| -> Option<String> {
        top.get(key)
            .and_then(|v| v.as_str())
            .map(str::to_string)
            .or_else(|| meta.and_then(|m| m.get(key)).and_then(|v| v.as_str()).map(str::to_string))
    };
    let get_int = |key: &str| -> Option<i64> {
        top.get(key)
            .and_then(|v| v.as_integer())
            .or_else(|| meta.and_then(|m| m.get(key)).and_then(|v| v.as_integer()))
    };

    let version = get_str("version").and_then(|s| version::parse_version(&s).ok());
    let entries_src = if top.contains_key("entry") {
        array_of_tables(top, "entry")
    } else if top.contains_key("entries") {
        array_of_tables(top, "entries")
    } else if let Some(e) = opt_table(top, "export") {
        array_of_tables(e, "entries")
    } else {
        Vec::new()
    };
    let entries = entries_src
        .into_iter()
        .map(snapshot_entry_from_table)
        .collect();
    let baseline_entries = array_of_tables(top, "baseline_entry")
        .into_iter()
        .map(snapshot_entry_from_table)
        .collect();
    let strict_params = top
        .get("strict_params")
        .and_then(|v| v.as_bool())
        .or_else(|| meta.and_then(|m| m.get("strict_params")).and_then(|v| v.as_bool()));

    Snapshot {
        file: file.to_string(),
        id: if provider.is_empty() {
            unit.clone()
        } else {
            format!("{provider}#{unit}")
        },
        provider,
        unit,
        api_iface: get_str("api_iface"),
        form: get_str("form"),
        version,
        compat_gen: get_int("compat_gen").map(|v| v.max(0) as u64),
        freeze_state: get_str("freeze_state"),
        status: get_str("status"),
        hash: get_str("hash"),
        hash_scope: get_str("hash_scope"),
        truth: get_str("truth"),
        not_abi: top.contains_key("NOT_ABI")
            || meta.map(|m| m.contains_key("NOT_ABI")).unwrap_or(false),
        strict_params,
        baseline_hash: get_str("baseline_hash"),
        baseline_entries,
        entries,
    }
}

fn snapshot_entry_from_table(t: &toml::Table) -> SnapshotEntry {
    SnapshotEntry {
        kind: t.get("kind").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        name: t.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        sig: t.get("sig").and_then(|v| v.as_str()).map(str::to_string),
        layout: t.get("layout").and_then(|v| v.as_str()).map(str::to_string),
        value: t.get("value").and_then(|v| v.as_str()).map(str::to_string),
        ops: t
            .get("ops")
            .and_then(|v| v.as_array())
            .map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect()),
        status: t
            .get("status")
            .and_then(|v| v.as_str())
            .unwrap_or("experimental")
            .to_string(),
    }
}

impl Snapshot {
    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        let entries: Vec<_> = self
            .entries
            .iter()
            .map(|e| {
                json!({
                    "kind": e.kind, "name": e.name, "sig": e.sig, "layout": e.layout,
                    "value": e.value, "ops": e.ops, "status": e.status,
                })
            })
            .collect();
        json!({
            "file": self.file,
            "id": self.id,
            "provider": self.provider,
            "unit": self.unit,
            "api_iface": self.api_iface,
            "form": self.form,
            "version": self.version.map(|v| v.to_string()),
            "compat_gen": self.compat_gen,
            "freeze_state": self.freeze_state,
            "status": self.status,
            "hash": self.hash,
            "hash_scope": self.hash_scope,
            "truth": self.truth,
            "not_abi": self.not_abi,
            "strict_params": self.strict_params,
            "baseline_hash": self.baseline_hash,
            "entries": entries,
        })
    }
}

/// One plugin record of `brickie.lock` (`lock.plugins[]` in `schema/lock.schema.json`).
#[derive(Clone, Debug, Default)]
pub struct LockPlugin {
    pub name: String,
    pub version: String,
    pub compat_gen: u64,
}

/// One interface unit record of `brickie.lock` (`lock.units[]` in `schema/lock.schema.json`).
#[derive(Clone, Debug, Default)]
pub struct LockUnit {
    pub id: String,
    pub provider: String,
    pub unit: String,
    pub api_iface: String,
    pub version: String,
    pub compat_gen: u64,
    pub hash: String,
    pub hash_scope: String,
    pub truth: String,
}

/// `[lock.product]`: extension field; the lock schema allows additional properties.
#[derive(Clone, Debug, Default)]
pub struct LockProduct {
    pub name: String,
    pub version: String,
    pub stage: String,
}

/// Normalized model of `brickie.lock` (**read-only**; agent-B owns writing).
#[derive(Clone, Debug)]
pub struct Lock {
    pub schema: i64,
    pub profile: String,
    pub product: Option<LockProduct>,
    pub plugins: Vec<LockPlugin>,
    pub units: Vec<LockUnit>,
}

impl Default for Lock {
    fn default() -> Self {
        Lock {
            schema: 1,
            profile: "dev".to_string(),
            product: None,
            plugins: Vec::new(),
            units: Vec::new(),
        }
    }
}

/// Read `brickie.lock` (absent ⇒ `None` = unpublished); weak v0.1 validation, nothing required.
pub fn load_lock(root: &Path, d: &mut Diags) -> Option<Lock> {
    let abs = root.join("brickie.lock");
    if !abs.is_file() {
        return None;
    }
    let text = match fs::read_to_string(&abs) {
        Ok(t) => t,
        Err(e) => {
            d.env(format!("cannot read `brickie.lock`: {e}"), "lock file is read-only");
            return None;
        }
    };
    let value: toml::Value = match text.parse() {
        Ok(v) => v,
        Err(e) => {
            d.env(format!("`brickie.lock` is not valid TOML: {e}"), "written by agent-B; read-only here");
            return None;
        }
    };
    let Some(top) = value.as_table() else { return None };
    let mut lock = Lock {
        schema: top.get("schema").and_then(|v| v.as_integer()).unwrap_or(1),
        ..Default::default()
    };
    let Some(lk) = opt_table(top, "lock") else {
        // schema's `lock` is not required (v0.1 weak validation): schema without lock still counts.
        return Some(lock);
    };
    lock.profile = lk
        .get("profile")
        .and_then(|v| v.as_str())
        .unwrap_or("dev")
        .to_string();
    if let Some(p) = opt_table(lk, "product") {
        lock.product = Some(LockProduct {
            name: p.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            version: p.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            stage: p.get("stage").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        });
    }
    for t in array_of_tables(lk, "plugins") {
        lock.plugins.push(LockPlugin {
            name: t.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            version: t.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            compat_gen: t.get("compat_gen").and_then(|v| v.as_integer()).unwrap_or(0).max(0) as u64,
        });
    }
    for t in array_of_tables(lk, "units") {
        lock.units.push(LockUnit {
            id: t.get("id").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            provider: t.get("provider").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            unit: t.get("unit").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            api_iface: t.get("api_iface").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            version: t.get("version").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            compat_gen: t.get("compat_gen").and_then(|v| v.as_integer()).unwrap_or(0).max(0) as u64,
            hash: t.get("hash").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            hash_scope: t.get("hash_scope").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            truth: t.get("truth").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        });
    }
    Some(lock)
}

/// One reverse edge of `build/index/dependents.json`.
#[derive(Clone, Debug, Default)]
pub struct DependentEdge {
    pub from: String,
    pub kind: String,
}

/// Reverse dependency index (authoritative schema: `schema/index.schema.json`).
#[derive(Clone, Debug, Default)]
pub struct DependentsIndex {
    pub schema: i64,
    /// Dependee plugin → edges depending on it (keys sorted).
    pub dependents: BTreeMap<String, Vec<DependentEdge>>,
}

impl DependentsIndex {
    /// Rebuild fully from the plugin tree; edges come only from declared `[[dep]]` (runtime
    /// registry use creates no edge).
    pub fn build(tree: &TreeLoad) -> Self {
        let mut idx = DependentsIndex {
            schema: 1,
            dependents: BTreeMap::new(),
        };
        for p in &tree.plugins {
            for d in &p.deps {
                idx.dependents
                    .entry(d.name.clone())
                    .or_default()
                    .push(DependentEdge {
                        from: p.name.clone(),
                        kind: d.kind.clone(),
                    });
            }
        }
        for edges in idx.dependents.values_mut() {
            edges.sort_by(|a, b| (a.from.as_str(), a.kind.as_str()).cmp(&(b.from.as_str(), b.kind.as_str())));
        }
        idx
    }
}

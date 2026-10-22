# 0040 — 命名收敛：`platform` → `runtime`，`service/` + `runtime/` → `provider/`

> 状态: **已落地**（`brickie check` 0 错 0 警；`brickie build` 153 步含 `ar` × 28 + 符号审计全绿；
> `brickie test -j8` 全绿；`make tools-test` 731/731；`brickie-core --selftest` 586/586）
>
> 落点: `tools/brickie/rust/src/{rules.rs,model.rs,solver.rs,show.rs,audit.rs,build.rs,plan.rs,selftest.rs}`、
> `tools/brickie/schema/*.json`、`tools/brickie/templates/native/runtime/`、
> `runtime/qemu-aarch64/**`、`provider/**`、`api/iface/**`、`tests/gates.toml`、`product.toml`、全部文档
>
> 回灌: 设计 `1-01` §6.2（层）、`4-02` §2（插件布局）、`2-01` §7（声明面）；契约 §9 R-25

## 0. 一句话

两个词被两个意思共用，本刀把它们拆开：**`runtime` 指"镜像跑在什么上"**（原 `platform`），
**`provider` 指"谁提供了什么"**（原 `service/` 与 `runtime/posix`、`runtime/core`）。

## 1. 改动一：`platform` 类型 → `runtime`

| 项 | 旧 | 新 |
|---|---|---|
| `plugin_type` | `platform` | **`runtime`** |
| 命名空间 | `platform/` | **`runtime/`** |
| 目录 | `platform/qemu-aarch64/` | `runtime/qemu-aarch64/` |
| 契约头 | `<br/platform/…>` | **`<br/runtime/…>`** |
| 产品根键 | `[product].platform = { brick = … }` | **`[product].runtime = { … }`** |
| JSON 键 | `platform_capacity` / `has_platform` | `runtime_capacity` / `has_runtime` |
| C 常量 | `BR_PLUGIN_TYPE_PLATFORM` | `BR_PLUGIN_TYPE_RUNTIME` |
| 管理器入口 | `br_plugin_manager_platform_init()` | **`br_plugin_manager_runtime_init()`** |
| 视图字段 | `plat_index` | `rt_index` |

**为什么**：`platform` 是"硬件/板级"的行话，但这个插件在为整个镜像提供**运行环境**（reset/向量表/
link.ld/早期 console/timer/GIC/页表/region 表）。叫 `runtime` 才能与它真正的职责对齐，也才让
"`runtime/` 是它的命名空间"读得通（`BRV-TAX-0025` 要求命名空间与类型一致）。

**没有改的**（刻意的）：`br_plat_*` 这组**符号**名（`br_plat_ticks_now` / `br_plat_early_init` …）保留。
它们是"板级实现细节"的缩写，动它们会波及 3 个套件与 platform 内部 20 余处，而收益只是统一前缀。
契约**头**的目录已经改成 `br/runtime/`，符号前缀留给下一刀（登记在本条 §5）。

## 2. 改动二：`service/` + `runtime/` → `provider/`

| 旧 | 新 |
|---|---|
| `service/{trace,dump,hexdump,memleak,backtrace}` | **`provider/{trace,dump,hexdump,memleak,backtrace}`** |
| `runtime/posix` | **`provider/posix`** |
| `runtime/core`（规格皮肤） | **`provider/core`** |
| `iface/min` 的再导出目标 | `provider/core#{log,time,version}` |

**为什么合**：`service/` 与 `runtime/` 在规则上**完全同构**（都是 `ability` + `subkind = service`，
都 LATE），区别只是"大小与角色"——而"大小"不是一条可执法的判据。留两个名字的代价是：
每次新增实现都要问"这算 service 还是 runtime"，答案却没有规则支撑。

**为什么保留 `subkind = "service"`**：subkind 驱动相位推导（`is_service_like` ⇒ LATE），是**行为**轴；
命名空间说的是"实现住在哪"。合并命名空间不需要动 subkind，`provider/` ⇒ `ability` + `service`。

**腾出来的 `runtime` 词**：正好给改动一的类型用 —— 这是两刀必须一起做的原因。
如果先做这一刀而不做上一刀，`runtime` 这个命名空间会空着，而 `platform` 仍然占着它误导性的名字。

## 3. 方向表（改后全貌）

| 消费者 ↓ / 提供者 → | core | runtime | hal | ability | interface | third_party |
|---|---|---|---|---|---|---|
| `runtime` | ✅ | — | ✅ | ❌ | ❌ | ❌ |
| `core` | — | ✅ | ✅ | ✅ | ❌ | ❌ |
| `hal` | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ |
| `ability` | ✅ | ✅ | ✅ | ✅ | ❌ | ✅(例外) |
| `interface` | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ |
| `app` | ❌(A-2) | ❌ | ❌ | ❌ | ✅ | ✅(例外) |

关键三条：**`hal → ability` ❌**（基础设施不依赖实现）、**`runtime → hal` ✅**（SoC 实现 HAL 契约）、
**`ability → hal` ✅**（实现选中机制）。

## 4. 消费者要改什么

**清单里的依赖目标变了，依赖的"形状"没变**：

```toml
# provider/core（原 runtime/core）: reexport_of 目标随实现走
reexport_of = ["provider/core#log"]           # 原 runtime/core#log
[[dep]] name = "core/c_core"                  # 皮肤的多提供者依赖，ADR-0039 起就是 4 条
[[dep]] name = "hal/int"
[[dep]] name = "hal/mm"
[[dep]] name = "hal/sched"

# 产品的根键
[product]
runtime = { brick = "runtime/qemu-aarch64", requires = {} }   # 原 platform = …
```

其余 20 份消费者清单只把 `[[dep]] runtime/core` 改成了 `[[dep]] provider/core`（一个字符串）。
`app/hello` 的两条 `[[dep]] iface/*` 一行未改 —— Interface 层不受影响。

## 5. 未做与已知代价

| 项 | 说明 |
|---|---|
| `br_plat_*` 符号前缀 | 保留（见 §1）。真要统一成 `br_runtime_*` 是一次纯机械改名，波及 platform 内部与 3 个测试套件，收益是命名一致而非规则 |
| `br_core_main` 的名字 | 保留：它是 core 的入口，不因为调用方叫 runtime 而改名 |
| `docs/decisions/00*.md` 里的旧名 | **刻意不改**：历史 ADR 记录的是当时的决定，改它等于篡改决策留痕。新 ADR 里凡引用旧名都注明"was `platform`" |
| `br-wa-isa-001` | 与 `runtime/qemu-aarch64` 的路径同步更新；ISA 库仍非插件 |
| 迁移是一次性硬切换 | 没有兼容期：`plugin_type = "platform"` 现在是形状错（退出码 2），`service/`、`platform/` 命名空间不再是推荐形式 |

## 6. 验证

| 项 | 结果 |
|---|---|
| `brickie check`（五域） | 0 错 / 0 警 / 28 info（dev，全是"再导出未冻结单元"） |
| `brickie build` | 153 步全绿（编译 + `ar` × 28 + 符号审计 + 链接 + post 门禁） |
| `brickie test -j8` | 11 QEMU + 7 宿主 + 11 脚本门禁全绿 |
| `make tools-test` | 731 / 731（快照 12 份随命名更新重发） |
| `brickie-core --selftest` | 586 / 586 |
| 闭包 | `by type: ability 14, app 1, core 1, hal 9, interface 2, runtime 1`；`provider/` 7 件 |
| 种子 | 重发三件 + `prebuilt-check` 3/3 ok + `prebuilts/README.md` §5.1 指纹刷新 |

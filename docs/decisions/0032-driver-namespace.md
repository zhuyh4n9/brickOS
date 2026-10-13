# 0032 — 驱动命名空间: `io/` → `driver/`, 且 `driver/` 只放**本仓自研驱动**(`api_type = "native"`)

> 状态: **已落地**(工具侧 `rules.rs`/`model.rs`/`selftest.rs`; 插件 `io/uart-pl011` → `driver/uart-pl011`;
> 对外头 `include/br/io/` → `include/br/driver/`; 接口单元 `io/uart-pl011#uart-pl011` →
> `driver/uart-pl011#uart-pl011`; 声明面/门禁/文档指称)。
> 判据: `brickie-core --selftest` **520 例**、`make tools-test` **644 例**、全部门禁(7 host +
> 11 QEMU + 10 script)全绿; 新增判据 = `driver/` 下 `api_type != native` ⇒ **`BRV-TAX-0021`**
> (负例已实测红)。
> 前序: **ADR-0015**(`service/svc-posix` → `runtime/posix`, 同一个"命名空间改名"手法)、
> **ADR-0009**(VFS 存储栈, 引入 `io/uart-pl011`)、**ADR-0021/0029**(eMMC 与三方移植件落 `adapter/`)。
> 设计依据: `1-03` §1(插件清单)、`4-02` §2(命名空间集合)、`8-01` §1.3/§6(框架件与驱动编写者契约)、
> `brickie-v0.1` §8.3(名字契约)。
> **设计侧仍写 `io/`** —— 本 ADR 只改原型树的命名空间身份, 两者的收口见 §5.1。

## 1. 背景: 用户裁定

> "io/目录改名为 driver, native driver 将放在这个目录"

两条要求: ① 目录/命名空间改名 `io/` → `driver/`; ② 该目录是**本仓自研(native)驱动**的家。

原型当时的形态:`io/uart-pl011` 是**唯一**一个 `io/` 插件, `subkind = "io"`;
而两个设备驱动里更"重"的那个(`adapter/uboot-mmc`, eMMC/SDHCI, 4 张卡)在 `adapter/` 下,
`api_type = "third_party"` —— 也就是说**"驱动"这个能力被两个命名空间按来源分着放**, 但这条
区分**只存在于人的习惯里**: 工具既不认识 `driver/`, 也不检查"谁该放哪"。

## 2. 决策

### 2.1 命名空间 `io/` → `driver/`(`rules.rs` 的 `NAMESPACES`)

```rust
namespace_plugin_type("driver") => Some("ability")
namespace_subkind("driver")     => Some("io")      // ★ 能力不变, 只换住址
```

`io/uart-pl011` → **`driver/uart-pl011`**; 对外头 `include/br/io/br_uart_pl011.h` →
`include/br/driver/br_uart_pl011.h`(包含卫哨 `BR_IO_BR_UART_PL011_H` → `BR_DRIVER_BR_UART_PL011_H`,
按本仓既有约定"`br/` 之后按目录大写")。

### 2.2 `subkind` **不改**(仍是 `"io"`): 命名空间与类别是两个维度

这是 ADR-0015 §2.2 已经立过的原则, 这里再执行一次:

| 维度 | 回答的问题 | 本刀的值 |
|---|---|---|
| `plugin_type` / `subkind` | **是什么能力**(相位模型 + 设计分类学) | `ability` / **`io`**(不变) |
| 命名空间(目录) | **住哪里 / 谁写的** | `driver/`(自研)、`adapter/`(移植) |

⇒ `namespace_subkind("driver") = "io"`, 于是 `driver/<short>` 的默认 subkind 推导仍是 `io`。
**三个连带好处**: ① 相位推导不变(`ability + io` ⇒ CORE, 与从前逐字相同); ② 目标侧
`br_plugin.h` 的 `BR_SUBKIND_IO` 与 `plan.rs` 的 `subkind_const` **一个字都不用动**;
③ `adapter/uboot-mmc` 的 `subkind = "io"` 保持正确 —— 它**也是** I/O 驱动, 只是不姓"driver"。

把 subkind 一起改名(如 `driver`)会为"目录换名"去动一个**能力枚举**与冻结面, 而获得的行为差异为零。

### 2.3 `driver/` ⇒ `api_type = "native"`: 约定变成**执法**

`api_type` 已经是一条独立轴(native / runtime_adapter / third_party), 所以"自研驱动"不必新造
字段, 只要把命名空间与它**钉在一起**:

```rust
pub fn is_driver_namespace(name: &str) -> bool { name_namespace(name) == "driver" }
```

`model.rs` 的 `validate_driver_namespace()` 在插件加载时判:`driver/` 下 `api_type != "native"`
⇒ **`BRV-TAX-0021`**(error, exit 1)。于是三种来源各有其位, **每一格都有机器判据**:

| 来源 | `api_type` | 住址 | 判据 |
|---|---|---|---|
| 本仓自研 | `native` | **`driver/`** | `BRV-TAX-0021`(本刀新增) |
| 移植的上游原件 | `third_party` | `adapter/` | `BRV-MF-0001` 的 `native→third_party` 禁则 + ADR-0024/0029 的"不许夹带副本"门禁 |
| 适配既有运行时 | `runtime_adapter` | `adapter/` | `rules::api_type_edge_reason` 的既有禁则 |

**为什么值得一条硬规则**: 这条区分以前靠人守, 而"把第三方代码放进自研目录"恰恰是最容易
无声发生的事(复制粘贴一步就完成, 且编译器、链接器都不反对)—— 与 ADR-0031 的
"`core` 必须有且仅有一个"同型: **能被静默违反的约定, 就该有判据**。

### 2.4 只改命名空间, 不动 `adapter/` 与"三方隔离"

- `adapter/` 不是本仓的推荐命名空间(`NAMESPACES` 里没有它, 所以 `brickie new adapter/x`
  会给出 `BRV-TAX-0013` **警告**)—— 这是**有意**的: 移植件的落点由 ADR-0024/0029 的
  "原件原样编译 + 适配层"流程决定, 不是 `brickie new` 的日常产物。本刀不改这条。
- `BRV-TAX-0021` **只**认 `driver/`: `adapter/uboot-mmc` 的 `third_party` 不受影响
  (`selftest` 里有一条正面判据专门钉住这一点, 免得规则被误推广成"任何 io 驱动都得 native")。

## 3. 落地清单(与 ADR-0015 同型的三步: 声明面 → 生成物 → 源码)

| 面 | 落点 |
|---|---|
| 插件身份 | `driver/uart-pl011/brick.toml`(`name = "driver/uart-pl011"`; `subkind = "io"` 不变) |
| 源码/头 | `driver/uart-pl011/{src,include}/`; `include/br/io/` → `include/br/driver/`; 卫哨改名 |
| 命名空间 | `rules.rs`(`NAMESPACES` + 两个映射 + `is_driver_namespace`)、`model.rs`(`BRV-TAX-0021`)、`selftest.rs`(命名空间 4 条 + `BRV-TAX-0021` 正/负例 3 条) |
| 生成物 | `brickie gen` 重发 `build/gen/driver/uart-pl011/plugin_desc.c`;**手工清** `build/{gen,obj}/io/**`(旧名残留, 见 ADR-0015 §2.5) |
| 接口 | `api/iface/io/uart-pl011/` → `api/iface/driver/uart-pl011/`; `iface publish` 重发;**hash 变了**(`b948dff0…` → `4617512c…`)—— 单元 id 参与声明面 hash, 所以改名**必然**换 hash |
| 两处真值同步 | `brick.toml [[export]].hash` 与快照必须同时改(`iface status` 的 `BRV-IFACE-0012` 就是钉这条) |
| `brickie.lock` | 由 `iface publish` 重新生成(3 处名字) |
| 声明面 | `product.toml [select].plugins`; `tests/gates.toml` 的 `[IOCONF]` 注释 |
| 指称 | `README.md`/`WORKAROUNDS.md`/各插件 README 与用例的 `io/uart-pl011` → `driver/uart-pl011`; 分析文档里的**原型侧落点提案** `io/emmc`/`io/mmc`/`io/sdhci` → `driver/…`; `io/tty` → `driver/tty` |

**验证(实测)**

```
$ brickie-core --selftest          # ok 520 cases(含 driver/⇒io、driver/⇒native 正负例)
$ make tools-test                  # passed 644 / failed 0
$ brickie iface status driver/uart-pl011#uart-pl011   # 一致(hash 4617512c…)
$ brickie test                     # 7 host + 11 QEMU + 10 script 全绿
  [IOCONF] SUMMARY pass=12 fail=0 total=12            ← 改名后一字未改(用例 id 与判据都不动)
```

## 4. 被否决的替代

| 方案 | 为什么不取 |
|---|---|
| `subkind` 也改成 `driver` | 为"目录换名"去动能力枚举与 `BR_SUBKIND_IO` 冻结面, 行为差异为零(§2.2); 且会把"住址"混进"能力" |
| 保留 `io/` 作兼容别名(`NAMESPACES` 同时收 `io` 与 `driver`) | 两个命名空间表达同一件事 ⇒ 立刻出现"新插件该放哪"的第二答案; 现在就一件住户, 是改名最便宜的时刻 |
| 新造一个 `[plugin].origin = "own"` 字段来表达"自研" | `api_type` 已经是这条轴(native/third_party/runtime_adapter); 新字段会与它重叠并产生第三个真值 |
| 反向也执法(凡 `subkind = "io"` 的 `native` 插件必须在 `driver/` 下) | `adapter/` 里的 `api_type = "native"` 适配器(如将来某个自研 host 控制器)会被误报; 且"能力"不该反推"住址" |
| 连 `adapter/` 一起改名/收进 `driver/` 的 `third_party/` 子目录 | 会改动 ADR-0024/0029 的整条隔离流程与门禁(`check-adapter-copies`/`check-third-party-pristine`), 与"只改命名空间"的目标不成比例 |
| 只立约定不执法(写进 README 就算) | 本仓口径是"每条约定都有判据"; 而"三方代码进自研目录"是**一步复制就能静默发生**的事(§2.3) |

## 5. 后果与遗留项(诚实清单)

1. **设计侧的收口**(属设计仓库, 本 ADR 只登记): 设计 `4-02` §2 的命名空间集合仍写 `io/`,
   `1-03` §1 的插件清单仍写 `io/uart-pl011`。建议按 ADR-0015 §4 的同款处置: 要么把设计侧
   名字改成 `driver/`, 要么在 `1-03` §1 加一条"**修订**: 原型侧命名空间为 `driver/`"的交叉引用
   —— **两条都要做一次, 别让两棵树各说各话**。
2. **本刀只覆盖"自研 ⇒ driver/"这一格**: 上表另外两格(移植件/适配器)的执法**早就有**
   (ADR-0024/0029 的门禁与 `api_type` 禁则), 本刀没有新增它们; 若将来出现
   `adapter/` 下 `api_type = "native"` 的自研适配器, "它该不该去 `driver/`"仍是**判断**,
   不是判据(§4 的否决理由)。
3. **`driver/` 现在只有一件住户**(`uart-pl011`)—— 与 ADR-0015 的 `runtime/` 同型:
   命名空间是"为将来分类"还是"过度设计", 要看 eMMC 之外的下一件自研驱动(a CAN/GPIO/display)
   落不落这里。落得下来, 这条命名空间就有第二个住户; 落不下来, 它只是一次改名。
4. **hash 变更的可见面**: `driver/uart-pl011#uart-pl011` 的声明面 hash 从 `b948dff0…` 变成
   `4617512c…`(单元 id 参与 hash)。本仓今天没有任何 `requires_iface` 指向它(它不被别人
   `[[dep]]`), 所以这次变更**没有消费者受影响**; 将来若有, 改名就会变成一次兼容性事件。
5. **`io/` 作为字符串在仓库里彻底消失**, 只剩两处**设计引文**保留原拼写:
   ADR-0009 §2.7("设计清单里的 `io/virtio-blk`")与 ADR-0021 §1("驱动侧原计划是
   `io/virtio-blk`")—— 它们引的是**设计侧的名字**, 不是本仓的路径, 按 ADR-0015 的处置保留原样。

## 6. 判据(本刀怎么算绿)

```bash
make -C tools/brickie core && build/host/x86-64/linux/bin/brickie-core --selftest   # 520 例
make tools-test                                                                     # 644 例
build/host/x86-64/linux/bin/brickie iface status driver/uart-pl011#uart-pl011       # 一致
make all && build/host/x86-64/linux/bin/brickie test                                # 全部门禁
```

负例(必须红): 把 `driver/uart-pl011/brick.toml` 的 `api_type` 改成 `third_party` ⇒
`BRV-TAX-0021`(exit 1, 信息里指出"移植件归 `adapter/`"); 把 `driver/uart-pl011` 改回
`io/uart-pl011` 而不改 `NAMESPACES` ⇒ `brickie check` 仍绿(命名空间不是硬禁则)但
`brickie new io/x` 会给出"不符推荐形态"警告 —— 这正是 §2.1 必须同时改 `NAMESPACES` 的原因。

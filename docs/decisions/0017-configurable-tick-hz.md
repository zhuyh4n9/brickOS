# 0017 — 时钟节拍频率可配(`product.toml [kernel].hz`): CONFIG_HZ / jiffies 口径

> 状态: **已落地**(`brickie check` 0 错误/0 警告/0 提示; `brickie build` 通过;
> `smoke` / `irq-test` 等门禁全绿; 200 Hz 与 100 Hz 两种配置实测生效 —— 见 §3)。
> 影响面: `product.toml`(**新增 `[kernel].hz`**)、
> `tools/brickie/schema/product.schema.json`(形状 + `minimum: 1`)、
> `tools/brickie/rust/src/model.rs`(解析 `[kernel].hz`)、
> `tools/brickie/rust/src/build.rs`(`config_defines`: 转成 `-DBR_CFG_TICK_HZ=<hz>` 下发到全部 C 编译单元)、
> `tools/brickie/templates/product/c/product.toml.tmpl` 与 `docs/contract.md` §7.2/R-18(声明面文档)、
> `core/include/br/core/br_time.h`(`BR_CFG_TICK_HZ` 缺省 + `_Static_assert` + `BR_CFG_TICK_PERIOD_US`
> + `br_clock_tick_hz()`)、`core/src/time.c`(实现 getter)、
> `platform/qemu-aarch64/src/board_irq.c`(周期 = `BR_CFG_TICK_PERIOD_US`)、
> `platform/qemu-aarch64/src/plat_qemu_virt.c`(arm 日志报 Hz/周期)、
> `runtime/posix/include/time.h`(`clock_getres` 如实报 `1/HZ`)。
> 设计依据: `3-01` §4(时间组)/§14 CA-1(us)、`3-03`(tickless 目标与超时扫描)、
> `3-01` §11 CA-3(ISR-safe 白名单; 本配置不新增 ISR 面)。
> 相关: ADR-0016(心跳计数收归 core —— 本 ADR 给它加"节拍多快"), ADR-0006(超时分辨率 = tick),
> ADR-0011(RR 时间片以 tick 计), ADR-0007(同步超时分辨率)。
> 参照: Linux Kernel 的 `CONFIG_HZ` / jiffies 口径(HZ = 每秒时钟中断次数)。

## 1. 背景

心跳计数(ADR-0016)收归 core 之后, "每拍多久"仍写死在 platform 里:

```c
/* platform/qemu-aarch64/src/board_irq.c(本刀之前) */
#define BR_BOARD_TIMER_PERIOD_US 100000u   /* 100 ms ⇒ 10 Hz */
```

后果有三:

1. **改节拍要改代码**: 想从 10 Hz(100 ms)提到 200 Hz(5 ms)必须动 platform 源码 ——
   而这是**产品策略**(要中断多密、超时分辨率多细), 该由产品声明面定;
2. **没有"一处真值"**: core 的 jiffies 口径、platform 的装弹周期、POSIX `clock_getres`
   各自写死 100 ms, 任何一个改了不同步就会静默漂;
3. 设计 `3-03` 的 tickless 目标与 Linux 的 `CONFIG_HZ` 一样, 都要求节拍是**一个数**,
   而不是散落在各处的字面量。

## 2. 决策

### 2.1 产品级语义键: `product.toml [kernel].hz`

```toml
[kernel]
hz = 200        # 每秒时钟中断次数; 缺省(不写) = 200 ⇒ 5 ms 一拍; 合法 1..100000
```

* **缺省 200**: 整表不在场或不写 `hz` 都是 200(与 Linux 常见的 100/250/1000 同族)。
* **值域**: `schema/product.schema.json` 用 `integer` + `minimum: 1` 拦明显非法的值;
  上限 100000 由 `br_time.h` 的 `_Static_assert` 兜底(再高周期会截断成 0)。
* 刻意**不**用 `[build].cflags` 写裸 `-D`(理由见 §2.5)。

### 2.2 brickie 把它转成**全单元**的编译期配置

`brickie-core` 的构建规划器:

1. `model.rs` 解析 `[kernel].hz`(缺省 200, 越界报 `BRV-BLD-0010`);
2. `build.rs` 把它放进 `config_defines` 通道, 对**每个 C 编译单元**(core / 各插件 / 生成物)
   追加 `-DBR_CFG_TICK_HZ=<hz>`(放在单元自己的 `[build].defines` 之后, 产品配置优先)。

于是"每秒多少次时钟中断"在镜像里只有一处真值: **同一个宏**。

### 2.3 core 侧: 缺省 + 编译期守卫 + 运行期读点

`br_time.h`:

```c
#ifndef BR_CFG_TICK_HZ
#define BR_CFG_TICK_HZ 200u                      /* 不经 brickie 的编译(宿主用例)的回退 */
#endif
_Static_assert(BR_CFG_TICK_HZ >= 1u && BR_CFG_TICK_HZ <= 100000u, "BR_CFG_TICK_HZ 越界");
#define BR_CFG_TICK_PERIOD_US (1000000u / BR_CFG_TICK_HZ)
br_u32 br_clock_tick_hz(void);                   /* 日志/用例的单一取值点 */
```

* **`#ifndef` 回退**是刻意的: 宿主用例与单文件自检不经 `brickie build`, 它们用与产品缺省
  相同的 200, 不会因为"没有声明面"而编不过;
* `br_clock_tick_hz()` 让 platform 的启动日志、将来的用例都能报出**生效值**(而不是各自抄宏)。

### 2.4 platform / POSIX 读同一个配置

* `board_irq.c`: `#define BR_BOARD_TIMER_PERIOD_US BR_CFG_TICK_PERIOD_US` —— 装弹周期随 HZ;
  ISR 里的 `br_clock_tick_notify()` 位置不变(每拍一次, ADR-0016);
* `plat_qemu_virt.c`: arm 日志从写死的 `100 ms` 改成
  `"... N Hz = M us"`(`N = br_clock_tick_hz()`, `M = BR_CFG_TICK_PERIOD_US`);
* `runtime/posix/include/time.h`: `BR_POSIX_CLOCK_RES_NS = 1e9 / BR_CFG_TICK_HZ` ——
  `clock_getres` 如实报"一拍", 不再固定 100 ms(否则配置成 200 Hz 后它会撒谎);
  对应的 `TC-POSIX-015` 判据改成与宏比较(随配置自适应)。

### 2.5 为什么是语义键, 不是 `[build].cflags` 里的裸 `-D`

`.cflags` 写 `-DBR_CFG_TICK_HZ=200` 也能编过, 但:

* **不可校验**: 工具看到一个字符串, 拦不住 `hz = 0` / 拼写错 / 非整数;
* **不是声明面**: 配置混进通用标志表后, "这个产品的节拍是多少"要 grep 字符串才知道;
* **单一真值**: 语义键可以被 `brickie check` 的 schema 域执法(与 `[budget]`/`[selftest]` 同族)。

代价是动了 `brickie-core`(Rust)—— 已在 `docs/contract.md` §9 登记 **R-18**。

### 2.6 连带后果(都要知道)

* **超时/睡眠分辨率**: `1/HZ`(缺省 5 ms, 100 ms → 5 ms)。性质不变(周期 tick 扫描,
  不早醒、晚到无上界; ADR-0006 的 L1 仍未还清), 只是更细;
* **RR 时间片**: `RR_SLICE_TICKS = 2` 以 tick 计 ⇒ 缺省从 200 ms 变成 **10 ms**
  (Linux 的 jiffies 口径同型: 时间片随 HZ 缩放)。本原型单 APP 线程, 无功能影响;
* **中断率**: 缺省 200 次/s(100 ms → 5 ms)。timer 线有 `NO_STORM_GUARD`(绑定表),
  不会触发风暴告警; IRQCONF 的逐用例不受影响(§3 实测);
* **jiffies 容量**: `br_clock_tick_count()` 是 `br_u32` ⇒ 缺省 200 Hz 下 2^32 拍 ≈
  **248 天**不回绕。它是 jiffies 口径的节拍计数; **墙钟时间一律用 `br_clock_now()`**,
  tick 差值比较用无符号回绕安全写法(ADR-0016 已记)。

## 3. 验证记录(本刀实测)

| 判据 | 命令/配置 | 结果 |
|---|---|---|
| 声明面 | `brickie check` | `0 错误/0 警告/0 提示`(`[kernel].hz = 200` 被 schema 接受) |
| 配置真的下发 | `brickie build --json` | 77 条 C 编译步骤都带 `-DBR_CFG_TICK_HZ=200` |
| **200 Hz 生效** | `brickie test smoke` | arm 日志 `200 Hz = 5000 us`; `irq_ticks` 137 → 333(稳态增量 ≈196/s) |
| **改成 100 Hz 仍生效** | 临时 `hz = 100` + 重建 | plan 带 `-DBR_CFG_TICK_HZ=100`; arm 日志 `100 Hz = 10000 us`; `irq_ticks` 70 → 169(稳态增量 ≈99/s) |
| 构建 | `brickie build` | 通过(重编全部 C 单元 —— define 变了, argv 指纹随之变) |
| 中断/内存/调试/插件门禁 | `smoke`/`irq-test`/`plugin-test` | 绿(`[IRQCONF]`/`[MEMCONF]`/`[DBGCONF]` SUMMARY fail=0) |
| 台账/头文件门禁 | `check-workarounds`/`check-headers` | 绿 |

实测 QEMU 日志(200 Hz):

```
[    0.016591] INFO  int: timer PPI armed by platform (virq=0 INTID=30, 200 Hz = 5000 us)
…
[    1.115448] INFO  tick=1 uptime=1116597 us delay=1000504 us (>=1000000 us: ok) irq_ticks=137
[    2.117419] INFO  tick=2 uptime=2118593 us delay=1001828 us (>=1000000 us: ok) irq_ticks=333
```

> 诚实说明: 第 1 拍(1.115 s)的计数 137 低于 `1.098 s × 200 ≈ 220` —— 因为自检 pass 里
> 有**关中断**的长临界区用例(`TC-TASK-103` 的 400 ms), 关中断期间 timer 电平条件只
> 累积成"一次"投递。这不是节拍失真: 稳态(tick=1 → tick=2, 1.002 s)增量 196 ≈ 200/s。
> 换成 100 Hz 时同型(70 → 169, 稳态 +99 ≈ 100/s)。

## 4. 遗留与后续

1. **tickless 的"按最近期限装弹比较器"仍未做**(ADR-0006 L1): 本 ADR 只是让周期可配,
   超时分辨率仍等于一个 tick。
2. `build/` 里的宿主用例不经产品 `cflags`/`config_defines`, 因此 `br_clock_tick_hz()`
   在宿主上恒为缺省 200 —— 当前无用例依赖它; 若将来要测"配置生效", 应把值加进
   `tests/gates.toml [host]`(另一条声明面)。
3. 设计侧 `3-01` §4 的时间组没有"节拍频率"这一项(原型扩展); 与 ADR-0016 一样,
   将来设计收编时应一并登记。
4. 进库自举种子(`prebuilts/seed/**`)随本刀重新发布(`brickie-core` 变了 ⇒ `tools-prebuilt-check`
   必须重新对齐)。

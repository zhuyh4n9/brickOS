# 0016 — IRQ 心跳计数收归 core: APP 不再依赖 platform 插件接口(`br-wa-boot-001` ②)

> 状态: **已落地**(`brickie check` 0 错误/0 警告/0 提示; `brickie build` 通过;
> `smoke` / `irq-test` / `plugin-test` / `check-workarounds` / `check-headers` 全绿 —— 验证记录见 §3)。
> 影响面: `core/include/br/core/br_time.h`(**新增** `br_clock_tick_notify()` /
> `br_clock_tick_count()`)、`core/src/time.c`(计数实现)、
> `platform/qemu-aarch64/src/board_irq.c`(ISR 改为通知 core; `br_plat_timer_ticks` 变转发)、
> `platform/qemu-aarch64/include/br/platform/br_plat.h`(注释: 消费者请读 core)、
> `platform/qemu-aarch64/src/plat_qemu_virt.c`(平台身份日志移到自己的 start)、
> `app/hello/src/main.c`(去掉 platform 头; 读 core 计数)、
> `app/hello/plugin.toml`(删 `[[dep]]` platform)、`product.toml`(`[lint].allow_edges` 清空)。
> 设计依据: `1-01` §8(平台能力三层模式)/§9(启动序列)、`3-01` §4(时间组)/§10(平台侧 core 接口)/
> `§13.6`(特权分级; P-IRQ-17: APP 是 P0, 中断控制归 P3)、`3-02` §11.1(timer PPI 的 tick 路径)、
> `3-05`(插件管理器: 声明边的方向表)。
> 相关: ADR-0005 §2.10(本 ADR 删掉它列的那条"保留"边)、ADR-0008(`core.init` 独立入口)、
> ADR-0010(自检归 core, APP 不再驱动用例)。
> 登记表: `WORKAROUNDS.md` 的 `br-wa-boot-001` ② —— **本刀还清**。

## 1. 背景

`app/hello` 曾直读 platform 插件的三样东西(为一行启动横幅与 `irq_ticks=` 判据):

```c
#include <br/platform/br_plat.h>
...
br_log_info("platform: %s (%s)", br_plat_name(), br_plat_isa());
br_log_info("tick=... irq_ticks=%lu", ..., (br_u64)br_plat_timer_ticks());
```

它们在声明面留下一条 `app → platform` 的**运行期边**:

* 设计 `1-01` §8 / `3-01` §13.6 把 APP 定为 **P0 消费者**(没有"中断控制"能力),
  让它去认识 platform 插件的接口, 与 `3-02` §11.1 / P-IRQ-17 的**放置纪律**相悖;
* 设计 §7.3 的方向表里 `app ✗ platform`, 所以这条边只能靠
  `product.toml [lint].allow_edges` 显式豁免 —— 它是 `br-wa-boot-001` ②(台账原话:
  "平台身份与心跳**发布成服务**(service registry), 删掉最后一条 `allow_edges`")。

本 ADR 把 ② 还清, 但**没有**引入服务注册表(理由见 §2.5): 心跳计数收归 core,
平台身份日志归 platform 自己。

## 2. 决策

### 2.1 单一真值在 core: `br_clock_tick_notify()` / `br_clock_tick_count()`

* `void br_clock_tick_notify(void)` —— **写入口**, 只给 platform 的 timer ISR;
* `br_u32 br_clock_tick_count(void)` —— **读接口**, 任意上下文可调, 消费者只认识它。

实现落在 `core/src/time.c`(`static volatile br_u32 s_tick_count`)。三个刻意:

1. **放时间组**(`br_time.h`)而不是中断组: 它是"时间基在走"的观测点, 不是中断控制器语义;
   与 `br_clock_now()` 同一份世界观(设计 `3-01` §4)。
2. **只自增, 无锁无日志无分配** ⇒ ISR 安全(platform 的 ISR 里第一件事就是它)。
   单核原型的形态; 多核/PM 的 per-CPU 计数要到设计的 PM/IPI 落地时再谈, 现在不假装。
3. **`br_u32`**: 本 ADR 作成时节拍固定 100 ms ⇒ 2^32 拍 ≈ 13.6 年不回绕。★ **后续
   (ADR-0017)**: 节拍改为产品可配(`product.toml [kernel].hz`, 缺省 200 ⇒ 5 ms 一拍),
   缺省下 2^32 拍 ≈ 248 天; 它是 **jiffies 口径**的节拍/诊断计数, 墙钟时间(微秒)一律用
   `br_clock_now()`, 不要从 tick 数反推时间。

### 2.2 platform 只"通知", 不再持有计数

`platform/qemu-aarch64/src/board_irq.c` 的 ISR:

```c
br_clock_tick_notify();                 /* 先记账: "这一拍到了" */
br_sched_on_tick(br_clock_now());       /* 再推进内核时间基/超时扫描/调度器 on_tick */
br_timer_rearm(BR_BOARD_TIMER_PERIOD_US);
```

`br_plat_timer_ticks()`(platform 的导出面)**保留**, 但语义变成"读 core 的同一份计数"的
转发口 —— 于是不存在第二份计数:

* 为什么不直接删: 删除一条已发布的导出条目要走设计 §6.3 的
  `deprecated` → 弃用周期 → `REMOVED`(`brickie iface publish` 对未弃用的 `REMOVED`
  直接报 `BRV-MF-0001`)。本刀的目标是"APP 不再依赖", 不是"平台删接口";
  platform 侧访问口留给平台内部/诊断用。将来真要删, 按 §6.3 走。

### 2.3 平台身份: 谁的身份谁自己报

`platform/qemu-aarch64/src/plat_qemu_virt.c` 的 `qemu_aarch64_start()` 开头加一行:

```c
br_log_info("platform: %s (%s)", br_plat_name(), br_plat_isa());
```

START 相里 platform 先于 APP 跑(ADR-0005: APP 的 start **最后**), 所以这行仍排在 APP 的
启动横幅之前 —— **启动日志的读法与以前逐字一致**, 变的只是打印者(platform 自己,
它本来就有这个能力, 也本来就在同函数里打 `int: timer PPI armed …`)。

### 2.4 APP: 零插件依赖

`app/hello/src/main.c` 去掉 `#include <br/platform/br_plat.h>`; `irq_ticks=` 改读
`br_clock_tick_count()`; 不再打 `platform: …` 那行。于是:

* `app/hello/plugin.toml` 的 `[[dep]]` 全部删除(APP 现在只 include core 头);
* `product.toml [lint].allow_edges` **清空**(最后一条豁免消失)。

★ 这不是"加了白名单所以不报": 删空之后 `brickie check` 仍 0 错误 —— 那条非法边真的
不存在了(§3)。

> ★ **后续(ADR-0018)**: 之后 APP 要用 POSIX(`usleep`/`pthread_*`), 于是新增了**一条
> 正解边** `app/hello → iface-posix`(Interface 薄皮肤; 设计 §7.3/D18)。这不是回头路:
> `app → interface` 是方向表里的合法边, `allow_edges` 保持空; 且声明与使用从 ADR-0018
> 起交叉校验(未声明的跨插件 `#include` 在 check/build 都报红)。

### 2.5 为什么不是"发布成服务"(与台账退出条件原文的差异)

台账的退出条件写的是"平台身份与心跳**发布成服务**(service registry)"。本刀改用
**core 接口**, 理由:

1. 服务注册表的契约是 `名字 → void*`, 类型安全由插件之间的**契约头**保证。要让 APP
   经它读心跳, 就得新增一个跨方的 ops 结构契约**且**处理"查不到"(返回 `BR_NULL`)
   —— 为**一格诊断计数**引入一整套契约, 收益与面积不成比例;
2. 设计 `3-01` §10 明说 core 拥有一批"面向特定作者"的接口(`br_pic` ops、`br_mm_register`
   …): 心跳是平台**通知 core 的写入口**, 天然属于这一格, 而不是"插件之间会合点";
3. 当时服务注册表**真实用户为零**(设计侧 `1-04` §1.6 的"诚实结论")—— 首个用户应当是
   真正的共享状态(fd 表 / socket 表 / trace 环, 设计 `3-06` §2), 而不是一个计数器。

⇒ 要达成的**性质**("APP 不依赖 platform 插件接口")已经达成; 将来 Interface 插件
(`iface-min`, M2)交付后, APP 若真要消费平台/服务能力, 再走 iface 或服务注册表那两条
正解路 —— 本 ADR 不为将来预先造契约。

## 3. 验证记录(本刀实测)

| 判据 | 命令 | 结果 |
|---|---|---|
| 声明面 | `brickie check` | `0 错误 / 0 警告 / 0 提示`; 闭包 16 插件不变; `allow_edges` 为空仍绿 |
| 构建 | `brickie build` | 通过(80 源文件) |
| 启动/MainLoop | `brickie test smoke` | 绿; `irq_ticks=[1-9]` 命中(计时器心跳仍真在跑) |
| 中断逐用例 | `brickie test irq-test` | 绿(ISR 里多一次通知不影响 IRQCONF) |
| 插件管理/拓扑 | `brickie test plugin-test` | 绿(删一条 `runtime` 边不改 init-DAG) |
| 台账一致性 | `brickie test check-workarounds` | 绿 |
| 对外头自洽 | `brickie test check-headers` | 绿(`br_time.h` 新增一对声明) |

QEMU 日志(冒烟)实测:

```
[    0.016313] INFO  platform: qemu-aarch64/virt (aarch64)     ← 由 platform 自己打
[    0.019246] INFO  clock: 62500000 Hz (arch timer), 62500 ticks/ms …
…
[    1.119579] INFO  tick=1 uptime=1120710 us delay=1006751 us (>=1000000 us: ok) irq_ticks=8
[    2.121687] INFO  tick=2 uptime=2122860 us delay=1001836 us (>=1000000 us: ok) irq_ticks=18
```

`irq_ticks` 每拍增长 = core 的计数确实被 platform 的 ISR 推着走。

> ★ 上面这组数字是**节拍固定 100 ms(10 Hz)** 时的记录。**ADR-0017** 把节拍改成产品配置
> (`product.toml [kernel].hz`, 缺省 200 ⇒ 5 ms 一拍)之后, 同一条日志里 `irq_ticks`
> 每拍约 +HZ —— 计数机制不变, 变的只是"一拍多长"。

## 4. 遗留与后续

1. `br-wa-boot-001` ③(日志/trace 直写 console, 未经服务注册表)仍欠, 与
   `br-wa-debug-002` 同源, 属 M3 的 debug bridge。
2. platform 的 `br_plat_timer_ticks()` 现在是转发口。若将来要删, 走设计 §6.3 的
   `brickie iface deprecate` → 弃用周期 → `REMOVED`(本刀不动接口 hash)。
3. 设计侧文档(`brickOS-Design` 分支的 `1-04` §1.5 与 `decisions/0007`)仍写着 ② 欠着 ——
   本原型这侧已还, 设计侧待主控同步。

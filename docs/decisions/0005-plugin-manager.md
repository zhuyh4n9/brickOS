# 0005 — 插件管理器(plugin_manager)+ 服务注册表: 段机制、相位驱动、生成物契约

> 状态: **已落地**(v0.2.0, QEMU virt aarch64; `brickie test plugin-test` 全绿)。
> 设计出处: `1-01` §9(启动序列)/§6.1(描述符)/§6.2(生命周期四相与"两个完成点")/
> §6.5(init-DAG 与环)/§13.3(段收集)、`3-05`(plugin_manager)、`3-06`(服务管理)、
> `3-01` §9/§10/§13.3、`4-02` §2(段与布局)、`6-01` §3.8/§3.9(TC-SVC-*/TC-PLUG-*)。
> 工具侧: `2-toolchain/brickie/brickie-v0.1.md` §8.1(manifest)/§8.4(生成物布局)。
> 本 ADR 覆盖 map 里的 `br-wa-entry-001` 第 ③ 条与 `br-wa-boot-001` 的**启动链部分**。
>
> ⚠ 命名: `core/include/br/core/br_plugin.h` 的文件头把本 ADR 写成
> `0005-plugin-manager-and-scheduler.md`。本文件的实际名是 `0005-plugin-manager.md`
> (调度器归 F2 的 ADR-0006)。**头文件那一行需要主控改**(冻结件由主控维护)。

## 1. 背景

v0.1 的启动链是**替身链**: `start.S` 直调 `br_irq_cpu_init` → `br_plat_early_init` →
`br_core_main`, 后者在 APP 里直调各子系统的 `*_init` 与三套一致性用例, 然后进
`for(;;)` 忙等。欠债登记为 `br-wa-entry-001`(调用点不由 `.br_plugins` 段驱动)与
`br-wa-boot-001`(没有阶段机)。

本刀把编排权收敛到 core 的**插件管理器**: 扫段 → 建 init 边 → 拓扑排序 → 按相位调用
各插件的生命周期钩子。于是:
* 插件描述符真的**进镜像**(`product.toml [build].gen_sources` 非空);
* `start.S` 只做 reset/BSS/core.init(的第一格), 然后交管理器;
* 各子系统的"调用点"回到它们的插件里(platform 的 start / service/dump 的 LATE init);
* APP 只剩"自检 + MainLoop"。

## 2. 决策

### 2.1 段机制: `.br_plugins` 输出段 + 普通赋值边界符号 + KEEP

* 段名 `.br_plugins`(三处一致: `1-01` §6.1 / `3-01` §13.3 / `3-05` §2.2), 描述符由
  `BR_PLUGIN_SECTION`(`static const` + `used` + `section` + `aligned(8)`)发射。
* 边界符号 `__br_plugins_start/__br_plugins_stop` 由链接脚本**普通赋值**给出(不是
  `PROVIDE`): 它们必须**恒存在**且不能被别处定义 —— `PROVIDE` 的"已有定义就不覆盖"
  语义在这里只会掩盖接线错误(与同文件 `.br_extable` 同款)。
* `KEEP(*(.br_plugins))`: 描述符没有 C 引用者(`static const`), `--gc-sections`
  会把它当垃圾 —— 段必须显式保根。
* **唯一与 `.br_extable` 的写法差别**: 本段是**自己的输出段**(而不是折进 `.rodata`)。
  理由 = 可验证性: `objdump -h` 只列输出段, 折进去就"看不见段是否存在/多大", 而
  `3-05` §2.2 把它当成有名有界的段。它仍属 `rodata` 的 PT_LOAD(R 权限, 镜像不会
  退化成 RWX 大段)。

### 2.2 相位驱动: 类别决定 `init` 相, EARLY 第一步显式取 platform, APP 最后

* **EARLY**: 第一步**必须是 platform 插件的 `early_init`**。取法 = 扫段找
  `plugin_type == BR_PLUGIN_TYPE_PLATFORM` 的那一条(显式), 不靠拓扑序的巧合
  (此刻 console/PIC/页表都还没起来, "谁排第一"与"能不能观测"无关)。之后其余插件的
  `early_init` 按拓扑序。
* **CORE / LATE**: `init` 落在哪一相由**类别**决定(非 Service/Interface ⇒ CORE;
  Interface 或 `ability.subkind == service` ⇒ LATE)。没有 `init` 钩子 ⇒ 完成点 = EARLY
  (设计 §6.2 的"② 与 ① 重合")。
* **全局开中断**: 在 LATE 之后、START 之前(`br_irq_cpu_enable()`, core 的 API)。
* **START**: 复用 **init 拓扑序**, 但 **APP 最后是显式规则**(两趟: 先全部非 APP, 再
  APP)。为什么必须显式: 实测拓扑序里 `app/hello` 排**第一**(init 边为零时按段序),
  靠"拓扑序的巧合"会把 APP 的 MainLoop 放到 platform 的 timer bring-up 之前 —— 那正是
  F3 的 20 ms 超时用例会红的情形。
* 自检(打在 `[PLGCONF] PASS TC-PLUG-003` 下): ① EARLY 第一个真的跑起来的
  `early_init` 属于 platform; ② 进第一个 APP 的 `start` 之前, 非 APP 的 `start` 已全
  跑完; ③ `br_plugin_conformance()` 必须正跑在 APP 的 `start` 里(它就是这么被调的)。

### 2.3 拓扑与环: Kahn + 完整环路径 + 负例自证

* 只取 `kind == BR_DEP_INIT` 的边(`runtime`/`type` 不参与排序)。边的方向:
  `[[dep]].name = B` ⇒ `A 依赖 B` ⇒ **B 先**。
* Kahn 排序;**有环 ⇒ 打印完整环路径**(`a -> b -> c -> a`, 收尾闭合)并 `br_panic`。
  环是组合期硬错误(`brickie check` 已执法), 但运行期也必须能自证 —— `1-01` §6.5 的
  措辞是"运行期不做任何检测", 这里采用**两处互斥读法的第二种**(见 §3 裁定 2)。
* TC-PLUG-002b 用**假边集**跑同一个排序函数: 3 节点环必须被检出且报完整路径, 且同一
  算法对 3 节点 DAG **排得出序**(负控制 —— 否则"总能报环"也是假绿)。

### 2.4 生成物契约(工具侧改了哪几条规则)

生成的 `build/gen/<plugin>/plugin_desc.c` 现在是**可编译单元**, 规则逐条:

| # | 规则 | 依据 / 备注 |
|---|---|---|
| 1 | `#include <br/core/br_plugin.h>` + `#include "{{descriptor_include}}"` | 元契约头是唯一真值; 第二行是声明面锚点 |
| 2 | `descriptor_include` = 插件 `include/` 下**声明该插件 `[[export.entries]]` 最多符号**的那个头(同数取字典序第一); 一个头都没有 ⇒ 回落 `br/core/br_plugin.h` | **改了旧规则**(旧: `{short}/{short}.h`, 指向不存在的路径)。依据: 锚头必须**真实存在**且**确定性**(它进生成物, `gen --check` 会因它变化报红); 用"声明面事实"而不是目录遍历顺序选。实测: dump→`br/debug/br_dump.h`, platform→`br/platform/br_plat.h`(4 个头里导出符号数胜出), hello→`hello/hello.h` |
| 3 | 钩子原型**生成物自证**(`int <prefix>early_init(void);` …) | 钩子名是**生成器**按 `symbol_prefix` 推导的, 插件对外头只声明它自己的 API 面。没有这一条, `dump_init` 会因隐式声明在 `-Werror` 下编不过 |
| 4 | `plugin_type`/`subkind`/`sched_class` 由 manifest 字符串映射为 `BR_*` 常量; `phase_self` 原样带出(仅呈现) | `1-01` §6.1 的结构里**没有** `plugin_type`/`subkind`/`phase_self`(见裁定 1) |
| 5 | `deps` = `[[dep]]` **逐条**(全部 kind), 以 `.name = BR_NULL` 结尾 | 声明面产物要忠实; manager 只筛 `BR_DEP_INIT`。`br_dep_t.phase`: init 边用断言, 其余用 `BR_PHASE_NONE` |
| 6 | 钩子发射规则: `early_init`/`start` 对**全部插件**; `init` 仅在 `phase != "early"` 时发射, 否则 `BR_PLUGIN_NO_HOOK` | `1-01` §6.2 的表: EARLY 与"开中断后的 start"两相覆盖全部插件; "无 init 钩子 ⇔ phase = early"(`rules::derive_phase`) |
| 7 | `ver` 四段用 manifest 的真值(不再硬编码 `{0,1,0,0}`); `.res` 用 `[[res]]` 求和; `.abi_id`/`.api_rev` 带出 | v0.1 模板的三处"骨架初值"是欠账, 现在与 manifest 同源 |
| 8 | 每个生成物给一条**同值的弱定义** `br_plugin_gen_total` | 见 2.8 的口径与局限 |
| 9 | 不再本地 `#define BR_PLUGIN_SECTION`(用冻结头里的), `aligned(4)` → `aligned(8)` | 见裁定 3 |

工具侧改动**只在** `tools/brickie/rust/src/plan.rs` 与
`tools/brickie/templates/descriptor/native/c/plugin_desc.c.tmpl`。`emit.rs`/`model.rs`
本刀**没动**(模型里 `deps`/`res`/`compat` 已经够用)。

### 2.5 失败粒度、入口归属与"过渡桥"

* **首败即停机**(裁定 G6): 首个钩子返回非 0 ⇒ 打
  `[PLUGIN] FAIL <name> phase=<early|core|late|start> rc=<n>` 后 `br_panic`;
  `br_plugin_init_failures()` 记 1(首败即停, 只有 0/1 两态)。
  * 一致性用例的失败**不**算 `init`/`start` 的失败(它是观测, 由门禁判红)——
    platform 的 `start` 只把 `br_plat_irq_start()` 的 errno 当失败返回, dump 的 `init`
    只把 `br_dump_init()` 的返回当失败。这样"设备 bring-up 失败"与"用例红"在日志里
    可区分(前者 `[PANIC]`, 后者 `[...CONF] FAIL`)。
* `br_plugin_phase_name()` 返回设计文档的大写相名; **日志里的相名一律小写**
  (`phase=early`)。理由: 延时自检那条日志用 `... us: EARLY)` 表达"早醒"(门禁的
  forbid 判据), 启动横幅里的 `(EARLY/CORE/LATE -> …)` 曾**误触**裸词 `EARLY` 的
  forbid —— 主控把判据精确到 `us: EARLY)` 并补了正向 require `us: ok)`。即便如此,
  管理器的相名仍取小写: **判据不该被"恰好也用到这个词"的无关输出影响**。
* **入口归属**: `br_core_main` 与 `core/include/br/core/br_main.h` **删除**
  (裁定: 设计说它的归宿是"被拆掉")。它的三块内容分别落到:
  ① 时钟/日志起点 → `plugin_manager` 头部(那是 `core.init` 的落点, 见 2.6);
  ② 三套一致性用例 → platform 的 `start`(IRQ/MEM)与 service/dump 的 LATE `init`(DBG);
  ③ MainLoop → `hello_start()`(APP 的 start)。`entry chain` 日志随之更新。
* **过渡桥**(调度器缺席时的口径): `if (br_sched_registered()) br_sched_run();`
  —— 注册了就交给它; 没注册就由**最后一个 `start()`** 自己占住 CPU(本刀 = APP 的
  MainLoop)。APP 的 `start` 因此曾经"不返回"; F2 的接线把它改成"创建 APP 线程后返回",
  桥的出口自然打开。这条桥写在 `br_plugin.h` 的文件头, 由 `br_sched_registered()`
  一个判据决定, 没有第二处开关。

### 2.6 core.init 的落点(本刀的权宜与它的边界)

设计的启动序列第一格是 `core.init`(TPIDR_EL1/中断框架, 无线程)。本刀:
* `br_irq_cpu_init()` 仍由 `start.S` 在 BSS 清零后直接调(它是"在任何 C 代码前就要
  成立"的一格, 与 v0.1 相同);
* `br_clock_init()` + `br_log_init()` 由 `br_plugin_manager_run()` 的头部代做。

**为什么必须由管理器代做, 而不是包进 platform 的 `early_init`**: 管理器要在 EARLY 相
之前扫段/报错, 而"日志"依赖 `br_log_init` 的时间基点。反过来, **管理器的输出必须
排在 platform 的 `early_init` 之后** —— 早期 console 是 `br_console_init()` 配好的,
在那之前写 PL011 的 DR 会被 QEMU 丢掉(`CR.UARTEN = 0`)。所以实现是:
**扫段(静默) → 找 platform(静默) → 跑它的 `early_init` → 从这一行起才有日志 →
建边/排序(此时报错可见) → EARLY 其余 → CORE → LATE → 开中断 → START**。
这条"排序在 platform.early_init 之后"的顺序与 `3-05` §2 的"扫段 → 拓扑 → EARLY"
字面不同, 是**为了可观测性**的必要重排; 排序本身不影响 EARLY 第一步(它是显式取的)。

### 2.7 服务注册表: 语义照设计 + 三个观测入口 + `selftest.` 前缀

* `br_service_publish/lookup` 逐字照 `3-01` §9: 重名 `-EEXIST`(不覆盖)、表满
  `-ENOSPC`、空 name/ops `-EINVAL`、查不到 `BR_NULL`。
* 发布/查找都发生在 init 相(EARLY/CORE/LATE)⇒ 全局关中断、无线程 ⇒ **不加锁**
  (设计 `3-06` §2 第 1 项); START 相之后发布是调用方的错, 本刀不检测也不假装线程安全。
* 新增三个**观测**入口(`br_service_count`/`name_at`/`lookup_hits`)—— 偏离登记;
  它们不改发布/查找语义, 只给"dump 风格呈现"与一致性用例读表用。
* `br_service_conformance()` 自用 `selftest.` 前缀, 并在结束前**白盒移除**自己的条目
  (否则 dump 的注册表清单会被用例污染)。TC-SVC-001..003 与 `6-01` §3.8 一一对应。

### 2.8 `br_plugin_gen_total`(生成物数量的运行期自证)与它的局限

TC-PLUG-001 要断言"段里的条数 == 生成物数量"。运行期没有第二个真值来源, 所以:
**每个 `plugin_desc.c` 给一条同值的弱定义** `br_plugin_gen_total = <插件树里的插件数>`,
`plugin_mgr.c` 以**弱引用**读它。取插件树数(而不是本次 `gen` 的输出数)是为了让
`brickie gen --plugin X` 不写出与全量不一致的值。

* 局限: 它等于"**插件树**里的插件数", 不等于"**本产品闭包**里的插件数"。由于
  `[build].gen_sources` 的闭包过滤已由工具负责(`BRV-BLD-0013`), 两者在本产品里一致;
  若将来出现"树里有、闭包里没有"的插件, 该断言会**红**(这正是它要暴露的事:
  生成了却没链进来)。
* 弱引用的第二重意义: 把 `gen_sources` 退回 `[]` 时 core 仍能单独链接, 此时
  TC-PLUG-001 明确报红, 而不是链接失败得不明白。

### 2.9 声明面: `service/dump` 的 `init` 边与 `runtime` 边**并存**

dump 对 trace/backtrace/hexdump/memleak 现在各有**两条**边:
`kind = "runtime"`(带 `symbol`, 记录真实调用面)与 `kind = "init"`(`phase = "late"`,
管顺序)。`brickie check` 允许同一声明同一提供方的不同 kind 边; 语义分工写进了
`service/dump/plugin.toml` 的注释。没有 init 边时, LATE 相的 init 顺序会退化成字典序,
dump 会排到四个服务**之前**(而它要转调它们的 selftest)⇒ "编得过、跑不对"。

另外四个 service 插件**不需要** init 边: 它们的 `init` 只调 core 的 API 与自身代码,
不依赖别的插件先 init(逐个核对过)。platform 的 `early_init` 由管理器显式排第一,
不靠拓扑序。

### 2.10 APP 直调的两条边: 一条删掉, 一条保留

* `app/hello → service/dump`(**删掉了**): 调试域一致性用例与启动快照搬进 dump 的 LATE
  `init`, 所以 `product.toml [lint].allow_edges` 里 `["app/hello", "service/dump"]`
  这条 M0 豁免**可以删**(见 §4 遗留项: 由主控执行)。
* `app/hello → platform/qemu-aarch64`(**保留**): APP 的 MainLoop 要读平台身份
  (`br_plat_name`/`br_plat_isa`)与心跳计数(`br_plat_timer_ticks`, `irq_ticks=` 那条
  门禁判据的取值来源)。设计 §7.3 的表里 app ✗ platform, 所以这条仍是**已登记的 M0
  引导例外**; 它的正解是 `iface-min`(M2)或"平台把心跳发布成服务", 都在本刀范围外。

## 3. 设计缺口与逐条裁定

1. **`1-01` §6.1 的描述符缺 `plugin_type`/`subkind`/`phase_self`** ⇒ **新增三个字段**。
   没有它们, 管理器只能**猜**类别(命名约定): "EARLY 第一步是 platform"就退化成
   "谁的名字像 platform"。字段本来就写在 `plugin.toml` 里, 进描述符不增加真值来源。
   `phase_self` 只作呈现/诊断(init 相由类别决定, 与冻结头的口径一致)。
   *顺带*: 冻结的 `BR_PLUGIN_DEFINE` 宏的形参表**没有**这三个字段; 生成物因此
   **直接实例化结构体**(具名初始化器), 不用那个宏。宏保留给手写/测试用, 本刀不动它。
2. **运行期做不做拓扑排序? 设计有两处互斥读法** —— `3-05` §1 说"组合期做静态校验,
   运行期只做编排", `1-01` §6.5 结尾说"运行期不做任何检测(纯静态, 构建期全解)";
   但 `1-01` §9 的启动序列又写"plugin_manager 扫 `.br_plugins` 段 + 拓扑排序(环 = 硬错误)",
   且 `1-03` 的 M0 用例表有环检测用例。**取"运行期也排"**: ① §9 是行为规格, §6.5 那句
   是"依赖正确性的**责任**在组合期"的意思; ② 静态组合下段内容虽由构建期决定, 但
   "谁在段里"是**链接产物**的事实, 运行期自证一次极便宜(8 条 × 5 边); ③ 有环时报
   完整路径比"启动到一半挂死"可诊断得多。
3. **`aligned(4)` → `aligned(8)`**: `br_plugin_t` 含指针(`1-01` §13.3 的 4 是给纯
   标量表写的)。段起点对齐也由链接脚本 `ALIGN(8)` 保证。
4. **`br_plugin.h` 归 core 手写**(裁定 G8): 它不是生成物。生成物是各插件的
   `plugin_desc.c`, 由它实例化描述符 —— 于是"元契约"只有一处真值。
5. **失败粒度 = 首败即停机**(裁定 G6): 不降级、不"跳过后继续"。EARLY 失败时
   全局关中断、系统还不可用, "静默继续"只会让症状远离根因(`3-02` §14.3 的同一义务)。
   代价是**没有"多插件同时报错"的聚合**: 排障要一轮一轮来 —— 原型阶段可接受。
6. **边界符号用普通赋值**(不是 `PROVIDE`): 见 2.1。
7. **EARLY 第一步是 platform 的 `early_init`**: 见 2.2。管理器在找不到 platform 或
   platform 没有 `early_init` 时**直接 panic**(镜像本就不可能成立)。
8. **`descriptor_include` 规则**: 见 2.4 表第 2 行。
9. **钩子不进 `[[export]]`**: 钩子是**组合期契约**(由生成物引用), 不是插件对外能力;
   写进 `[[export]]` 会改接口 hash(而 `api/iface/**` 快照由人维护), 得不偿失。
   为满足 `-Wmissing-prototypes`, 各插件的 `.c` 在定义前自带原型。
10. **`[build].gen_sources` 的写法**: 用 `["build/gen/**/plugin_desc.c"]`(递归通配),
    而不是 `build/gen/*/*/plugin_desc.c` —— 后者把"能不能发现生成物"绑死在"插件名恰好
    两段"上(`2-toolchain` 未规定命名空间深度上限)。闭包过滤是工具的责任
    (`build.rs` 的 `BRV-BLD-0013`)。
11. **`.br_plugins` 用独立输出段**(折进 `.rodata` 也可), 见 2.1 末段 —— 换成独立段是
    为了 `objdump -h` 可验证。
12. **日志相名小写**: 见 2.5。
13. **"钩子可空"的实现形态 = 由相推导, 而不是新增 manifest 字段**。原始要求里
    service 插件的 `*_early_init` / `*_start` 是"可空(BR_PLUGIN_NO_HOOK)"。两种读法:
    (a) 生成物按 manifest 的某个"钩子清单"字段决定发不发; (b) 按**相**推导 ——
    `1-01` §6.2 的表把 `early_init` 与"开中断后的 `start`"都写成**全部插件**的相,
    只有 `init` 是相位相关的。**取 (b)**: `early_init`/`start` 对全部插件发射(于是
    各插件必须给这两个钩子一个定义, 无事可做就是 `return 0`); `init` 在
    `phase == "early"`(⇔ 无 init 钩子)时发 `BR_PLUGIN_NO_HOOK`。理由: (a) 要在
    manifest 里新增一个"钩子清单"字段, 而它与 `phase` 表达的信息**重叠**(必然出现
    "phase=early 却有 init 钩子"这种自相矛盾的声明); (b) 不需要改 manifest schema
    (那属 4-03, 不在本刀范围), 且每个插件的 EARLY/START 相在日志里**都有痕迹**
    (`[PLUGIN] phase=early name=… rc=0` 8 行), 反而是更好的可观测性。代价 =
    "零钩子"的插件也要写两个 3 行函数 —— 原型阶段可接受。

## 4. 被否决的替代方案

| 方案 | 为什么不取 |
|---|---|
| 运行期**不**排序, 只按段序枚举 | 段序是链接顺序(实测 = 对象路径字典序), 与 init 依赖无关; dump 会排在四个服务之前 |
| 生成物只带 `plugin_type`, 不带 `subkind` | LATE/CORE 的分相判据是"Interface 或 `ability.service`", 少 `subkind` 就得猜 |
| 用命名约定(名字前缀 `service/`)判类别 | 与 `1-01` §6.3 的"数量约束与依赖方向只由 `plugin_type`+`api_type` 决定"冲突; manifest 里明明有字段 |
| EARLY/START 的相内顺序也用"字典序" | 相内顺序必须是拓扑序(§6.2: "同阶段内按依赖拓扑序"); APP 最后也必须显式 |
| 管理器在 platform.early_init **之前**打日志 | PL011 未 init, QEMU 直接丢字节(实测过: 段/边那两行日志消失) |
| `plugin_desc.c` `#include` 插件自己的头来拿钩子原型 | 插件头只声明该插件的 API 面, **不**声明钩子; 拿不到原型 ⇒ 隐式声明在 `-Werror` 下编不过 |
| 生成物用 `BR_PLUGIN_DEFINE` 宏 | 宏的形参表冻结且**不含** `plugin_type`/`subkind`/`phase_self`; 改宏形参会破坏冻结面 |
| TC-PLUG-001 只断言"段非空 + 无重名" | 那样"描述符没进镜像"这类错就检不出来; 引入弱符号计数正是为了这个 |
| `br_service_*` 表加锁 | 发布/查找都在 init 相(单线程); 加锁会让人误以为 START 相之后也能发布 |
| 把 `br_plugin_conformance()` 放在管理器里调用 | 它要断言"正跑在 APP 的 start 里"与"非 APP 的 start 已全跑完"; 放在 APP 的 start 是唯一能同时看到这两件事的位置(也与 `br_dump_conformance` 的历史位置同形) |

## 5. 后果与遗留项(诚实清单)

**本刀还清的债**
* `br-wa-entry-001` 第 ③ 条: `start.S` 的调用点改由 `.br_plugins` 段枚举 + 相位驱动。
* `br-wa-boot-001` 的**启动链**部分: 阶段机(EARLY/CORE/LATE/START)、拓扑排序、环检测、
  首败即停机、全局开中断的位置、五个 service 插件的 init 由管理器驱动、`br_core_main`
  被拆掉、`app → service/dump` 的 M0 豁免可以删。

**仍在欠的(不要误报为已还)**
1. `br-wa-boot-001` 的"睡眠退化为忙等"**仍是欠债**: MainLoop 还用 `br_delay_ms`
   (`br_task_sleep` 的接线归 F2 的调度接线步骤)。
2. `br-wa-boot-001` 的"日志/trace 直写 console/RAM 环, 未经服务注册表"**仍是欠债**
   (`br-wa-debug-002` 的 bridge 也仍欠)。
3. `app/hello → platform/qemu-aarch64` 的 M0 引导例外**仍在**(见 2.10)。
4. 管理器头部的"core.init 代做"(时钟/日志)是**权宜**: 设计没有独立的 `core.init`
   入口, 本刀把这一格塞进管理器并写进 2.6。真正的落点应是 `core.init` 独立函数
   (由 `start.S` 调, 或由平台入口调)。
5. `br_sched_registered()` 过渡桥本身: 调度器就位后应删掉"没注册就 WFI 停机"这一支
   (以及 APP `start` 里的"占住 CPU"托底)。
6. 源码里的 `br-wa-entry-001` / `br-wa-boot-001` 的标记**保留了**
   (本工作流不动 `WORKAROUNDS.md`, 由主控同步删除登记表与源码标记; 见 §6 清单)。
   ⚠ 注意 `tools/check-workarounds.sh` 把**任何**文件里 `WORKAROUND(<id>)` 形式的
   文本都算作"源码标记" —— 删登记表条目时, 本 ADR **不含**该形式(只写裸 id),
   所以不必为本文件担心; 但下面列出的源码文件要一起处理。
7. `platform/qemu-aarch64/src/irq_conf.c` 与 `mm_conf.c` 的文件头注释仍写"由
   `br_core_main` 调用" —— 这两个文件不在本刀领地, 需要主控改一行注释。
8. `br_plugin.h` 文件头引用的 ADR 文件名(`0005-plugin-manager-and-scheduler.md`)与
   实际文件名不一致; 冻结件由主控改。
9. TC-PLUG-001 的 `gen_total` 口径(插件树数 vs 闭包数)—— 见 2.8, 依赖
   `build.rs` 的闭包过滤持续成立。

## 6. 需要主控执行的合并动作(本工作流不写这些文件)

* `WORKAROUNDS.md`: 删/改 `br-wa-entry-001`(第 ③ 条已还)与 `br-wa-boot-001`
  (启动链部分已还, 但"忙等睡眠"与"日志直写 console"仍欠 ⇒ **建议改写**而不是整条删除);
  同时同步源码里的欠债标记(`WORKAROUND` 加括号的那种形式; 见 §5 第 6 条)。
* `product.toml [lint].allow_edges`: 删 `["app/hello", "service/dump"]`
  (**保留** `["app/hello", "platform/qemu-aarch64"]` —— 理由见 2.10)。
* `platform/qemu-aarch64/src/irq_conf.c` / `mm_conf.c`: 把"由 `br_core_main` 调用"
  改成"由 platform 的 start() 调用"。
* `core/include/br/core/br_plugin.h`: 把 ADR 文件名改成 `0005-plugin-manager.md`。
* `app/hello/README.md` / `platform/qemu-aarch64/README.md`: 描述里的"`br_core_main`
  的归宿"应改成"已拆掉"。
* `app/hello/src/main.c`: 已按主控裁决**移交 F2**(调度器接线: APP 线程化 + 交给
  `br_sched_run`)。三条钩子的名字/语义与 APP 的 conformance 调用序列是硬约束
  (见 §2.5 与主控 2026-10-09 的裁决)。
* `tests/gates.toml` 的 smoke 判据: 主控已把 `forbid = ["EARLY"]` 精化为
  `us: EARLY)` 并补正向 require `us: ok)` —— 无需再动。

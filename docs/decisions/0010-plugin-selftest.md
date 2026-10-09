# 0010 — 插件自检(selftest): 接口测试与核心代码分离 + core 统一驱动 + 生成期开关

> 状态: **已落地**(v0.2.0, QEMU virt aarch64)。
> 影响面:
> `core/include/br/core/br_plugin.h`(`br_plugin_t.selftest` 钩子 + `br_plugin_manager_selftest` 入口)、
> `core/src/plugin/plugin_mgr.c`(START 之后新增 SELFTEST pass)、
> `core/selftest/**`(**新**: `core_selftest.c` + 四套件的搬迁)、
> `core/src/{plugin,svc,sched,sync}/**`(生产文件里的套件**搬走**)、
> `core/include/br/core/{br_plugin,br_svc,br_sched,br_sync}.h`(测试入口**移出**对外头)、
> 各插件 `<plugin>/src/*_selftest.c`(**新**: 测试与生产分离)与 `plugin.toml`(`[selftest]` 表、
> 测试入口**移出** `[[export]]`)、
> `product.toml`(`[selftest]` 开关 + `[build].selftest_sources`; 并删掉一条 `allow_edges`)、
> `app/hello/src/main.c`(不再直调任何套件)、`tests/gates.toml`(7 道门禁各加一条 SELFTEST 判据)、
> 工具侧 `tools/brickie/rust/src/{model,plan,build}.rs` + `templates/descriptor/native/c/plugin_desc.c.tmpl`
> + 两份 schema(`[selftest]` 的解析与生成期裁决)
> 设计依据: `6-01-test.md`(§1 被测不变量 / §2 运行基建 / §3 用例目录 —— 设计把"目标侧一致性
> 用例"当作**交付物的一部分**, 但没有规定它的**代码落点**; 本 ADR 补的正是这个缺口)、
> `1-01` §9/§6.2(四相与"每插件两个完成点" —— 自检**不是**第五相, 见 §2.2)、
> `2-01`(manifest 与生成物契约)、`4-03`(manifest 语义)
> 相关: ADR-0005(插件管理器: 相位/首败即停机/钩子不进 `[[export]]`)、
> ADR-0008(四阶段启动链: SELFTEST 落在阶段 ④ 的 START 之后)、
> ADR-0009(VFS 存储栈: 它的 `[VFSCONF]` 总套件改由本机制驱动, 顺带还清一条 `allow_edges`)

## 1. 背景: 测试住在生产文件里

本刀之前, 目标侧一致性用例的实现**混在生产 `.c` 里**, 且由不同的调用者驱动:

| 套件 | 实现在哪 | 谁驱动它 |
|---|---|---|
| `[PLGCONF]` | `core/src/plugin/plugin_mgr.c` | APP 的 `start` |
| `[SVCCONF]` | `core/src/svc/svc.c` | APP 的 `start` |
| `[TASKCONF]` | `core/src/sched/sched_core.c` | APP 的 `start` |
| `[SYNCCONF]` | `core/src/sync/sync.c` | APP 的 `start` |
| `[VFSCONF]` | `framework/vfs-core/src/br_vfs.c` | APP 的 `start`(靠 `allow_edges` 豁免) |
| `[IRQCONF]`/`[MEMCONF]` | `platform/…/src/{irq_conf,mm_conf}.c` | platform 自己的 `start` |
| `[DEVCONF]`/`[CDEVCONF]`/`[IOCONF]` | `framework/{dev,cdev}-core`、`io/uart-pl011` | 各自的 `start` |
| `[DBGCONF]` | `service/dump/src/dump.c` | dump 的 **LATE init**(还要转调另外四件) |

四类问题:

1. **测试与生产同文件**: 读实现的人要在几百行用例里找出机制; 改一处机制要跨过测试;
   "关掉测试"在编译层面做不到(代码总在镜像里)。
2. **驱动者五花八门**: APP、platform 的 start、各插件的 start、dump 的 LATE init ——
   同一个概念("跑本件的接口测试")有五种调用形态, 而**没有一个地方能回答**
   "这个镜像里跑了哪些测试、哪些红了"。
3. **测试入口进了 golden 接口面**: `br_plat_irq_conformance`/`br_vfs_conformance`/
   `br_bt_selftest` … 都在 `plugin.toml` 的 `[[export]]` 里 ⇒ 改一个用例的名字/签名
   就是**接口变更**(接口 hash 变、可能要重发快照)。这与 ADR-0005 裁定 9 对生命周期
   钩子的处置("钩子是组合期契约, 不是插件对外能力")自相矛盾。
4. **APP 被卷进来**: 它要直调四个 core 套件 + 一个 framework 套件, 靠 `allow_edges`
   逐条例外 —— APP 因此"认识"了存储域, 而它本该只是消费者。

## 2. 决策

### 2.1 测试与核心代码分离(每个插件一份 `*_selftest.c`)

* 每个有测试的插件: 生产 `.c` **只留机制**, 用例搬到 `<plugin>/src/<short>_selftest.c`;
* 每个插件一个**钩子** `int <prefix>selftest(void)`, 返回 **失败项数**(0 = 全绿);
* 用例需要的**插件私有**访问器放 `<plugin>/src/<short>_internal.h`
  (`src/` 而不是 `include/` —— `include/` 下的一切都是对外面, 会被接口域治理),
  且每条访问器**必须转调生产实现**, 不许复制逻辑(否则用例验证的是副本);
* 已经天然分离的(`platform` 的 `irq_conf.c`/`mm_conf.c`)**保持文件名不变**, 只补一个
  聚合钩子 `src/selftest.c`。重命名会让 `git log`/门禁 tag/文档一起漂, 而它们已经是
  想要的形态(ADR-0010 §3 裁定 8)。

### 2.2 core 统一驱动: START 之后、`br_sched_run()` 之前(不是第五相)

`br_plugin_manager_selftest()` 在 START 相跑完后、交给调度器之前, 按 **init 拓扑序**
逐个调 `selftest`。core 自己的四套(连同存储域总套件)由 `core/selftest/core_selftest.c`
聚合, 经**弱引用**被同一个函数调用。

**为什么不是第五相**: 相位是"能力完成点"(`1-01` §9/§6.2 的四相, `rank` 单调)、
参与相位单调校验、也参与用例的 id 编号; 而自检是**验证**, 对所有插件都在**同一时刻**
发生, 没有任何依赖语义。把它升成相会污染相位模型(而且是为一件事造一个维度)。

**为什么是这个时刻**:
* 此刻 timer 已 armed、中断已开、设备已注册、挂载已就位 ⇒ **现有全部套件的前提都成立**;
* 调度器一旦接管, 控制流再也回不到"单线程顺序跑测试"的形态(APP 线程已经在跑);
* 唯一被这次重排影响到的是 **platform 的两套** —— 它们以前跑在 `br_plat_irq_start()`
  之前(timer 未 arm), 现在跑在之后。这是**有意的时序变化**, 理由与实测记录在 §5。

### 2.3 生成期开关: 关掉 = 测试代码**不进镜像**

`product.toml`:

```toml
[selftest]
enabled = "inherit"   # true | false | "inherit"（= 跟随 [product].stage: dev 开 / release 关）
# plugins = []        # 可选: 只跑列出的插件(其余的连钩子都不发)
```

**两条路径合起来才叫"关掉"**:

| 侧 | 关掉时发生什么 | 落地位置 |
|---|---|---|
| 插件 | 描述符 `.selftest` 写 `BR_PLUGIN_NO_HOOK` ⇒ `<plugin>/src/*_selftest.c` 无人引用 ⇒ `--gc-sections` 裁掉 | `plan.rs` 的 `facts.emit_selftest` |
| core | `core/selftest/**` **不参与编译** ⇒ `br_core_selftest` 符号不存在 ⇒ 弱引用为 NULL ⇒ 跳过 | `build.rs` 的 `selftest_on` |

* 这就是 `[build].selftest_sources` 必须与 `core_sources` **分开**的原因:
  `core_sources` 的通配(`core/src/*.c`)管不到 `core/selftest/`, 而"关掉时也把
  `core/src/**` 里的套件一起排除"是做不到的(它们已经搬走了)。分开之后,
  "要不要编"是一个**源集合**问题, 不是编译期 `#if`。
* 判据是**机械的**(两条, 实测过关闭态):
  1. `nm` 里**没有**任何断言钩子符号 —— 关闭态实测: 11 个插件钩子 + `br_core_selftest`
     全部消失, 只剩 `br_plugin_manager_selftest`(管理器自己的入口, 它必须一直在);
  2. 日志里 `[SELFTEST] SUMMARY … ran=0`。★ 注意**不是**"没有 `[SELFTEST]` 行":
     管理器照旧打 banner 与 SUMMARY, 只是 `ran=0 skipped=15` —— 这是**诚实**的
     ("我跑了, 但没有任何钩子"), 比"什么都不打"更有诊断价值。
  `tests/gates.toml` 的 7 道门禁都要求 `ran=[1-9][0-9]* fails=0 errors=0`
  ⇒ 关闭态**必然**让它们全红(`ran=0` 匹配不上), 这正是要的绊线:
  谁把开关关错, 门禁立刻报"自检没跑", 而不是静默少跑一堆测试。
* **为什么"关掉 = 不编"而不是"编了不跑"**: 只有前者能兑现"发布镜像不带测试代码"
  (代码体积/审计面/26262 的论述负担), 而后者只是省了点时间。同一个开关同时表达
  两件事, 不存在"开关说关、代码却在"的中间状态。

### 2.4 自检失败**不停机**(与 init/start 的两条纪律)

| | init/start 失败 | selftest 失败 |
|---|---|---|
| 含义 | 系统起不来 | 某条接口判据不成立 |
| 动作 | `plugin_fail` → `br_panic`(ADR-0005 裁定 G6: 首败即停机) | **只记不停**, 继续跑下一个插件 |
| 谁判红 | 内核自己 | 门禁(`tests/gates.toml` 的 `forbid` 里的 `[XXXCONF] FAIL`) |

把两者混起来会让排障无从下手: "第 3 个插件的第 7 条用例红了"与"第 3 个插件起不来"
是两种完全不同的故障, 而前者不该阻止后面 11 个插件的自检交出它们的结论。
**返回值语义**也分开: `>= 0` = 失败项数; `< 0` = 钩子自身出错(两者在 SUMMARY 里
分别记 `fails=` / `errors=`)。

### 2.5 测试入口**移出** `[[export]]`

11 个测试入口从 `plugin.toml` 的 `[[export]]` 里删除(清单见 §4)。三条理由:

1. 与 ADR-0005 裁定 9 一致: **钩子是组合期契约, 不是插件对外能力**;
2. 测试面进 golden ⇒ 改一个用例就是接口变更(接口 hash 变), 这是**反向激励**:
   越是想把测试写好的人, 越要付接口治理的成本;
3. 它们现在经**描述符**被 core 调用(与 `early_init`/`init`/`start` 同一条路),
   而描述符不属接口面 —— 于是"谁调它"与"它的身份"两件事自洽。

后果: 受影响的接口快照必须重发(§4), 且 `brickie check --profile release` 的
冻结面随之变小(好事: 冻结面只该有真正的对外能力)。

## 3. 设计缺口与逐条裁定

1. **设计没有规定"目标侧用例的代码落点"**。`6-01` §2 说用例是交付物、§3 给目录,
   但没说实现在哪个文件。本刀的裁定: **与生产分离, 每插件一份 `*_selftest.c`**,
   理由是 §1 的四类问题。这是一条**本 ADR 新立的纪律**, 建议回灌 `6-01` §2。
2. **"自检相" vs "自检时机"**: 不开第五相, 只在 START 之后插一个 pass(§2.2)。
3. **开关的默认值**: `"inherit"` = 跟随 `stage`(dev 开 / release 关)。
   为什么不是"默认开": release 镜像带测试代码与 §2.3 的收益冲突; 为什么不是
   "默认关": dev 镜像若默认不带测试, 那 `make smoke` 就永远看不到用例输出。
   `stage` 已经表达了"这是开发镜像还是发布镜像", 复用它比新增一个正交开关更省。
4. **`[selftest].enabled` 是 `oneOf(boolean, "inherit")`** 而不是纯布尔:
   让"我明确要用默认"可以**写出来**(评审时一眼看到), 而不用靠"字段缺席"暗示。
5. **`[selftest].plugins` 名单的语义**: 空/不写 = 所有声明了 `[selftest]` 的插件都跑。
   它的用途只有一个: 某个镜像只想留一两套自检时, 把**其余的也一并裁掉**(§2.3 的
   生成期裁决按名单过滤)。它不是"跳过"开关 —— 不在名单里 = 不发钩子 = 代码被裁。
6. **`cases`(自称用例条数)只作呈现**。真值是钩子的返回值(它由用例自己数)。
   两者若漂移,**不判红** —— 因为"用例条数"对自动化没有用(门禁按 tag 点名),
   而对人有用(一眼看到这个插件测了多少), 为它加一条校验规则不划算。
   ⚠ 这是本刀唯一一处"声明面不进执法"的字段, 故明确登记在这里。
7. **core 侧的 `*_conformance` 改名 + 返回值改 `int`**: 它们是**同一份代码**的搬迁,
   只改"入口名/返回值/所在文件"。`void` → `int` 是必须的: 汇总器要能把各套件的
   失败数**相加**并报一行总数(以前每个套件各打各的 SUMMARY, 没人汇总)。
8. **`platform` 的文件名不改**(`irq_conf.c`/`mm_conf.c`): 见 §2.1 末段。
9. **宿主侧用例(`tests/host/*.c`)继续直调套件**: 那里没有描述符、没有插件管理器、
   也没有 `product.toml` 开关 —— 宿主门禁的判据是**退出码**, 它就是要在宿主上
   无条件跑全部套件(CI 的"白捡 ASan"形态)。所以它们改为 `#include` 相应的
   `core/selftest/*.c` 的**声明**并直接调用, 且 `tests/gates.toml` 的
   `[[hosttest]].sources` 要把 `core/selftest/*.c` 列进去(见 §5)。
10. **`service/dump` 不再替别人编排**: 它以前在 LATE init 里转调另外四件的 selftest
    (因为当时没有统一驱动者)。现在 core 驱动全部五件, dump 只报**自己**的用例。
    ⇒ `[DBGCONF] SUMMARY` 的含义从"调试域合计(含四件子套件)"收窄为"dump 自己的用例";
    四件子插件的结论改由它们各自的钩子输出。这条**含义变化**必须写进 dump 的源码注释,
    否则读日志的人会以为覆盖变少了。

## 4. 被否决的替代方案

| 方案 | 为什么不取 |
|---|---|
| 保持现状(测试在生产文件里, 各自 start 驱动) | §1 的四类问题一个都不解决; "关掉测试"在编译层面做不到 |
| 新增**第五相** `SELFTEST` | 相位是"能力完成点"且 `rank` 单调、参与用例编号; 自检没有依赖语义, 升成相会为一件"验证"污染整个相位模型 |
| 自检放在 `br_sched_run()` **之后**(由 idle 或 APP 线程跑) | 那时 CPU 已交给调度器, "顺序跑完全部用例"的形态没了; 且 APP 的 MainLoop 会与用例抢输出 |
| 自检放在 LATE 之后、开中断之前 | 需要把依赖 timer/中断的用例(sched/sync/uart/irq)单独拆出来另找时机 —— 两处时机 = 两套前提 = 更多特例 |
| 运行期判断开关(描述符总带钩子, 管理器看开关跳不跳) | 测试代码仍在镜像里 ⇒ 丢掉 §2.3 的主要收益(发布镜像不带测试代码与审计面) |
| 用编译期 `#if BR_SELFTEST` 包住用例 | 开关值要先进编译命令行(第二个真值), 且 `#if` 包住的代码仍在源文件里与生产代码交错 —— §2.1 的"分离"没达成 |
| 每个套件保留自己的 SUMMARY, 不加 `[SELFTEST]` 汇总行 | 门禁就无法用**一条判据**表达"自检跑过且全绿"; 而现在那句 SUMMARY 同时证明"被驱动过"与"开关是开的"(关掉时该行不存在) |
| 测试入口留在 `[[export]]` | §2.5 的三条理由; 特别是"改用例 = 改接口"的反向激励 |
| 把 `#include` 各 `*_selftest.c` 进一个 core 大文件 | 每个插件是独立构建单元(各自的 `[build].sources`), 跨插件 `#include` 会打碎这个边界 |
| 让 `tests/host` 只跑"非套件"部分 | 宿主门禁是 sched/sync 的**主判据**(退出码), 少跑套件等于削弱验收 |

## 5. 后果与遗留项(诚实清单)

**本刀交付的**
* `br_plugin_t.selftest` + `br_plugin_manager_selftest()`: 一个统一的、有汇总行的自检时机。
* 11 个插件的测试与生产分离(其中 5 个是搬迁, 4 个是"本来就分离, 补钩子", 2 个见下);
  core 的四套搬进 `core/selftest/`。
* `product.toml [selftest]` 的生成期开关, 两条路径(描述符 NO_HOOK + core 源集合)。
* 11 个测试入口**移出** `[[export]]`, 冻结面因此更干净。
* APP 的 `start` 变回纯业务, **`allow_edges` 又回到一条**(`app → platform`)——
  ADR-0009 §2.5 那条存储域豁免随本刀删除。
* 7 道 QEMU 门禁各加一条 `[SELFTEST] SUMMARY … fails=0 errors=0` 判据。

**时序变化与实测(必须记录)**
* platform 的 `[IRQCONF]`/`[MEMCONF]` 从"timer arm **之前**"变为"arm **之后**"。
  风险是中断在用例执行期间投递(timer 每 100 ms 一次, 而用例是软件触发 SGI),
  可能扰动风暴窗口判据。**实测结论**: `irq-test`(21 个 tag)/`dbg-test`(33 个 tag)/
  `fs-test`(39 个 tag)与其余门禁全绿 ⇒ 判据在两种时序下都成立;
  本 ADR 记下这一条, 因为"它现在是绿的"不等于"两种时序等价"。
* 同一原因, 自检现在跑在**中断已开**的上下文里(以前 platform 的用例跑在……也开着中断,
  但 timer 未 arm)。这更接近真实运行条件, 是**收益**而不是代价。

**仍在欠的(不要误报为已还)**
1. **`br-wa-test-001` 未还, 且范围又扩大了**: 新增的 `[SELFTEST]` 汇总行不解决
   "`TC-*` id 与 `6-01` 未对齐"这件事; core 四套搬迁后 id 一字未改, 仍是自编号。
2. **`cases` 字段无执法**(§3 裁定 6) —— 声明面与人读面可能漂移, 已知且有意。
3. **宿主侧 `tests/host/*.c` 仍直调套件**(§3 裁定 9): 那里没有"开关"的概念, 所以
   `product.toml [selftest] = false` 对宿主门禁**无效** —— 宿主永远跑全部用例。
   这是刻意的(CI 要全量), 但值得写明, 免得有人以为关掉开关就能关掉宿主测试。
4. **`core/selftest/core_selftest.c` 的聚合顺序是手工维护的**(4 行 `fails += ...`)。
   新增一套 core 用例要记得加一行 —— 没有"自动发现"(原型口径: 无动态注册表)。
5. **`service/dump` 的 `[DBGCONF] SUMMARY` 含义收窄**(§3 裁定 10): 已有注释说明, 但
   外部文档(设计 `5-01`/`6-01`)若描述了"dump 汇总四件"的口径, 需要同步(属设计仓库)。
6. **未做**: 自检的**选择性重跑**(只跑某个插件)、自检的 per-case 计时/预算、
   release 镜像里"自检曾存在"的留痕(现在关掉就是彻底不存在)。

## 6. 需要主控执行/注意的动作

* 设计侧建议(属设计仓库):
  1. `6-01` §2 应写明"目标侧用例的**代码落点** = 插件 `src/*_selftest.c`, 由描述符的
     selftest 钩子驱动"(§3 裁定 1);
  2. `4-03` 的 manifest 语义应补 `[selftest]` 表(`enabled`/`plugins`/`cases`;
     `cases` 不进执法 —— §3 裁定 6);
  3. `5-01`/`6-01` 若写了"dump 汇总调试域四件"的口径, 应改为"五件各自输出, core 统一
     驱动"(§3 裁定 10);
  4. `1-01` §9 的启动序列图可以在 START 之后补一个注:"自检(非相)" —— 便于读者对上
     日志里的 `[SELFTEST]` 段。
* `tests/gates.toml`: 7 道门禁各加了一条 SELFTEST 判据, 并给
  `[[hosttest]] sched-test`/`sync-test` 的 sources 补了 `core/selftest/*.c`(§3 裁定 9)。
* 接口快照: 11 个测试入口移出 `[[export]]` ⇒ 相关 `api/iface/**` 快照与
  `plugin.toml` 的 `[[export]].hash` 必须重发(`brickie iface publish`), 否则
  `brickie iface status` 会报不一致。

# 0036 — `show` 命令族: `show bricks` + `show brick-init-flow`(运行期启动顺序), 且 `phase = "app"` 收紧为非法

> ★ **§2.3 被 [ADR-0037](0037-app-ordering.md) 取代**: 本 ADR 把 `phase = "app"` 判为非法, 理由是
> "APP 是启动相而非 init 完成点, 运行期 `init_phase_of()` 只返 EARLY/CORE/LATE"。ADR-0037 让
> `init_phase_of()` **真的**多出 APP 相(APP 的 `init` 必须晚于全部其它 init), 于是那个理由消失,
> `phase = "app"` 重新成为 APP 的唯一正确取值(`core` 反而非法)。
> 本 ADR 的**其余部分仍然有效**: `show` 命令族、两处"顺序"的区分(§2.1)、`core` 不在链里(§2.2)、
> `show bricks` 是阅读视图(§2.4)、以及"声明相 vs 运行相"的守卫(它现在是 ADR-0037 D-3 的守卫)。
> 本刀同时把 `derive_phase`/`phase_reason`/8 处 `phase` 声明**改回** `app`, 并删掉 APP 的
> `early_init`(ADR-0037 D-1)。

> 状态: **已落地**(工具侧新模块 `rust/src/show.rs`、`main.rs` 派发、`rules.rs` 相位口径收紧、
> `python/brickie/cli.py` 的 `show` 组、`present.py` 两个渲染器、`schema/` 无改动;
> 声明面 `app/hello/brick.toml` 与 7 份 `tests/fx/**/app/*/brick.toml` 的 `phase` 修正)。
> 判据: `brickie-core --selftest` **577 例**、`make tools-test` **669 例**(含新增 V-20 与两份
> 协议快照)、`brickie check` 0 errors / 24 插件闭包、
> **`show brick-init-flow` 与真实运行日志逐事件相同(88/88, 见 §4)**、
> `make tools-prebuilt-check` 3/3 ok(种子已重发)。
> 相关: 设计 `2-02` §5 / A-27(`show` 曾被"`dep closure`/`ver show` 取代", 本 ADR 部分推翻)、
> ADR-0005(插件管理器与相位数轴)、ADR-0008(core 启动链)、ADR-0010(自检 pass)、
> ADR-0031(core 无生命周期)、ADR-0034/0035(选择面)、WORKAROUND `br-wa-fs-001`。

## 1. 背景: 两个问题各缺一个"看得见"的答案

1. **闭包能算不能读**。`dep closure` 输出的是机器口径的全表(全树的 `plugins[]` + `selected`
   布尔 + 三类边 + 预算)—— 判定用很好, 但"这个镜像里到底有哪些砖、每个是产品点名要的还是被
   谁拽进来的"要人自己对照 `product.toml` 与边表去推。
2. **启动顺序根本没地方看**。运行期真正的调用顺序只存在于一台跑起来的机器的日志里:
   `[PLUGIN] phase=… name=… rc=…` 七十行。组合期能给出的 `closure.topo_order` **不是**它 ——
   见 §2.1 的实测。
3. 顺带暴露: 声明面里 `phase` 与"它实际在哪个相 init"存在**一个工具允许、运行期不存在**的
   取值(`phase = "app"`), 见 §2.3。

## 2. 决策

### 2.1 `show brick-init-flow` 打印**运行期**顺序, 而不是复用 `topo_order`

运行期的拓扑序由 `plugin_manager.c` 的 `topo_sort()` 决定: **Kahn 算法 + FIFO 队列**, 队列的
初始顺序与"新就绪节点"的入队顺序都取 **`.br_plugins` 段序**(= 链接时描述符目标文件的顺序
= `build/gen/**/plugin_desc.c` 的字典序)。而 `solver::topo_order` 用的是"名字最小的先出队"。
两者都是合法拓扑序, 但**只有前者是机器真正走的**。实测(本产品):

```
                        app/hello core/c_core framework/... platform/...
dep closure topo_order  0         1          2              11
运行期 s_order          0         1          2              6
```

`topo_order` 的 tie-break 被**保留不动**(裁定: 两处顺序各自有受众 —— `topo_order` 服务于
"可复现的构建/链接顺序", 本命令服务于"机器怎么启动"), 但 `dep closure` 的呈现里它不再被写成
"init topological order", 而是明确标注"名字 tie-break; 运行期顺序见 `show brick-init-flow`"。

派生规则(全部来自声明面, 逐条对应运行期代码):

| 步 | 内容 | 运行期依据 |
|---|---|---|
| ① | `platform` 的 `early_init` **恒为第一步** | 按 `plugin_type` 找, 不走拓扑("页表/控制台还没就绪") |
| ② | EARLY: 其余插件按 `s_order`, 有 `early_init` 钩子的 | `early_init`/`start` 对全部插件发射; `core` 例外(§2.2) |
| ③ | CORE: `init` 钩子 ∧ 非 service-like | `init_phase_of()`: 非 Service/Interface ⇒ CORE |
| ④ | LATE: `init` 钩子 ∧ service-like | 同上: interface 或 `ability.subkind = service` ⇒ LATE |
| ⑤ | `br_irq_cpu_enable()`(LATE 之后、START 之前) | 无插件, 一个独立步骤 |
| ⑥ | START: 非 APP 按 `s_order`, 然后 APP 按 `s_order` | "APP 最后"是显式规则, 不是拓扑序 |
| ⑦ | 自检 pass: 有 `selftest` 钩子的插件按 `s_order` | ADR-0010: 观测, 不参与启动成败 |

### 2.2 `core` 不在链里, 这是它的身份

`plugin_type = "core"` 的三个生命周期钩子恒发 `BR_PLUGIN_NO_HOOK`(ADR-0031: 它驱动相位机,
不参与其中)。所以 EARLY/CORE/LATE/START 四相都**没有它**, 但它**有**自检钩子(自检不是生命周期),
出现在第 ⑦ 步。本命令把它如实地留在链外 —— 这正是一条"读代码看不出来、读日志才看得出来"的事实。

### 2.3 `phase = "app"` 收紧为非法(设计 §7.2 的原文口径)

> ⚠ **本节已被 ADR-0037 取代**(见文首注): APP 现在**是** `init` 的完成点(APP 相), 所以
> `phase = "app"` 重新合法, 而 `core` 变成非法。下文保留当年的推理与实测。

`phase` 声明的是**第 ② 个完成点**, 而它由**类别**决定: 非 Service/Interface ⇒ CORE,
Service/Interface ⇒ LATE。APP 是**启动相**(`start` 在那里跑, 且 APP 最后),
**不是** `init` 的完成点 —— 运行期 `init_phase_of()` 只会返回 EARLY/CORE/LATE。

工具此前允许 `app` 插件声明 `phase = "app"`, 且 `rules::derive_phase("app", …, true)` 也返回
`"app"`; 于是 `app/hello` 声明 APP、实际在 CORE 相 init(实测日志 `phase=core name=app/hello`)。
本 ADR 按设计口径收紧:

* `derive_phase`: app(有 init)⇒ `core`;
* `phase_reason`: 非 service-like ⇒ 只允许 `{early, core}`(app 不再有 `app`);
* `brickie new app` 的骨架因此生成 `phase = "core"`;
* 声明面与夹具里 8 处 `phase = "app"`(1 个真产品 + 7 个夹具)改为 `core`;
* `show brick-init-flow` **仍然**在派生时逐插件比对"声明相 vs 运行相", 不一致就报
  `BRV-MF-0001`(exit 1)。收紧之后这条判据按构造不可能触发 —— 它是**证明不变量的守卫**,
  不是期望会响的告警(实测: 手工把 app 的 phase 改回 `app` 时它会响)。

`PHASES` 常量仍是四项(`[[dep]].phase` 断言里 `app` 仍是合法秩); 收紧只针对插件自身的 `phase`。

### 2.4 `show bricks` 是**阅读**视图, 不是 `dep closure` 的别名

按命名空间分组的表格, 每行: 插件 / 类型 / 子类 / 声明相 / RAM / stack / **来历**
(`root:<记录种类>` 还是 `dep`), 末尾给闭包总量与三套限额(产品预算、平台物理容量)。
`--json` 给同一份数据(`bricks[]` + `totals`), 便于脚本。它不新增任何判定。

## 3. 与设计文档的关系

`2-02` §5 的裁定 **A-27** 写过 "`show`/`env` 被 `dep closure`/`ver show` 取代"。本 ADR
**部分推翻**它, 理由是本刀新增的不是"再导出一次闭包", 而是两条设计侧当时没有落点的事实:
**闭包的来历**(哪些是产品点名的根)与**运行期启动链**(§2.1 那张表)。`env` 仍然未被交付。

## 4. 判据(本刀怎么算绿)

最硬的一条是"与真机一致", 做法是把本命令的步骤与一次真实 `smoke` 运行的日志逐事件比对
(阶段名 + 插件名, 含 `irq=on` 与 `[SELFTEST]` 行):

```bash
brickie show brick-init-flow --json | jq -r '.data.steps[] | "\(.phase) \(.plugin)"' > /tmp/flow.txt
grep -oE '\[PLUGIN\] (phase=[a-z]+ name=[^ ]+|irq=on)|\[SELFTEST\] [^ ]+ rc=' build/logs/smoke.log  # 逐条对齐
# 本刀实测 88/88 相同(23 early + 15 core + 8 late + 1 irq + 23 start + 18 selftest);
# ADR-0037 之后该数是 87/87(App 离开 EARLY/CORE, 多出一条 APP 相的 init)—— 比对方法不变
```

```bash
brickie-core --selftest                      # 577 例(含顺序规则/相位派生/流派生/守卫的正负例)
make tools-test                              # 669 例(新 V-20 段 + show-* 两份快照)
build/host/x86-64/linux/bin/brickie check     # 0 errors; 24 插件闭包 / RAM 172 KiB / stack 4 KiB
make tools-prebuilt-check                    # 3/3 ok
```

负例(已实测红):

* `app/hsm` 声明 `phase = "late"` ⇒ 形状错(exit 2), 提示"该组合只允许 core";
* 手工把某插件改成"声明相 ≠ 运行相"⇒ `show brick-init-flow` 报 `BRV-MF-0001`(exit 1);
* `brickie show <未知子命令>` ⇒ exit 2; `brickie show`(无子命令)⇒ exit 2;
* `frobnicate` 等未交付命令仍 ⇒ exit 2(N-2 保留, `show` 从该名单移出)。

正例(已实测绿): 独立节点在 `runtime_order` 里**保持节序**(而非名字序); 有向边保证 provider 在前;
环 ⇒ `unsorted` 非空且为空序(运行期会 panic); 无 `[selftest]` 声明时第 ⑦ 步为空。

## 5. 后果与遗留项(诚实清单)

1. **两处"顺序"并存**(§2.1): 这是裁定, 不是疏漏。代价是读者可能拿错 —— 已通过 `dep closure`
   的标题改写与本 ADR 记录缓解; 若将来要统一, 是"改 `topo_order` 的 tie-break"一刀,
   会影响 `dep closure --json` 与两份快照。
2. **`show bricks` 的"来历"只看产品记录, 不看边**: 一个既被产品点名、又被别人 `[[dep]]` 的插件
   显示为 `root`(它是根, 事实如此); 边图仍归 `dep tree`/`dep-graph`。
3. **`show-init-flow` 的派生依赖"描述符按字典序链接"这条实现事实**(`build/gen/**` 的 glob)。
   若将来构建改成另一种顺序(例如按 `topo_order` 排描述符对象), 本命令必须同步 —— 否则它会
   打印一个不再存在的启动序。判据(§4 的真机比对)是这个耦合的守卫。
4. **重复的 `init` 边会让运行期 panic**, 本命令不"修好"它: `runtime_order` 忠实保留
   `plugin_manager.c` 的 `indeg == 0 ⇒ 跳过` 守卫, 于是重复边表现为 `unsorted`, 与真机一致。
5. **未跑真机就无法自证**: `show brick-init-flow` 是**派生**, 不是测量。`build/logs/*.log` 是
   它唯一的对照物, 而日志只在跑过 QEMU 门禁后才有。CI 只跑组合期时, 这条命令的输出是
   "按声明面应当如此", 不是"刚刚确实如此"。
6. **种子已重发**: `brickie-core` 与入口 `brickie` 变(负载含 CLI/渲染器), `brickie-gen` 未变;
   `prebuilts/README.md` §5 台账 + §5.1 指纹同一次刷新。

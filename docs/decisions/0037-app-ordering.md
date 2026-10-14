# 0037 — APP 的两条排序规则: **不得有 `early_init`**, 且它的 `init` 必须是**最后一个**

> 状态: **已落地**(目标侧 `plugin_mgr.c`(`init_phase_of` 增 APP 相、init 循环到 APP、开中断移到
> APP 之后、扫描时重证 APP 无 `early_init`、`plugin_fail` 改带相名字符串)+ `plugin_selftest.c`
> (新增 **TC-PLUG-004**)+ `br_plugin.h`/`plugin_mgr_internal.h` 口径;
> 工具侧 `rules.rs`(`derive_phase`/`phase_reason`: app ⇒ `app`)、`plan.rs`(APP 不发 `early_init`
> 也不发其原型)、`show.rs`(APP 相 + APP 不出现在 EARLY)、`present.py`、描述符/APP 骨架模板;
> 声明面 `app/hello`(phase → `app`, 删 `hello_early_init` 与头声明)与 7 份 `tests/fx/**` 夹具)。
> 判据: `brickie test` **全绿**(11 道 QEMU 门禁含 plugin-test 的 `TC-PLUG-004` 三条 PASS)、
> `brickie-core --selftest` **585 例**、`make tools-test` **681 例**(含新增 V-21)、
> `show brick-init-flow` 与真机日志 **87/87 逐事件相同**、`brickie check` 0 errors / 24 插件闭包、
> `make tools-prebuilt-check` 3/3 ok(种子已重发)。
> 取代: **ADR-0036 §2.3**(它把 `phase = "app"` 判为非法, 理由是"APP 是启动相而非 init 完成点;
> 运行期 `init_phase_of()` 只返 EARLY/CORE/LATE")—— 本 ADR 让 `init_phase_of()` **真的**多出 APP 相,
> 于是那个理由不再成立, `phase = "app"` 重新成为 APP 的唯一正确取值。
> 部分修正 **ADR-0005 §2.4 规则 6 / §3 裁定 13**("`early_init` 对全部插件发射")。
> 相关: ADR-0031(`core` 无生命周期)、ADR-0010(自检不是相)、ADR-0008(启动链)、
> 设计 `1-01` §6.2(四相与两个完成点)、§6.3(APP 只被依赖、不被依赖)。

## 1. 背景: APP 在 init 阶段"混在中间", 而且有一个什么都没做的 EARLY 钩子

两条实测事实(都来自 ADR-0036 建立的"运行期顺序可视化"):

1. **`app/hello` 的 `early_init` 是个空壳**。它存在的唯一理由是 ADR-0005 §3 裁定 13 那条
   "`early_init`/`start` 对全部插件发射" —— 于是 APP 也必须给一个 `return 0`。但 APP 的注册
   根本不靠钩子: `.br_plugins` 段由 `br_plugin_manager_init()` **扫描**得到, 与任何钩子无关。
   一个"必须写、写了也没用"的钩子, 是纯噪声(而且它在日志里占一行 `phase=early name=app/hello`,
   让 EARLY 相看起来像有 APP 参与)。
2. **APP 的 `init` 排在 CORE 相里, 比它消费的东西还早**。`init_phase_of()` 按"非 Service/Interface
   ⇒ CORE"把 APP 判进 CORE, 于是实测顺序是 `... phase=core name=app/hello`(第 1 个 core)
   → `... phase=late name=runtime/posix`(POSIX 运行时, 在 APP **之后**才 init)。APP 是唯一的
   业务负载, 却先于它要用的运行时/服务完成初始化 —— 今天恰好没事(APP 的 `init` 只调
   `br_log_set_level`), 但这是"靠实现细节侥幸", 不是一条可依赖的规则。

## 2. 决策

### D-1 APP **不得**有 `early_init`(生成期禁止 + 运行期自证)

* 生成期: `plan.rs` 对 `plugin_type = "app"` 恒发 `.early_init = BR_PLUGIN_NO_HOOK`, 并且
  **连原型也不发**(否则 APP 源码里那个手写钩子会因 `-Wmissing-prototypes` 编不过 —— 这条是
  必须一起改的, "只改描述符"的半个改动编不出来)。APP 骨架模板同步删掉 `early_init` 桩。
* 运行期: `br_plugin_manager_init()` 在扫描段时**重证**每个 `plugin_type = "app"` 的描述符
  `early_init == BR_PLUGIN_NO_HOOK`, 不是则 `br_panic`(与"恰一个 core"同型的自证: 组合期产物的
  关键不变量, 运行期不假设)。
* 判据: **TC-PLUG-004** 在 `[PLGCONF]` 下 PASS, 且日志里 `phase=early name=app/*` 的**行数 = 0**
  (消失本身就是证据, 不是因为别的相变多了)。

### D-2 APP 的 `init` 落在 **APP 相**, 即全部 init 的最后

`init_phase_of()` 从"三类"变"四类":

| 类别 | init 相 |
|---|---|
| 无 `init` 钩子(`phase = "early"`) | EARLY(与 ① 重合) |
| `plugin_type = "app"` | **APP** |
| Interface 或 `ability.subkind = service` | LATE |
| 其余(platform / 其它 ability) | CORE |

init 循环随之从 `CORE..LATE` 变成 `CORE..APP`, 于是运行期顺序是:

```
EARLY(platform 的 early_init 恒第一; 无 APP)
  → CORE → LATE → APP(只有 APP 的 init)
  → br_irq_cpu_enable()          ← 仍然"全部 init 返回之后"才开中断
  → START(非 APP 按拓扑序, APP 最后)
  → 自检 pass
```

* **为什么不是"只在 CORE 相内最后"**: 那样 APP 的 `init` 仍早于 LATE 的 `runtime/posix` 与
  `iface/posix`, 即"APP 先于它消费的运行时初始化"——正是本 ADR 要消除的形状。
* **为什么开中断仍在 APP 之后**: 原注释与日志的口径是"所有插件的 init 都返回之后才开中断"
  (`irq=on (LATE done, all plugin init returned)` → 现在写 `irq=on (all plugin init returned)`)。
  APP 的 init 也是 init, 所以它必须在开中断之前。把 APP 放到开中断之后会让 APP 的 init 里
  出现"中断已开但启动链还没走完"的窗口, 与 ADR-0008 的单一线性启动链冲突。
* **为什么不再需要 `phase_self` 破例**: `phase_key()` 现在把 APP 相打成 `phase=app`, 而 start
  钩子仍固定打 `phase=start`(它是**钩子名**, 不是相名); `plugin_fail()` 因此从"收相号"改成
  "收相名字符串", 免得 APP 的 init 失败被误报成 `phase=start`。

### D-3 `phase = "app"` 恢复为 APP 的唯一合法取值

`derive_phase(app, has_init = true) = "app"`;`phase_reason(app, _)` 只允许 `{early, app}` ——
`core` 被**拒绝**(它会把完成点说早一整相, 而 `show brick-init-flow` 的"声明相 vs 运行相"
守卫会因此报红, 所以两处必须同时收紧)。ADR-0036 §2.3 的"app ⇒ core"整个撤回。

## 3. 与设计文档的关系

* 设计 `1-01` §6.2 说 `init` 的相由 `plugin_type` 决定:"非 Service/Interface ⇒ CORE"。本 ADR
  是**在此之上加一条例外**: APP 的完成点是 APP 相。理由是 APP 在设计里本就是特殊角色
  (§6.3: 唯一业务负载、只被依赖、不被依赖), 而"它的 init 必须晚于全部其它 init"是把这个角色
  写成可执法规则的最短形式。
* 设计 §6.2 把 `early_init` 写成"全体回调" —— D-1 与它相反。**这是有意的偏离**: 该表述的前半
  理由(注册可用)由段扫描承担, 后半(每个插件在 EARLY 都"有痕迹")由日志里的段/拓扑信息承担;
  代价是 APP 少一行日志, 收益是删掉一个永远 `return 0` 的钩子。
* ADR-0005 §3 裁定 13 的推理(钩子由**相**推导而非新增清单字段)不变 —— 变的只是"相"这一侧:
  `early_init` 的发射条件从"全部插件"变成"非 core 且非 app"。

## 4. 判据(本刀怎么算绿)

```bash
brickie test                                   # 11 道 QEMU 门禁全绿(plugin-test 含 TC-PLUG-004)
brickie-core --selftest                        # 585 例
make -C tools/brickie test                     # 681 例(新增 V-21)
brickie show brick-init-flow                   # 87 步: APP 不在 EARLY, APP init 在 irq 之前
brickie check                                  # 0 errors / 24 插件闭包 / RAM 172 KiB
make -C tools/brickie prebuilt-check           # 3/3 ok
```

真机比对(最硬的一条): 把 `show brick-init-flow` 的 87 个步骤与 `build/logs/plugin.log` 逐事件
对齐(阶段 + 插件, 含 `irq=on` 与 `[SELFTEST]` 行)⇒ **87/87 相同**;
`grep -c 'phase=early name=app/' build/logs/plugin.log` ⇒ **0**;
`grep 'phase=app name=app/hello'` ⇒ 恰好一行, 且紧邻其后的就是 `irq=on`。

负例(均已实测):

* 手工把某 APP 描述符的 `.early_init` 指向一个函数 ⇒ 运行期 `br_plugin_manager_init()` panic
  ("the APP plugin must not have an early_init hook");
* 把某 APP 的 `phase` 改成 `core` ⇒ `brickie check` 形状错(exit 2);
* 把某 APP 的 `phase` 改成 `late` ⇒ 同样形状错;
* 在内存里造出"声明相 ≠ 运行相"⇒ `show brick-init-flow` 报 `BRV-MF-0001`(exit 1)。

## 5. 后果与遗留项(诚实清单)

1. **`phase = "early"` 的 APP 仍然合法**, 此时它没有 `init` 钩子, D-2 便**没有主体**。
   TC-PLUG-004 这时如实上报"该镜像的 APP 无 init 钩子 ⇒ 本规则无主体", 而不是静默 pass 一条
   从未成立过的判据。
2. **APP 的 `start` 与 `init` 之间隔着开中断**: `init` 在中断关闭时跑(它因此可以碰不该并发访问的
   东西), `start` 在中断开启后跑(可以建线程)。这是两条规则叠加的结果, 不是疏漏; 需要"开中断后
   再初始化"的代码应放进 `start`。
3. **`br_plugin_phase_name(BR_PHASE_APP)` 同时服务于两种钩子**(APP 相的 `init` 与 start 钩子),
   日志靠"钩子名固定为 `start`"区分。若将来再往 APP 相塞第三种钩子, 这里需要重新设计命名,
   而不是继续复用默认分支。
4. **APP 的 init 与 start 都"最后", 但两者之间没有拓扑序保证**: 多个 APP 时(产品级判据禁止,
   但运行期不重证 APP 基数)它们的相对顺序是段序。要重证"恰一个 APP"是独立的一刀(与
   `BRV-TAX-0020` 同型), 本 ADR 不做。
5. **`app/hello` 的日志文案改了**(`(APP phase, after every other plugin; ...)`):
   门禁只判相位与用例, 不判这条文案, 所以它是"给人读的准确性"而非判据。
6. **种子已重发**: `brickie-core` 与入口 `brickie` 变(负载含模板/CLI/渲染器), `brickie-gen`
   逐字节未变; `prebuilts/README.md` §5 台账 + §5.1 指纹同一次刷新。

# service/dump

> **现场倾倒(呈现编排者)+ 自带自检套件**。dump 只编排**呈现**(regions → heap → leaks →
> trace → backtrace), **不再**替别的插件跑自检 —— 自检由 core 统一驱动(ADR-0010)。
> 本 README 由实现者维护(声明面 `plugin.toml` 是唯一真值, 不在这里复述)。

## 能力面(声明面 `include/br/debug/br_dump.h` 的转述)

| 函数 | 一句话 |
|---|---|
| `br_dump_init()` | LATE 相 init: 注册两个自有 trace 事件名(`dump.begin` / `dump.end`), 返回 0 |
| `br_dump_regions()` | 逐条打印 region 表(`kind` 名字 + 属性解码), 末行 `[DUMP] regions=<n>`; 返回条数 |
| `br_dump_memory(addr, len, flags)` | 边界受检的内存 dump(表头 + `br_hexdump`); 返回实际字节数或负 errno |
| `br_dump_heap()` | 三池几何 + 堆统计 + 页池统计(`[DUMP] heap ...` / `[DUMP] page ...`); 返回行数 |
| `br_dump_trace(max)` | 转调 `br_trace_svc_report`; 返回取走条数 |
| `br_dump_backtrace()` | 转调 `br_bt_print`; 返回帧数 |
| `br_dump_leaks()` | 转调 `br_memleak_report`; 返回存活块数 |
| `br_dump_all()` | 一份完整现场: regions → heap → leaks → trace → backtrace, 前后各一条 `snapshot start/end` 标题 |

flags/常量: `BR_DUMP_F_STRICT`(缺省, 未声明地址 ⇒ `-EINVAL`)、`BR_DUMP_F_FORCE`、
`BR_DUMP_MAX_BYTES`(4096, 单次 dump 的静态预算)。

`dump_selftest()` **不在**导出面里(ADR-0010 §2.5): 它在 `src/dump_selftest.c`, 是描述符的
`.selftest` 钩子, 由 core 的 `br_plugin_manager_selftest()` 在全部 `start()` 之后按 init
拓扑序驱动, 跑 dump **自己的** `TC-DBG-030/031/032/100` 并返回失败项数(失败不停机)。
它打出的 banner 行随之定为
`[DBGCONF] dump conformance (region 清单 / 边界判定 / 全量现场)`
—— 三个短语就是本插件用例的全部覆盖面, 它不再代表调试域整体。

## 设计出处

- `Design/docs/5-debug/5-01-debug.md` §2(`MEMRD` / `TRACE_READ` / `GETINFO`)、
  §3(mini ramdump 的捕获集: **内存 region 表** + arena 统计 + 栈回溯; 约束"捕获路径
  **只用静态缓冲**, 不碰堆/调度器")、§4(红区/毒化由 core 出账)。
- `Design/docs/3-os-core/3-01-core-api-list.md` §6(堆/池/观测)、§7(region 表)。
- ADR-0003 §2.6(五个调试插件的分工与依赖边)、§2.4/§2.5(RAM 窗口之内有效、窗口之外
  invalid; `0x42000000` 是"确定可取 translation fault"的锚点)。

## 归属边界: 为什么 dump 是"呈现编排者"

现场倾倒的每一件**呈现**都已有属主, dump 不重复实现它们:

| 事 | 属主 | dump 做什么 |
|---|---|---|
| 十六进制 + ASCII 行 | `service/hexdump` | 只负责"这段地址能不能读", 然后调用 |
| 栈回溯(捕获) | `service/backtrace` | 只转调 + 计入行数; 符号化归 host 离线工具 |
| trace 环的消费 | `service/trace` | 只转调 report |
| 泄漏/红区账 | `service/memleak` | 只转调 report |
| **region 表边界判定** | **本插件** | 未声明 / 跨 region 尾 ⇒ 拒绝或截断 |
| **现场编排 + 自有用例** | **本插件** | `br_dump_all()` 编排上面四家的**呈现**; `dump_selftest()` 只跑本插件自己的用例(不再汇总四家自检) |

这条边界也是"消费方是插件 ⇒ 契约必须在 core 的对外头文件里"的又一例:
`br_mm_region_count/get/find` 与 `br_mem_layout` / `br_heap_stats_get` / `br_page_stats`
都是**观测契约**, 不藏在 core 内部(ADR-0003 登记为增量)。

## 关键裁定(实现侧)

1. **行数口径(`br_dump_all` 的返回值)**
   `2(首末标题)+ [regions: 条数 + 1 行 regions=<n>] + heap(子调用自报) +
   leaks(折算口径: 1 行 summary + 每存活块折算 1 行) + trace(条数) + bt(帧数)`。
   leaks 段之所以是"折算口径": `br_memleak_report()` 只回传存活块数, 真实输出行数是
   `1 + 归属数 + Σ min(8, 每归属存活块)` —— 归属分布与每归属 8 条明细的上限都不在
   已冻结的声明面上。折算值与真实行数**同阶但两端都可能偏**(归属多则低估, 单归属块多
   则因 8 条上限而高估), 故只用于 `>= 阈值` 的"全打了"判定, **不做等值断言**。
   要精确到行, 需要 memleak 回传行数(见文末「待回灌」)。

2. **`BR_DUMP_F_FORCE` + 未声明地址 ⇒ 只打表头, 不读内存**
   页表按 region 表快照构造, 表外地址 invalid; `hexdump` 是纯呈现原语, **没有**
   extable 保护的读原语(设计 §2 的 MEMRD 完整形态还没交付)。真读一次 = 一次不可恢复的
   data abort, 会把"调试服务"变成"崩溃源"。所以 FORCE 在 v1 的语义收敛为
   **绕过 region 表判定并如实说明"未读取"**, 返回 0 —— 用例 `TC-DBG-031` 的
   `0x42000000`(RAM 窗口之外)因此不崩。要让它真读, 需要 extable 保护的 probe(回灌项)。

3. **属性解码**用 `RO|NX|DEVICE|CACHED|kind=<name>`, 四位全空编成 `RW`;
   `kind` 名字来自 `BR_MM_KIND_*`(见 `br_mm.h` 的位域)。

4. **`br_dump_init()` 不代别的插件 init**(各插件 init 是调用方按相位顺序的义务);
   注册自有事件名失败(trace 未 init)不致命, 继续返回 0。

5. **用例顺序(由 core 的拓扑序给出, 不由 dump 编排)**: core 按 **init 拓扑序**逐个调
   `.selftest` 钩子, 而 dump 对 trace/backtrace/hexdump/memleak 各有一条 `kind = "init"`
   的边 ⇒ 四家的钩子必定排在 `dump_selftest()` 之前; 本插件内部则按
   `TC-DBG-030/031/032 → TC-DBG-100` 顺序跑。把 `TC-DBG-100`(内含 `br_dump_all`)放最后,
   是因为它的 `br_memleak_corruptions()==0` 判据要用到 memleak 自检把堆恢复干净之后的观测
   —— 这条前提现在由 core 的拓扑序保证, 不再由 dump "先替它调一遍"来保证。

## 被谁调用

- **plugin_manager**: `product.toml [select].plugins` 直接选中本插件; `dump_early_init()` /
  `dump_init()` / `dump_start()` 由 core 的插件管理器按相驱动(`dump_init()` 在 LATE 相,
  并在其中打一份启动快照)。**APP 不再直调 dump** —— `app/hello/plugin.toml` 已没有
  `service/dump` 的 `[[dep]]`, `product.toml [lint].allow_edges` 里那条
  `["app/hello", "service/dump"]` M0 豁免也随 ADR-0010 删除(全库只剩 `app → platform`)。
- **init 顺序**: `plugin.toml` 对 trace/backtrace/hexdump/memleak 各有一条 `kind = "init"`
  的边 ⇒ plugin_manager 的拓扑序保证它们的 LATE init 在 `dump_init()` **之前**返回(没有
  这四条边, LATE 相顺序会退化成字典序, dump 会排到最前)。
- **自检**: `dump_selftest()` 由 core 的 `br_plugin_manager_selftest()` 在全部 `start()`
  之后、`br_sched_run()` 之前驱动; 四家兄弟的钩子同样由 core 驱动, 且因 init 拓扑序排在
  `dump_selftest()` 之前。**dump 不代任何插件 init, 也不代任何插件跑自检。**
- **依赖边**: plugin.toml 有四条 `kind = "runtime"` 边(trace / backtrace / hexdump /
  memleak; 每条 `symbol` 只登记一个呈现面的代表符号, 如 `br_hexdump_to`)+ 四条
  `kind = "init"` 边(同上四件, LATE 相顺序)。自检钩子**不**另立依赖 —— 它由描述符挂出、
  由 core 统一调, 与 `early_init`/`init`/`start` 同一条路。

## 验证

```sh
build/host/<triple>/bin/brickie build   # 全镜像交叉编译(含本插件; 零警告)
make dbg-test                           # 运行期门禁(需 core/platform 内存实现就位)
```

运行期用例(编进镜像, 逐项 `[DBGCONF] PASS/FAIL`):
`TC-DBG-030`(region 清单同源同数)、`031`(STRICT 拒绝 / FORCE 不崩)、
`032`(堆池基址 dump 32 B)、`100`(全量现场 + 子段观测交叉验证);
另有四个调试插件的 `TC-DBG-00x/01x/02x/04x` 由它们各自的 selftest 提供 —— 全部由 core 驱动。

★ **`[DBGCONF] SUMMARY` 的含义收窄了**(ADR-0010 §3 裁定 10): 拆分前 dump 是"编排者",
那句 `fail` = 四家失败数之和 + 自己的失败数; 现在它只报**本插件自己的**
`TC-DBG-030/031/032/100`(三个数都只数自己)。四家各自打自己的 PASS/FAIL 与 SUMMARY, 而
"每一家都跑过且都绿"由 core 的
`[SELFTEST] SUMMARY plugins=N ran=N skipped=N fails=0 errors=0` 一行 + `[DBGCONF] FAIL`
的 forbid 一并保证 —— **没有任何一环被少测**, 只是汇总点从"dump 之和"移到了"core 的 pass"。
门禁里那条 `\[DBGCONF\] SUMMARY pass=[0-9]* fail=0 ` 因此仍由本插件的 SUMMARY 满足(它只
要求 `fail=0`); 调试域总账的正确落点是 core 的自检汇总, 不是某个插件。

## 待回灌(发现缺口, 声明面已冻结 ⇒ 只登记, 不擅自加函数)

1. **extable 保护的读原语**(设计 §2 的 MEMRD 完整形态): 有了它, `BR_DUMP_F_FORCE`
   才可能对表外地址"真读并降级成 `-EFAULT`", 而不是像现在这样"不读"。
   named consumer 已在 `br/core/br_fault.h` 的 extable 注释里写明(debug bridge 的 MEMRD)。
2. **精确行数回传**: `br_memleak_report()` 的声明面只回存活块数; 若要让
   `br_dump_all()` 的返回值精确到行, 需要 memleak 侧新增"上次报告行数"的观测(属新函数,
   本原型不动声明面)。
3. **`regions` 轴的分级**: ADR-0003 §2.6 已记 —— 3-01 §13.6 的 memory ops 三轴没有
   "读/inspect", 所以 dump 只能声明 P0; 回灌后 dump 才有对应的特权级别可声明。
4. **设计文档口径同步**(ADR-0010 §6.3): 设计 `5-01`/`6-01` 若写了"dump 汇总调试域四件",
   应改为"五件各自输出, core 统一驱动" —— 属设计仓库的动作; 本原型侧已按新口径落地
   (plugin_manager 按 `[[dep]]` 拓扑序驱动各插件 init, core 在自己的 pass 里驱动全部
   `.selftest` 钩子; APP 不再直调 dump)。

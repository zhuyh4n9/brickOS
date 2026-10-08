# service/dump

> **现场倾倒(编排者)+ 调试域 conformance 入口**。
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
| `br_dump_conformance()` | **调试域一致性用例入口**(与 `br_plat_irq_conformance()` 同型); 返回总失败数 |

flags/常量: `BR_DUMP_F_STRICT`(缺省, 未声明地址 ⇒ `-EINVAL`)、`BR_DUMP_F_FORCE`、
`BR_DUMP_MAX_BYTES`(4096, 单次 dump 的静态预算)。

## 设计出处

- `Design/docs/5-debug/5-01-debug.md` §2(`MEMRD` / `TRACE_READ` / `GETINFO`)、
  §3(mini ramdump 的捕获集: **内存 region 表** + arena 统计 + 栈回溯; 约束"捕获路径
  **只用静态缓冲**, 不碰堆/调度器")、§4(红区/毒化由 core 出账)。
- `Design/docs/3-os-core/3-01-core-api-list.md` §6(堆/池/观测)、§7(region 表)。
- ADR-0003 §2.6(五个调试插件的分工与依赖边)、§2.4/§2.5(RAM 窗口之内有效、窗口之外
  invalid; `0x42000000` 是"确定可取 translation fault"的锚点)。

## 归属边界: 为什么 dump 是"编排者"

现场倾倒的每一件**呈现**都已有属主, dump 不重复实现它们:

| 事 | 属主 | dump 做什么 |
|---|---|---|
| 十六进制 + ASCII 行 | `service/hexdump` | 只负责"这段地址能不能读", 然后调用 |
| 栈回溯(捕获) | `service/backtrace` | 只转调 + 计入行数; 符号化归 host 离线工具 |
| trace 环的消费 | `service/trace` | 只转调 report |
| 泄漏/红区账 | `service/memleak` | 只转调 report |
| **region 表边界判定** | **本插件** | 未声明 / 跨 region 尾 ⇒ 拒绝或截断 |
| **编排 + 调试域 conformance** | **本插件** | `br_dump_all()` / `br_dump_conformance()` |

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

5. **conformance 顺序**: 各插件 selftest 先行(trace → backtrace → hexdump →
   memleak), 编排者自有用例收尾(TC-DBG-030/031/032 → TC-DBG-100)。集合与
   `br_dump.h` 抬头一致; 把 `TC-DBG-100`(内含 `br_dump_all`)放最后, 是因为它的
   `br_memleak_corruptions()==0` 判据要用到 memleak 自检把堆恢复干净之后的观测。

## 被谁调用

- **APP**: `app/hello/plugin.toml` 有一条 `[[dep]] service/dump`(`symbol = "br_dump_all"`),
  且 `product.toml [select].plugins` 选中本插件、`[lint].allow_edges` 显式豁免
  `["app/hello", "service/dump"]` —— 这是 1-03 §1 的 **M0 引导例外**(按 `brickie-v0.1 §7.3`,
  app 只许依赖 interface; iface-min 属 M2)。
- **调用时序**: `br_dump_conformance()` 约定在 `br_plat_irq_conformance()` 与
  `br_plat_mem_conformance()` **之后**调用(堆已初始化、MMU 已开、中断已放行)。
- ⚠ **集成注意**: `br_dump_conformance()` 只编排各插件的 `*_selftest()`, 不代它们 init。
  接线方必须按 LATE 相顺序先调 `br_trace_svc_init()` / `br_bt_init()` /
  `br_hexdump_init()` / `br_memleak_init()`(以及 `br_dump_init()`), 或确认各 selftest
  在未 init 下自足。v0.1 **没有运行期插件管理器**, 这条 init 链是人工编排的 ——
  即 `WORKAROUND(br-wa-boot-001)` 的欠债(见 `WORKAROUNDS.md`)。
- **依赖边**(plugin.toml 的四条 `[[dep]]`, 都是真实调用边): trace / backtrace /
  hexdump / memleak。每条 `symbol` 只登记一个代表符号(如 `br_hexdump_to`), 实际还用到
  同一插件的 `br_*_selftest` —— 声明面形状如此, 非新增依赖。

## 验证

```sh
build/host/<triple>/bin/brickie build   # 全镜像交叉编译(含本插件; 零警告)
make dbg-test                           # 运行期门禁(需 core/platform 内存实现就位)
```

运行期用例(编进镜像, 逐项 `[DBGCONF] PASS/FAIL`):
`TC-DBG-030`(region 清单同源同数)、`031`(STRICT 拒绝 / FORCE 不崩)、
`032`(堆池基址 dump 32 B)、`100`(全量现场 + 子段观测交叉验证);
另有四个转调插件的 `TC-DBG-00x/01x/02x/04x` 由各自的 selftest 提供。
门禁 grep 的是 `[DBGCONF] SUMMARY pass=N fail=0 total=N`。

## 待回灌(发现缺口, 声明面已冻结 ⇒ 只登记, 不擅自加函数)

1. **extable 保护的读原语**(设计 §2 的 MEMRD 完整形态): 有了它, `BR_DUMP_F_FORCE`
   才可能对表外地址"真读并降级成 `-EFAULT`", 而不是像现在这样"不读"。
   named consumer 已在 `br/core/br_fault.h` 的 extable 注释里写明(debug bridge 的 MEMRD)。
2. **精确行数回传**: `br_memleak_report()` 的声明面只回存活块数; 若要让
   `br_dump_all()` 的返回值精确到行, 需要 memleak 侧新增"上次报告行数"的观测(属新函数,
   本原型不动声明面)。
3. **`regions` 轴的分级**: ADR-0003 §2.6 已记 —— 3-01 §13.6 的 memory ops 三轴没有
   "读/inspect", 所以 dump 只能声明 P0; 回灌后 dump 才有对应的特权级别可声明。
4. **描述符驱动编排**: 目前 APP 直调 dump; 运行期插件管理器到位后, 应由 LATE 相按
   依赖拓扑调用各插件 init, 本 README「被谁调用」的第 3 条欠债随之偿还。

# service/trace

> 实现态(不再是骨架)。声明面以 `plugin.toml` 为唯一真值, **本目录不改它**。

trace 观测服务: **core 16 B 事件环的唯一消费方** —— 取走(drain)、id→名字解码、
按 id 计数、成行呈现;另提供"动态事件名注册 + marker 打点"。

## 能力面(对外声明面 = `include/br/debug/br_trace_svc.h`)

| 函数 | 语义 |
|---|---|
| `br_trace_svc_init()` | LATE 相: 清计数 + 注册本插件两个自有事件名;返回 0 |
| `br_trace_svc_marker(tag)` | tag → (hash, 槽位);注册(幂等)+ 发一条事件;返回动态 id 或负 errno |
| `br_trace_svc_register(name)` | 只注册动态事件名(供别的插件给自有 id 起名);返回 id 或负 errno |
| `br_trace_svc_name(id)` | 先查动态表, 再退回 core 的固定名表;未知 ⇒ `"?"` |
| `br_trace_svc_report(max)` | 取走最多 `max` 条(`0` = 不限, 以环容量为硬上界), 逐条打 `[TRACE]` 行;返回取走数 |
| `br_trace_svc_summary()` | 打 `[TRACE] total= drained= overrun= ids= live=`;返回 drained |
| `br_trace_svc_seen(id)` | 某 id 的累计**已消费**条数 |
| `br_trace_svc_reset()` | 只清消费侧计数(不动 core 环、不动名字表) |
| `br_trace_svc_selftest()` | 跑 TC-DBG-001/002/003, 打 `[DBGCONF] PASS/FAIL`, 返回失败数 |

## 设计出处

- `docs/5-debug/5-01-debug.md` §1 —— 定长 16 B 事件 + 静态环形缓冲 +
  **热路径零字符串**("id→名字的映射表随插件元数据导出, 离线解码")+
  "泄放途径: panic 时 console 倒出 / bridge 实时流 / host 工具解码"。
- `docs/1-architecture/1-03-roadmap.md` §1 的 v1.0 清单行
  (`service/trace` | Service | 16B 事件环形缓冲 | core | M2)。
- `docs/decisions/0003-*.md` 的归属裁定(见下)。

## 归属边界 —— 为什么"环在 core"

5-01 要的是"trace 是插件(可整层移除)", 但没说"环由谁持有"。本原型的裁定
(ADR-0003)是:

- **环与发射点在 core**(`br/core/br_trace.h` 的 `br_trace_emit`)。因为 CA-3 的
  ISR 白名单要求中断/fault 路径**不依赖"服务是否已 init"**, 且发射不能取锁/分配;
  环在 core 才能让"插件缺席时事件照旧记录, 只是没人消费"。
- **解码/过滤/计数/呈现在本插件** —— 这正是 5-01 意义上的 trace 插件, 也是
  `service/dump` 的 `TRACE_READ`/`br_dump_trace` 的落点。

## 关键裁定(实现侧)

1. **hash 与槽位**: 事件 id 用 `FNV-1a 32`(offset `2166136261`, prime `16777619`)
   对 tag 取 hash, 起始槽位 = `hash % BR_TRACE_SVC_ID_SLOTS(32)`,
   id = `BR_TRACE_SVC_ID_BASE(1024) + 槽位`。撞槽时从起始槽位**向后线性探测**空槽
   (环绕, 最多扫过全表 32 步);扫完全表无空槽 ⇒ `BR_ERR(BR_ENOSPC)`。
   **为什么不是"撞车即错"**: 32 槽表上一次撞车就失败, 会让"谁先注册"决定别人能不能
   注册 —— 别的插件的 marker 能凭 hash 挤掉本插件 selftest 的槽位, 让用例无辜变红
   (测试依赖哈希槽位是设计味道)。线性探测只在撞车时退化为最多 32 步扫描, 寻址仍是
   hash O(1), 对 32 槽的表完全可接受。
2. **名字不拷贝**(前提与风险): 槽位里存的是调用方给的 `const char *`。前提是
   **tag 的生命周期 = 映像**(字符串字面量或静态数组);若传栈/堆上的临时缓冲,
   指针会悬空。原型没有插件管理器来强制该前提, 由调用纪律承担(风险已知)。
3. **身份判据 = 指针相等**: 同一字面量在同一编译单元内被合并
   (`-fmerge-constants`), 所以"同一处 tag 重复 marker"幂等(返回同一 id);
   而**内容相同但指针不同**说明调用方给了两个不同缓冲, 身份不可判定(也破坏了
   第 2 条的生命周期前提)⇒ `-ENOSPC` —— 不复用、也不新建第二份名字。
   本插件自己的 tag 一律用静态数组, 保证指针恒等。
4. **消费计数表 64 项**: 线性查找;表满后余量并入"其它"桶, summary 追加一行
   `[TRACE] other=<n>` 体现(冻结的 summary 主行字段不变)。
5. **判据可自证**: TC-DBG-001 里除了幂等/解码断言, 还断言"两个 hash 同槽的 tag
   都注册成功且 id 不同, 名字各自正确"(线性探测), 以及"内容同名但指针不同 ⇒
   `-ENOSPC`";同槽这件事由用例内 `trc_slot_of(A) == trc_slot_of(B)` 当场复核,
   免得 hash 改动后断言悄悄变成空转。用例用的都是静态数组, 重复跑仍幂等。

## 编译期前提

无额外前提: 与全镜像一样 `-ffreestanding -nostdlib`、`-std=c11 -O2` 并吃全部
告警开关;不引 libc、不分配。全部函数 **thread-only**(drain 会推进消费游标);
ISR/fault 路径只允许直接调 core 的 `br_trace_emit`。

## `[[dep]]`(结构依赖)

依赖的**唯一真值**在 `service/trace/plugin.toml`, 本目录不改动它。当前:

- 入边: `service/dump → service/trace`(kind = runtime, symbol =
  `br_trace_svc_report`), 声明在 `service/dump/plugin.toml`。
- 出边: 无。本插件只依赖 core 的 `br/core/br_trace.h`、`br_clock_now()`、`br_log`。

## 如何被调用

- 启动期: `br_trace_svc_init()`(LATE 相), 随后由
  `service/dump` 的 `br_dump_conformance()` **统一驱动** `br_trace_svc_selftest()`
  (打印 `[DBGCONF]` 行), 再由 `br_dump_trace()` / `br_dump_all()` 调 `report`/`summary`。
- 其它插件: `br_trace_svc_register(name)` 给自己的事件 id 起名;
  线程上下文打点用 `br_trace_svc_marker(tag)`, ISR 内直接 `br_trace_emit`。

## 用例(见 `tests/smoke.toml`)

| id | 判据 |
|---|---|
| TC-DBG-001 | marker 两次 → 同一动态 id(≥ BASE)、名字可解码;`report(0)` ≥ 2 条、`seen(id)` ≥ 2;两个 hash **同槽**的 tag 线性探测后都注册成功(id 不同、名字正确);内容同名但**指针不同** ⇒ `-ENOSPC` |
| TC-DBG-002 | 连发 `RING_SIZE + 8` 条打满环 ⇒ `overrun > 0`, 且 summary 打印值与断言同源一致 |
| TC-DBG-003 | `report(2) == 2` 后续 `report(0) >= 3`(截断消费后从最旧未消费处续读) |

## 已知欠账(不在本插件目录可修)

- `build/gen/service/trace/plugin_desc.c` 仍引用骨架符号 `trace_early_init`/`trace_init`/
  `trace_start`(生成物; v0.1 不编译它 —— V-9)。plugin.toml 的导出面里并没有这三个
  符号, 改 `plugin.toml` 是禁区, 故此处按导出面实现 `br_trace_svc_*`;
  描述符重建时需要对账。

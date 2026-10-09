# service/backtrace

> 实现态(不再是骨架)。声明面以 `plugin.toml` 为唯一真值, **本目录不改它**。

栈回溯**捕获**服务: 沿 aarch64 的 `x29`(fp)链走查, 得到一列
`{pc, fp, sp, depth}`;打印 `[BT]` 行;保留最近一次快照供 dump 复用同一份现场
(不重复走栈)。

## 能力面(对外声明面 = `include/br/debug/br_bt.h`)

| 函数 | 语义 |
|---|---|
| `br_bt_init()` | LATE 相: 清"最近一次捕获"快照;返回 0(**不动栈边界**, 见下) |
| `br_bt_set_stack_bounds(bottom, top)` | 告知栈范围;`bottom==0 或 top<=bottom` ⇒ `-EINVAL` |
| `br_bt_capture(out, max)` | 从当前上下文捕获(内联汇编读 `x29`/`x30`);返回帧数 |
| `br_bt_capture_from(fp, pc, out, max)` | 从给定现场(异常帧/已知 fp)捕获;返回帧数 |
| `br_bt_print()` / `br_bt_print_from()` | 捕获并逐帧打印 `[BT] <depth>: pc= fp= sp=`, 末尾 `[BT] frames=`, 返回帧数 |
| `br_bt_last()` / `br_bt_last_count()` | 最近一次捕获的快照与帧数 |

上表是**对外声明面**(`[[export]]`)。自检**不在**这里(ADR-0010 §2.5): 用例在
`src/backtrace_selftest.c`, 入口 `backtrace_selftest()` 是描述符的 `.selftest` 钩子, 由
**core** 的 `br_plugin_manager_selftest()` 在全部 `start()` 之后统一驱动 —— 跑
TC-DBG-010/011, 打 `[DBGCONF] PASS/FAIL`, 返回失败项数(失败不停机)。TC-DBG-011 收尾要把
栈边界复原成用例前的原样, 经 `src/backtrace_internal.h` 的访问器读/写生产状态(转调,
不复制)。

帧的语义: `pc` = "下一条要执行的地址"(即调用点的返回地址);`sp` = `fp + 16`,
是 AAPCS64 栈帧里的**规范位置**(`stp x29,x30,[sp,#-N]!` 之后 `x29` 指向保存区,
规范位置在 `fp+16`), **不是真实 SP**(真 SP 随函数体变化, 无稳定语义)。

## 设计出处

- `docs/5-debug/5-01-debug.md` §3 —— 捕获集含"TCB 全集 + **各线程栈**";
  "约束: 捕获路径**只用静态缓冲**, 不碰堆/调度器";host 侧
  "离线分析(线程时序对照 trace、fault 解码、**栈回溯**)"。

## 归属边界 —— 为什么这里不做符号化

5-01 已把职责划死: **target 只捕获, host 才符号化**。

- target 侧(本插件): 按帧指针链走栈, 得到 `pc/fp/sp` 序列。只做"确定能做的事",
  不分配(static 快照), 不在镜像里塞符号表(那要两遍链接, 属 v1.x)。
- host 侧: `addr2line`/`llvm-symbolizer` 把 `pc` 变成 函数+偏移 —— 这是
  "离线解码"的一部分, 不在本插件里。

fault 现场的捕获入口接收异常帧里的 `fp/pc`(由 platform 的 fault 分派路径传入),
本插件不自己再触发异常。全部函数 **thread-only**。

## 编译期前提

**全镜像必须 `-fno-omit-frame-pointer`**(已写进顶层 `Makefile` 的 `CFLAGS`,
并有注释点名本插件)。它是本插件唯一的编译期前提:

- `-O2` 默认会把 `x29` 当普通寄存器省掉 ⇒ 链断在第一帧;
- 代价是每个函数多一对 `stp/ldp`, 换"崩溃时能走栈"。

另: `br_bt_capture`/`br_bt_capture_from` 的定义上加了 `__attribute__((noinline))`。
这不是优化提示而是**语义要求** —— 帧 0 的 `fp` 必须是捕获函数自己的帧(否则
`mov x29` 读到调用者的帧, 链少一层);头文件声明面不含该属性, 只加在定义上, ABI 不变。

## 帧链护栏(全部通过才允许解引用 `fp`, 不许先读后校验)

1. `fp != 0`;
2. `fp % 16 == 0`(AAPCS64 对齐);
3. 在栈范围内: `br_bt_set_stack_bounds` 设过就用它;未设 ⇒ 以**走查者当前 SP**
   为下界、`SP + 1 MiB` 为上界(保守回退: 下界取走查者的 SP 比任何调用者帧都低,
   不会误杀;上界靠固定窗口挡住远处的野值);
4. **严格递增**: `next_fp > fp`, 否则视为环/断裂并停止(栈向低地址增长 ⇒ 上行必增);
5. 深度 < `BR_BT_MAX_DEPTH(32)`;
6. 帧数 < `max`。

`br_bt_init` **不清**栈边界(⭐ 待确认项): 边界由 platform 在 `early_init` 里按链接
脚本符号设置, 而 init 是 LATE 相, 清零会把那次设置抹掉 —— 所以实现保留边界。
但声明面 `br_bt.h` 的 init 注释现在写的是"清状态并**复位栈范围**", 与同文件里
`set_stack_bounds` 的"platform 在 early_init 调用"两句在时序上互斥。本实现按
"不清边界"落地(行为更安全), 该措辞冲突已回报给主控, 等声明面二选一后再对齐。

## `[[dep]]`(结构依赖)

声明面在 `plugin.toml`, 本目录不改动它。当前:

- 入边: `service/dump → service/backtrace`(kind = runtime, symbol = `br_bt_print`),
  声明在 `service/dump/plugin.toml`。
- 出边: **无**。

`br_bt_init` **不跨插件**调 `br_trace_svc_register`: 没有
`service/backtrace → service/trace` 的 `[[dep]]` 边。这一点已与声明面对齐
(`br_bt.h` 的 init 注释同样声明"不跨插件注册 trace 事件名");要接得先补 `[[dep]]`
(声明面变更, 是禁区)。

## 如何被调用

- 启动期: `br_bt_init()`(LATE 相); 自检钩子 `backtrace_selftest()` 由 **core** 的
  `br_plugin_manager_selftest()` 在全部 `start()` 之后统一驱动(打印 `[DBGCONF]` 行)。
- 现场倾倒: `service/dump` 的 `br_dump_backtrace()` → `br_bt_print()`;
  dump 也可以取 `br_bt_last()` / `br_bt_last_count()` 复用快照。
- platform: 在 `early_init` 里用链接脚本的栈符号调 `br_bt_set_stack_bounds()`;
  fault 分派路径用 `br_bt_capture_from(fp, pc, ...)` 捕获异常现场。

## 用例(见 `tests/smoke.toml`)

| id | 判据 |
|---|---|
| TC-DBG-010 | `noinline` 探针内捕获: 帧数 ≥ 2、所有 `pc != 0`、`fp` 16 对齐且严格递增、`out[0].depth == 0`, 且某帧 `pc ∈ [&bt_probe, &bt_probe+512)`(证明链穿过了调用点) |
| TC-DBG-011 | `capture_from(0,0) == 0`;`capture_from(0x1234, …) == 0` 且不死;非法 bounds 被拒、窄 bounds 掐断;`capture(buf,1) == 1`(max 生效);收尾复位 bounds, 不残留 |

## 已知欠账(不在本插件目录可修)

- `build/gen/service/backtrace/plugin_desc.c` 仍引用骨架符号
  `backtrace_early_init`/`backtrace_init`/`backtrace_start`(生成物; v0.1 不编译,
  按 V-9/ADR-0003 记账)。
- 声明面 init 注释的"复位栈范围"措辞与 platform `early_init` 设边界的时序冲突
  (见上, 待主控二选一)。

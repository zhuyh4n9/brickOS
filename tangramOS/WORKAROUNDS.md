# WORKAROUND 登记表 — tangram-prototype v0.1.0

> **为什么要有这张表**: 原型允许走捷径, 但不允许**悄悄**走捷径。
> 每条捷径在这里登记 id / 欠的是什么 / 为什么先欠着 / **还债的具体动作**。
> 代码里对应位置标 `WORKAROUND(<id>)`, 两侧由 `make check-workarounds` 绑死
> (源码标记 ↔ 本表条目, 任一侧多/少即报红)。

**v0.1.0 的三条欠债** —— 全部来自同一个根因:
`tg` 组合器与插件管理器**尚未存在**, 于是"插件化"这条主线在 v0.1.0 只能是手工的。

| id | 位置 | 欠的是什么 | 为什么先欠着 | 还债动作(退出条件) |
|---|---|---|---|---|
| tg-wa-entry-001 | platform/include/tg/platform/tg_plat.h<br>platform/src/aarch64/start.S<br>platform/src/aarch64/link.ld<br>platform/src/aarch64/console_pl011.c<br>platform/src/aarch64/timer_arch.c<br>platform/src/aarch64/plat_qemu_virt.c | **Platform Entry 不是插件**。reset 汇编 / 向量表 / 链接脚本 / 早期 console / arch timer 被 Makefile 直接编进镜像, 没有描述符、没有 manifest、没有 `tg add` | 设计侧 Platform 类是"每 SoC 一个插件"(1-01 §6.3), 但**插件发现与描述符是 `tg` 工具与插件管理器的产物**(4-02/4-03, 3-05)。工具不在, 插件化无从谈起 | ① `tg` 能按布局约定发现并校验插件(4-02); ② 本目录收敛为插件 `platform/qemu-aarch64`(带 `TG_PLUGIN` 描述符 + 声明片段); ③ `start.S` 的调用点从"Makefile 直编"改为 `.tg_plugins` 段枚举驱动(3-05 §2.2 的 `__tg_plugins_start/__tg_plugins_stop`) |
| tg-wa-boot-001 | core/src/startup/main.c<br>core/src/time.c<br>core/src/log.c | **启动链被压缩成一个死循环**。设计的 `core.init → plugin_manager(拓扑排序/环检测) → EARLY/CORE/LATE → 开中断 → start() → app.start() → tg_sched_run()`(1-01 §9)在 v0.1.0 **整条缺席**, 由 `tg_core_main()` 的 MainLoop 顶替; "睡眠"退化为忙等 `tg_delay_*`, 日志直写 console 而未经服务注册表 | 这条链的每一环都以"有插件"为前提(描述符段、注册表、调度器); 没有 `tg` 就没有第一阶段 | **`tg_core_main` 的归宿不是长大而是被拆掉**: 平台名打印 → 描述符枚举; `tg_log_init` → core.init 的一个步骤; 循环体 → `app.start()` 里的 APP 线程; 循环边的 `tg_delay_ms` → `tg_task_sleep`(M1) |
| tg-wa-toolchain-001 | Makefile | **工具链用外部 gcc**(宿主 `aarch64-linux-gnu-gcc`), 未经内部工具链 | 内部工具链未就绪; 原型阶段先验证"能不能跑起来" | 内部工具链就绪后**只改 `CROSS_COMPILE`**(或换 `CC`), 构建规则本身不动。同时补 `abi_id` 指纹(设计 2-02 §C5/TG-D5: 影响 ABI 的输入要可复现地记下来) |

## 相关但**不算** workaround 的事(避免误登记)

以下三件看起来像捷径, 实际都是**设计内的 M0 形态**, 所以不登记:

| 事项 | 为什么是设计内行为 |
|---|---|
| 早期 console 用**轮询**、不注册设备 | 设计 1-01 §8 明确 console 双形态: platform 早期 console(轮询) → I/O 插件完整 tty(中断驱动)。1-03 §1 里 M0 的 `io/uart-pl011` 也不注册设备 |
| 不开中断、没有中断控制器(GICv3) | v0.1.0 的 MainLoop 不需要中断; 中断框架是 M0 的事(设计 3-02) |
| 恒等映射/MMU/cache 维护全无 | 同上: MainLoop 不碰内存管理; region 表与页表是 platform 插件在 M0 之后的事 |

## 检查

```bash
make check-workarounds      # 源码标记 ↔ 本表, 不一致则非零退出
```

# WORKAROUND 登记表 — brickOS-prototype v0.1.0

> **为什么要有这张表**: 原型允许走捷径, 但不允许**悄悄**走捷径。
> 每条捷径在这里登记 id / 欠的是什么 / 为什么先欠着 / **还债的具体动作**。
> 代码里对应位置标 `WORKAROUND(<id>)`, 两侧由 `make check-workarounds` 绑死
> (源码标记 ↔ 本表条目, 任一侧多/少即报红)。

**v0.1.0 的三条欠债** —— 根因已经收窄: **组合期工具 `brickie` v0.1 已交付**
(发现/校验/描述符生成/接口发布/版本治理), 但**运行期的插件管理器与调度器仍未存在**
(M0), 所以"插件化"这条主线的**运行期**一半仍然欠着 —— 声明期一半已经由 brickie 接管。

| id | 位置 | 欠的是什么 | 为什么先欠着 | 还债动作(退出条件) |
|---|---|---|---|---|
| br-wa-entry-001 | platform/qemu-aarch64/plugin.toml<br>platform/qemu-aarch64/include/br/platform/br_plat.h<br>platform/qemu-aarch64/src/start.S<br>platform/qemu-aarch64/src/link.ld<br>platform/qemu-aarch64/src/console_pl011.c<br>platform/qemu-aarch64/src/timer_arch.c<br>platform/qemu-aarch64/src/plat_qemu_virt.c | **Platform 的插件化只完成了一半**。reset 汇编 / 向量表 / 链接脚本 / 早期 console / arch timer 仍由 Makefile 直接编进镜像; **manifest 与描述符已就位**(`plugin.toml` + 生成物 `build/gen/platform/qemu-aarch64/plugin_desc.c`, 由 `brickie gen` 重建), 但**调用点**还不是 `.br_plugins` 段枚举驱动 | 设计侧 Platform 类是"每 SoC 一个插件"(1-01 §6.3); 插件**发现与校验**已由 brickie v0.1 交付, 但**描述符段驱动的启动链**依赖插件管理器与调度器(M0 运行期), 它们不在 v0.1 | ① ~~`br` 能按布局约定发现并校验插件(4-02)~~ ⇒ **已还**(`brickie check`/`gen`, 见 `docs/decisions/0001-platform-plugin-manifest.md`); ② ~~本目录收敛为插件 `platform/qemu-aarch64`(带 `BR_PLUGIN` 描述符 + manifest)~~ ⇒ **已还**; ③ **仍欠**: `start.S` 的调用点从"Makefile 直编"改为 `.br_plugins` 段枚举驱动(3-05 §2.2 的 `__br_plugins_start/__br_plugins_stop`) —— 依赖插件管理器(M0) |
| br-wa-boot-001 | app/hello/src/main.c<br>core/src/time.c<br>core/src/log.c | **启动链被压缩成一个死循环**。设计的 `core.init → plugin_manager(拓扑排序/环检测) → EARLY/CORE/LATE → 开中断 → start() → app.start() → br_sched_run()`(1-01 §9)在 v0.1.0 **整条缺席**, 由 `br_core_main()` 的 MainLoop 顶替; "睡眠"退化为忙等 `br_delay_*`, 日志直写 console 而未经服务注册表 | 这条链的每一环都以"有插件"为前提(描述符段、注册表、调度器); 没有 `br` 就没有第一阶段。MainLoop 现已明确归属 APP 插件 `app/hello`(1-03 §1 的 M0 引导例外), 但阶段机与调度器仍未落地 | **`br_core_main` 的归宿不是长大而是被拆掉**: 平台名打印 → 描述符枚举; `br_log_init` → core.init 的一个步骤; 循环体 → `app.start()` 里的 APP 线程; 循环边的 `br_delay_ms` → `br_task_sleep`(M1) |
| br-wa-toolchain-001 | Makefile | **工具链用外部 gcc**(宿主 `aarch64-linux-gnu-gcc`), 未经内部工具链 | 内部工具链未就绪; 原型阶段先验证"能不能跑起来" | 内部工具链就绪后**只改 `CROSS_COMPILE`**(或换 `CC`), 构建规则本身不动。同时补 `abi_id` 指纹(设计 2-02 §C5/BR-D5: 影响 ABI 的输入要可复现地记下来) |

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

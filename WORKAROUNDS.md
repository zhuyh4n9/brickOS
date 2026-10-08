# WORKAROUND 登记表 — brickOS-prototype v0.1.0

> **为什么要有这张表**: 原型允许走捷径, 但不允许**悄悄**走捷径。
> 每条捷径在这里登记 id / 欠的是什么 / 为什么先欠着 / **还债的具体动作**。
> 代码里对应位置标 `WORKAROUND(<id>)`, 两侧由 `make check-workarounds` 绑死
> (源码标记 ↔ 本表条目, 任一侧多/少即报红)。

**v0.2.0 的欠债** —— 根因分四类(净计数见下方表格; `br-wa-entry-001` 已注销, 新增 `br-wa-test-001`):
① **运行期缺席**: `brickie` v0.1 已交付声明期(发现/校验/描述符生成/接口发布/版本治理),
   但**运行期的插件管理器、阶段机与调度器仍未存在**(M0/M1), 所以"插件化"与"启动链"
   这两条主线各欠着一半;
② **分层未抽出**: 设计把中断控制器方言、异常向量桩与**页表构造**归 **ISA 共享库(非插件)**,
   而原型还没有这一层, 于是它们暂居 platform 插件目录内(靠**文件边界**保住层次);
③ **生成链路未抽出**: 设计的"池比例 = manifest 预算"(3-04 §2)与"debug 呈现经 bridge"
   (5-01 §2)都还没有生成/传输链路, 于是前者写死在平台数据里、后者直写 console。

| id | 位置 | 欠的是什么 | 为什么先欠着 | 还债动作(退出条件) |
|---|---|---|---|---|
## 已注销(还清了)

> 注销 = 退出条件**全部**满足, 台账条目与源码标记一并撤下。历史留在这里, 免得"为什么当初要这么做"失传。

| id | 注销于 | 还清它的是哪一刀 | 证据 |
|---|---|---|---|
| `br-wa-entry-001` | v0.2.0(插件管理器 + brickie 构建) | ① `brickie check`/`gen` 按布局发现与校验插件(ADR-0001) ② 本目录收敛为插件 `platform/qemu-aarch64`(manifest + 生成物描述符) ③ 顶层 Makefile 不再直编镜像 —— 源集合改由 `[build].sources` 声明、`brickie build` 消费(ADR-0004) ④ **启动链的调用点改成 `.br_plugins` 段枚举驱动**: `start.S` 只调 `br_plugin_manager_run()`, 由它扫段 → Kahn 拓扑 → 相位驱动(`docs/decisions/0005-plugin-manager.md`) | `.br_plugins` 段 8 条描述符(0x300 B = 8×96); `[PLGCONF] 6/0`; `plugin-test` 门禁; `start.S` 里已无 `br_plat_early_init`/`br_core_main` 直调 |
| br-wa-boot-001 | app/hello/src/main.c<br>core/src/log.c<br>core/src/trace.c<br>core/src/mem/mem.c<br>platform/qemu-aarch64/src/irq_conf.c<br>platform/qemu-aarch64/src/mm_conf.c | **启动链的相位机与管理器已就位**(v0.2.0: `start.S` → `br_plugin_manager_run()` → EARLY/CORE/LATE → 开中断 → start(APP 最后) → `br_sched_run()`; 环检测/首败即停机/APP 最后都在内, 见 `docs/decisions/0005-plugin-manager.md`)。**仍欠三件**: ① **没有独立的 `core.init` 入口** —— 堆/中断框架/注册表/调度对象的初始化仍借 `platform.early_init` 与管理器头部(设计 `1-01` §9 把它们归 `core.init`); ② APP 仍**直读平台身份**(`br_plat_name/isa/timer_ticks`, 只为启动日志与 `irq_ticks` 判据) ⇒ `product.toml` 还剩一条 `allow_edges` 豁免; ③ 日志/trace **直写 console/RAM 环**, 未经服务注册表(与 `br-wa-debug-002` 同源) | 这三件都以"M2 的 iface-min / 服务化心跳"或"M3 的 debug bridge"为前提, 而它们不在本刀范围 | ① 把 `core.init` 抽成独立入口(堆/中断框架/注册表/调度对象各自归位) ② 平台身份与心跳**发布成服务**(service registry), 删掉最后一条 `allow_edges` ③ 日志/trace 经服务注册表(见 `br-wa-debug-002`) |
| br-wa-isa-001 | platform/qemu-aarch64/src/gicv3.c<br>platform/qemu-aarch64/src/vectors.S<br>platform/qemu-aarch64/src/mmu.c<br>core/include/br/core/br_exc.h | **ISA 共享库这一层还没有独立存在**。设计 1-01 §8 / 3-02 §1.3 把"控制器方言(GICv3 寄存器序列)+ 异常向量入口汇编"、3-04 §2 把"cache 一致性协议的 aarch64 实现"归 **ISA 共享库(可复用库, 非插件)**; 原型里它们与 platform 插件同目录(靠文件边界分层: `gicv3.c`/`mmu.c` 只有"换型号/换架构就变"的序列, 板级事实上在 `board_irq.c`/`memmap.c`/`plat_qemu_virt.c`); 异常帧布局(`br_exc.h`)因 core 的 extable fixup 必须改写现场而暂放 core | 抽成独立构建单元要动 Makefile 的源集合/包含路径, 而 **platform → isa 的依赖没有声明载体**(ISA 库不是插件, 无 `[[dep]]` 可表达); 收益是分层整洁, 不是本次任务的功能 | 建 `isa/aarch64/`(**非插件**)独立构建单元 + 把 `gicv3.c`/`vectors.S`/`mmu.c`/`br_exc.h` 迁入, 并在 `plugin.toml` 的声明面里表达"platform 使用该 ISA 单元"(机制待 4-02/2-01 定) |
| br-wa-toolchain-001 | product.toml<br>platform/qemu-aarch64/plugin.toml | **工具链用外部 gcc**(宿主 `aarch64-linux-gnu-gcc`), 未经内部工具链。目标事实(`[build.target].cross` 等)现在写在**声明面**里; 工具**候选序与解析在 `brickie-core`**(`data.tools[i]` 的 candidates/resolved, contract §9 R-14) | 内部工具链未就绪; 原型阶段先验证"能不能跑起来" | 内部工具链就绪后**只改声明面**(`product.toml` / platform 的 `[build.target].cross` 或 arch 事实), 构建规则本身不动 —— 候选序与解析归 core。同时补 `abi_id` 指纹(设计 2-02 §C5/BR-D5: 影响 ABI 的输入要可复现地记下来) |
| br-wa-mem-001 | platform/qemu-aarch64/src/memmap.c | **三池比例写死在 platform 的 region 表里**(heap 1 MiB / contig 256 KiB / page 1 MiB / DMA 256 KiB / 保留 16 KiB), 不是由 manifest 的 `[budget]`/`[[res]]` 预算生成; 页池位图与页表也都是编译期静态上界 | 设计 3-04 §2 说"三池划分…比例 = manifest 预算(主文档 §6.4 资源总账)"。**编译编排已落地**(`brickie build` 按声明面组合镜像, ADR-0003 的 S1–S3), 仍欠的是 **budget → region 的生成链路**: 声明面目前只有"怎么编", 还没有"按预算生成平台数据"这一步 | 组合器把 `[budget]`/`[[res]]` 变成 region 表(或 core 侧引入由 manifest 裁剪的池描述符); 判据 = 改 `product.toml` 的预算就能改池大小, 且 `brickie check` 的预算域随之执法 |
| br-wa-debug-001 | service/memleak/src/memleak.c | **memleak 是"归属标签"近似, 不是 v2 的 per-plugin arena 记账**: 有红区/毒化/双 free 拦截与"分配时记归属"的标签, 但**没有**预算上限、没有强制归属、没有 OOM 策略 | 5-01 §4 把 "per-plugin arena 记账" 排在 v2.0, 并把 "TLSF 红区 + freelist 毒化 + 栈 canary" 列为 v1.x 的便宜替代; 本次先交付"能报谁没还、在哪分配的"这一半(另一半要插件的资源预算真值) | v2 落地 per-plugin arena(分配带插件归属 + 预算 + 超限策略); 本插件的报告面**不变**(它消费的是 core 的观测契约), 只多出"超预算"一栏 |
| br-wa-debug-002 | service/dump/src/dump.c | **dump/trace 直写早期 console**, 未经 5-01 §2 的 debug bridge(COBS + 16-bit CRC 成帧 + `MEMRD`/`TRACE_READ` 命令面) | bridge 排在 M3(v1.0 插件清单的 `service/dbg-bridge`), 依赖 vfs-core 的 `/dev/uart0` 与 trace 服务; v0.1 没有这两个 | 把呈现层改为经 bridge 成帧输出(内容与边界判定不变, 只换落点); 判据 = 主机侧 `brickie dbg` 能收帧并解码 |
| br-wa-test-001 | core/src/plugin/plugin_mgr.c<br>core/src/sched/sched_core.c<br>core/src/sync/sync.c | **三套目标侧一致性用例的 `TC-*` id 是自编号, 与设计 `6-01` 的用例表尚未逐条对齐**(含义漂移: 例如本刀的 `TC-TASK-004` 是"exit 后 ZOMBIE", 而 `6-01` §3.1 的 `TC-TASK-004` 是"sleep(1ms) 实测 ≥1ms"); 日志里 id 后面的**自述文字是准确的**, 不会误导读日志的人, 但"按 id 检索"会对不上 | `6-01` 的用例表是**验收契约**, 而本刀的用例是边实现边补的(先保证判据真、再对齐编号); 对齐需要一次集中改名 + 补齐设计要求但尚未实现的用例, 属文档/用例债而非功能债 | ① 三套套件的 id 与 `6-01` §3.1/§3.2/§3.3 逐条对齐(自编号那部分改到非 `TC-` 命名空间, 如 `SYNCCONF-NN`); ② 补齐设计有、本刀无的用例: `TC-TASK-002/003`(attr 校验 / 二次 join)、`TC-TASK-004/005`(sleep 不早醒 / sleep_until 已过期)、`TC-SYNC-003`(lock_to(1ms) 耗时 ≥1ms)、`TC-TIME-002`(单位校准 1.0–2.0s)、`TC-TIME-003`(deadline 饱和); ③ 让主机侧套件按 `6-01` 的 id 打 PASS 行(`6-01` 的"平台"列里 TC-TASK/TC-SYNC/TC-TIME 多数标 **host**) |

## 相关但**不算** workaround 的事(避免误登记)

以下看起来像捷径, 实际都是**设计内**的形态, 所以不登记:

| 事项 | 为什么是设计内行为 |
|---|---|
| 早期 console 用**轮询**、不注册设备 | 设计 1-01 §8 明确 console 双形态: platform 早期 console(轮询) → I/O 插件完整 tty(中断驱动)。1-03 §1 里 M0 的 `io/uart-pl011` 也不注册设备 |
| **SLOW 级联域在 v0.1 不可用**(`br_irq_domain_create` 返回 NULL) | 设计 3-02 §1.1.1 的两段计: SLOW 域的**存在前提就是 bh**(Stage 2 / v1.0 起可用)。拒绝并留痕正是"分期边界的定义", 不是缺陷 |
| **`br_fault_handler_register` 未实现**(仅有签名) | 设计 3-02 §1.1/§10.5: fault 的"分槽/分类/extable/panic"属 Stage 1, **注册 API 归 v2** |
| **`board_irq.h` 是手写静态头**(不是组合器生成物) | 设计 3-02 §3.1(IR-2)明示的**退路**: "若生成头机制未落地: platform 插件导出静态头, 内含同型宏。语义等价, 仅'谁生成'不同" |
| **`BR_IRQ_DISPATCH_BH/_THREAD` 接受但忽略** | 设计 3-02 §3.4 原文如此: "Stage 1 置位不报错但忽略(无 bh), trace 提示" |
| 恒等映射/MMU/cache 维护全无 | MainLoop 不碰内存管理; region 表与页表是 platform 插件在其后的事 |
| 描述符池/运行期表/域池/trace 环是 **core 内的静态数组**(非 manifest 裁剪) | 设计 3-02 §14.6 允许 manifest 裁剪 `BR_IRQ_MAX` 等; v0.1 没有 manifest→core 的生成链路, 故取设计默认值(64 线) |

## 检查

```bash
make check-workarounds      # 源码标记 ↔ 本表, 不一致则非零退出
```

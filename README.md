# brickOS-prototype v0.1.0

brickOS 的**可运行原型**。分支 `brickOS-prototype-v0.1.0`(从 `main` 开出)。

> **仓库是"设计 / 原型"双线分行的**:
> - 设计文档在 **`brickOS-Design`** 分支(`docs/` + `comment/`);
> - 原型代码在本分支(仓库根 `brickOS/`), `main` 上只有 `LICENSE`。
>
> 所以本分支**没有** `docs/`。下文引用设计文档时写的是"章节号", 去 `brickOS-Design`
> 分支上读(如 `docs/3-os-core/3-01-core-api-list.md` = `3-01`)。
>
> **落点约定**: 开发中的原型树 = 仓库根 `brickOS/`(core / platform 两个顶层);
> 一个版本稳定后再冻结为 `brickOS-prototype-v<版本>/`(或按分支/标签冻结), 冻结件只读。

---

## 1. v0.1.0 做什么

**一条链, 三个环节:**

```
(Platform 插件) Start.S ─► br_core_main ─► 四阶段启动链(ADR-0008)
                                            │
    ① core 平台无关(时钟/日志/中断框架/插件管理扫段)
    ② platform 插件初始化(早期 console + GICv3 PIC + IRQ 绑定表 + region 表 + 页表 ops)
    ③ core 依赖平台(三池认领 br_mem_init + 恒等映射页表建立/开 MMU)
    ④ plugin_manager: EARLY → CORE → LATE → 开中断 → START(APP 最后)
         → **自检 pass**(ADR-0010: 逐插件 selftest, 全部 start 之后)
         → br_sched_run() → APP 线程 MainLoop(延时 + 日志 + 心跳)

运行时(ADR-0011 之后)多两条路径, 它们都发生在**异常出口**上:
    timer ISR ──► br_sched_on_tick ──► 超时扫描 + 调度器 on_tick(时间片--)
                                            └─ 时间片用尽 ⇒ need_resched
    eoi ──► 下半部 br_work_drain(有界) ──► br_sched_irq_epilogue(抢占换栈) ──► ERET
```

| 环节 | 内容 | 代码 |
|---|---|---|
| **Platform Entry**(`platform/qemu-aarch64` 插件) | reset、选核、EL2→EL1 降级、栈、**异常向量表**、BSS 清零 | `platform/qemu-aarch64/src/start.S` / `vectors.S` |
| | 早期 console(PL011 轮询)、arch timer 读数 | `platform/qemu-aarch64/src/console_pl011.c` / `timer_arch.c` |
| | **GICv3 驱动**(`br_pic_ops` 方言) + 板级 IRQ 绑定表 + 中断初始化/开跑 | `platform/qemu-aarch64/src/gicv3.c` / `board_irq.c` |
| | **4 KiB 恒等映射页表**(4 级 / TTBR0 / MAIR / 低 1 GiB Device 块 + 8 MiB RAM 页窗)+ **region 表** + 开 MMU | `platform/qemu-aarch64/src/mmu.c` / `memmap.c` |
| | **中断一致性用例**(target-only, 编进镜像) | `platform/qemu-aarch64/src/irq_conf.c` |
| | **内存/MMU 一致性用例**(target-only) | `platform/qemu-aarch64/src/mm_conf.c` |
| | 链接脚本(含 `.br_extable` 收集) | `platform/qemu-aarch64/src/link.ld` |
| **框架件**(`framework/`) | `vfs-core`(纯 VFS)/ `dev-core`(通用设备注册表)/ `cdev-core`(字符设备 + 会话适配) | `framework/*/src/*.c`(+ 各自的 `*_selftest.c`) |
| **存储**(`fs/`) | `tmpfs`(**rootfs "/"**)、`devfs`(**/dev** 设备节点投影) | `fs/tmpfs/src/tmpfs.c` / `fs/devfs/src/devfs.c` |
| **I/O**(`io/`) | `uart-pl011`: PL011 注册为 cdev ⇒ `/dev/uart0` | `io/uart-pl011/src/uart_pl011.c`(+ `uart_selftest.c`) |
| **APP**(`app/hello` 插件) | MainLoop: 每秒打一行日志, 延时自带"不早醒"判据; 只**读**平台的心跳计数(纯 P0 消费者)。**不再驱动任何用例**(ADR-0010: 自检归 core) | `app/hello/src/main.c` |
| **调试服务插件**(`service/*`) | `trace`(core 16B 事件环的唯一消费方)/ `backtrace`(x29 帧链捕获)/ `hexdump`(16 B/行契约格式)/ `memleak`(按归属标签出账)/ `dump`(现场编排 + `[DBGCONF]` 入口) | `service/*/src/*.c` |
| **调度器**(`sched/`) | `rr`(**时间片轮转抢占**, 镜像选它): FIFO 就绪队列 + 每线程时间片, 用尽即请 core 在 IRQ 出口换栈; `coop`(协作式, 留树/宿主用例覆盖): 只在显式点换栈 | `sched/rr/src/rr.c` / `sched/coop/src/coop.c` |
| **Core**(内核本体, 不是插件) | **中断框架 Stage 1 + 下半部**: 号空间/描述符池、生命周期(ack→ISR→eoi 单出口)、三层屏蔽、优先级语义、级联域(**FAST 与 SLOW**)、fault/extable、最小 trace 环; **按线的 BH 分发**与 **IRQ 出口的 workqueue drain**(ADR-0011) | `core/src/irq/*.c`, `core/src/work/work_core.c`, `core/src/{panic,trace}.c` |
| | **调度框架**: 线程状态机 / 切换 asm / 超时唤醒 / idle / **IRQ 出口的抢占换栈** / bh 禁令 | `core/src/sched/{sched_core.c,switch.S}` |
| | **内存**: TLSF 堆(红区 + 毒化 + owner 记账)、contig/DMA 池、4 KiB 页位图池、region 表 + `br_mm_ops` 派发 + cache 维护 | `core/src/mem/*.c`, `core/src/mm/mm.c` |
| | 时钟换算(us)+ 忙等延时 | `core/src/time.c` |
| | 日志(格式化 + 等级过滤), 不走 libc printf | `core/src/log.c` |
| | **宿主侧内存语义门禁**(算法性质压测, 不需要 QEMU) | `tests/host/mem_test.c` |

**验证目标**(不是"能编译"): 镜像能在 QEMU virt 上从 reset 跑到 MainLoop, 日志时间戳单调,
且每次延时**实际不短于请求值**(设计 `3-01 §2.1` 的"不早醒"语义)。后一条是**自动化判据**,
不是人眼看着差不多 —— `make smoke` 会 grep 它。

**八套一致性用例**(共 60+ 个 tag)都有**自动化判据**, 而且是**真跑硬件/真跑页表路径**(不是静态检查)。
ADR-0010 之后它们**全部**由 core 的**自检 pass** 统一驱动(在各插件的 `src/*_selftest.c`,
经描述符的 `selftest` 钩子; 开关 = `product.toml [selftest]`, 关掉则测试代码被 `--gc-sections`
裁出镜像):

| 套件 | 内容 | 门禁 |
|---|---|---|
| `[IRQCONF]` 91 项 | 中断逐用例(设计 `6-01 §3.7` 的 `TC-IRQ-*` + GICv3 方言事实; ADR-0011 起含 **BH 分发 / 抑制不重放 / SLOW 域走 bh**) | `irq-test` |
| `[MEMCONF]` 35 项 | 恒等映射 / RO / NX / 未映射 fault(§3.5/§3.6 的 `TC-MEM-*`/`TC-MM-*`) | `dbg-test` |
| `[DBGCONF]` | 调试域(`TC-DBG-*`: trace/backtrace/hexdump/dump/memleak 各自报自己的) | `dbg-test` |
| `[PLGCONF]` 7 项 | 插件管理器: 段条数 / 拓扑序 / 相位单调 / 环检测负例 / APP 最后 / **自检时机** | `plugin-test` |
| `[SVCCONF]` 3 项 | 服务注册表: 发布 / 查找 / 错误码(`br_svc.h`) | `plugin-test` |
| `[TASKCONF]` 14 + `[SYNCCONF]` 12 | 调度框架与所选调度器(真线程 create/yield/join/sleep; **按 `ops.kind` 分叉**: coop 跑 TC-TASK-101, rr 跑 TC-TASK-102/103) | `sched-test`/`sync-test` |
| `[WQCONF]` 8 项 | **下半部/工作队列**(ADR-0011): 队列有界/FIFO/预算/非重入/bh 上下文与禁令(`TC-WQ-*`) | `sched-test`(宿主 `work-test` 是第二条腿) |
| `[VFSCONF]` 13 + `[DEVCONF]` 6 + `[CDEVCONF]` 8 + `[IOCONF]` 12 | 存储/设备域: 挂载表/走查链/文件面/目录面 + 设备注册表 + 会话适配 + PL011 经 `/dev/uart0` 往返 | `fs-test` |

每道 QEMU 门禁都额外要求 `[SELFTEST] SUMMARY … ran=[1-9]… fails=0 errors=0` ——
它同时证明"自检被驱动过"与"开关是打开的"(关掉时该行是 `ran=0`, 匹配不上 ⇒ 门禁红)。
**自检失败不停机**(ADR-0010 §2.4: 它是观测, 红绿由门禁判; 这与 init/start 的"首败即停机"
是两条不同的纪律)。

其余判据:
- `make mem-test` —— 把 `core/src/mem/*` + `core/src/mm/mm.c` 编成**宿主可执行**, 跑 20 万次随机
  交错分配/释放 + 参考模型对拍(TLSF 的合并/分裂/碎片与页位图的 run 分配是算法性质, 宿主上几秒
  能跑上百万次操作);
- arch timer 的 PPI(INTID 30)在跑, 日志的 `irq_ticks` 每秒 +10 —— 真实中断投递的活证据。

## 2. v0.1.0 刻意不做什么

原型最容易失控的地方是"顺手多做一点"。以下是**明确不做**的, 连同理由:

| 不做 | 理由 |
|---|---|
| ~~**插件化(描述符 / manifest / 组合器)**~~ | **已交付**(v0.2.0): 声明期 + 编译编排由 brickie 接管(插件发现/校验/描述符生成/接口发布/版本治理/`brickie build`, 见 [`tools/brickie/README.md`](tools/brickie/README.md)); **运行期**的插件管理器 + `.br_plugins` 段 + 四相驱动由 ADR-0005 落地, 启动链入口由 ADR-0008 收敛为 `br_core_main`, 自检由 ADR-0010 统一驱动。此行的历史留档见 §5 的"已注销" |
| ~~调度器 / 线程 / `br_sched_ops`~~ | **已交付**: core 的调度框架(ADR-0006)+ `sched/coop`; ADR-0011 追加 `sched/rr`(时间片抢占)与 **IRQ 出口的抢占换栈**。镜像选 `sched/rr`(一次只能装一个调度器: `br_sched_register` 恰一次) |
| **中断的 Stage 2** —— **分发到 bh 已交付**, 其余仍未做 | 设计 `3-02 §1.1`: Stage 1(无调度器世界)已落地; **`BR_IRQ_F_DISPATCH_BH` 与 SLOW 级联域已在 ADR-0011 落地**(ISR 推迟到下半部, 处理期间 mask 同线)。**仍未做**: `BR_IRQ_F_DISPATCH_THREAD`(线程化 IRQ)、§12.3 动态升级、§13 亲和性/负载均衡/IPI(v2b)、`CAP_NEST` 嵌套、PM save/restore |
| `br_fault_handler_register`(fault handler 链) | 设计 `3-02 §10.5`: 分槽/分类/extable/panic 属 Stage 1(已做), **注册 API 归 v2** |
| ~~**SLOW 级联域**(状态需总线事务的 PMIC 型)~~ | **已交付**(ADR-0011): bh 落地后它**可用** —— 父线 ISR 只做"单飞 + mask + 提交", demux 在**下半部**读状态寄存器(`[IRQCONF] TC-IRQ-102` 的判据从"明确拒绝"翻成"可用且状态从未在 ISR 里被读") |
| MMU 的**重定位**(镜像加载任意 VA)/ MPU 目标(vx) | 设计 `3-04 §1` 的虚拟内存政策: v1 = **恒等映射 + 属性隔离**(已落地); 重定位归 v2, MPU 归无 MMU 目标的实验 |
| per-plugin arena **预算**(memleak 的 v2 形态) | 现在给的是 v1.x 便宜层(红区 + 毒化 + 双 free 拦截)加一个"归属标签"近似: 能报**谁没还、在哪分配的**, **不能**报"某插件超预算"(无上限/无强制归属/OOM 策略)—— 见 ADR-0003 §2.8 与 `br-wa-debug-001` |
| debug bridge(COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面) | `5-01 §2` 的 M3。现在 dump/trace **直写早期 console**(同类约束照守: 只用静态缓冲、不碰堆、只读), 见 `br-wa-debug-002` |
| 栈 canary / 目标侧完整 ASan | `5-01 §4` 的分层: 红区 + freelist 毒化已在; 栈 canary 需编译器插桩, 完整 ASan 是 host 平台插件的白捡项与 vx 实验 |
| `br_fault_handler_register` 的消费方(ramdump) | v2(`3-02 §10.5`); 挂接点与现场结构已在, 缺的是注册 API 与捕获插件 |
| `br_mm_map`/`br_mm_unmap` 的**运行期实现** | v1 按设计返回 `-ENOTSUP`(`6-01` 的 `TC-MM-003` 就是这个期望): 签名先行、实现随 v2 重定位; 运行期**改属性**走 `br_mm_set_attrs`(已可用, 用例用它做真 RO 保护验证) |
| 设备注册 / devfs / cdev | M2。早期 console 是**轮询**形态 —— 这是设计内的 M0 形态(`1-01 §8` console 双形态), **不是** workaround |
| libc / svc-posix | `-nostdlib -ffreestanding`; 只有 `stdarg.h`(编译器自带) |

## 3. 目录结构

```
brickOS/
├── Makefile                     只编工具 + 镜像侧的过渡别名(真身 = brickie build)
├── README.md                    本文件
├── WORKAROUNDS.md               WORKAROUND 登记表(欠债清单)
├── product.toml                 产品声明: app 选择 / 插件选择 / 预算 / M0 引导例外豁免
├── mk/
│   └── host.mk                  宿主三元组 + 宿主产物目录(build/host/… 与 prebuilts/…)
├── docs/decisions/              决策记录(ADR): 0001 插件化 / 0002 中断 / 0003 内存 / 0004 构建 /
│                              0005 插件管理器 / 0006 调度 / 0007 同步 / 0008 core 启动链 /
│                              0009 VFS 存储栈 / 0010 **插件自检(selftest)**
├── prebuilts/toolchain/                    外部工具链**下载缓存**(派生, 不进库; fetch-prebuilt.py)
├── prebuilts/                   宿主工具**自举种子**(进库; 见下, 详见其 README)
│   └── seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}
├── tools/
│   ├── host-detect.sh           宿主 arch/os 探测(布局映射的唯一真值)
│   ├── check-workarounds.sh     源码标记 ↔ 登记表 一致性检查
│   ├── check-build.sh           构建接线门禁(宿主产物出树 / 自举种子进库)
│   └── brickie/                 【组合期工具】brickie(原名 `br`; 详见其 README)
│       ├── Makefile             宿主 g++/cargo + python3, **不碰交叉工具链**
│       ├── freeze.py            打包器: Python 包 + 模板 + schema + 原生工具 → 嵌入 ELF 的载荷
│       ├── cxx/                 L2 渲染器 + L5 入口 ELF 的源码(产物出树, 见下)
│       ├── rust/                L0/L1 Rust 核心 brickie-core(判定 / 求解 / 版本 / IFACE-IR)
│       ├── python/brickie/      L5 前端(子命令 / 输出 / 退出码 / 文件编排)
│       ├── schema/*.schema.json 声明面形状(机器可读)
│       ├── docs/contract.md     跨语言接口契约(唯一权威)
│       ├── templates/           骨架模板(四类 × c)
│       └── tests/               端到端用例
├── app/hello/                   【APP 插件】M0 MainLoop(镜像唯一 app; M0 引导例外)
│   ├── plugin.toml              人写 ← 插件级唯一真值
│   └── src/main.c               MainLoop(延时 + 日志 + 读心跳计数; 原 core/src/startup/main.c)
├── core/                        【Core】内核本体(**不是插件**; 被 [compat].core 引用)
│   ├── include/br/core/
│   │   ├── br_types.h           基础类型(不用 <stdint.h>)
│   │   ├── br_error.h           负 errno 子集(设计 3-01 §11)
│   │   ├── br_version.h         版本标识
│   │   ├── br_console.h         早期 console 契约(接口在 core, 实现在 platform)
│   │   ├── br_log.h             日志契约
│   │   ├── br_time.h            时钟/延时契约
│   │   ├── br_irq.h             【中断】native API(9 件)+ 域 API + 绑定表 + 状态字
│   │   ├── br_pic.h             【中断】PIC 填表契约(core ↔ ISA 方言, 非 native 面)
│   │   ├── br_exc.h             异常帧布局(asm 桩 ↔ fault 路径的同一处真值)
│   │   ├── br_fault.h           fault 分类 / extable 宏 / panic 两入口
│   │   ├── br_trace.h           最小 trace 环(ISR-safe 留痕)
│   │   ├── br_mem.h             【内存】native 11 件 + 观测契约(堆遍历/自检)+ 归属记账
│   │   ├── br_mm.h              【内存】region 表/属性 + MMU 派发 + 平台侧 br_mm_ops_t
│   │   ├── br_plugin.h          【插件】元契约(描述符/依赖边/相位)+ 插件管理器入口
│   │   ├── br_svc.h             【插件】服务注册表(publish/lookup + 观测)
│   │   ├── br_sched.h           【调度】线程/任务 + br_sched_ops 注册点 + idle
│   │   ├── br_sync.h            【调度】mutex / sem / cond / spinlock
│   │   ├── br_work.h            【调度】延迟工作(bottom half): br_work_submit/drain + bh 契约(ADR-0011)
│   │   └── br_main.h          【启动】core 启动入口契约: br_core_main() 四阶段链(ADR-0008)
│   └── src/
│       ├── log.c                格式化 + 等级过滤
│       ├── time.c               时钟换算 + 忙等延时 + br_deadline_from_now
│       ├── main.c               ★ core 启动入口: 四阶段启动链(main.c 的 br_core_main)
│       ├── plugin/              插件管理器(扫段/拓扑/四相/自检 pass)+ 段边界符号
│       ├── svc/                 服务注册表
│       ├── sched/               调度框架(TCB/切换 asm/超时表/idle/IRQ 出口抢占)+ 等待链原语
│       ├── sync/                同步原语(mutex/sem/cond/spinlock)
│       ├── work/                延迟工作: 有界静态环 + 非重入 drain + bh 标志/统计(ADR-0011)
│       ├── panic.c              br_panic_bare / br_panic(无锁/无堆/无调度器)
│       ├── trace.c              16 B 定长事件的环形缓冲
│       ├── string.c             ★ 编译器支持例程(memcpy/memmove/memset/memcmp)
│       ├── string_internal.h    上面的原型(core 私有; 它们不是 native API)
│       ├── panic_internal.h     panic 重入护栏(fault 路径判定"正在 panic 打印")
│       ├── mem/                 【内存】三池 + 堆
│       │   ├── tlsf_internal.h  TLSF 参数字典 + 单实例接口(core 私有)
│       │   ├── tlsf.c           纯 TLSF(二级位图/分裂/立即合并)
│       │   ├── mem.c            br_mem_init + malloc 族 + contig/DMA + 红区/毒化 + owner 记账
│       │   └── page.c           页位图 + 连续 run 首次适配
│       ├── mm/                  【内存】region 表 + MMU 派发
│       │   └── mm.c             region_add/find + map/unmap(-ENOTSUP)+ cache + ops 派发
│       └── irq/                 【中断框架 Stage 1 + 下半部(ADR-0011)】
│           ├── irq_internal.h   描述符/运行期表/CPU-local/域 的结构(32 B / 8 B 静态断言)
│           ├── irq_pic.c        PIC 注册表 + 绑定表 + hwirq→virq(有序表 + 二分)
│           ├── irq_core.c       描述符池 / lock-unlock / register-enable-disable / 风暴 / 入口 /
│           │                    **BH 分发 + §11.4.1 抑制 + 出口 drain**
│           ├── irq_domain.c     级联域池 + 窗口切片 + 一份 demux(FAST=ISR / **SLOW=bh**)+ 逐子 ack
│           └── irq_fault.c      fault 入口 + extable 查找 + double-fault 兜底
│   └── selftest/                【自检套件】(ADR-0010: 与生产代码分离; 开关关掉则整个目录不编译)
│       ├── core_selftest.c      唯一入口 br_core_selftest(): 汇总下面四套(弱引用, 见 plugin_mgr)
│       ├── plugin_selftest.c    [PLGCONF] 段条数/拓扑序/相位单调/环检测负例/APP 最后/自检时机
│       ├── svc_selftest.c       [SVCCONF] 服务注册表
│       ├── sched_selftest.c     [TASKCONF] 调度框架 + 所选调度器(真线程; 按 kind 分叉)
│       ├── sync_selftest.c      [SYNCCONF] 同步原语 + 时间
│       └── work_selftest.c      [WQCONF] 下半部/工作队列(有界/FIFO/非重入/bh 禁令)
├── sched/                       【调度器插件】(ADR-0006/0011)
│   ├── rr/                      时间片轮转**抢占**: FIFO 队列 + 每线程时间片(镜像选它)
│   └── coop/                    协作式: 只在显式点换栈(留树; 宿主 sched-test 覆盖其分界)
├── framework/                   【框架件插件】(设计 D19: 能力框架 = 插件身份 + core 纪律)
│   ├── vfs-core/                **纯 VFS**(D21): br_file_t 句柄 + 挂载表单路由 + 四层 ops + lookup 走查
│   │                            (+ src/vfs_selftest.c: TC-VFS-* 自检, 开关见 product.toml [selftest])
│   ├── dev-core/                通用设备注册表: 唯一扁平命名空间 + 子分类注册协议 + open_file 钩子透传
│   └── cdev-core/               字符设备子分类: br_cdev_ops + flash 子型 + **通用 br_file_ops 会话适配**
├── fs/                          【FS 插件】(依赖 vfs-core; 顶层 = namespace)
│   ├── tmpfs/                   **rootfs("/")**: RAM 文件系统(完整文件语义, 预建 /dev /data /tmp)
│   └── devfs/                   **/dev**: 把 dev-core 注册表投影成节点(实时枚举, open 经类钩子)
├── io/                          【I/O 插件】(向 cdev-core/bdev-core 注册设备)
│   └── uart-pl011/              PL011 注册为 cdev ⇒ /dev/uart0(M2 形态 A 侧; v1 轮询, 见 br-wa-io-001)
├── service/                     【调试服务插件】(每个目录 = 一个插件; 顶层 = namespace)
│   ├── trace/                   core 16 B 事件环的唯一消费方(drain/解码/计数/呈现)
│   ├── backtrace/               x29 帧链捕获(符号化归 host 离线工具)
│   ├── hexdump/                 16 字节/行契约格式(地址 + hex + ASCII)
│   ├── memleak/                 按归属标签出泄漏账 + 红区/毒化违约计数
│   └── dump/                    现场编排(regions/heap/leaks/trace/backtrace)+ [DBGCONF] 入口
├── tests/host/                  【宿主侧用例】不需要交叉工具链/QEMU
│   ├── mem_test.c               TLSF/页位图/region 表: 20 万次随机操作 + 不变量
│   ├── string_test.c            编译器支持例程: 语义 + 越界哨兵 + 参考实现对拍
│   └── host_stubs.c             br_log_write / br_console_* 的宿主替身
└── platform/qemu-aarch64/       【Platform 插件】与平台/ISA 绑定的部分
    ├── plugin.toml              人写 ← 插件级唯一真值(声明两个接口单元: #plat / #gicv3)
    ├── include/br/
    │   ├── board_irq.h          板级 virq 名(设计 3-02 §3.1 的"退路": 手写静态头)
    │   └── platform/
    │       ├── br_plat.h        Platform Entry 契约(含 IRQ/内存初始化 + 两套一致性入口)
    │       ├── br_gicv3.h       GICv3 方言契约(ISA 层)
    │       └── br_mmu.h         页表/MMU 观测面 + 窗口常量(ISA 层)
    ├── src/
    │   ├── start.S              入口: reset / 选核 / BSS / 交 core(br_core_main)
    │   ├── vectors.S            16 槽异常向量表 + 保存/恢复桩(ISA 层)
    │   ├── link.ld              链接脚本(text/rodata/data + .br_extable 收集 + .stack)
    │   ├── plat_qemu_virt.c     平台身份 + early_init + 异常兜底
    │   ├── console_pl011.c      PL011 轮询 putc
    │   ├── timer_arch.c         CNTFRQ_EL0 / CNTPCT_EL0
    │   ├── gicv3.c              GICv3 方言(实现 br_pic_ops_t)
    │   ├── board_irq.c          绑定表 + 中断初始化 + timer PPI 心跳 + 触发/hwirq 查询
    │   ├── irq_conf.c          中断一致性用例(TC-IRQ-*, 编进镜像)
    │   ├── memmap.c            region 表声明(镜像/三池/MMIO/保留区; 只声明不认领)
    │   ├── mmu.c               4 KiB 恒等映射页表 + 开 MMU + br_mm_ops(ISA 层)
    │   └── mm_conf.c           内存/MMU 一致性用例(TC-MEM-*/TC-MM-*, 编进镜像)
    └── tests/smoke.toml         用例骨架(v0.1 不消费; 已登记 TC-IRQ 梗概)
```

跨层边现在只有**一条**(在 `product.toml [lint].allow_edges` 里列名豁免; 另两条已被
ADR-0008/0010 消除 —— 见下):

- **Platform Entry → Core**: `start.S` 只做 reset/BSS, 然后 `bl br_core_main()`
  (core 的四阶段启动链: ① 平台无关初始化 → ② platform 插件初始化 → ③ 堆 + 地址映射的
  建立 → ④ 插件相位驱动; 见 `docs/decisions/0008-core-main-boot-chain.md`);
- ~~**APP → VFS(存储域总套件)**~~ ⇒ **ADR-0010 之后已删**: 自检改由 core 统一驱动,
  APP 不再认识任何框架件 ⇒ `allow_edges` 回到**一条**(只剩下面的 app → platform)。
- **APP → Platform**: `app/hello` 只直读平台身份与心跳(`br_plat_name()` / `br_plat_isa()` /
  `br_plat_timer_ticks()`)—— 一致性用例的调用点 ADR-0010 之后全部搬走, APP 不再调任何
  用例。这条边是 `product.toml [lint].allow_edges` 里**最后一条** M0 引导例外
  (正解是 iface-min(M2))。
  `service/dump` 的 `br_dump_all()`/`br_dump_trace()` 曾是 APP 的另一个面, 现在也不在
  APP 里 —— 启动快照由 dump 自己的 LATE init 打(见 `service/dump/README.md`)。

**中断的三层归属**(设计 `1-01 §8` / `3-02 §1.3`)在本原型的落点: 机制在 **core**、
GICv3 寄存器序列在 **`gicv3.c` + `vectors.S`**(设计归 ISA 共享库, 原型同目录 + 文件边界,
欠债 `br-wa-isa-001`)、绑定表/基址/静态 prio 在 **platform 数据**。

## 4. 构建与运行

构建的**入口是 `brickie build`**(ADR-0003 的 S1/S4 + ADR-0004), 顶层 `Makefile` 只编工具:

```
① tools/    工具段(顶层 `make`, 缺省目标)  宿主 g++ 编 C++ 渲染器 + Python 前端;
                                            cargo 编 Rust 核心 → 合并进同一个自包含入口 ELF
                                                          │  不碰交叉工具链(组合期零编译依赖)
                                                          ▼
② 声明面    product.toml [build] + 各插件 plugin.toml [build] + platform 的 [build.target]
            + tests/gates.toml(门禁)  ── 这是**镜像的唯一真值**
                                                          │
                                                          ▼
③ 镜像       `brickie build`(入口 ELF): 组合期 check → gen 生成物 → 编译/链接
            → `[build].post` 门禁; 步骤计划由 brickie-core 给, L5 只执行
```

**`make` 是"编工具", `make all` 是"编镜像"的过渡别名**: 顶层 Makefile 里
`all` / `run` / `smoke` / `irq-test` / `mem-test` / `string-test` / `dbg-test` /
`check-string` / `check-headers` / `size` / `disasm` / `clean-brickos` 都只是**薄委派**
(调 `$(BRICKIE) build|run|test …|size|disasm|clean`), 真身在 `brickie build` 与
`tests/gates.toml`。源码集合 / 编译标志 / 链接规则**不再**出现在顶层 Makefile 里 ——
它们由声明面决定(见 [docs/decisions/0004-brickie-build-and-make-retirement.md](docs/decisions/0004-brickie-build-and-make-retirement.md))。

这条"工具只编工具 / 镜像只委派 / 声明面是唯一真值"的纪律由 `make check-build` 把关 ——
接线坏起来通常是**静默**的(比如把镜像源码或标志又写回 Makefile, 构建就有了第二处真值),
所以钉成六条能真报红的机械判据(缺省目标=工具 / 无字面源码与标志 / 委派点齐全 /
工具段零交叉依赖 / 宿主产物出树 / 自举种子三件齐)。

**产物落点(参考 Android)**: 工具是**宿主**程序, 一律出树到
`build/host/<host-arch>/<host-os>/` 下, 与镜像产物(`build/obj`、`build/brick.*`)
分居 `build/` 两侧, 源码树里不留任何 `.o`/可执行文件:

```
build/host/<host-arch>/<host-os>/bin/brickie        # 单文件自包含 ELF(前端 + 模板 + schema + 两个原生工具)
                              …/bin/brickie-gen     # L2 渲染器(宿主可执行)
                              …/bin/brickie-core    # L0/L1 Rust 核心(宿主可执行)
                              …/lib/libbrickie-gen.a # 宿主静态库
                              …/obj/cxx/*.o          # 中间产物
```

`<host-arch>` = 处理器架构(`x86-64` / `aarch64` / …), `<host-os>` = 操作系统
(`linux` / `darwin` / `win`)。映射只在 `tools/host-detect.sh` 一处; `mk/host.mk`
把它变成 make 变量, `python/brickie/hostinfo.py` 是它的镜像(由用例断言同口径)。
查当前宿主:

```bash
make print-host-triple        # x86-64/linux
make print-host-bin-dir       # …/build/host/x86-64/linux/bin
make print-prebuilt-bin-dir   # …/prebuilts/seed/brickie/x86-64/linux/bin
```

**Python 前端 + 原生工具都在一个 ELF 里**: `brickie` 是**单文件自包含入口 ELF** ——
由 `tools/brickie/cxx/launcher.cpp` 把 Python 包、模板、`schema/**` 与**两个原生工具**
(`brickie-gen`、`brickie-core`)经 `tools/brickie/freeze.py` 打成未压缩 tar 后嵌进二进制,
运行时解包到临时目录再用系统 `python3` 解释, 并把 `BRICKIE_GEN` / `BRICKIE_CORE` 指到
解包出来的内嵌工具。于是**只拷 `brickie` 一个文件**就能跑: 不需要 `PYTHONPATH`、
不需要源码树、不需要同目录的原生工具、不需要 `g++`/`cargo`; 且**零新增第三方依赖**
(不用 PyInstaller/Nuitka)。详见 [tools/brickie/README.md](tools/brickie/README.md) 与 ADR `0004` §7。

**自举种子(进版本库, `prebuilts/`)**: 除"本机刚编的" `build/host/**` 外, 同一套
宿主三元组下还随源码提交预编译件:

```
prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie        # ★ 单文件自包含(内含两个原生工具)
                                           /brickie-gen    # L2 渲染器(冗余副本, 供开发态直用)
                                           /brickie-core   # L0/L1 Rust 核心(冗余副本)
```

它让**没有 `g++`/`cargo` 的全新 checkout** 也能直接跑 `brickie`(`brickie` 自带代码、
模板、schema 与原生工具; 开发态的 Python 前端找不到 `build/` 也会退到种子), 也是将来
**用 brickie 自举管理 brickie 自身编译**的"第一块砖"。三个可执行必须**同时**在位
(缺 `brickie-core` ⇒ `make tools-prebuilt` FAIL)。改了工具源码后重新发布:

```bash
make tools-prebuilt          # 编工具(brickie/brickie-gen/brickie-core) → 发布种子(cmp 相同则不写盘)
make tools-prebuilt-check    # 只检查三件种子是否落后于源码
```

> **`prebuilts/` 下分两侧**(单一顶层目录, 见 ADR `0005`):
> `seed/`(**进库**, 就是上面这份自举种子)与 `toolchain/`(**派生、不进库**, 外部工具链)。
> 原先"两个顶层目录只差一个 `s`"的歧义已消除。详见 [prebuilts/README.md](prebuilts/README.md)。

### 配置开发环境(`setup.sh`)

把 `prebuilts/` 的工具链与 brickie 放进当前 shell(**必须 `source`** —— 子进程改不了父 shell):

```bash
source setup.sh              # 配置 PATH + 环境变量
source setup.sh --quiet      # 静默
source setup.sh --with-make  # 额外把 prebuilts 的 make 放到最前(会遮蔽宿主 make)
source setup.sh --unset      # 撤销(逐字节还原)
```

| 项 | 内容 |
|---|---|
| `PATH` 前置(左优先) | `build/host/<triple>/bin`(**本机新编优先**) → `prebuilts/seed/brickie/<triple>/bin` → `prebuilts/toolchain/<arm…>/bin` → `prebuilts/toolchain/ninja/bin` |
| 环境变量 | `BRICKOS_ROOT` · `BRICKOS_HOST_TRIPLE` · `BRICKIE_REPO_ROOT` · `BRICKIE_TOOL_ROOT` · `CROSS_COMPILE` |

设计取舍: **默认不加 `make`** —— 它会遮蔽宿主 `make`(构建内部本来就用 `prebuilt` 的
`$(MAKE)`, 见 ADR-0002 §7.5); **不设 `BRICKIE_GEN`** —— 那会盖掉"本机新编优先"的查找顺序。
CI 里不想 `source` 就用 `eval "$(make -s env)"`(同一份片段)。

> 工具链还没取件、工具还没编时, `setup.sh` 照样工作, 只是把缺失目录列出来并提示
> `make prebuilt` / `make tools`(PATH 里放不存在的目录无害)。

**工具段**只要宿主 `g++` + `python3`(≥3.11); L0/L1 的 Rust 核心另需 `cargo`, **都不需要
交叉工具链**:

```bash
make tools              # 只编工具 L5/L2(没装交叉编译器的机器/CI 工具作业可用)
make tools-core         # 只编 L0/L1 的 brickie-core(cargo; 出树到同一 bin 目录)
make tools-test         # 工具自身用例: 渲染器自检 + 端到端回归(含 V-1…V-19 / §V-B 构建族)
```

**镜像段**的工具链是外部的(内部工具链未就绪, 见 §5 的 `br-wa-toolchain-001`)。需要:
`aarch64-linux-gnu-gcc`(或带版本号的 `gcc-16`/`gcc-15`/`gcc-14`/`gcc-13`; **候选序与探测在
`brickie-core`**, 不在 Makefile)、`binutils-aarch64-linux-gnu`、`qemu-system-aarch64`。

**目标表**(`make <目标>`; 镜像侧的每个名字都是**薄委派**, 真身在右侧):

| 目标 | 做什么 | 真身(委派到) |
|---|---|---|
| `make`(缺省) | **只编工具**(不碰交叉工具链) | `make -C tools/brickie cxx` |
| `make all` | 编工具 → 编镜像 + `[build].post` 门禁 | `brickie build` |
| `make tools` / `tools-core` / `tools-test` / `tools-clean` | 工具段(L5/L2 / Rust 核心 / 用例 / 清理) | `tools/brickie/Makefile` |
| `make brickie-check` / `brickie-check-release` / `brickie-compose` | 组合期校验(dev/release)/ 重建生成物 | `brickie check` / `check --profile release` / `gen` |
| `make run` | QEMU 上跑(Ctrl-A X 退出) | `brickie run` |
| `make smoke` / `irq-test` / `dbg-test` / `sync-test` / `plugin-test` / `sched-test` / `fs-test` | QEMU 门禁(冒烟 / 中断逐用例 / 内存+调试 / 同步 / 插件管理器 / 调度 / 存储) | `brickie test <名>` |
| `make mem-test` / `string-test` / `sched-test` / `rr-test` / `work-test` | **宿主侧**用例(不需交叉/QEMU; ADR-0011 起 `sched-test` 专指 **coop**, 抢占那条是 `rr-test`) | `brickie test <名> --no-build` |
| `make check-string` / `check-headers` | 支持例程自递归 / 对外头文件自洽 | `brickie test check-string` / `test check-headers --no-build` |
| `make size` / `disasm` | 体积 / 反汇编 | `brickie size` / `brickie disasm` |
| `make clean-brickos` / `clean` | 清镜像派生物 / 连工具一起清 | `brickie clean` / `+ tools-clean` |
| `make check-workarounds` / `check-build` | WORKAROUND 登记 / 构建接线门禁 | `tools/check-*.sh` |
| `make tools-prebuilt` / `tools-prebuilt-check` | 发布 / 检查三件自举种子 | `tools/brickie` Makefile |
| `make print-host-triple` / `print-host-bin-dir` / `print-prebuilt-bin-dir` / `print-cross-compile` / `env` | 查询口(宿主三元组 / 落点 / 种子 / 交叉前缀 / 环境片段) | `mk/host.mk` / `setup.sh` |

```bash
# 在仓库根执行(本分支根目录 = 原型树, 没有 brickOS/ 前缀)
make                    # 只编工具(缺省目标)
make all                # 编工具 → brickie build(镜像 + 构建后门禁)
# 也可以直调工具(推荐; 这就是"真身")
build/host/<triple>/bin/brickie build [--backend make|ninja] [--dry-run] [-j N]
build/host/<triple>/bin/brickie test [<门禁名>|--list]
build/host/<triple>/bin/brickie check --profile release
make check-workarounds  # WORKAROUND 登记一致性
make check-build        # 构建接线门禁: 缺省目标=工具/无字面源码/委派/零交叉依赖/出树/种子
make tools-prebuilt     # 发布三件自举种子到 prebuilts/seed/brickie/<arch>/<os>/bin/
make clean              # 清掉工具与镜像两侧的派生物(只清工具: make tools-clean)
```

**三条与内存/中断相关的构建/运行纪律**(都不是"可选优化"; 现在都写在**声明面**上):

| 项(声明位置) | 值 | 为什么 |
|---|---|---|
| `[build.target].arch_flags`(platform 插件) | `-mstrict-align` 必须带 | 它**曾经是硬要求**: MMU 未开 ⇒ 全部访存按 Device-nGnRnE ⇒ 非对齐访问必取 Alignment fault(实测: `stur xzr,[sp,#36]` 给 12 字节局部结构清零 ⇒ 开机 data abort)。**现在 MMU 已开**(4 KiB 恒等映射, RAM 是 Normal 属性、`SCTLR.SA/SA0` 显式清零), 非对齐访问不再 fault ⇒ 本项退化为**防御性旋钮**(Device 区的非对齐访问仍会 fault, 留着它把"编译器的内存模型"与"MMIO 的真实约束"钉在一起) |
| `product.toml [build].cflags` | `-fno-omit-frame-pointer` 必须 | **`service/backtrace` 的编译期前提**: 栈回溯靠 `x29` 帧链(设计 `5-01 §3` 的"各线程栈"捕获), `-O2` 默认把 fp 当普通寄存器省掉 ⇒ 链断在第一帧。代价是每函数多一对 `stp/ldp` |
| `[build.target.qemu].machine`(platform 插件) | `virt,gic-version=3` 必须显式钉住 | QEMU virt 的**缺省是 GICv2**(`-M virt,dumpdtb` 的 compatible = `arm,cortex-a15-gic`)。镜像里是 GICv3 驱动, 配错型号的症状是 `mrs icc_sre_el1` 未定义指令 ⇒ panic, 或"PIC 初始化完毕却收不到中断" |

> ⚠ **工具接管了镜像的"声明面"与"编译编排", 但还没接管"调用点"**: `brickie build`
> 现在真的按声明面组合镜像(哪些源、用什么标志、链哪个脚本都由 `product.toml` /
> 插件 `[build]` / `[build.target]` 决定)。但**启动链本身**仍是压缩的替身 ——
> `.br_plugins` 段枚举驱动的调用点(插件管理器)属 M0 运行期, 仍未落地。这正是
> `br-wa-entry-001` 剩下的那条欠债(直编那半条已注销), 见 §5 与 `WORKAROUNDS.md`。
>
> ⚠ 声明面不自洽时 `brickie build` 会**在组合期止步**(红 / 退出码非 0), 镜像不编。
> 这是**有意**的: 声明面自洽是镜像构建的前置。只编工具用 `make tools` / `make tools-core`;
> 只想看结论用 `make brickie-check`。

**自检开关(ADR-0010)**: 自检是**生成期**开关, 不是运行时开关 ——
关掉它, 测试代码**整段不进镜像**(描述符写 `NO_HOOK` ⇒ 各插件 `src/*_selftest.c` 无人引用
⇒ `--gc-sections` 裁掉; `core/selftest/` 则直接不参与编译):

```toml
# product.toml
[selftest]
enabled = "inherit"   # true | false | "inherit" —— inherit = 跟随 [product].stage(dev 开 / release 关)
# plugins = []        # 可选: 只跑列出的插件(其余的连钩子都不发)
```

判据是**机械的**, 两条都要看:

```bash
# ① 钩子符号必须消失(只剩管理器自己的入口)
aarch64-linux-gnu-nm build/brick.elf | grep -c _selftest     # 关掉后: 1(只有 br_plugin_manager_selftest)
# ② 日志里 ran=0(★ 不是"没有 [SELFTEST] 行": 管理器照旧打 banner 与 SUMMARY)
grep 'SELFTEST\] SUMMARY' build/logs/smoke.log
```

7 道 QEMU 门禁都 require `ran=[1-9][0-9]* … fails=0 errors=0` ⇒ **关掉开关会让门禁全红**
(而不是静默少跑一堆测试)。这是有意的绊线: 开关只该在"我真的不要测试"时关,
而那一刻门禁本来就不该绿。

换交叉工具链前缀改**声明面**(这是 `br-wa-toolchain-001` 的还债口), 构建规则不动:

```bash
# platform/qemu-aarch64/plugin.toml 的 [build.target].cross
# 候选序与解析在 brickie-core(contract §9 R-14); 查询口仍可用:
make print-cross-compile
```

实际输出(QEMU virt, `-cpu cortex-a53`, `gic-version=3`; 逐用例的 67/35/… 行已省略):

```
[    0.000050] INFO  brickOS-prototype v0.2.0 -- core MainLoop (interrupt heartbeat + delay + logging)
[    0.000684] INFO  platform: qemu-aarch64/virt (aarch64)
[    0.002937] INFO  mem: heap=1048576 contig=262144 page=1048576(256 pages) dma=262144
[    0.003849] INFO  mem: identity map on (SCTLR=0x30d51825, regions=9, 4 KiB pages=2048, 2 MiB device blocks=512)
[    0.004479] INFO  mem: TTBR0=0x400a3000 TCR=0x200803d19 MAIR=0x4400ff
[    0.001157] INFO  entry chain: start.S -> br_core_main (core.init -> platform.init -> core.plat.init -> plugin_manager: EARLY/CORE/LATE -> irq on -> START) -> app thread -> br_sched_run
[    0.009748] INFO  vfs: mount / (root type=1)
[    0.011129] INFO  vfs: mount /dev (root type=1)
[    0.012543] INFO  devfs: /dev mounted (1 device(s) visible so far)
[    0.032862] INFO  int: timer PPI armed by platform (virq=0 INTID=30, 100 ms)
...                                                                    ← START 相跑完; 下面是**自检 pass**(ADR-0010)
[    0.030486] INFO  [SELFTEST] ---- plugin self-tests (START 之后, 调度器接管之前) ----
[    0.075021] INFO  [DEVCONF] SUMMARY pass=6 fail=0 total=6
[    0.121820] INFO  [CDEVCONF] SUMMARY pass=8 fail=0 total=8
[    0.124976] INFO  [IOCONF] SUMMARY pass=12 fail=0 total=12
[    0.031887] INFO  [IRQCONF] SUMMARY pass=67 fail=0 total=67
[    0.054880] INFO  [MEMCONF] SUMMARY pass=35 fail=0 total=35
[    0.113357] INFO  [TRACE] total=461 drained=264 overrun=8 ids=3 live=197
[    0.122500] INFO  [DBGCONF] SUMMARY memleak pass=12 fail=0 total=12
[    0.127891] INFO  [DUMP] ===== snapshot start =====
[    0.130603] INFO  [DUMP] regions=9
[    0.133820] INFO  [DUMP] ===== snapshot end =====
[    0.135335] INFO  [DBGCONF] SUMMARY pass=8 fail=0 total=8
[    0.136957] INFO  [PLGCONF] SUMMARY pass=7 fail=0 total=7
[    0.139543] INFO  [SVCCONF] SUMMARY pass=3 fail=0 total=3
[    0.227397] INFO  [TASKCONF] SUMMARY pass=12 fail=0 total=12
[    0.522121] INFO  [SYNCCONF] SUMMARY pass=12 fail=0 total=12
[    0.525079] INFO  [VFSCONF] SUMMARY pass=13 fail=0 total=13
[    0.529387] INFO  [SELFTEST] SUMMARY plugins=14 ran=11 skipped=4 fails=0 errors=0
[    0.530000] INFO  [PLUGIN] manager: 调度器已注册 ⇒ br_sched_run()
[    1.033218] INFO  tick=1 uptime=1034999 us delay=1000054 us (>=1000000 us: ok) irq_ticks=9
[    2.033521] INFO  tick=2 uptime=2035324 us delay=1000001 us (>=1000000 us: ok) irq_ticks=19
```

几处值得看的证据: `identity map on (SCTLR=0x30d51825, ...)` 是 MMU 真的开了(读回 `SCTLR_EL1`);
`[LEAK] summary live=1 bytes=123` 是 memleak 用例**故意泄漏**那一块的现场(它随后自己释放);
`[BT] frames=4` 是栈回溯真的走了 4 帧(符号化归 host 离线工具);
`dbg: steady-state trace drained=40` 是**第 2 拍**才取走的 trace —— 里面是 timer PPI 在中断上下文
落下的事件, 即"trace 在 ISR 里可用"的活证据。

`irq_ticks` 每拍 +10 就是"timer PPI 的 ISR 真的在跑"的活证据(100 ms 心跳);
`make irq-test` 会把中断的 91 项用例逐条判红绿, `make dbg-test` 再判 `[MEMCONF]`/`[DBGCONF]` 两套,
`make sched-test` 判 `[TASKCONF]`(含**时间片抢占**与 `[WQCONF]` 的下半部八项)。

体积(aarch64 裸机 ELF): `.text` ≈ 78 KiB / `.bss` ≈ 59 KiB / `.data` 53 B(`make size`);
其中向量表占 2 KiB(每个入口 0x80 字节是 AArch64 的硬性间距)、**页表 28 KiB**(7 张 4 KiB 表:
L1 + Device L2 + RAM L2 + 4 张 L3, 静态放 .bss 因为"页池可用"以"映射已生效"为前提)。
静态 RAM 的其余大头: 中断描述符池/运行期表/域池 ≈ 8.3 KiB、trace 环 256×16 B = 4 KiB、
TLSF 堆与 contig/页池的控制块(池本体在 region 表划的 RAM 里, 不占 .bss)。

两个值得知道的实现取舍:

- **时钟换算不是 `ticks / (freq/1e6)`**: QEMU 的 arch timer 是 62.5 MHz, 每微秒 62.5 拍不是整数,
  取整会带来约 1% 的**系统性漂移**(跑 1000 秒差 8 秒)。改用**毫秒**为基准(`freq/1000 = 62500`,
  对 62.5 MHz 精确), 再把 `ticks*1000` 拆成整数+余数两步, 既无漂移也不溢出。
  延时换算**向上取整** —— 这是"不早醒"的落点。
- **向量表 16 个入口全走"喊一声 + 停机"**: v0.1.0 不开中断, 但 fault/SError 仍会来;
  没有向量表就会跳到地址 0 静默挂死。已实测: 注入 `brk #0` 会打出
  `[FATAL] unhandled exception -- vector=4 -- parked`(vector 4 = Current EL with SP_ELx, Synchronous)。
  这条路径**故意不打日志**(不走可能已损坏的 core 设施), 对应设计 `3-02 §8.3.1` 的 bare 路径。

## 5. WORKAROUND(欠债清单)

v0.2.0 的欠债全部登记在 **[WORKAROUNDS.md](WORKAROUNDS.md)** —— 本节只列**还在欠**的
(已注销的 `br-wa-entry-001` 见该文件的"已注销"表):

| id | 一句话 |
|---|---|
| `br-wa-fs-001` | **挂载点是代码常量**: `fs/tmpfs` 的 `/`、`fs/devfs` 的 `/dev` 写死在源码里, 而 `plugin.toml` 的 `[[mount]]` 只是**人读的声明面** —— 缺"manifest → 挂载计划"的生成链路(**两处真值**, 与 `br-wa-mem-001` 同源) |
| `br-wa-io-001` | **PL011 是"轮询 cdev", 不是设计写的"中断 tty"**: 不开 UART 中断、阻塞 read 靠 `br_task_sleep` 轮询、无 tty 层(欠债在 ADR-0011 后**收窄**成"驱动侧还没用 bh 做 RX 唤醒" —— bh 本身已就位); 另: `SET_BAUD` 会连带改内核日志速率(与 platform 的早期 console 共用同一根 UART) |
| `br-wa-boot-001` | **① 已还**(ADR-0008: 有了独立的 `core.init` 入口 —— 四阶段启动链)。**仍欠两件**: ② APP 仍直读平台身份(`br_plat_name/isa/timer_ticks`)⇒ 还剩一条 `allow_edges` 豁免; ③ 日志/trace 直写 console/RAM 环, 未经服务注册表(与 `br-wa-debug-002` 同源) |
| `br-wa-isa-001` | **ISA 共享库这一层还没有独立存在**: GICv3 方言、异常向量桩、**4 KiB 页表构造(`mmu.c`)** 暂居 platform 插件目录(靠文件边界分层); 异常帧布局因 extable fixup 暂放 core |
| `br-wa-toolchain-001` | 工具链用外部 gcc; 目标事实写在声明面(`product.toml` + platform 的 `[build.target]`), **工具候选序与解析在 `brickie-core`**(裁定 R-14) |
| `br-wa-mem-001` | **三池比例写死在 platform 的 region 表里**(heap 1 MiB / contig 256 KiB / page 1 MiB / DMA 256 KiB / 保留 16 KiB), 未经 manifest 的 `[budget]`/`[[res]]` 生成 —— 编译编排**已落地**(`brickie build`), 仍欠的是 `budget → region` 的**生成链路** |
| `br-wa-debug-001` | **memleak 的"归属标签" ≠ v2 的 per-plugin arena 记账**: 能报"谁没还、在哪分配的", 没有预算上限/强制归属/OOM 策略 |
| `br-wa-debug-002` | **dump/trace 直写早期 console**, 未经 `5-01 §2` 的 debug bridge(COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面, M3) |

**`br-wa-entry-001` 已注销**(v0.2.0): ① `brickie check`/`gen` 按布局发现与校验插件;
② `platform/` 收敛为插件 `platform/qemu-aarch64`; ③ 源集合改由 `[build].sources` 声明
(ADR-0003 的 S1/S4), 且 ④ **启动链的调用点改成 `.br_plugins` 段枚举驱动**(ADR-0005,
后由 ADR-0008 收敛成 `bl br_core_main`)。历史留档见
[WORKAROUNDS.md](WORKAROUNDS.md) 的"已注销"表。

**`br-wa-test-001`(用例编号债)**从三套扩到七套: 新增的 `TC-VFS-*`(13)/
`TC-DEV-*`(6)/`TC-CDEV-*`(8)/`TC-IO-*`(12)在设计的 `6-01` 里**整组不存在**
(7-storage/8-device 两域尚无用例表)⇒ 自编号; 补齐设计侧用例组后再改回正式编号。
`fs-test` 门禁对这 39 个 tag **逐条点名**, 所以"裁掉一个用例"会立刻变红。
ADR-0011 追加的自编号: `TC-TASK-102/103/104`(抢占与 I2 的可观测面)、
`TC-IRQ-015/016/017`(BH 分发 / 原子上下文守卫 / §11.4.1 抑制不重放)、
`TC-WQ-001..007`(下半部/工作队列)—— 同样待 `6-01` 补齐正式编号。

代码里的标记形如 `WORKAROUND(br-wa-boot-001)`, 与登记表由 `make check-workarounds` 绑死:
**任一侧多/少即报红** —— 欠债最怕的不是欠着, 是没人知道欠着。

## 6. 代码 ↔ 设计对应

| 本原型 | 设计出处(`brickOS-Design` 分支) | 形态差异 |
|---|---|---|
| `start.S` 的 reset/BSS | `1-01 §9` 启动序列 | 设计是 Platform **插件**的汇编; 此处随插件 `[build].sources` 编进镜像。reset/BSS 后只 `bl br_core_main()`, 编排权全在 core(ADR-0008; 旧的"APP 直调"已还清 `br-wa-entry-001`) |
| `br_plat_early_init()` | `1-01 §9` 的 `platform.early_init`; `1-01 §8` 三层模式 | console + GICv3 PIC 注册 + 绑定表(§14.3 步 1–3)+ **region 表声明 + 页表 ops 注册**(池的认领与 MMU 激活归 core 的阶段 ③, 见 ADR-0008) |
| `br_console_*` | `1-01 §8` console 双形态; `3-01 §10` 平台侧接口表 | 形态一致(轮询早期 console) |
| `br_clock_now()` / `br_time_t` | `3-01 §4`(br-sched 组); `3-01 §14` CA-1(us) | 读数 + `br_deadline_from_now`; 超时表/唤醒已交付(ADR-0006 §3.3: **周期 tick 上的到期扫描**, 分辨率 = tick = 100 ms; tickless 的比较器装弹仍欠) |
| **`br_sched_ops` / `br_sched_run` / 线程面** | `3-01 §2/§5.1`; `3-03`(总纲) | ADR-0006: ops 成文表 + 分工(core 机制 / 插件策略); **ADR-0011 追加抢占接缝** `br_sched_request_resched` 与 **bh 禁令**(bh 内阻塞 `-EPERM`/让出拒绝/退出 panic)。偏离: 同步原语归 core(ADR-0006 §3.5)、`irq_epilogue` 是 core 内部函数(同 §2 裁定 S-2) |
| **`sched/rr`(时间片轮转抢占)** | `3-01 §5.1` 的注册点 + `3-02 §11.2` 的 IRQ 出口接缝 | **ADR-0011 新增**: `on_tick` 递减时间片, 用尽请 core 在 IRQ 出口换栈(设计 `3-02 §11.2` 的接缝第一次真的切栈)。它与 `sched/coop` 是"同一套 core 机制换一个策略"的证据: 插件里**没有一行切换代码** |
| **`br_work_submit` / 下半部 / workqueue** | `3-01 §5`(延迟工作 = bottom half; 队列深度静态、满 ⇒ `-EAGAIN`)+ `3-02 §11.1/§11.4.1/§12.4/§9.4/IR-10` | **ADR-0011 新增, 且归属偏离**: 设计把 work queue 放调度插件, 本原型按用户裁定**整体放 core**(理由见 ADR-0011 §3.1)。执行点 = IRQ 出口(eoi 之后、ERET 之前)⇒ 延迟**有界**(与设计 §11.3 的"无上界"相反, 是更强的承诺); 代价是 bh 上下文更严(关中断、禁阻塞)。三个消费者: 按线的 `DISPATCH_BH`、SLOW 域 demux、任意 ISR 的裸提交 |
| `br_log_*` | `5-01`(trace 观测)/ `11-01`(日志 Service) | v0.1.0 是 core 内的最小打印设施, 不是那个服务 |
| `br_core_main()` | `1-01 §9` + `§6.2` 阶段表 | **四阶段启动链**(① 平台无关 → ② platform 插件 → ③ 堆/地址映射 → ④ 插件相位驱动), ADR-0008 |
| **插件自检(selftest)** | `6-01`(用例是交付物; §2 运行基建)+ `1-01 §6.2`(四相与"两个完成点") | ADR-0010: 测试搬进各插件 `src/*_selftest.c`, 由 core 的**自检 pass**在 START 之后统一驱动(描述符的 `selftest` 钩子); 开关 `product.toml [selftest]` 是**生成期**的(关掉 ⇒ 描述符 NO_HOOK + `core/selftest/` 不编译 ⇒ 测试代码被裁出镜像)。**自检不是第五相**(它没有依赖语义, 对所有插件在同一时刻发生) |
| **测试入口不进 `[[export]]`** | ADR-0005 裁定 9(钩子是组合期契约) | 11 个 `*_conformance`/`*_selftest` 从各 `plugin.toml` 的 `[[export]]` 移除 ⇒ 改用例不再算接口变更(接口快照已重发)。测试需要的插件私有符号走 `<plugin>/src/<short>_internal.h`(在 `src/` 而非 `include/`, 因为 `include/` 下的一切都是对外面) |
| **`br_irq_*` 九件 + 域四件** | `3-01 §8/§8.1`(签名冻结)+ `3-02 §3–§9`(机制) | Stage 1 全量; `register/enable/disable` 的 thread-only 加了**运行期拒绝**(设计侧靠静态扫描), ADR-0011 起判据是"ISR **或** bh"(`br_irq_in_atomic`)。**`BR_IRQ_F_DISPATCH_BH` 真的生效**、**SLOW 域可用**(ADR-0011); 新增三个查询面(`br_irq_in_isr`/`br_irq_in_atomic`/`br_irq_lock_depth`)与 `br_irq_stat_t` 的 bh 三项 |
| **`br_pic_register` / `br_irq_bindings_set` / `br_pic_ops_t`** | `3-02 §4.1/§14.3`(platform 侧契约) | 新增的核心符号, 未回灌 `3-01 §10` 的登记(属 Design 仓库, 见 ADR-0002 §4) |
| **GICv3 方言(`br_gicv3_*`)** | `3-01 §8` 表"中断控制器 → GICv3 驱动"; `3-02 §4.1.1/§5.3/§7.1` | ISA 层代码暂居 platform 目录(`br-wa-isa-001`); 1020–1023 折算 / EOImode=0 / MSB 对齐量化 |
| **异常向量表 + 帧** | `3-02 §5.2/§10.2` | 每槽 0x80 B 放不下完整桩 ⇒ 槽内跳板 + 槽外桩体(`P-IRQ-ASM-1`); 帧布局放 core(`br-wa-isa-001`) |
| **fault 分类 + extable + panic** | `3-02 §10.3/§10.4/§10.6`; `§8.3.1` 的 bare 路径 | 分类/extable/double-fault 兜底齐; `br_fault_handler_register` 属 v2(签名在 `3-02 §14.5`) |
| **trace 环** | `5-01 §1`(16 B 定长事件, ISR 内可记) | 环与 ISR 发射点在 core(CA-3 白名单要求中断路径不依赖服务 init), `service/trace` 是它的**唯一消费方**(drain/解码/计数/呈现) |
| **`br_malloc` 族 / contig / page / dma / heap_usage** | `3-01 §6`(br-mem 11 函数, 签名逐字一致)+ `CA-6/CA-7/CA-8` | 三池由 platform 的 region 表按 `BR_MM_KIND_*` 划; TLSF + 红区/毒化是 `5-01 §4` 的 v1.x 便宜层; 归属标签只是 v2 arena 的近似(`br-wa-debug-001`) |
| **`br_mm_region_add` / `br_mm_map` / `br_mm_unmap` / `br_mm_cache_*`** | `3-01 §7`(br-mm 5 函数) | `map`/`unmap` 按 `TC-MM-003` 的期望返回 `-ENOTSUP`(v2 重定位); 运行期改属性走 `br_mm_set_attrs`(用例做真 RO 保护验证); region 种类位是原型新增载体 |
| **`br_mm_ops_t` / `br_mm_register` / `br_mm_activate`** | `3-01 §10`"平台侧接口"行(同 `br_pic` ops 手法)+ `1-01 §8` 三层模式 | 状态机(未注册→已注册→已激活)在 core, 页表构造在 platform; 描述符编码属 ISA 层 ⇒ `br-wa-isa-001` 范围扩大 |
| **`framework/{vfs-core,dev-core,cdev-core}`** | `7-01`(§1 SD-1/§2 SD-15/§3 SD-7)、`8-01`(§1 框架件归属与形态 A/B/C、§2 注册表与 open_file 钩子、§3 子分类与 D22 预留)、`1-01` §4.5(D19) | 三件的依赖方向**逐条照 §7.3/`8-01` §1.3**: `cdev-core→dev-core`(init)、`cdev-core→vfs-core`(type)、`dev-core→vfs-core`(**type** —— 写成 runtime 会推翻形态 B); `vfs-core` 的 deps **为空**(D21 撤销设备路由后的纯 VFS)。增量: `br_inode_t` 加 `fpriv`、补路径级便捷面与 `br_open_err`、补 5 个存储域错误码(见 ADR-0009 §3) |
| **`fs/{tmpfs,devfs}`** | `7-03`(§2 tmpfs rootfs / §3 devfs 设备节点 / §6 挂载计划) | tmpfs 挂 "/" 并预建 `/dev /data /tmp`; devfs 把 dev-core 注册表**实时投影**为节点(打开经类 open_file 钩子)⇒ `/dev/uart0` 可 `br_open`。两件的挂载点是**代码常量**(`[[mount]]` 声明面工具侧尚未消费)⇒ `br-wa-fs-001` |
| **`io/uart-pl011`** | `1-03 §1`(M0 轮询 console → **M2 注册 cdev**)、`8-01` §6(驱动编写者契约)、§5(ioctl 编码) | v1 交付**注册 cdev + 轮询式会话**(`/dev/uart0` 可开/读/写/ioctl/poll + 严格独占); **不开 UART 中断**(bh 已在 ADR-0011 就位, 欠的是驱动侧还没用它做 RX 唤醒)⇒ `br-wa-io-001`。与 platform 的 `console_pl011.c` 是同一硬件的两种**设计内**形态(后者是 panic 通道, 不能注册设备) |
| **`service/{trace,backtrace,hexdump,memleak,dump}`** | `1-03 §1`(v1.0 插件清单: `service/trace`)+ `5-01`(§1 trace / §2 bridge / §3 ramdump 捕获集 / §4 memleak) | `service/trace` 与清单同名; 另四件是本次补的调试服务(6-01 尚无 `TC-DBG-*` 组, 属回灌项); 调用点靠 `product.toml` 的两条 M0 引导例外(`br-wa-boot-001`) |
| **`tests/host/mem_test.c` + `make mem-test`** | `1-03` 的"host 平台插件: CI 秒级 + ASan 白捡" | 完整 host 平台插件未落地; 本原型先做到"内存实现编成宿主可执行跑算法压测"(无需交叉工具链/QEMU) |
| **`board_irq.h`** | `3-02 §3.1`(IR-2 生成头) | 走设计明示的**退路**: platform 导出静态头(brickie v0.1 不生成该头, 属 2-01 的 O-4) |

## 7. 下一步(往 M0 走)

按设计 `1-03 §3`, M0 = **启动链 + 插件管理**, 验收 = "hello + init 链打印 + 故意造环看组合器报错"。
本原型离它还差以下几步, 顺序有依赖关系(状态按当前 checkout):

1. **组合器 `brickie` 的声明面** —— **已交付**(三语言: L5 Python 前端 23 条叶子命令、
   L0/L1 Rust 核心、L2 C++ 渲染器, 入口 = 单文件自包含 ELF)。`platform/qemu-aarch64` 与
   `app/hello` 已被它接管(manifest + `check` + 快照 + lock + 描述符生成物)。这一段是
   `br-wa-entry-001` 第 ③ 条的前提, 前提已具备。
2. 插件描述符段(`.br_plugins` + `__br_plugins_start/stop`)与**声明面唯一真值**(`2-02` BR-D2)。
   `platform/qemu-aarch64` 与 `app/hello` 的 manifest / 描述符生成物已在 `build/gen/**`,
   缺的是**运行期按段枚举**。
3. `start.S` 的调用点改为段枚举驱动 → 兑掉 `br-wa-entry-001` 的第 ③ 条
   (第 ①② 条已随插件化还清)。
4. ~~中断框架(M0/M1 的 Stage 1)~~ ⇒ **已交付**(设计 `3-02` 的 Stage 1 全量: 号空间/PIC 抽象/
   生命周期/屏蔽三层/优先级/触发/级联域/fault+extable, 加 GICv3 方言与向量桩;
   `make irq-test` 的 91 项在 QEMU 上全绿)。**ADR-0011 追加了 Stage 2 的第一件**:
   按线的 **BH 分发** + **SLOW 级联域走下半部**(workqueue 在 core)。**仍欠的**:
   `BR_IRQ_F_DISPATCH_THREAD`(线程化 IRQ)、§12.3 动态升级、v2b 的亲和性/负载均衡/IPI,
   与 `br_fault_handler_register`(v2)。
5. ~~内存: 恒等映射 + 三池 + 堆~~ ⇒ **已交付**(设计 `3-04`/`3-01 §6/§7`: 4 KiB 恒等映射页表 +
   region 表 + TLSF 堆 + contig/DMA 池 + 页位图池 + cache 维护; `make dbg-test` 的
   `[MEMCONF]` 与 `make mem-test` 在目标/宿主两侧全绿)。**仍欠的**是重定位(v2)、
   MPU 目标(vx)、池比例由 manifest 生成(`br-wa-mem-001`)。
6. ~~调试插件: trace / dump / hexdump / backtrace / memleak~~ ⇒ **已交付**(五个 service 插件,
   声明面由 brickie 治理; `make dbg-test` 的 `[DBGCONF]` 全绿)。**仍欠的**是 debug bridge(M3)、
   per-plugin arena 记账(v2)、栈 canary 与目标侧 ASan。
7. ~~调度器(M1)~~ ⇒ **已交付**(ADR-0006 的调度框架 + `sched/coop`; ADR-0011 追加
   `sched/rr`(时间片抢占)与 **IRQ 出口的抢占换栈**): APP 线程 + `br_sched_run()`,
   timer PPI 的 ISR 仍按 `P-IRQ-17` 留在 platform, 调试插件的 init 由插件管理器按
   `[[dep]]` 驱动。**仍欠的**: `sched-tt`(v3 调度表)、优先级/PI、SMP。

⇒ 于是剩下的主线其实是**同一条**: **插件管理器 / 阶段机 / 调度器**(它们互为前提)。
这条主线**已经走通**(上面 4/7 两行); 下一段的主线变成"**服务化**"(平台身份/心跳进服务注册表,
让 `product.toml` 的最后一条 `allow_edges` 豁免消失)与"**debug bridge**"(M3)。

> **v0.2.0 现状补记(ADR-0008)**: 第 7 项已交付(插件管理器 + 相位机 + 调度器,
> `br_sched_run()` 接管首次调度)。`br_core_main` **没有"被拆掉", 而是以"四阶段启动链"
> 的形态重建** —— 上面那句"归宿是被拆掉"针对的是 M0 那个"在 APP 里直调各子系统"的
> 替身版本, 不是这个名字。新的分工: `start.S` 只 reset/BSS 再 `bl br_core_main()`;
> 后者按 ① 平台无关初始化 → ② platform 插件初始化 → ③ 堆 + 地址映射的建立 →
> ④ 插件相位驱动 编排(`core/include/br/core/br_main.h` / `docs/decisions/0008-*.md`)。
> timer PPI 的 ISR 仍按 P-IRQ-17 留在 platform 的 `start` 相(权限模型, 见 ADR-0002)。

### 中断侧的实现裁定(与设计文档的偏差都记在这里, 细节见 ADR-0002)

| # | 裁定 | 一句话 |
|---|---|---|
| `P-IRQ-1` | 绑定表只覆盖直连线 | 域子中断的描述符由 `domain_create` 的窗口切片填充 |
| `P-IRQ-2` | extable 线性扫描 | 段顺序 = 链接顺序, 没有无运行期初始化的排序点 |
| `P-IRQ-3` | core 的量化判据假定 MSB 对齐 | LSB 对齐的方言(NVIC)需方言侧自行处理 |
| `P-IRQ-5` | `br_irq_cpu_init()` 兼初始化描述符池的 `pic_id/dom_id = -1` | 0 是合法编码(BSS 零不等于"无效") |
| `P-IRQ-15` | GICv3 拒绝"量化后落在最低可实现档"的请求(`-ENOTSUP`) | 那档与 PMR 相等 ⇒ 永远收不到; 降到上一档是**提权**, IR-7 禁止 |
| `P-IRQ-16` | 一致性用例用**非对齐访存**造可恢复 fault | ~~MMU-off ⇒ Device 内存 ⇒ 非对齐必取 Alignment fault~~ **已被 `P-MM-3` 取代**: MMU 开 + Normal 属性 + `SA/SA0=0` 之后非对齐不再 fault, 改用**窗口外未映射地址**取翻译 fault |
| `P-IRQ-17` | timer PPI 的 ISR 与"全局开中断"归 **platform**, 不归 APP | 设计 `3-01 §13.6` 把"中断控制"归 **P3**、APP 是 **P0** ⇒ 放置避免越权 |
| `P-IRQ-ASM-1/2` | 槽内跳板 + 槽外桩体; 恢复路径不写 `msr daif`(无此编码, `eret` 从 SPSR 恢复) | 一槽 0x80 B 装不下 320 B 帧的存取 |
| `P-IRQ-PLAT-1` | `ICC_CTLR_EL1.PRIbits` 按 `field + 1` 解码并夹到 [5,8] | 实测 QEMU: field=4 ⇒ 5 位; 回读断言(0x01→0x08)是它的执法 |

### 调度/下半部侧的实现裁定(与设计文档的偏差都记在这里, 细节见 ADR-0006 / ADR-0011)

| # | 裁定 | 一句话 |
|---|---|---|
| `P-SCHED-1` | 抢占 = **"插件置位 + core 在 IRQ 出口换栈"** | 插件只回答"什么时候该换"(`br_sched_request_resched`), 换栈必须与 state 机 + 异常出口顺序绑在一起 ⇒ 归 core |
| `P-SCHED-2` | 新线程首次进入时按 `br_irq_lock_depth()` 修正中断形态 | 在 IRQ 出口被选中的新线程**不经 ERET** ⇒ 不管就会带着关中断一路跑(timer 再也不来); 深度 > 0 时**只留痕不修**(那意味着有人在临界区里让出, 是既有禁令) |
| `P-SCHED-3` | 被抢占线程的现场留在**它自己的 IRQ 出口调用链**上 | 切栈点是 C 调用边界(`br_sched_switch_to`)⇒ `switch.S` 仍只存 callee-saved, `vectors.S` **不必改** |
| `P-BH-1` | workqueue **整体在 core**(与 `3-01 §5` "实现在调度插件"相反) | ISR 白名单里的 `br_work_submit` 必须在"调度器还没注册"时也成立; 执行点(IRQ 出口)与调度器无关 |
| `P-BH-2` | bh 上下文 = **"已 eoi、未 ERET"**那一小段(关中断、跑在被中断线程的栈上) | 顺序四个约束: eoi 之后(不占 Active)、ERET 之前(不依赖 worker)、抢占决策之前(同一次出口能看到它唤醒的线程)、INV-C 之后 |
| `P-BH-3` | bh 里**禁阻塞/让出/退出**, 运行期执法 | `-EPERM` / 拒绝 + 留痕 / `br_panic` —— 诚实失败优于"以为切换发生了" |
| `P-BH-4` | `br_irq_disable` 的承诺收紧为"不再**开始**新的 handler"; 已排队的**抑制不重放** | 照 `3-02 §11.4.1` 逐条落地; 抑制判定与派发在同一临界区(bh 关中断 ⇒ 天然原子) |
| `P-BH-5` | `br_work_submit` 失败 ⇒ **必须回滚**已做的动作 | IR-10 的唯一死锁入口: 按线 BH 回滚 = 放行该线; SLOW 域回滚 = 清 busy + 放行父线; 两处都计数 + 留痕 |
| `P-BH-6` | SLOW 域的 demux 与 FAST **共用一份循环**(`br_irq_demux`), 差异只在调用者 | "状态寄存器只能在非 ISR 上下文读"这条契约因此可以被方言自己断言(用例就是这么判的) |

### 内存/调试侧的实现裁定(与设计文档的偏差都记在这里, 细节见 ADR-0003)

| # | 裁定 | 一句话 |
|---|---|---|
| `P-MEM-1` | **native 面零增量**: br-mem 11 + br-mm 5 与 `3-01 §6/§7` 逐字一致 | 新增的初始化/观测/记账/平台面共 16 个符号**分类登记**在 ADR-0003 §2.1, 不动 CA-5 的 48 预算 |
| `P-MEM-2` | TLSF 二级位图(32 × 32), `BR_MALLOC_ALIGN = 16` | AAPCS64 的 SP 16 对齐 ⇒ 结构体自然对齐上界是 16; 取 4/8 会让 `aligned(16)` 对象放不进 malloc 的 buffer |
| `P-MEM-3` | 用户块 = `[头 32B][用户区][红区 0xA5][尾 8B]`; 双 free 检出后**直接返回** | `5-01 §4` 的 "TLSF 红区 + freelist 毒化" 落地; 二次入链会毁堆, 拦截比报错重要 |
| `P-MEM-4` | region 种类位 `attrs[11:8]`(`BR_MM_KIND_*`)= `3-01 §13.6` regions 轴的**可执行载体** | 设计只给了取值域没给载体; core 按种类认领池(机制), platform 只声明数据 |
| `P-MEM-5` | 镜像 + 启动栈声明为**一条** `IMAGE` region | region 重叠禁则 + 页对齐: 镜像尾与栈底只差十几字节, 拆两条必然重叠 |
| `P-MEM-6` | 页池位图按 `BR_PAGE_MAX_PAGES = 4096` 静态预置(**16 MiB** 池上界, 位图 512 B) | 无运行期分配; 超出上界在 `br_mem_init` 就 `-ENOMEM`, 不静默截断 |
| `P-MM-1` | 4 KiB 粒度 / 4 级 / 仅 TTBR0 / T0SZ=25 / IPS=40 位; 7 张静态页表(28 KiB) | 逐页 4 KiB 映射是 `set_attrs`(RO/NX)与 v2 重定位能按页生效的前提 |
| `P-MM-2` | 低 1 GiB 整段 512 条 2 MiB **Device 块** | QEMU virt 的外设都在低 1 GiB, 且 GICR 的第 4–8 帧排到 `0x081A0000` —— 只映射 GICD 附近会漏 |
| `P-MM-3` | RAM 开 8 MiB 的 4 KiB 页窗; **窗口外 invalid** ⇒ 可恢复翻译 fault | 取代 `P-IRQ-16` 的"非对齐 fault"锚点(SA/SA0 已清零, Normal 上的非对齐不再 fault) |
| `P-MM-4` | `br_mm_map`/`unmap` = `-ENOTSUP`; 属性改写走 `br_mm_set_attrs` | `TC-MM-003` 的期望就是 `-ENOTSUP`(签名先行); `set_attrs` 是同一条重定位通路在 v1 的演练 |
| `P-MM-5` | `br_mm_query` **只报页表真实属性**, "这块是什么"归 region 表 | 两处不互相冒充; 否则"属性"就有两个真值 |
| `P-MM-6` | NX 只做**描述符读回**验证, 不做真的取指测试 | executing-NX 走 instruction abort, 而 `3-02 §10.3` 的 extable 只覆盖 data abort ⇒ 会 panic; 不假装测过 |
| `P-DBG-1` | trace 的环与 ISR 发射点留 core, 解码/消费在插件 | CA-3 白名单要求中断/fault 路径不依赖服务 init; "可整层移除"由消费侧满足 |
| `P-DBG-2` | backtrace **只捕获不符号化**(host 离线解码), `-fno-omit-frame-pointer` 是前提 | `5-01 §3` 把栈回溯分析划给 host; 镜像内符号表要两遍链接, 归 v1.x |
| `P-DBG-3` | hexdump 格式是契约(87 B/行, 自己渲染不依赖 `br_log`) | 否则"格式契约"会随 log 实现漂移; 用例逐字节断言 |
| `P-DBG-4` | dump 的边界判定 = region 表; `BR_DUMP_F_FORCE` 是 P4/machine 语义 | 未声明地址默认 `-EINVAL`; 同时暴露分级模型缺口: ops 轴没有"读/inspect" ⇒ dump 只能声明 P0(回灌项) |
| `P-DBG-5` | 调试域用例编号 `TC-DBG-*`(6-01 无此组) | 与 `TC-IRQ-1xx` 同型: 域特有组用百位编号; 回灌 `6-01` 是留待项 |

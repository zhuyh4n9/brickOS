# 8-02 — netdev 与开源网络驱动移植: 差距分析

> 章节: **8-device**。状态: **分析稿**(差距清单 + 边界论证 + 落地顺序; 需拍板的项在 §5, 本篇不代替决策)。
> 定位: 回答一个问题 —— **"把开源网络驱动移植到 brickOS 还差什么"**。本篇是 `8-01` O-S5 的**前置分析**(O-S5 只说了"netdev 是第三个子分类框架", 没说它与驱动移植的关系)。
> 基线: 原型 HEAD `724fa56`。★ 工作树有**在途**改动(`core/include/br/core/br_work.h` + `core/src/work/` + `sched/rr/`, 引用 `docs/decisions/0011-round-robin-bh-workqueue.md`, 该 ADR **尚未落盘**)—— §3 的 #2 与 §6 的阶段 1 受它影响, 已逐处标注。
> 依赖: `8-01`(设备体系/子分类/D20 §3/O-S5 §8)、`1-03`(v2.0 插件清单 `service/lwip`/`io/virtio-net`)、`7-02`(bdev 的 ISR→bh→唤醒流程, 本篇的形态参照)、`11-01`(服务清单/socket 路由)、`2-toolchain/brickie/brickie-v0.1.md`(§3.1 依赖禁则 = §5 #2 的冲突源)。
> 相关: `1-04`(原型实况对账; 其 §2 的行号与插件数**早于** ADR-0009/0010, 引用它的数字前先复核)。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **bh** | bottom half | 中断下半部(`8-01` §1.1; 原型在途, 见 §3 #2) |
| **cdev / bdev / netdev** | character / block / network device | 设备子分类框架(dev-core 之下的对称子分类) |
| **DMA** | Direct Memory Access | 外设不经 CPU 读写内存 |
| **DTB** | Device Tree Blob | QEMU `-M virt` 的设备树(`dumpdtb` 可导出, 见 §7) |
| **ISR** | Interrupt Service Routine | 中断服务例程 |
| **lwIP** | lightweight IP | 目标网络栈(`1-03` v2.0 的 `service/lwip`) |
| **MMIO** | Memory-Mapped I/O | 寄存器映射窗口 |
| **MTU** | Maximum Transmission Unit | 最大传输单元(以太网 1500) |
| **netif** | network interface | lwIP 的网络接口抽象(驱动与协议栈的接缝) |
| **O-S5** | — | `8-01` §8 的开放问题: netdev 待定 |
| **SD-2/4/6/10/12** | — | `8-01`/`7-01` 的存储与设备域决策编号 |
| **virtio / vring** | — | 半虚拟化设备规范 / 其共享环(descriptor + avail + used) |

## 1. 结论

三句话:

1. **移植的边界必须选在 `netif`, 不选 `driver`。** lwIP 自带 `ethernetif.c` 就是给移植准备的接缝, 它只要两个函数(`low_level_output` / `low_level_input`)。按"移植一个完整驱动"做, 会在 brickOS 里重建 pbuf 链/ARP 队列/重传, 那才是成本失控点。
2. **真正的缺口不是驱动, 是驱动要注册进去的契约**: `framework/netdev-core` 不存在, 且 `8-01` O-S5 明写"v2.0 设计前必须定"。契约(ops 形状)进 golden, 后补字段 = 二进制不兼容(D14)⇒ **必须先定形状再写驱动**。
3. **网络不需要 VFS**。`8-01` §1.2 的**形态 B**(只用设备框架、不链 vfs-core)是网络的正确形态: 消费者 `service/lwip` 直取 `br_netdev_get()`, 不产生 `/dev` 面。第一个里程碑可完全不碰 vfs-core/devfs/tmpfs。

## 2. 已有能力(可直接用, 不需新建)

| 能力 | 位置(原型) | 对网卡的意义 |
|---|---|---|
| 设备注册表 + 子分类协议 | `framework/dev-core/src/dev_core.c`(`br_dev_add`/`br_dev_lookup`) | 网卡入册; `class_id` 加一个 `BR_CLASS_NETDEV` 即可, **dev-core 一行不改**(`8-01`:185 的"通用与形状分离") |
| **子分类框架的完整模板** | `framework/cdev-core/src/cdev_core.c`(415 行)+ `cdev_selftest.c`(149 行) | netdev-core 照它的骨架写: 槽位池 / 注册校验 / 取回 / 自检。**这是最大的成本节省** |
| `br_sem_give` **ISR-safe** | `core/include/br/core/br_sync.h`:87(白名单用例 `TC-SYNC-005`) | ★ RX 路径能成立的支点: ISR 只 `sem_give`, 重活在 RX 线程 |
| `br_cond_signal` 可在 ISR 上下文 | `br_sync.h`:110 | 多等待者(多队列 RX)场景 |
| `br_task_create` / `br_task_join` | `core/include/br/core/br_sched.h`:63 | RX 线程 / 协议栈线程 |
| `br_dma_alloc`/`br_dma_free`, `dma_addr == vaddr` | `core/include/br/core/br_mem.h`:69,73-74 | vring 与 DMA 缓冲**不需要地址翻译**(恒等映射, CA-6/INV-6) |
| `br_mm_cache_flush` / `cache_invalidate` | `core/include/br/core/br_mm.h`:92-93 | DMA 前后的 cache 维护(仅 `BR_DMA_F_CACHED` 时必需; 风险 R4 的纪律) |
| IRQ register/enable/disable + 级联域 | `core/include/br/core/br_irq.h`;`BR_IRQ_MAX = 64`(:38) | 网卡中断线; 现用 5 条(HEAD), 余量充足 |
| 自检机制(`selftest` 钩子, 生成期开关) | `prototype/docs/decisions/0010-plugin-selftest.md` | ★ **没有协议栈也能把驱动验收到可交付**, 见 §6 阶段 3 |

## 3. 差距

按阻塞强度排序。**阻塞** = 不解决就无法开工; **延迟** = 可先落地后补齐。

| # | 差距 | 证据 | 阻塞 |
|---|---|---|---|
| 1 | **netdev-core 契约不存在**(形状未定) | 无 `framework/netdev-core`;`8-01`:284(O-S5)"v2.0 设计前定";`br_dev.h`:35-37 只有 `BR_CLASS_NONE/CDEV/BDEV` | 🔴 阻断 |
| 2 | ~~无 bh / work-queue~~ **在途落地** | HEAD: `br_irq.h`:78-79 `BR_IRQ_F_DISPATCH_BH` = "Stage 1 忽略 + trace";`8-01`:234"ISR 禁令: 设备/存储域 API 白名单为空"。**工作树**: `core/include/br/core/br_work.h`(102 行)+ `core/src/work/work_core.c` + `core/selftest/work_selftest.c` | 🟡 → 🟢 |
| 3 | **lwIP 的 `api_type` 归类未拍** | `brickie-v0.1.md`:838 判 `third_party`;同文 `:136` 规定 `runtime_adapter` "**只允许**依赖 `native`" ⇒ `runtime/posix → service/lwip` 这条**必需边**(`11-01`:41-42 的 socket 路由)**判红** | 🔴 阻断(协议栈) |
| 4 | **平台数据缺 virtio-mmio** | `memmap.c`:56/58/60 的 MMIO region 只有 GICD/GICR/UART0;绑定表 `BR_IRQ_BOARD_NR = 5u`(HEAD `board_irq.h`:41)无 virtio 线。**实测 DTB**: 32 槽 `0x0a000000 + N*0x200`(size `0x200`), IRQ = **SPI 16+N ⇒ GIC INTID 48+N** ⟳ | 🟡 必修(~50 行) |
| 5 | **coop 无抢占** | `sched_core.c`:25(ADR-0006 I4)"COOP 从不置位 ⇒ 无抢占、IRQ 出口不切栈" | 🟡 延迟影响(见 §4 注) |
| 6 | bdev-core / runtime/posix 未落地 | 无 `framework/bdev-core`;`11-01`:35 列 runtime/posix 为 M2 | 🟡 可绕(抄 cdev; socket 面后置) |
| 7 | `3-02` SLOW 级联域不可用 | `br_irq.h`:136"SLOW 域不可用(其存在前提是 bh)" | 🟢 与本刀无关(除非 PHY 走 I²C 级联) |

**#2 的含义(重要)**: 在途的 bh **不是**"设计 §11.3 的线程上下文 bh", 而是**收紧形态** —— 执行时机 = IRQ 出口(eoi 之后、ERET 之前), 在被中断线程的栈上, `PSTATE.I == 1`, **禁阻塞/禁让出**, 单次出口上限 `BR_WORK_BH_BUDGET = 4`(`br_work.h`:44-47)。对网络驱动的推论:
- ✅ 可用于 **SLOW 域 demux** 与"轻量的收帧后处理"(唤醒线程、更新计数);
- ❌ **不可**在 bh 里做重的收帧(拷贝大帧、上送协议栈)——那必须在 RX 线程里。
- ⇒ **结论不变**: netdev 的重活走线程, bh 只是多一条可选的轻量通路。这意味着 **netdev 契约不应该依赖 bh**(否则 Stage 2 换 bh 形态时要改契约);它应依赖 `br_sem_give`(已在 HEAD 可用), bh 仅作实现优化。

## 4. 移植边界: 为什么选 netif

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 按 netdev 契约新写** | 定 netdev-core + 手写 virtio-net | ✅ 形状对; 与 `1-03`:169 的 `io/virtio-net` 一致 |
| **B. 移植 Linux 驱动**(stmmac/fec/dwc) | 剥离 sk_buff / NAPI / PCI / DMA-API | ❌ 框架耦合过深。**但寄存器序列**(描述符环布局、MAC 配置、PHY 初始化)是**可搬运的知识** —— 搬知识不搬代码 |
| **C. 移植 RTOS 驱动框架**(RT-Thread `netdev` / Zephyr `net_if`) | 抄框架 + 驱动分层 | ⚠️ 最实用的**参照**: 两者与 brickOS 的 `*-core` 子分类同型, 可作 netdev-core 的设计参照; 但代码不能直接用(依赖各自的设备模型/工作队列) |
| **D. 移植协议栈的移植层**(lwIP `ethernetif.c`) | 只实现 `low_level_output` / `low_level_input` | ✅ **移植真正该发生的边界** |

**三层各选各的参照**: netdev-core 形状 ← RT-Thread/Zephyr(学设计); 器件驱动 ← 厂商寄存器序列(学知识); 协议栈对接 ← lwIP `ethernetif.c`(搬代码)。

**为什么不塞进 cdev**: 形状不匹配 —— 字节流 vs 有边界帧; 消费者发起 vs 设备发起(中断); write 可部分 vs 整帧成败; 驱动 FIFO vs 必须**提前**把缓冲交给硬件(DMA)。硬塞的后果是 `read()` 要发明"半帧就绪"语义, 错误藏在 `br_file_*` 抽象层下难查。

## 5. 需拍板(3 项)

| # | 事项 | 为什么必须现在拍 |
|---|---|---|
| 1 | **`br_netdev_ops_t` 槽位形状**(O-S5) | 形状进 golden(D14): 后补字段 = 二进制不兼容 ⇒ 一次占齐(含 `ioctl`/`suspend`/`resume` 的 D22 预留) |
| 2 | **lwip 的 `api_type`**(`r1/03` P0-3① / `r1/04` D2) | 决定 `runtime/posix → lwip` 是"允许"还是"不需要"。两篇评审都要求先拍;`r1/04` 倾向 `third_party/upstream`。**不拍无法声明协议栈** |
| 3 | **netdev 要不要 VFS 面**(`/dev/eth0` 抓包) | 建议**不要**(形态 B)。若要, 就是**多一个契约**(open_file 钩子 + 帧协议), 需显式决定而非默认 |

附带建议(非拍板项): netdev 的 `open_file` 钩子留空 ⇒ `/dev/eth0` **看得见、打不开**(-ENOTSUP), 与 raw flash 的处置一致(诚实: 看得见打不开 > 看不见)。原始帧抓取留 v2 调试用。

## 6. 落地顺序(每阶段有可独立验收的判据)

| 阶段 | 内容 | 判据 | 依赖 |
|---|---|---|---|
| **0. 设计定稿** | 拍 §5 三项; 写 netdev ADR; 回灌 `8-01` O-S5 / `1-03` v2.0 | 设计落盘; `brickie check` 能表达 netdev 依赖 | — |
| **1. netdev-core** | 新插件 `framework/netdev-core`: ops + 注册 + ISR↔线程交接 + 自检骨架 | `[NETCONF]` 全绿, 且**用假 ops 表**(非法注册/重名/未注册取回/kick-wait 往返)⇒ **不需要真硬件** | 阶段 0;`br_sem_give` |
| **2. 平台数据** | virtio-mmio region + IRQ 绑定 + `BR_IRQ_BOARD_NR` | region 计数 +1(`[MEMCONF]`);`br_irq_register(virtio0)` 成功 | — |
| **3. virtio-net** | `io/virtio-net`: MMIO 探测 / 特性协商 / vring / RX-TX / ISR | ★ 复用自检机制(ADR-0010): 驱动自带 `virtio_net_selftest()`, 判据 = **环回顾返 + 描述符环一致性 + 中断真的到过**。**无协议栈即可交付** | 阶段 1/2 |
| **4. lwIP** | `service/lwip`(`third_party`)+ 移植 `ethernetif.c` 两个函数 | `netif` 起来 / ARP 自答 / `ip4_addr` 配置成功 | 阶段 3;§5 #2 已拍 |
| **5. 端到端** | QEMU user-mode 网络(`-netdev user`) | ping / UDP 往返 | 阶段 4 + runtime/posix |
| **6. v2 增强** | 精确唤醒(等 preempt) / 零拷贝 / 多队列 | — | v2.0 |

**阶段 3 的验收设计是关键**: brickOS 现无 socket 面, 所以"能 ping"不能当第一个判据。正确做法是**复用已有的自检机制** —— 与其他子系统(`[IRQCONF]`/`[MEMCONF]`/`[VFSCCONF]`)的验收方式完全一致: 自带判据 + 门禁逐 tag 点名。

## 7. 证据索引(可重跑)

> 路径相对 `brickOS/prototype`;`B = build/host/x86-64/linux/bin/brickie`。**⟳** = 本篇撰写时实测。

| # | 命令 | 期望 |
|---|---|---|
| E-1 | `git log --oneline -1` | `724fa56`(本篇基线)⟳ |
| E-2 | `git status --short \| grep -E 'work\|rr'` | 非空 ⇒ **已有在途 bh 改动**(ADR-0011 未落盘), §3 #2 据此标注 ⟳ |
| E-3 | `grep -n 'BR_IRQ_BOARD_NR' platform/qemu-aarch64/include/br/board_irq.h` | HEAD 版 = `5u`;工作树 = `7u`(在途新增 2 条)⟳ |
| E-4 | `qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=/tmp/v.dtb -m 128M -nographic` 后解析 `/tmp/v.dtb` | 32 个 `virtio,mmio` 节点, `reg = 0xa000000 + N*0x200`, size `0x200`, `interrupts = 16+N` ⟳ |
| E-5 | `grep -n 'ISR-safe' core/include/br/core/br_sync.h` | `:87` ⇒ `br_sem_give` 是 ISR 白名单成员 ⟳ |
| E-6 | `grep -n 'br_dma_alloc\|dma_addr' core/include/br/core/br_mem.h` | `:69` 恒等(`dma_addr == vaddr`), `:73-74` 分配/释放 ⟳ |
| E-7 | `grep -n 'MEM_.*_BASE' platform/qemu-aarch64/src/memmap.c` | 只有 GICD/GICR/UART0 三条(无 virtio)⟳ |
| E-8 | `ls docs/decisions/`(设计侧) | 至 `0010`;**无 `0011`** ⇒ 在途 ADR 尚未落盘 ⟳ |

## 8. 风险

| # | 风险 | 说明 |
|---|---|---|
| R-1 | **coop 下的吞吐/延迟** | 无抢占(`sched_core.c`:25)⇒ 中断唤醒后要等当前线程到显式切换点。语义正确、性能受限;与 `7-01` R-S1"poll v1 在 preempt 下的延迟毛刺"同类, 归宿同为 v2.0 的 preempt |
| R-2 | **在途 bh 形态若再变** | 若 `0011` 最终改成"线程上下文 bh", §6 阶段 1 的交接实现会变。**规避 = netdev 契约只依赖 `br_sem_give`, 不依赖 bh** —— 见 §3 #2 推论 |
| R-3 | **virtio 现代特性** | `VIRTIO_F_ACCESS_PLATFORM`/IOMMU 不需要(恒等映射); MSI-X 需 ITS(GICv3 有但原型未启用)⇒ 用 legacy INTx(单线), 对第一版是正确选择 |
| R-4 | **cache 一致性** | vring 建议非缓存; 数据帧若 `BR_DMA_F_CACHED` 必须 DMA 前后显式 `br_mm_cache_flush/invalidate`(风险 R4 的纪律), 漏了是"偶发帧损坏"这类难查故障 |
| R-5 | **`8-01`:284 的措辞把 netdev 与 lwip 绑定** | O-S5 只提"netdev-core + service/lwip 对接", 没提 io 驱动。本篇 §4 的边界论证(移植发生在 netif)是对它的**细化**, 建议回灌一句 |

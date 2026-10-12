# 8-03 — USB 主机栈与 libusb 移植: 差距分析

> 章节: **8-device**。状态: **分析稿**(结论 + 边界论证 + 落地顺序; 需拍板项在 §5, 本篇不代替决策)。
> 定位: 回答一个问题 —— **"libusb 能不能移植进 brickOS"**。方法与 `8-02` 同型: 先定移植边界, 再列差距。
> 基线: 设计侧 HEAD `6aae49a`; 原型 HEAD **`e7cf0b5`**(`brickOS-prototype-v0.x.0`, `product.toml` 版本 `0.2.0.0`; 工作树有在途改动)。
> 依赖: `8-01`(设备体系/D20 子分类/形态 A·B·C)、**`8-02`(netdev 移植差距: 本篇的方法样板)**、`11-01`/`11-02`(服务清单与 POSIX 子集对账)、`1-01` §7.6(三方移植双模式)、`7-01` O-S3(热插拔事件模型)、原型 ADR-0021(PCIe 机制)/ADR-0024/0026(三方隔离)。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **HCD** | Host Controller Driver | USB 主机控制器驱动(xHCI/EHCI/OHCI/UHCI) |
| **TRB** | Transfer Request Block | xHCI 的传输/命令/事件环元素 |
| **URB** | USB Request Block | 内核 USB 传输请求(libusb `submit_transfer` 的对应物) |
| **usbfs** | USB filesystem | Linux 把 usbcore 暴露给用户态的字符设备面(`/dev/bus/usb/BBB/DDD` + `USBDEVFS_*` ioctl) |
| **sysfs / udev** | — | libusb 在 Linux 上取**拓扑**与**热插拔事件**的两个外部来源 |
| **api_type** | — | 插件 API 分类(`native`/`runtime_adapter`/`third_party`; `1-01` §6.3 表 C) |

## 1. 结论

1. **libusb 不能"移植", 只能"接上"—— 它下面那一层 brickOS 没有。** libusb 是宿主侧**用户态**库, 自身**不含任何 HCD, 也不含 USB 核心**(枚举/地址分配/roothub·hub/URB 调度/接口认领)。在 Linux 上它 100% 依赖内核 `usbcore` 经 **usbfs** 暴露的设备节点与 ioctl(`linux_usbfs.c` 定 `/dev/bus/usb` + `USBDEVFS_{CONTROL,BULK,SUBMITURB,REAPURB,CLAIMINTERFACE,SETCONFIGURATION,RESET,GET_CAPABILITIES,…}`), 拓扑读 sysfs, 热插拔读 udev/netlink。brickOS 今天**一件都没有**: 我方源码全树 grep `usb` 零命中, `io/` 只有 `uart-pl011`, 无 `framework/usb-core`, 也没有 `pipe`/`eventfd`。
2. **真实成本在它下面的栈, 不在库本身。** libusb 侧增量 ≈ **1 个 backend + 2 个 OS 适配文件**(兑现 `usbi_os_backend` 的 `os/<new>_usb.c`, 以及 `events_*`/`threads_*` 的对应物); 参照 libusb 现成最简 backend: `netbsd_usb.c` **617 行**、`sunos_usb.c` 1619 行(而 `linux_usbfs.c` 2818 行是"内核已把活干完"的薄层)⇒ 一个新 backend 量级 **1–2k 行**。它被**门控**在 xHCI 驱动 + USB 核心之后 —— 两者差一个数量级。**不存在"libusb 是主要工作量"的版本。**
3. **先问"谁需要 libusb", 再决定要不要它。** 若需求是**某个 USB 类设备**(CDC 串口 / mass storage / HID), 正确做法是**不移植 libusb**: 在 `usb-core` 上写类驱动, 出口是 cdev/bdev/netdev —— 与 `8-02` §4 "边界选在 `netif` 而不是 driver"是同一方法论。只有当诉求是"**APP 要任意 USB 器件访问**"时 libusb 才值得; 那时它作为 `third_party` 原件 + `adapter/libusb/`, 接在 `usb-core` 之上(§4 方案 A)。

## 2. 已有能力(可直接用, 不需新建)

| 能力 | 位置(原型) | 对 USB 的意义 |
|---|---|---|
| **PCIe 机制**(ECAM 访问/总线枚举/BAR 尺寸探测与分配/INTx swizzle) | `framework/pci-core`(+ ADR-0021) | ★ **最大的成本节省**: QEMU virt 上 `-device qemu-xhci` 就是 PCIe 端点, 寄存器窗口与中断线已能拿到 |
| **子分类框架的完整模板** | `framework/cdev-core/src/cdev_core.c` + `cdev_selftest.c` | `usb-core` 照它的骨架写(槽位池/注册校验/取回/自检)——与 `8-02` §2 对 netdev-core 的处置相同 |
| ISR→线程交接 | `br_sem_give`(ISR-safe 白名单)+ `br_work_submit`(bh) | 传输完成路径: ISR 只给信号量, 重的收尾在线程 |
| DMA + cache 纪律 + IRQ 余量 | `br_dma_alloc`(恒等映射, `dma_addr == vaddr`)+ `br_mm_cache_flush/invalidate`;`BR_IRQ_MAX = 64` / `BR_IRQ_BOARD_NR = 11u` | xHCI 的 DCBAA/命令环/事件环/TRB 缓冲**不需要地址翻译**; 一条 INTx 线, 余量充足 |
| **三方隔离的机械判据** | `third_party/`(逐字节上游只读)+ `adapter/<插件>/{compat,glue}`; `tools/check-third-party-pristine.sh`(ADR-0026) | libusb 走同一条路: 上游原件 + 我们只写 `compat/`(它要的环境)与 `glue/`(我们的对接) |
| 自检机制(生成期开关) | 原型 ADR-0010 | **无真机也能把 HCD 验收到可交付**(见 §6 阶段 4) |

## 3. 差距(按阻塞强度排序)

| # | 差距 | 证据 | 阻塞 |
|---|---|---|---|
| 1 | **无 HCD**(xHCI/EHCI/…): 寄存器/环/TRB/端口状态机全缺 | `ls prototype/io` = 只有 `uart-pl011`; 我方源码 `grep -rli usb` 零命中 | 🔴 |
| 2 | **无 USB 核心**(枚举/地址分配/roothub·hub/端点/传输队列/接口认领) | 无 `framework/usb-core`; `8-01` §3 子分类表只有 cdev/bdev(flash 子型) | 🔴 |
| 3 | **libusb 的事件底座缺件**: 需要 `eventfd` 或 `pipe()`+`fcntl`+多 fd `poll` | libusb `os/events_posix.c:26` `#include <sys/eventfd.h>`、`:84` `pipe2()`; 原型 core/runtime 无 `eventfd`/`pipe`。runtime/posix 的 `poll` 是 `br_file_poll` + sleep(`11-02` §2.1, TR-B, 粒度 ≈100 ms) | 🔴(可绕, 见 §4 注①) |
| 4 | **libusb 的 backend 是编译期内建, 不是公开扩展点** | `libusb/libusbi.h:1475` `extern const struct usbi_os_backend usbi_backend;` —— 单个全局符号, 由被选中 backend 的 `.c` 定义; libusb 无"注册 backend"API | 🟡(可绕, 见 §4 注②) |
| 5 | **runtime/posix 的 pthread 缺口** | 无 TLS(P-4)、无 `mutex/cond_destroy`(P-5)、`usbi_cond_timedwait` 要墙钟(P-1); `11-02` §1 | 🟡 |
| 6 | **无 sysfs/udev 等价面**: 拓扑与热插拔没有来源 | `linux_usbfs.c:76` `/dev/bus/usb`、`SYSFS_DEVICE_PATH` 目录扫描; brickOS 只有 devfs 的**平面**注册表 | 🟡 |
| 7 | **热插拔/运行时挂载的事件模型未定** | `7-01` O-S3: "事件模型 v2 设计(与 USB 栈联动), 挂载落地不早于 v3" | 🟡(延迟) |
| 8 | **USB 不在任何版本清单里** | `1-03` §1 各版插件清单无 USB 条目(v2.0 只有 `io/virtio-net`); 仅注释提"与 USB 栈联动" | 🟡(排期) |

## 4. 移植边界: 三条路, 选最窄的消费者契约

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 新 backend** | `adapter/libusb/glue/` 下兑现 `usbi_os_backend` 的全部槽位, 底下直调 `usb-core` | ✅ 上游 libusb 主体保原件; ❌ 仍需 `usb-core` 提供全部语义, `events_*`/`threads_*` 的适配跑不掉(§3 #3) |
| **B. usbfs 模拟** | 内核侧做 `/dev/bus/usb/BBB/DDD` cdev + `USBDEVFS_*` ioctl + sysfs 型文本面, 让 `linux_usbfs.c` **零改动**跑起来 | ✅ libusb 完全不必碰(最合 `third_party/` 纪律); brickOS 的 ioctl 编码本就 Linux 兼容(`8-01` SD-5)⇒ 数值可复现。❌ 内核要长期背一个 Linux ABI; 且仍要 pipe/eventfd |
| **C. 不移植 libusb, 定义 native 面**(推荐默认) | `usb-core` 直出类驱动(cdev/bdev/netdev); 若确需 libusb 形状的 API, 再做 `iface-libusb` 薄皮肤再导出 native `service/usb` | ✅ 最窄、可裁剪, 与 `8-01` 形态 B/C 及 D19 一致;✅ 不把 libusb 的 POSIX 依赖变成硬前置。❌ 不是"原样跑 libusb 用户代码" |

**边界论证(与 `8-02` §4 同型)**: `usbi_os_backend` 是 **libusb 的**接缝, 不是 **brickOS 的**接缝。移植该发生的地方是"消费者真正需要的最窄契约": 类设备 ⇒ 设备类 ops(方案 C); 任意器件访问 ⇒ `usb-core` 的通用传输面(方案 A/B)。

> **注①(§3 #3 的绕法)**: libusb 允许 backend **不带可轮询的事件源** —— 实现 `handle_transfer_completion` 而非 `handle_events`, 由 backend 主动 `usbi_signal_transfer_completion()`, 把"多 fd poll"降级为"一次唤醒", 与 brickOS 的 `br_sem_give` + 线程模型同形。但 libusb **核心**仍会建一条内部事件管道(`events_posix.c` 的 `eventfd`/`pipe`)供 `libusb_handle_events` 阻塞与唤醒 ⇒ 仍需一个"可阻塞/可唤醒"的内核对象: 要么新增 `pipe` 语义, 要么在 `events_brickos` 侧整份改用 `br_sem_t`/`br_cond_t`。

> **注②(§3 #4 的绕法)**: 原型按 ADR-0026 由 `plugin.toml` 的 `sources` **直接编译**选定的上游 `.c` ⇒ `adapter/libusb` 可以**自供** `usbi_backend` 定义、只编译 libusb 的**平台无关半**(`core.c`/`descriptor.c`/`sync.c`/`hotplug.c`…), 从而不必改 libusb 的 `configure.ac`/CMake。代价 = 在 `compat/` 里提供 `config.h` 与一份 `libusb/os/` 替换集。

## 5. 需拍板(5 项)

| # | 事项 | 为什么必须现在拍 |
|---|---|---|
| 1 | **`usb-core` 是否第四个子分类框架**(依赖 dev-core, 对称 cdev/bdev/netdev) | D20 给了答案空间但没定; ops 形状进 golden(D14)⇒ **一次占齐**(含 `ioctl`/`suspend`/`resume` 的 D22 预留) |
| 2 | **边界取 A/B/C**(§4) | 决定要不要在内核侧**永久**背一个 usbfs ABI, 以及 libusb 是"原件"还是"改写" |
| 3 | **libusb 的 `api_type` 与导出面** | `api_type = third_party`;是否抛 `[[export]]` —— 不抛则能力只经注册表消费(与 `11-01` 对 lwip 的同类争议同源) |
| 4 | **热插拔事件模型的归属** | 与 `7-01` O-S3 **合并拍**: 无它则 USB 只能"启动时枚举一次" |
| 5 | **USB 的版本归宿** | 现不在任何清单; 不排期 ⇒ 本篇停在纸面 |

## 6. 落地顺序(每阶段有可独立验收的判据)

| 阶段 | 内容 | 判据 | 依赖 |
|---|---|---|---|
| **0. 设计定稿** | 拍 §5 五项; 写 USB ADR; 回灌 `8-01`/`1-03`/`7-01` | 设计落盘 | — |
| **1. usb-core** | 新插件 `framework/usb-core`: 设备/端点模型 + 传输队列 + roothub/端口 + 接口认领 + 自检骨架 | `[USBCONF]` 全绿, 且**用假 ops 表**(非法注册/重名/未注册取回/kick-wait 往返)⇒ **不需要真硬件** | 阶段 0 |
| **2. 平台数据** | xHCI 的 MMIO region + IRQ 绑定 | region 计数 +1;`br_irq_register(xhci0)` 成功 | — |
| **3. io/xhci** | MMIO 探测/复位/DCBAA/命令环/事件环/TRB/端口状态机/roothub | 复用自检: 环回顾返 + 端口连上并复位 + 中断真的到过 | 阶段 1/2;`pci-core` |
| **4. 枚举** | `usb-core` 的地址分配 + `GET_DESCRIPTOR` + 配置解析 | QEMU `-device qemu-xhci -device usb-storage` 下描述符逐字段读回正确 | 阶段 3 |
| **5. 第一个真价值消费者** | 类驱动: `usb-storage`→bdev 或 `cdc-acm`→cdev(**这才是产品要的东西**) | 挂载/读写往返 | 阶段 4 |
| **6. libusb(仅当确有需要)** | `third_party/libusb`(原件)+ `adapter/libusb/{compat,glue}`(backend + events/threads 适配)+ 补 runtime/posix 的 P-4/P-5 | `libusb_get_device_list` → `claim_interface` → `bulk_transfer` 往返 | 阶段 4;§5 #2/#3 已拍 |
| **7. 热插拔** | 事件模型 + 运行时挂载 | 插拔一次触发挂载/卸载 | `7-01` O-S3 |

**阶段 5 是关键**: 它把 USB 从"能跑"变成"有用"。若 §5 #2 判 **C**, **阶段 6 可以永远不做** —— 这是本篇最重要的排期含义。

## 7. 证据索引(可重跑)

> 路径相对 `brickOS/prototype`。**⟳** = 本篇撰写时实测。

| # | 命令 | 期望 |
|---|---|---|
| E-1 | `git log --oneline -1` | `e7cf0b5` ⟳ |
| E-2 | `ls io framework \| grep -i usb` | 空 ⇒ 无 USB 件 ⟳ |
| E-3 | `grep -rli usb --include=*.c --include=*.h --include=*.toml core io framework runtime fs service iface app sched platform adapter tests tools` | 空 ⟳ |
| E-3b | `grep -rli usb third_party/` | 空 ⇒ 已 vendor 的 `third_party/uboot-imx/` 只含 `drivers/fs/include/lib`, **不含 `drivers/usb`** ⟳ |
| E-4 | `grep -rn 'eventfd\|pipe(' --include=*.c core runtime framework` | 空 ⇒ 无 libusb 事件底座 ⟳ |
| E-5 | `grep -n 'BR_IRQ_MAX\|BR_IRQ_BOARD_NR' core/include/br/core/br_irq.h platform/qemu-aarch64/include/br/board_irq.h` | `64u` / `11u` ⟳ |
| E-6 | `ls framework/pci-core/src` + `docs/decisions/0021-bdev-pci-emmc.md` | PCIe 机制已落地 ⟳ |
| E-7 | `ls third_party adapter`;`tools/check-third-party-pristine.sh` | 三方隔离机制在 ⟳ |
| E-8 | libusb **v1.0.27**: `libusb/libusbi.h:1475`、`libusb/os/events_posix.c:26`、`libusb/os/linux_usbfs.c:76` | backend 单全局符号 / `sys/eventfd.h` / `/dev/bus/usb` ⟳ |
| E-9 | `wc -l libusb/os/{netbsd_usb,sunos_usb,linux_usbfs}.c` | `617` / `1619` / `2818` ⇒ §1 #2 的 backend 量级估计 ⟳ |

## 8. 风险

| # | 风险 | 说明 |
|---|---|---|
| R-1 | **工作量与排期** | HCD + USB 核心是数人月量级, 且 USB 不在任何版本清单(§3 #8)⇒ **先拍排期, 否则只是文档** |
| R-2 | **libusb 把 runtime/posix 变成硬前置** | 与 `1-01` §7.6 "runtime/posix 成为引力中心"的诚实代价同源; 方案 C 可规避 |
| R-3 | **coop 无抢占 + ≈100 ms tick** | libusb 同步 API(`libusb_control_transfer` 等)要等事件线程完成 ⇒ 延迟受 tick 限制; 吞吐/延迟达标要等 preempt(v2) |
| R-4 | **usbfs ABI 的长期负担**(方案 B) | 内核侧长期背 Linux ioctl 语义; libusb 上游改 ioctl 集要跟着改, 与"三方原件"纪律冲突 |
| R-5 | **无 sysfs/udev** | 拓扑与热插拔是"发明"而非"移植"; 方案 C 下只需 `usb-core` 自己的设备模型, 负担最小 |
| R-6 | **cache 一致性** | xHCI 的 DCBAA/命令环/事件环/TRB 是 DMA 结构: 漏 `br_mm_cache_*` 是"偶发传输损坏"这类难查故障(`8-02` R-4 同源) |

# eMMC(SDHCI)→ bdev 移植 gap 分析(拍板前)

> 状态: **决策前分析**(不是 ADR,不是实现记录)。本文只回答"差异在哪、缺口多大、要不要做、怎么做"。
> 拍板后另立 ADR(建议 `docs/decisions/0021-bdev-core-and-emmc.md`)并进入实现。
>
> | 项 | 值 |
> |---|---|
> | 目标 | ① 实现 `framework/bdev-core`(设计已冻结)② 移植通用 eMMC 驱动并经 bdev 对接 |
> | 移植来源 | u-boot NXP 分支:`nxp-imx/uboot-imx` `drivers/mmc/**` + `include/mmc.h` + `include/sdhci.h` |
> | 来源 pin | branch `lf_v2026.04`, commit `bb41975aeb4cbd153b49c9fd62da1d64cd65968d`(2026-09-03) |
> | brickOS 基线 | `prototype/` HEAD `21ff970`(v0.2.0) |
> | 运行目标 | QEMU `virt` + `-device sdhci-pci` + `-device emmc`(aarch64, GICv3) |
> | 本地取证 | 源码副本在**仓库外**的 `unikernel-dev/.scratch/uboot-mmc/`(含 u-boot 与 QEMU v10.2.1 相关文件) |

---

## 0. 结论(TL;DR)

1. **bdev-core 基本无 gap**:设计 `7-storage/7-02-bdev.md` §1 已把 ops/几何/注册/取回写死,`dev-core` 里
   `BR_CLASS_BDEV = 2` 的占位也已留好。照 `framework/cdev-core` 的成熟范式做即可,不依赖任何新平台能力。
   约 1 个新框架插件 + 8~12 条自检。
2. **eMMC 侧真正的"通用"是两层,不是一个文件**:
   - `drivers/mmc/mmc.c`(3352 行)= **协议核心**(CMD/ACMD 状态机、CID/CSD/EXT_CSD、块读写、总线宽度/时钟);
   - `drivers/mmc/sdhci.c` + `include/sdhci.h` + `sdhci-adma.c`(1050+546+87 行)= **通用 SDHCI 主机层**;
   - `drivers/mmc/mmc-uclass.c`(553 行)= u-boot DM 粘合层,**不可移植**,由 brickOS 插件模型 + bdev 取代;
   - `fsl_esdhc_imx.c` / `fsl_esdhc.c`(NXP i.MX 专有主机)**与 QEMU `sdhci-pci` 无关,不移植**。
     分层的价值正在这里:换到 i.MX 真机时只换主机层,协议核心不动。
3. **平台侧只有一块大缺口:PCIe 枚举**。`virt` 上挂 eMMC 必须走 `-device sdhci-pci`,而要自己
   访问 ECAM、枚举总线、**给 BAR 分配地址并打开内存空间/Bus Master**(QEMU 无固件时 BAR 出厂未映射,
   已实测)。这一块在当前 platform 里**一行都没有**。
4. **有捷径**:把 QEMU 机器参数改成 `highmem=off`,PCIe ECAM 从 `0x4010_0000_00` 落到 `0x3f00_0000`,
   正好在当前 platform 的**低 1 GiB 背景 Device 映射**里(且 32 位 MMIO 窗口 `0x1000_0000` 也在其中)
   ⇒ **不用改页表/不用扩 MMU**,只加 region 声明(对 Device 背景是 no-op)。
5. **u-boot 的 SDHCI 是轮询的**(`SDHCI_SIGNAL_ENABLE = 0`,不注册 IRQ)。所以 v1 **不需要** ISR/bh/
   wait-queue —— 这与设计 `7-02-bdev.md` §5 画的"ISR→bh→唤醒"**不是一回事**(那是 virtio-blk 的形态)。
   沿用轮询 = 大幅省事,但要在 ADR 里如实记偏离。
6. **两处设计欠账**:① bdev 的 `flush` 是必填语义,而 u-boot `mmc.c` **没有** cache flush 实现(只有
   BKOPS 相关宏)。v1 只能"等卡不忙 + 返回 0",或自行加 CMD6 `CACHE_FLUSH`。② u-boot 用 `-ECOMM`,
   `br_error.h` 没有这个码(需 append-only 补 `BR_ECOMM = 70`,或有损映射到 `-EIO`)。
7. **一个阻断级非技术缺口:许可**。u-boot 全部这些文件是 **GPL-2.0+**;brickOS 是 **WTFPL**。
   逐行移植 = 衍生作品 = GPL 义务传染到镜像。**这条必须先拍板**(clean-room 重写 / 接受 GPL / 放弃移植)。
8. QEMU 能力边界(实测):SDHCI **52 MHz 基频、1/4/8-bit、HS(52)、SDMA/ADMA1/ADMA2、无 SDR50/104/
   DDR50/HS200/HS400、无 64 位 DMA**;eMMC 模型 `CARD_TYPE=0b11`(仅 26/52 MHz)、EXT_CSD rev5、支持分区。
   ⇒ 移植目标面 = **legacy + HS**,不是全速模式。

---

## 1. 两侧架构差异(u-boot vs brickOS)

| 维度 | u-boot | brickOS 原型 | 移植含义 |
|---|---|---|---|
| 设备模型 | DM + uclass + `struct udevice` + 设备树 | 插件(`brick.toml`)+ 静态组合 | `mmc-uclass.c` 整文件不移植;配置从 DT 改为**静态平台数据头**(照 `pl011_internal.h` 模式) |
| 块设备出口 | `blk-uclass`(`blk_desc`/`lbaint_t`/`blk_ops`/分区 `part.h`) | **bdev-core**(`br_bdev_ops` + LBA/扇区) | `mmc_bread/bwrite` 改写成 `br_bdev_ops.read/write`;丢掉 `blk_desc`/hwpart/blk_dselect |
| 命名空间 | `mmc0`/`mmc 0` 命令行 | dev-core 扁平设备名 + devfs | bdev 设备名如 `emmc0`,自动出现在 `/dev/emmc0`(v1 看得见打不开) |
| 初始化时机 | `mmc_initialize()` + 各驱动 probe | 插件 CORE 相 `*_init()` + EARLY/CORE/START | PCIe 枚举放 EARLY/CORE(要在任何 `br_dma_alloc` 之前?见 §4.4) |
| 内存 | `malloc/memalign/free` | `br_malloc/br_calloc/br_free` + `br_dma_alloc`(恒等映射 `dma_addr==vaddr`) | 直接替换;DMA 表与 bounce buffer 走 `br_dma_alloc` |
| 时间 | `udelay/mdelay/get_timer/timer_get_us/time_after` | `br_delay_us/br_delay_ms/br_clock_now` | 直接替换;注意 brickOS 的 `br_delay_*` 在关中断/调度器未起时的行为需核对 |
| 日志 | `printf/debug/log_err/log_warning/pr_debug` | `br_log_info/warn/error/debug` | 直接替换;格式串要改成 brickOS 风格 |
| 错误码 | 负 errno(`-ECOMM/-ETIMEDOUT/-EIO/-ENOMEM/-EINVAL/-ENOSYS`) | `BR_ERR(BR_E*)`(已对齐 Linux 编号) | 缺 `BR_ECOMM`(70),其余齐全 |
| IRQ | 轮询(本驱动)+ 别的驱动用 `irq_install_handler` | `br_irq_register` + `br_work_submit`(bh)+ `br_sem_t` | **本移植 v1 用不上**(轮询);如要中断驱动,核心件已齐 |
| DMA 缓存维护 | `flush_cache/invalidate_dcache/dma_map_single/dma_unmap_single` + `phys2bus` | `br_mm_cache_flush/invalidate` + 恒等映射 | 语义等价且更简单(0 偏移) |
| 时钟/电源/引脚 | `clk`/`regulator`/`gpio`/`pwrseq`/pinctrl 框架 + DT | **无** | QEMU 固定 3.3V/固定 52 MHz/无 CD-WP ⇒ 相关分支整段裁掉 |
| 许可 | GPL-2.0+ | WTFPL v2 | **阻断项**,见 §6 |

---

## 2. 逐文件移植评估

| u-boot 文件 | 行数 | 职责 | 结论 | 移植后落点 |
|---|---|---|---|---|
| `drivers/mmc/mmc.c` | 3352 | MMC/SD 协议核心:初始化序列、CID/CSD/EXT_CSD、块读写、switch、总线宽度/时钟、分区 | **改写移植(主体可保留)**;裁掉 SD/UHS/HS200/HS400/tuning/RPMB/BKOPS 分支后约 900~1200 行 | `driver/emmc/src/mmc_core.c` |
| `include/mmc.h` | 1093 | 结构体/命令码/EXT_CSD 字段/能力位 | **摘录移植**:只取 `mmc_cmd/mmc_data` + 命令码 + 用到的 EXT_CSD 偏移 | `driver/emmc/src/mmc_regs.h`(私有) |
| `drivers/mmc/mmc-uclass.c` | 553 | DM 绑定、`mmc_of_parse`、`blk` 设备创建 | **不移植**(换成 bdev 注册 + 静态平台数据) | — |
| `drivers/mmc/mmc_private.h` | 135 | 内部原型 + `mmc_bread/bwrite` | **部分**:`mmc_send_status/poll_for_busy/set_blocklen` 原型保留 | `driver/emmc/src/mmc_internal.h` |
| `drivers/mmc/sdhci.c` | 1050 | 通用 SDHCI 主机:`send_command`/`transfer_data`/`set_ios`/`set_clock`/`set_power`/DMA | **改写移植(高价值)**;裁掉 DT/regulator/gpio/quirks/UHS;约 500~700 行 | `driver/emmc/src/sdhci_host.c` |
| `include/sdhci.h` | 546 | SDHCI 寄存器/位域/`sdhci_host` | **摘录移植**:寄存器偏移 + 用到的位;丢掉 `struct udevice` 字段与无关 ops | `driver/emmc/src/sdhci_regs.h`(私有) |
| `drivers/mmc/sdhci-adma.c` | 87 | ADMA2 描述符表构造 | **几乎原样移植**(`memalign/flush_cache` 替换) | `driver/emmc/src/sdhci_adma.c` |
| `drivers/mmc/fsl_esdhc*.c` | — | NXP i.MX 专有主机 | **不移植**(QEMU 用通用 sdhci-pci) | — |

> **"通用 eMMC 驱动"的实际形状** = `mmc.c`(协议)+ `sdhci.c`(主机)+ `sdhci-adma.c`(DMA 表)。
> u-boot 没有单独的"eMMC 驱动"文件;eMMC 与 SD 共用 `mmc.c`,`mmc_startup()` 里按响应区分。

---

## 3. 依赖 gap 表(逐个符号)

| u-boot 依赖 | brickOS 对应物 | 状态 |
|---|---|---|
| `malloc/memalign/free` | `br_malloc/br_calloc/br_free` | ✅ 直接替换 |
| `dma_alloc_coherent`/`dma_map_single`/`dma_unmap_single`/`phys2bus` | `br_dma_alloc/br_dma_free`(`dma_addr == vaddr`) | ✅ 恒等映射 ⇒ 0 偏移 |
| `flush_cache/invalidate_dcache_range` | `br_mm_cache_flush/br_mm_cache_invalidate` | ✅ 语义等价 |
| `udelay/mdelay/get_timer/timer_get_us/time_after` | `br_delay_us/br_delay_ms/br_clock_now` | ✅(需核对早期相位可用性) |
| `printf/debug/log_err/log_warning` | `br_log_info/warn/error/debug` | ✅ |
| `readl/writel/readb/writeb` | 直接 `*(volatile br_u32 *)`(照 `driver/uart-pl011`) | ✅(注意 MMIO 是 Device 属性、`-mstrict-align`、只用对齐访问) |
| errno `-EIO/-ETIMEDOUT/-ENOMEM/-EINVAL/-ENOSYS/-ENODEV` | `BR_ERR(BR_E*)` | ✅ |
| `-ECOMM` | **无** | ⚠ 需补 `BR_ECOMM = 70`(append-only)或映射 `-EIO` |
| `struct udevice` / `mmc_get_ops` / `dev_read_*` | 无 | 🔁 用 host-ops 结构体 + 静态平台数据取代 |
| `blk_desc/lbaint_t/part.h/blk_dselect_hwpart` | bdev-core | 🔁 用 `br_bdev_ops` + 几何取代 |
| `regulator/power/clk/gpio/pinctrl/mmc-pwrseq` | 无 | ✂ 裁掉(QEMU 固定) |
| `cyclic_info`(卡检测轮询) | 无 | ✂ 裁掉(非可移除卡,固定存在) |
| `CONFIG_MMC_SDHCI_ADMA/SDMA/QUIRK_*` | 无 | 🔁 固定选择(建议 PIO 先行,ADMA2 次之) |
| `mmc_of_parse`(bus-width/max-frequency/caps) | 无 | 🔁 平台常量头(照 `pl011_internal.h` 的"一份真值,生产与自检共用") |
| eMMC 分区(boot/GP/RPMB)/`mmc_switch_part` | 无 | ✂ v1 只做 user area;分区器是 bdev-core 的另一条线 |
| tuning/HS200/HS400/enhanced strobe | 无 | ✂ QEMU 不支持 |

---

## 4. 平台能力 gap(PCIe —— 唯一的大缺口)

### 4.1 现状

`platform/qemu-aarch64` 现有能力:GICv3、arch timer、PL011 早期 console、4 KiB 恒等映射 + region 表、
三池(heap/contig/page/dma)。**没有任何 PCI/PCIe 代码**;`memmap.c` 的 region 只声明了
image/stack/heap/contig/page/dma/reserved/GICD/GICR/UART0。

### 4.2 QEMU `virt` 的 PCIe 事实(实测)

| 项 | 值(实测/源码) |
|---|---|
| 控制器节点 | `/pcie@10000000`,`compatible = pci-host-ecam-generic`(GPEX) |
| ECAM(默认 highmem) | `0x4010_0000_00`, 256 MiB |
| ECAM(`highmem=off`) | **`0x3f00_0000`, 16 MiB** |
| 32 位 MMIO 窗口 | `0x1000_0000` .. `0x3eff_0000`(`size 0x2eff0000`) |
| PIO 窗口 | `0x3eff_0000`, 64 KiB |
| 总线号范围 | 默认 `0..0xff`;`highmem=off` 实测 `0..0x0f` |
| INTx A..D | GIC SPI 3..6(INTID 35..38,`a15irqmap[VIRT_PCIE]=3`) |
| `sdhci-pci` ID | **`1b36:0007`**, class `0x0805`(System SDHCI),BAR0 |
| BAR 出厂状态 | **未映射**(`info pci` 显示 `BAR0: 32 bit memory (not mapped)`)⇒ 必须自己分配 |
| eMMC 挂载方式 | `-drive if=none,id=emmc0,format=raw,file=… -device sdhci-pci -device emmc,drive=emmc0`(已实测 `info qtree` 确认 `emmc` 挂在 `sdhci-pci` 的 `sd-bus` 下) |

### 4.3 需要新增的能力

1. **ECAM 配置空间访问**:`ecam + (bus<<20 | dev<<15 | fn<<12 | reg)` 的 32/16/8 位读写。
2. **总线枚举**:扫 bus 0 的 dev 0..31 / fn 0..7,匹配 `1b36:0007`(或按 class 0x0805 匹配)。
3. **BAR 探测与分配**:写 `0xFFFFFFFF` 读回尺寸 → 从 32 位 MMIO 窗口分配 → 写回 BAR0 →
   `PCI_COMMAND` 置 **Memory Space Enable + Bus Master Enable**(DMA 必需)。
4. **region 声明**:把 ECAM + MMIO 窗口加进 `memmap.c` 的 region 表(Device 属性)。
5. **QEMU 参数**:`machine` 追加 `highmem=off`;`extra` 追加 `-drive … -device sdhci-pci -device emmc,…`。
6. **门禁镜像**:需要一块 eMMC 后备镜像(空 raw 写后读校验即可,v1 不需要预置文件系统)。
7. **(可选)对外 API**:给 `driver/emmc` 一个"取 BAR 基址"的口子。两种归属见 §7 开放问题 3。

### 4.4 为什么 `highmem=off` 是关键简化

当前 platform 的 MMU 只建了两段:`L1[0]` = 512 × 2 MiB Device 块(覆盖 `0..1 GiB`)、
`L1[1]` = RAM 窗口 8 MiB 的 4 KiB 页。`mmu_region_apply()` 的硬边界是:

- 区间完全落在 `< 0x4000_0000` 且属性 == `BR_MM_DEVICE` ⇒ **返回 0(no-op,背景已对)**;
- 落在 RAM 窗口 ⇒ 逐页改属性;
- 其他(含任何 ≥ 4 GiB 的地址)⇒ `-EINVAL`,因为 `mmu_desc_ptr()` 找不到 L1 表项。

`0x3f00_0000`(highmem=off 的 ECAM)与 `0x1000_0000`(MMIO 窗口)都落在 `L1[0]` 背景里 ⇒
**加 region 声明即为 no-op,零页表改动**。若保持默认 highmem(ECAM 在 `0x4010_0000_00`),则必须新增
L1/L2 表与映射路径 —— 工作量与风险都上一个台阶。这是推荐 `highmem=off` 的硬理由,不是"图省事"。
(附带好处:`highmem=off` 把一切留在 32 位空间,而 QEMU SDHCI **没有 64 位 DMA 能力**,正好匹配。)

---

## 5. QEMU 能力边界(决定移植面)

`sdhci-pci` 的 `capareg = 0x0578_34B4`(默认),逐位解码:

| 位 | 含义 | 值 | 结论 |
|---|---|---|---|
| `[15:8]` | Base Clock Frequency | `0x34`=52 | **52 MHz 上限** |
| `[5:0]`/`[7]` | Timeout clock / unit | 52 / MHz | 正常 |
| `[17:16]` | Max Block Length | 0 | 1024 B |
| `[18]` | 8-bit bus | 1 | ✅ 8-bit |
| `[19]` | ADMA2 | 1 | ✅ |
| `[20]` | ADMA1 | 1 | ✅ |
| `[21]` | High Speed | 1 | ✅ HS |
| `[22]` | SDMA | 1 | ✅ |
| `[24]/[26]` | VDD 3.3V / 1.8V | 1 / 1 | ✅ |
| `[28]` | 64-bit DMA | 0 | ❌ 只能 32 位 DMA |
| `CAPABILITIES_1` | SDR50/SDR104/DDR50 | 全 0(`uhs=0`) | ❌ 无 UHS |
| `HOST_VERSION` | spec = `sd-spec-version=2` | SDHCI 3.0 | divisor ≤ 2046 |

eMMC 模型(`hw/sd/sd.c`):`EXT_CSD_CARD_TYPE = 0b11`(仅 26/52 MHz)、`EXT_CSD_REV = 5`、
`EXT_CSD_STRUCTURE = 2`(高容量)、`PARTITION_SUPPORT = 0b111`、支持 CMD6/EXT_CSD/硬件分区。

⇒ **移植目标面**:MMC legacy(26 MHz)+ HS(52 MHz)、1/4/8-bit、单块/多块读写、EXT_CSD、
`CMD6` 总线宽度与 HS_TIMING。**不需要**:UHS/DDR/HS200/HS400/tuning/1.8V 切换/RPMB。

### 5.1 eMMC 的 flush 语义(设计欠账)

- 设计 `7-02` 把 `flush` 列为**必填**("必须落介质,page cache 依赖")。
- u-boot `mmc.c` **没有** eMMC cache flush 实现(EXT_CSD `CACHE_FLUSH` 未用;只有 BKOPS 开关宏)。
- v1 可选口径:`flush = mmc_poll_for_busy()`(写到卡内部的块已经落闪存,返回 0),并在头/ADR 如实写
  "未实现 CACHE_FLUSH";或自行实现 CMD6 `SET_BIT[32]`(工作量小但需 EXT_CSD `CACHE_CTRL` 判定)。

### 5.2 轮询 vs 中断(与设计 §5 的偏离)

- u-boot `sdhci.c` 在 `sdhci_send_command()`/`sdhci_transfer_data()` 里 `udelay()` 轮询 `INT_STATUS`,
  并把 `SDHCI_SIGNAL_ENABLE` 写 0 —— **不产生中断**。
- 设计 `7-02 §5` 的时序图是 virtio-blk 的 ISR→bh→唤醒。若要在 eMMC 上对齐,需自研:开 `SIGNAL_ENABLE`、
  PCIe INTx(SPI 35)路由、ISR 只清源、bh 里完成传输、`br_sem_give` 唤醒。这是**增量**,不是移植内容。
- 建议 v1 轮询(与 u-boot 一致、与当前 `driver/uart-pl011` 的"v1 轮询"口径一致),中断留 v2 并在 ADR 登记。

---

## 6. 许可 gap(阻断级,必须先拍板)

| 文件 | SPDX |
|---|---|
| `drivers/mmc/mmc.c` | `GPL-2.0+`(Copyright 2008 Freescale / 2020 NXP / Andy Fleming) |
| `drivers/mmc/sdhci.c` | `GPL-2.0+`(Copyright 2011 Marvell) |
| `drivers/mmc/sdhci-adma.c` | `GPL-2.0+` |
| `include/mmc.h` / `include/sdhci.h` | `GPL-2.0+` |

brickOS 仓库 `LICENSE` = **WTFPL v2**,所有 `brick.toml` 的 `license = "WTFPL"`。

**逐行/逐结构移植并静态链进同一镜像 = 衍生作品**,GPL-2.0+ 的义务(源码提供、同许可分发)会覆盖整个镜像。
三条路:

| 方案 | 说明 | 代价 |
|---|---|---|
| **A. clean-room 重写(推荐)** | 只把 u-boot 当**行为参考**,按 SD/MMC 物理层规范 + SDHCI 规范重写;寄存器表/命令码/EXT_CSD 偏移是规范事实,不受版权保护 | 工作量大一些,但许可干净、代码更贴合 brickOS 风格(无 libc/负 errno/静态池) |
| B. 接受 GPL | 把移植文件标 `GPL-2.0-or-later`,整镜像按 GPL 分发 | 需你/法务拍板;与现有 WTFPL 生态不一致 |
| C. 第三方目录隔离 | `third_party/uboot-mmc/` + GPL 文本 + NOTICE | **静态链接下仍构成组合作品**,"隔离"在裸机镜像里基本无意义,不建议 |

> 注:代码里已经大量存在"参考 u-boot/Linux"的注释,但那些是**独立实现**(如 PL011、GIC 驱动)。
> 本项的风险量级不同 —— 是把整块 GPL 源码搬进来。这条不拍板,后面都别动。

---

## 7. bdev-core 侧 gap(小,可先行)

设计已给全,逐条核对后**没有设计缺口**,只有实现取舍:

| 项 | 设计原文 | 实现取舍 |
|---|---|---|
| ops 形状 | `read/write/flush/geometry` + `ioctl/suspend/resume` 预留 | 直接照抄;`NULL ⇒ -ENOTSUP` |
| 成功返回 | `read/write` 返回传输**扇区数** | 明确写入头文件 |
| 几何 | `sector_size/n_sectors/max_rw/align/flags` | `max_rw` 取 `min(65535, 卡能力)`(SDHCI 块计数 16 位);`align` = DMA 对齐(建议 4,`BR_DMA_F_ALIGN4K`?按 SDHCI 描述符要求 = 8) |
| 注册/取回 | `br_bdev_register` / `br_bdev_get` | 依赖 dev-core(init)+ vfs-core(type,`open_file` 钩子签名)-> 与 cdev-core 同构 |
| raw 文件面 | **v2**(`open_file = BR_NULL`) | `/dev/emmc0` 看得见打不开 —— 与 flash 同款,建议自检钉判据(照 TC-CDEV-008) |
| 分区映射器 | SD-9,v1.x | 建议 v1 先不做(只留 API 设计),避免和 eMMC 分区/RPMB 混淆 |
| `[[export]]` golden | `br-bdevcore.txt` | 新建插件要给 `hash`;用 `brickie iface` 出快照后**抄回**(与 cdev-core 流程一致) |
| errno | SD-10 子集 | 需 append `BR_ECOMM`(见 §3) |

---

## 8. 建议的落地架构

```
framework/bdev-core             ← 新框架插件(独立可交付,无平台依赖)
  include/br/dev/br_bdev.h        br_bdev_geom_t / br_bdev_ops_t / br_bdev_register / br_bdev_get
  src/bdev_core.c                 静态池 + 几何校验 + BR_CLASS_BDEV 注册(open_file = NULL)
  src/bdev_selftest.c             TC-BDEV-*(注册/取回/几何/重名/表满/错误码/看不开)

platform/qemu-aarch64           ← PCIe 枚举(唯一平台新能力)
  src/pci_gpex.c                  ECAM 读写 + 枚举 + BAR 分配 + command 使能
  include/br/platform/br_pci.h    br_pci_find(vendor,device) → {bar0, ...}  (归属见开放问题 3)

driver/emmc                         ← 新 I/O 插件(三层,内部分文件)
  src/mmc_core.c                  CMD/ACMD 状态机、CID/CSD/EXT_CSD、块读写(host-ops 之上)
  src/mmc_internal.h / mmc_regs.h
  src/sdhci_host.c                SDHCI 寄存器/命令/数据传输/set_ios/clock/power(PIO 先行 → ADMA2)
  src/sdhci_regs.h / sdhci_adma.c
  src/emmc_bdev.c                 适配:read(ins)→lba+n_lba;write;flush;geometry;br_bdev_register
  include/br/driver/br_emmc.h         器件契约:设备名、几何上限、ioctl(可选 GET_INFO)
  src/emmc_selftest.c             TC-EMMC-*(经 bdev API 的端到端)

tests/gates.toml                ← 新增 `emmc-test` 门禁(或并入 fs-test)
product.toml                    ← [select] 加 3 件;允许边(若 app/EMMC 自检需要)
```

宿主可测部分(不必等 QEMU):`mmc_core.c` 用**假 host**(记录命令、按脚本回响应)可在 `[[hosttest]]`
里跑完整初始化序列与块读写;`bdev-core` 也可宿主测。这能先把协议核心的证据拿到手。

---

## 9. 分期计划与验收判据(供拍板后排期)

| 期 | 交付 | 验收判据 |
|---|---|---|
| **P0** | `framework/bdev-core` | `[BDEVCONF] SUMMARY pass=N fail=0`;宿主 `bdev-test` 绿;`brickie check` 0 错 |
| **P1** | platform PCIe(ECAM+枚举+BAR)+ `highmem=off` + region | 自检打印 `pci: found 1b36:0007 bar0=0x1…`;既有 7 道门禁回归绿(机器参数变更的影响面) |
| **P2** | `driver/emmc` 协议核心 + 假 host 宿主用例 | 宿主用例覆盖:go_idle→op_cond→CID→RCA→CSD→EXT_CSD→select→set_blocklen→单/多块读写 |
| **P3** | SDHCI 主机(PIO)+ 真机 init/几何 | QEMU 日志:容量/块长/总线宽度正确;写后读回校验一致 |
| **P4** | bdev 对接 + 门禁 | `[EMMCCONF] pass=N fail=0`;`/dev/emmc0` 在 devfs 可见;`brickie test emmc-test` 绿 |
| **P5(可选)** | ADMA2 / 中断 / CACHE_FLUSH / 分区 / RPMB | 按需另立刀 |

**工作量粗估**(clean-room 方案):P0 ≈ 0.7 kLOC;P1 ≈ 0.4 kLOC;P2 ≈ 1.0 kLOC;P3 ≈ 0.6 kLOC;
P4 ≈ 0.3 kLOC;加用例/清单/文档 ≈ 0.6 kLOC ⇒ **总量约 3~4 kLOC + 门禁**。

---

## 10. 需要拍板的问题(带推荐)

| # | 问题 | 推荐 |
|---|---|---|
| 1 | **许可**:GPL-2.0+ 的 u-boot 代码怎么处理? | **A. clean-room 重写**(u-boot 仅作行为参考) |
| 2 | 移植粒度:协议核心与 SDHCI 都做,还是先只做 SDHCI? | 都做(只做 SDHCI 没有 MMC 协议就点不亮卡) |
| 3 | PCIe 归属:`platform/qemu-aarch64` 内建 vs 新 `framework/pci-core`? | 先 platform 内建 + `br_pci_*` 小 API;若将来要接第二个 PCI 设备/第二个板子再升格为框架件(此时易,因为平台侧已是 host 抽象) |
| 4 | `highmem=off` 可否? | **可**(零页表改动 + 匹配 32 位 DMA);代价是机器参数偏离 QEMU 默认 |
| 5 | v1 轮询还是中断(设计 §5 是中断)? | **v1 轮询**(与 u-boot 一致),中断留 v2 并登记偏离 |
| 6 | DMA 路径先做哪个? | **PIO 先行**(协议正确性优先),ADMA2 第二;SDMA 可不做 |
| 7 | `flush` 口径? | v1 = 等卡不忙 + 返回 0,如实写"未实现 CACHE_FLUSH";P5 再补 CMD6 |
| 8 | eMMC 分区/RPMB? | v1 只 user area |
| 9 | 插件拆分:`driver/emmc` 一件 vs `driver/mmc` + `driver/sdhci` 两件? | **一件 `driver/emmc`**(内部三层分文件);两插件会让"通用主机 vs 器件"过早固化,且当前只有一个消费者 |
| 10 | 门禁镜像从哪来? | 门禁脚本用 `dd` 造空 raw 镜像,用例写特征模式再读回校验(不依赖外部文件系统镜像) |
| 11 | bdev-core 是否顺带做分区映射器? | **不做**(SD-9 是 v1.x),先留设计 |

---

## 附录 A:本文事实的取证命令(可复现)

```bash
# ① PCIe 地址 / ECAM(默认 vs highmem=off)
qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=/tmp/a.dtb -cpu cortex-a53 -m 128M -display none
qemu-system-aarch64 -M virt,gic-version=3,highmem=off,dumpdtb=/tmp/b.dtb -cpu cortex-a53 -m 128M -display none
#   → 解析 /pcie@10000000 的 reg:默认 0x4010000000+0x10000000;highmem=off 0x3f000000+0x1000000

# ② sdhci-pci 的 ID / class / BAR 未映射 / eMMC 挂载
qemu-system-aarch64 -M virt,gic-version=3,highmem=off -display none \
  -drive if=none,id=emmc0,format=raw,file=emmc0.img \
  -device sdhci-pci,addr=3 -device emmc,drive=emmc0 -monitor stdio
#   (monitor) info pci    → Bus 0, device 3: Class 2053: PCI device 1b36:0007; BAR0 not mapped
#   (monitor) info qtree  → sdhci-pci → sd-bus → emmc

# ③ SDHCI 能力寄存器默认值
qemu-system-aarch64 -device sdhci-pci,help      # capareg=91763892 (0x057834b4), sd-spec-version=2, uhs=0
```

## 附录 B:u-boot 侧关键代码位置(供实现期对照)

| 主题 | 位置 |
|---|---|
| 命令收送 | `mmc.c:mmc_send_cmd()` / `sdhci.c:sdhci_send_command()` |
| 块读写 | `mmc.c:mmc_read_blocks()` / `mmc_bread()` / `mmc_bwrite()` |
| 初始化序列 | `mmc.c:mmc_start_init()` / `mmc_get_op_cond()` / `mmc_startup()` / `mmc_startup_v4()` |
| EXT_CSD / switch | `mmc.c:mmc_send_ext_csd()` / `__mmc_switch()` / `mmc_set_card_speed()` |
| 总线宽度/时钟 | `mmc.c:sd_select_bus_width()` / `mmc_set_bus_width()` / `sdhci.c:sdhci_set_ios()` / `sdhci_set_clock()` |
| DMA | `sdhci.c:sdhci_prepare_dma()` / `sdhci_transfer_data()` / `sdhci-adma.c` |
| PIO | `sdhci.c:sdhci_transfer_pio()` |
| 卡忙/状态 | `mmc.c:mmc_send_status()` / `mmc_poll_for_busy()` |
| 能力解析 | `sdhci.c:sdhci_setup_cfg()` |

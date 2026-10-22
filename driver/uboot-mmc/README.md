# driver/uboot-mmc — u-boot MMC/SDHCI(上游**原样编译**; ability / io / third_party)

> ⚠ **本插件是 GPL-2.0-or-later**: 参与编译的上游文件(`third_party/uboot-imx/drivers/mmc/**`)与我们的适配层一起构成这个插件。来源与逐条偏离见 [NOTICE](NOTICE), 全文见 [LICENSE.GPL-2.0](LICENSE.GPL-2.0)。它与 [uboot-fat](../uboot-fat/README.md)、[uboot-erofs](../uboot-erofs/README.md) 一起构成本仓库的 GPL 代码区。差异分析(上游对照表/依赖 gap/QEMU 能力边界): [`docs/analysis/0001-emmc-bdev-gap.md`](../../docs/analysis/0001-emmc-bdev-gap.md)。

## 1. 它是什么

把 u-boot 的**通用 MMC 协议核心 + 通用 SDHCI 主机**接进 brickOS, 出口是 `bdev`。上游原件直接参与编译, 插件里没有一份副本:

```
QEMU virt ── PCIe(GPEX) ── sdhci-pci(1b36:0007) ── emmc
                                   │ BAR0(寄存器窗口) + INTx virq
                                   ▼
        hal/pci-core ──枚举/BAR 分配/INTx swizzle──► glue 的 br_pci_dev_at() 遍历
                                   ▼
        third_party/uboot-imx/drivers/mmc/sdhci.c        ★ 上游原件
             寄存器/命令/数据(ADMA2 或 PIO)/时钟/复位    ◄── glue 的 ISR + bh 唤醒
                     ▲ compat/sdhci.h(转发壳)+ compat/mmc_env.h(环境)
                                   ▼  上游 sdhci_setup_cfg() + mmc_create()(add_sdhci)
        third_party/uboot-imx/drivers/mmc/mmc.c          ★ 上游原件
             CMD0/1/2/3/7/8/9/12/13/16/17/18/24/25 + CID/CSD/EXT_CSD + 块读写
        third_party/uboot-imx/drivers/mmc/mmc_write.c    ★ 上游原件(块写/擦除)
        third_party/uboot-imx/drivers/mmc/mmc_legacy.c   ★ 上游原件(设备链/devnum/块设备挂钩)
        third_party/uboot-imx/drivers/mmc/sdhci-adma.c   ★ 上游原件(ADMA2 描述符表)
                                   ▼
        glue/emmc_bdev.c   实现 br_bdev_ops → br_bdev_register("emmc0")
                                   ▼
        hal/bdev-core → dev-core → /dev/emmc0(看得见、打不开: raw 面属 v2)
```

**三层分目录**:

| 目录 | 是什么 | 谁写的 |
|---|---|---|
| `third_party/uboot-imx/` | 上游原件(**只读**, sha256 锁) | u-boot |
| `compat/` | u-boot 运行环境(头名薄壳 + `mmc_env.h` + 配置矩阵) | 本项目 |
| `glue/` | 本项目的对接(PCIe/中断/实例表/bdev/ioctl/自检) | 本项目 |

**`src/` 那一层已删除**。"上游哪些文件参与编译 / 环境层提供了什么 / glue 做了什么 / 中断口径 / 偏离逐条 / 判据在哪"见 [ADAPTER-NOTES.md](ADAPTER-NOTES.md)。

## 2. 中断驱动: 上游的轮询 → 中断 + 信号量的睡眠等待

上游 `sdhci.c` 是**轮询**完成的 —— 它只通过 `get_timer()`/`udelay()` 让出 CPU。本件把这两个函数实现在**环境层**为"让出 CPU 直到'**下一 ms 或设备中断**'", 上游的轮询循环就自动变成了中断驱动的等待, **而上游一行代码都不用改**:

```
  上游 do { stat = sdhci_readl(INT_STATUS); … udelay(10); } while (!(stat & DATA_END));
        │
        ▼  环境层 udelay()/get_timer() → uboot_mmc_env_wait()
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │ 等待: 打开本卡 SIGNAL_ENABLE → br_sem_take(…, 1 ms) → 记账(中断唤醒/兜底超时) │
  │ ISR : 读 INT_STATUS(**只读, 绝不清** —— 上游靠读它判完成)                    │
  │       把 SIGNAL_ENABLE 写 0 **去断言电平线** → br_work_submit() → isr_count++ │
  │ bh  : br_sem_give() 唤醒等待者 → bh_count++                                 │
  └─────────────────────────────────────────────────────────────────────────────┘
```

- **为什么 ISR 不清 `INT_STATUS`**: QEMU 的 SDHCI 是**电平**中断, 断言条件 = `INT_STATUS & SIGNAL_ENABLE`; 上游要在自己的循环里读到那些位并**自己**清。去断言电平线的正统做法是掩掉**信号源**(`SIGNAL_ENABLE`), 由等待原语在睡下前重开。
- **不变量**: "没有线程在等 ⇒ **INTx 线未断言**"。判据 = `(INT_STATUS & SIGNAL_ENABLE) == 0`, 自检每卡打印一行 `idle-signal=flat|ASSERTED`(四条卡实测全 `flat`)。打开信号的**唯一**位置 = 等待原语睡下之前(bring-up 结束时**不**打开)。
- **等待窗口 = 1 ms**(不是 5 ms 的时钟节拍): 上游把 `get_timer()` 当**轮询节拍器**用, 一次"读时钟"等价于"睡一拍"会把整个识别拖成秒级。

**可观测性**: 自检每卡打印 `IRQ count=… (bh=…, dropped=…, wake irq/token/timeout=A/S/T)`, 且每个用例前后给一行增量:

| 计数 | 含义 |
|---|---|
| `A = wake irq` | 睡下之后被**设备中断唤醒**(窗口内 ISR 增长) |
| `S = wake token` | 进 `br_sem_take` 时**令牌已在**(ISR/bh 上一轮就 give 过)⇒ 立即返回, 没睡 |
| `T = wake timeout` | 真睡满 1 ms 窗口且期间无 ISR ⇒ **这才是兜底** |

判据是 **`A + S ≫ T`**。只看 `A` 会在"令牌被下一轮消费"的形态下得出**相反的错误结论**, 所以三种返回必须分开数。

> 环境层知道"等的是哪块卡": 上游的等待循环**没有设备参数**, 所以 `compat/mmc_env.h` 的 MMIO 访问器把刚被访问的地址告诉 glue(`uboot_mmc_env_reg_touched`), 由它反查窗口 ⇒ 四块卡各有自己的信号量与 INTx 通道(**多实例**), 不是全局单例。

## 3. 为什么初始化在 `start` 相

插件管理器的启动链是 `EARLY/CORE/LATE → 全局开中断 → START`。中断驱动意味着 **CORE 相里等不到中断**(全局关着, ISR/bh 不跑)⇒ 探测/识别/注册 bdev 全部落在 `start`(那时中断已开, 且 platform 的 timer 已就位)。`early_init`/`init` 是空转。这条相位事实也是 `fs/uboot-fat` / `fs/uboot-erofs` 必须声明对**本件** init 边的原因(它们要块设备)。

## 4. 设备与接口(私有契约, 不导出)

| 项 | 值 |
|---|---|
| 设备名 | `emmc0`..`emmc3`(→ `/dev/<name>`, `open_file == NULL`) |
| 几何 | 512 B 扇区; `n_sectors` = 上游 `blk_desc::lba`(= `capacity / read_bl_len`); `max_rw` = 64 扇区(32 KiB); `align` = 4 |
| ioctl | `BR_EMMC_IOC_GET_INFO`(`BR_IOR('e',0,br_emmc_info_t)`)、`BR_EMMC_IOC_SET_PATH`(`BR_IOW('e',1,br_u32)`) |
| 总线 | 1-bit 识别 → 4-bit → 上游按卡能力选 mode(QEMU 上实测 `HS-52MHz` @52 MHz) |
| 数据通路 | 默认 ADMA2(`host.flags |= USE_ADMA`); `SET_PATH` 切 PIO = **清那个位**。`geometry.flags` 每次现算 |

`max_rw` 与配置矩阵的 `CONFIG_SYS_MMC_MAX_BLK_COUNT` **同源**(前者就是后者的别名, 见 `glue/emmc_internal.h` 的两条 `_Static_assert`)。**这不是巧合而是必须**: 那个值同时决定上游 `cfg->b_max`(协议核心切开大请求的粒度)与 `ADMA_TABLE_SZ`(描述符表大小)⇒ 不一致会造成"描述符表装不下一次传输"的越界写。

## 5. 一致性用例(`TC-EMMC-*`, `glue/emmc_selftest.c`)

只经公开面(`br_bdev_get` + `br_dev_*` + ioctl)驱动, 且**一律先写后读**(门禁镜像跨次运行持久, 不能假设任何扇区的内容)。设备级写**只落在 `emmc3` 的擦写区**。

| id | 判据 |
|---|---|
| TC-EMMC-001 | `emmc0..emmc3` 四块卡都命中; 512 B 扇区; 几何自洽且有 DMA/PIO 位 |
| TC-EMMC-002 | 擦写区(`emmc3` 的 LBA 4096 起 512 KiB)整段落在卡内 |
| TC-EMMC-003 | 四块卡的 dev-core 条目 `class == bdev` 且 `open_file == NULL`(D21) |
| TC-EMMC-004 | `read(lba == n_sectors)` ⇒ 负 errno(**越界不落卡**; ISR 增量必须为 0) |
| TC-EMMC-005 | 默认 ADMA2 通路: 擦写区 8 扇区 write→read 逐字一致 |
| TC-EMMC-006 | `SET_PATH(PIO)` 只改该卡: 该卡报 PIO, `emmc0` 仍报 DMA(多实例) |
| TC-EMMC-007 | PIO 通路 8 扇区 write→read 逐字一致 |
| TC-EMMC-008 | 切回 DMA 后 32 扇区 ADMA2 多块一致; `flush == 0`; `GET_INFO` 自洽 |

门禁 `emmc-test`(见 [tests/gates.toml](../../tests/gates.toml))另要求 `[SELFTEST] SUMMARY … fails=0 errors=0`、四个 `pci: 00:0N.0 1b36:0007`、`uboot-mmc: eMMC ready`、`/dev/emmc0` 与 `/dev/emmc3 就绪`、`共接管 4 块 sdhci-pci 卡`、`[PCICONF] SUMMARY … fail=0`、`[EMMCCONF] SUMMARY … fail=0`、`irq_ticks=[1-9]`, 并禁止 `[PCICONF] FAIL`/`[EMMCCONF] FAIL`/`[PANIC]`。

## 6. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `hal/bdev-core` | init | 实现并注册 `br_bdev_ops` |
| `hal/pci-core` | init | 取 sdhci-pci 的 BAR0 与 INTx virq |
| `hal/dev-core` / `hal/vfs-core` | type | ioctl 编码宏 / `br_dev.h` 的类型引用 |

```
sources = ["../../third_party/uboot-imx/drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c",
           "glue/*.c"]        ← ★ 上游原件, 没有副本
includes = ["compat", "glue", "../../third_party/uboot-imx/drivers/mmc"]   ← 最后一条为 mmc_private.h
cflags   = ["-Idriver/uboot-mmc/compat/std-names",  ← 标准头名的单元级补丁(见 ADAPTER-NOTES §6 偏离 12)
            "-Wno-shadow", …]                        ← vendor 的告警口径
```

**不要**把 `third_party/uboot-imx/include` 加进 `includes`: brickie 给每个编译单元的 `-I` **按字典序**排, 而 `../..` 排在 `compat` 之前 ⇒ vendor 的 include 目录会**盖住** `compat/` 里的同名壳(`<mmc.h>`/`<part.h>` 会被解析到 u-boot 的真头, 那份还要 `cyclic.h`/`efi.h` 整族)。指向 vendor 头一律走 `compat/` 里的**相对路径转发壳**(与 `fs/uboot-erofs/compat/u-boot/lz4.h` 同款处置)。

## 7. 移植过程中踩到并修掉的平台缺陷(仍然有效)

**GIC 只把私有线设成 Group1NS, SPI 段仍是 Group0** ⇒ 任何 SPI 都不会被投递(Group1 使能而 SPI 留 Group0)。原型此前没有真 SPI 源(timer 是 PPI、UART 只被绑定不产生中断), 所以这个缺口一直没暴露; eMMC 的 PCIe INTx 是第一个真消费者。修法 = `platform/qemu-aarch64/src/gicv3.c` 步 5b 把 `GICD_IGROUPR`/`IGRPMODR` 的 SPI 段(按 `nr_irq` 覆盖)设为 Group1NS。症状特征: **`INT_STATUS` 硬件已置位、`br_irq` 的 `count` 恒 0**(handler 一次没进)。

## 8. 上游升到新 commit 时要做什么

1. 改 `third_party/upstream.lock` 的 `[upstream]`(repo/branch/commit/snapshot_sha256);
2. 用新 tar 刷新 `third_party/uboot-imx/` 里**锁登记的那些文件**(逐字节, 不编辑);
3. 重跑 `bash tools/check-third-party-pristine.sh`(逐文件 sha256 对账);
4. `brickie build` + `brickie test emmc-test / fat-test / erofs-test`。

**不要**为了适配新版本就地改 `third_party/` 里的文件 —— 要适配就写进 `compat/`/`glue/`(上游是叶子, 适配全在一侧)。

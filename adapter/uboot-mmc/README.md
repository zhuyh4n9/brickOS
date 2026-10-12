# adapter/uboot-mmc — u-boot MMC/SDHCI 移植(ability / io / third_party)

> ⚠ **本插件是 GPL-2.0-or-later**, 来源与逐文件对照见 [NOTICE](NOTICE), 全文见
> [LICENSE.GPL-2.0](LICENSE.GPL-2.0)。它是本仓库内唯一的 GPL 代码(ADR-0021 D-3)。
> 决策与偏离: [`docs/decisions/0021-bdev-pci-emmc.md`](../../docs/decisions/0021-bdev-pci-emmc.md);
> 差异分析(上游对照表/依赖 gap/QEMU 能力边界): [`docs/analysis/0001-emmc-bdev-gap.md`](../../docs/analysis/0001-emmc-bdev-gap.md)。

## 1. 它是什么

把 u-boot 的**通用 MMC 协议核心 + 通用 SDHCI 主机**移植进 brickOS, 出口是 `bdev`:

```
QEMU virt ── PCIe(GPEX) ── sdhci-pci(1b36:0007) ── emmc(8 MiB raw)
                                   │ BAR0(寄存器窗口) + INTx(slot 3 pin A ⇒ SPI 6 = INTID 38)
                                   ▼
        framework/pci-core ──枚举/BAR 分配/INTx swizzle──► br_sdhci_probe(base, virq)
                                   ▼
        src/sdhci_host.c     寄存器/命令/数据(ADMA2 或 PIO)/中断  ◄── platform 的 BH 分发
                                   ▼   br_mmc_host_ops_t {send_cmd, set_ios}
        src/mmc_core.c       CMD0/1/2/3/7/8/9/16/17/18/24/25 + EXT_CSD + 块读写
                                   ▼
        adapter/emmc_bdev.c  实现 br_bdev_ops → br_bdev_register("emmc0")
                                   ▼
        framework/bdev-core → dev-core → /dev/emmc0(看得见、打不开: raw 面属 v2)
```

三层分文件, 两层之间只隔一个 `br_mmc_host_ops_t`(见 `src/emmc_internal.h`)—— 这是"通用"
的落点: **换到 i.MX 真机只换主机层**(`fsl_esdhc_imx.c` 那类), 协议核心不动。

## 2. 上游对照与改写(要点)

| 上游 | 本插件 | 处理 |
|---|---|---|
| `drivers/mmc/mmc.c` | `src/mmc_core.c` | 保留识别/EXT_CSD/块读写; 裁掉 SD/UHS/HS200/HS400/tuning/RPMB/SPI |
| `drivers/mmc/sdhci.c` | `src/sdhci_host.c` | 保留寄存器序列; DM/DT/regulator/gpio/clk/quirks 裁掉 |
| `drivers/mmc/sdhci-adma.c` | `src/sdhci_host.c` | ADMA2 描述符构造并入 |
| `include/{mmc,sdhci}.h` | `src/mmc_regs.h` | 摘录子集(数值逐字对齐) |
| `drivers/mmc/mmc-uclass.c` | — | **不移植**(DM/blk 粘合), 由 bdev 与插件模型取代 |
| `drivers/mmc/fsl_esdhc*.c` | — | **不移植**(NXP 专有主机; QEMU 用通用 sdhci-pci) |

**两处有意偏离**(ADR-0021 D-4):

1. **中断而非轮询**: 上游在 `sdhci_send_command()`/`sdhci_transfer_data()` 里 `udelay()`
   轮询 `INT_STATUS`。本移植把 `INT_ENABLE`/`SIGNAL_ENABLE` 打开, ISR 只"读状态 + 清源 +
   `br_sem_give`", 传输线程 `br_sem_take` 等 —— 即设计 `7-02` §5 的 ISR→bh→唤醒
   (绑定表里 INTx 的 `dispatch = BH`, 由 core 在中断出口 drain)。
2. **ADMA2 + PIO 双通路, 运行期可选且必须被"指出"**: `geometry.flags` 报
   `BR_BDEV_F_DMA`/`BR_BDEV_F_PIO`; `BR_EMMC_IOC_SET_PATH` 切换; 启动与切换各留一行日志。
   小传输(< 4 扇区)或缓冲未 4 字节对齐时, 即使选了 DMA 也自动落 PIO。

## 3. 为什么初始化在 `start` 相

插件管理器的启动链是 `EARLY/CORE/LATE → 全局开中断 → START`。中断驱动意味着
**CORE 相里等不到中断**(全局关着, ISR/bh 不跑)⇒ 探测/识别/注册 bdev 全部落在 `start`
(那时中断已开, 且 platform 的 timer 已就位)。`early_init`/`init` 是空转。

## 4. 设备与接口(私有契约, 不导出)

| 项 | 值 |
|---|---|
| 设备名 | `emmc0`(→ `/dev/emmc0`, `open_file == NULL`) |
| 几何 | 512 B 扇区; `n_sectors` = EXT_CSD `SEC_CNT`; `max_rw` = 64 扇区(32 KiB); `align` = 4 |
| ioctl | `BR_EMMC_IOC_GET_INFO`(`BR_IOR('e',0,br_emmc_info_t)`)、`BR_EMMC_IOC_SET_PATH`(`BR_IOW('e',1,br_u32)`) |
| 总线 | 1-bit @400 kHz 识别 → 4-bit → HS 52 MHz(卡声明支持时) |

## 5. 一致性用例(`TC-EMMC-*`,`adapter/emmc_selftest.c`)

只经公开面(`br_bdev_get` + `br_dev_*` + ioctl)驱动, 且**一律先写后读**
(门禁镜像跨次运行持久, 不能假设任何扇区的内容):

| id | 判据 |
|---|---|
| TC-EMMC-001 | `br_bdev_get("emmc0")` 命中; 512 B 扇区; 几何自洽且有 DMA/PIO 位 |
| TC-EMMC-002 | dev-core 条目 `open_file == NULL`(D21: raw 面属 v2) |
| TC-EMMC-003 | 默认通路 8 扇区 write→read 逐字一致 |
| TC-EMMC-004 | `read(lba == n_sectors)` ⇒ 负 errno(越界不落卡) |
| TC-EMMC-005 | `SET_PATH(PIO)` 后 `geometry.flags` 恰好报 PIO |
| TC-EMMC-006 | PIO 通路 8 扇区 write→read 逐字一致 |
| TC-EMMC-007 | 切回 DMA 后 16 扇区 ADMA2 多块 write→read 一致 |
| TC-EMMC-008 | `flush == 0`; `GET_INFO` 的容量/扇区/BAR/通路回答自洽 |

门禁 `emmc-test`(见 `tests/gates.toml`)要求 `[EMMCCONF] SUMMARY ... fail=0`、
`[PCICONF] ... fail=0`、`uboot-mmc: eMMC ready` 与 8 条 tag 逐条点名。

## 6. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `framework/bdev-core` | init | 实现并注册 `br_bdev_ops` |
| `framework/pci-core` | init | 取 sdhci-pci 的 BAR0 与 INTx virq |
| `framework/dev-core` / `framework/vfs-core` | type | ioctl 编码宏 / `br_dev.h` 的类型引用 |

`src/*.c` 两件(上游派生): `mmc_core.c` / `sdhci_host.c`; `adapter/*.c` 两件(本项目):
`emmc_bdev.c` / `emmc_selftest.c`, 另有 `adapter/mmc_compat.h`(唯一的运行环境入口)。

## 6.1 `src/` 与 `adapter/` 的拆分(单向依赖)

GPL 边界靠**依赖方向**而非目录名维持:

- `src/`(**上游派生**, GPL-2.0+): `mmc_core.c` / `sdhci_host.c` / `mmc_regs.h` /
  `emmc_internal.h` —— 逐字对应 u-boot `drivers/mmc/**`, 不含任何 `#include <br/...>`;
- `adapter/`(**本项目**): `mmc_compat.h`(**唯一**提供 br_types/br_log/br_time/br_irq/
  br_mem/br_mm/br_sync 的地方)、`emmc_bdev.c`、`emmc_selftest.c`。

依赖**单向** `src/` → `adapter/`: `src/` 只经 `mmc_compat.h` 取运行环境; `adapter/` 反过来
include `src/` 的头是允许的(适配层当然要认识被适配的代码)。[plugin.toml](plugin.toml) 据此收
`sources = ["src/*.c", "adapter/*.c"]` / `includes = ["src", "adapter"]`, 门禁
`tools/check-third-party-isolation.sh`(见 `tests/gates.toml`)机械地守住这条边界。
完整理由与逐文件归属见 [`adapter/README.md`](adapter/README.md)。

## 7. 移植过程中踩到并修掉的平台缺陷

**GIC 只把私有线设成 Group1NS, SPI 段仍是 Group0** ⇒ 任何 SPI 都不会被投递(Group1
使能而 SPI 留 Group0)。原型此前没有真 SPI 源(timer 是 PPI、UART 只被绑定不产生中断),
所以这个缺口一直没暴露; eMMC 的 PCIe INTx(SPI 38)是第一个真消费者。修法 =
`platform/qemu-aarch64/src/gicv3.c` 步 5b 把 `GICD_IGROUPR`/`IGRPMODR` 的 SPI 段(按
`nr_irq` 覆盖)设为 Group1NS。症状特征值得记下: **`INT_STATUS` 硬件已置位、`br_irq` 的
`count` 恒 0**(handler 一次没进)。

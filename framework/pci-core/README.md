# framework/pci-core — 通用 PCI(e) 机制(ability / framework / native)

## 1. 它是什么(以及为什么不是 platform 的一部分)
接口面快照: `api/iface/framework/pci-core/pci.toml`。

`dev-core` 管设备命名空间,`cdev-core`/`bdev-core` 管设备形状;PCI 是**总线**这一层的机制。它做的四件事:

| # | 机制 | 关键点 |
|---|---|---|
| ① | **ECAM 配置空间访问** | `ecam + (bus<<20) + (dev<<15) + (fn<<12) + (off&0xFFC)`;32 位对齐;小端 |
| ② | **总线枚举** | bus×dev×fn;`vendor == 0xFFFF` = 空槽;header-type bit7 判多功能 |
| ③ | **BAR 尺寸探测与分配** | 写全 1 读回算尺寸 → 从平台 MMIO 窗口切 → 写回;再开 Memory Space + Bus Master |
| ④ | **INTx swizzle** | QEMU GPEX:`(pin_index + slot) % 4` ⇒ 由平台给的"INTx A virq"推出实际 virq |

**边界**:板级事实(ECAM 在哪 / MMIO 窗口多大 / INTx 起始线)**只在 `platform/qemu-aarch64`**(`br_plat_pci_host`);换一台有 PCIe 的板子,变的只有那几个数,本件不动。反过来(platform 调 `br_pci_register_host`)会让 platform 依赖插件 —— 方向表不允许。

## 2. 为什么 BAR 分配是本件的活
本镜像是 `-kernel` 直启,**没有固件**;PCIe 端点的 BAR 出厂为 0 且未映射(实测 `info pci` 显示 `BAR0: 32 bit memory (not mapped)`)。不分配 BAR 就没有寄存器窗口可写。两个**实测踩到过**的坑(写在代码注释里,因为它们是"看起来对"的写法):

* **`orig == 0` 不等于"这个 BAR 不存在"** —— 未分配的内存 BAR 地址就是 0。判据只能是"写全 1 再读回,两个半字都不接受写入"。用 `orig == 0` 判空会让 sdhci 的 BAR0 永远分不到地址(TC-PCI-003 如实报红)。
* **尺寸要按 BAR 位宽分别算** —— 32 位 BAR 的尺寸掩码只在低 32 位有意义;拿 64 位取反会把高 32 位当成"不可写",算出 `0xFFFFFFFF00000100` 这种假尺寸。64 位 BAR 才用合并掩码(否则 16 KiB 的 BAR 会被误判成 > 4 GiB)。

## 3. 对外面

```c
typedef struct br_pci_dev {
    br_u16 vendor, device, class_code;  br_u8 revision, bus, dev, fn, intx_pin;
    br_u32 intx_virq;                   /* 平台绑定好的 virq; 0 = 无 INTx */
    br_uintptr_t bar[6];  br_u32 bar_size[6];   /* bar[i] == 0 ⇒ 该 BAR 未分配(不是基址 0) */
} br_pci_dev_t;

int                 br_pci_init(void);                 /* 幂等; CORE init 会调 */
br_u32              br_pci_dev_count(void);
const br_pci_dev_t *br_pci_dev_at(br_u32 index);
const br_pci_dev_t *br_pci_find(br_u16 vendor, br_u16 device);
int                 br_pci_cfg_read32 (br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 *out);
int                 br_pci_cfg_write32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 value);
```

边界:不认任何具体器件、不做 MSI/MSI-X(端点用 INTx)、不做 PCIe 链路训练/电源管理、不做桥后面的总线号分配(v1 只面对 root bus 上的端点)。设备表在 CORE 相一次填好,之后**只读** ⇒ 查询面天然线程安全;`br_pci_cfg_write32` 会写,调用方自锁。

## 4. 与 QEMU 的对应(实测)

```
$ qemu-system-aarch64 -M virt,gic-version=3,highmem=off ... -device sdhci-pci,addr=3 -device emmc,...
  ECAM 0x3f00_0000 + 16 MiB(highmem=off; 默认在 0x40_1000_0000 —— 超 4 GiB); MMIO 窗口 0x1000_0000 + 0x2eff_0000
  sdhci-pci 1b36:0007, class 0x0805, BAR0 256 B → 本件从窗口切给它的地址; INTx pin A slot 3 ⇒ (0 + 3) % 4 = 3 ⇒ virq = platform.intx_virq_a + 3(hwirq 38)
```

## 5. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `platform/qemu-aarch64` | `init` | 读 `br_plat_pci_host`(ECAM/MMIO/总线范围/INTx 起始 virq) |

`src/pci_core.c` = 生产;`src/pci_selftest.c` = 自检(`[selftest].cases = 8`)。

## 6. 一致性用例(`TC-PCI-*`)

| id | 判据 |
|---|---|
| TC-PCI-001 | 设备表非空且 ≤ `BR_PCI_MAX_DEVS` |
| TC-PCI-002 | `br_pci_find(1b36:0007)` 命中且 class = 0x0805 |
| TC-PCI-003 | sdhci 的 BAR0 非 0、在平台 MMIO 窗口内、按尺寸对齐 |
| TC-PCI-004 | 端点命令寄存器:Memory Space + Bus Master 置位、INTx Disable 清位 |
| TC-PCI-005 | pin A 的 virq == `intx_virq_a + (slot % 4)` |
| TC-PCI-006 | 配置空间读写口往返正确;错对齐/空出参 ⇒ `-EINVAL` |
| TC-PCI-007 | `find(不存在)`、`dev_at(越界)` ⇒ `NULL` |
| TC-PCI-008 | 所有已分配 BAR 的区间两两不重叠 |

红绿由门禁 `smoke` 上的 `[PCICONF] SUMMARY pass=N fail=0` 与 forbid `[PCICONF] FAIL` 判;`TC-PCI-*` 逐条点名(裁掉用例也算红)。

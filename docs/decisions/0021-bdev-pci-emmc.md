# 0021 — bdev-core + framework/pci-core + 第三方 eMMC(SDHCI)移植

> 状态: **已落地**(P0~P4 全部实现并在 QEMU virt 上端到端验证:
> `[BDEVCONF] 8/0`、`[PCICONF] 8/0`、`[EMMCCONF] 8/0`, 全套门禁绿)。
> 前置分析: `docs/analysis/0001-emmc-bdev-gap.md`(差异/依赖/平台缺口/QEMU 能力边界的取证)。
> 设计依据: `docs/7-storage/7-02-bdev.md`(bdev 类形状)、`8-01-device.md` §1~§3(子分类与
> 框架件归属)、`1-03-roadmap.md` §1(bdev-core 在插件清单里)。
> 工具契约: `tools/brickie/docs/contract.md`(`api_type = third_party` 的隔离语义)、
> `tools/brickie/rust/src/rules.rs`(`native` 不得依赖 `third_party`; `third_party` 只许抛
> `third_party` 面)。

## 1. 背景

设计侧 bdev 子分类(`br_bdev_ops`/`br_bdev_geom_t`/`br_bdev_register`/`br_bdev_get`)已冻结在
`7-02` §1, 而实现侧一直缺件(`dev-core` 里 `BR_CLASS_BDEV = 2` 是占位)。驱动侧原计划是
`io/virtio-blk`; 本次改用 **eMMC**: 在 QEMU `virt` 上经 PCIe 挂 `-device sdhci-pci` +
`-device emmc`, 移植 u-boot 的通用 MMC/SDHCI 栈(`drivers/mmc/mmc.c` + `sdhci.c`)并接到 bdev。

u-boot 代码是 **GPL-2.0+**, 本项目是 **WTFPL**; 且 u-boot 的 SDHCI 是**轮询**驱动, 而设计
`7-02 §5` 要求中断驱动。三条都由本节决策钉死。

## 2. 决策

### D-1 bdev-core 严格按 `7-02` §1 的形状, 不外扩

* `br_bdev_geom_t` / `br_bdev_ops_t` / `br_bdev_register` / `br_bdev_get` 逐字段对齐设计;
  类型用本仓库的 `br_u32/br_u64/br_size_t` 拼写。
* 成功返回 = **传输扇区数**(`read`/`write`); 失败 = `BR_ERR(BR_E*)`(SD-10 口径)。
* **必填槽位**: `read` + `write` + `flush` + `geometry` **四者皆必填**;`ioctl`/`suspend`/`resume`
  可空(⇒ 调用方得 `-ENOTSUP`)。理由: 块介质没有"不落盘"这回事 —— `flush` 是 page cache(SD-8)
  的语义前提;只读介质用 `BR_BDEV_F_READONLY` 声明语义, 不放松 ops 形状。
* **注册期调用一次 `geometry` 并校验自洽**(增量): `sector_size` 为 2 的幂、`n_sectors > 0`、
  `0 < max_rw <= n_sectors`、`align` 为 0 或 2 的幂、`DMA`/`PIO` 不同置。"几何自洽"是注册的
  前置条件(与 cdev-core 的 flash 子型校验几何同型), 免得半成品等第一次 I/O 才炸。
* **raw 文件面 = v2**: bdev 条目的 `open_file` 恒 `BR_NULL`(`7-02` §1 末段), `/dev/<bdev>`
  **看得见打不开**; 与 flash 同款(ADR-0009 §3 裁定 6), 并用自检钉成机械判据。
* `BR_BDEV_F_*`(设计留的位): 本刀定义 `READONLY` / `STACKED` / `DMA` / `PIO`。
  **`DMA`/`PIO` 是本次新增的两位**(D-4): 几何查询必须**明确指出当前实际数据通路**。
* 分区映射器(`br_bdev_partition`, SD-9)是 v1.x, 本刀只留设计不实现。

### D-2 PCIe 独立成 `framework/pci-core`; 板级事实留在 platform; `highmem=off`

* `framework/pci-core`(ability/framework/native)= 通用 PCI 机制: ECAM 配置空间访问、
  总线枚举、BAR 尺寸探测与分配、命令寄存器(内存空间 + Bus Master)、设备表与 `br_pci_find`。
* **板级事实归 `platform/qemu-aarch64`**: 新增 `pci` 导出(`br_plat_pci_host_t`: ECAM 基址/大小、
  32 位 MMIO 窗口基址/大小、总线范围、INTx 起始 INTID)。pci-core 以 `init` 依赖 platform 读取它。
  * 为什么 framework 依赖 platform: 依赖方向表允许 `ability → platform`; 且"ECAM 在哪"是**目标事实**,
    放 framework 就是把 QEMU 知识塞进通用层。代价 = 该 framework 与具体平台耦合到"多平台"为止;
    退出条件写在 §4 风险里(将来由 core 的 bus 服务或 `br_pci_host_ops` 注册反转依赖)。
* **QEMU 机器参数追加 `highmem=off`**: ECAM 从 `0x40_1000_0000` 落到 `0x3f00_0000`, 与 32 位
  MMIO 窗口一起落在 platform 现有的**低 1 GiB Device 背景映射**内 ⇒ 只加 region 声明(no-op),
  **不改页表**。附带好处: 全 32 位地址空间, 与 QEMU SDHCI 无 64 位 DMA 的能力相符。

### D-3 三方模块隔离: 移植件 = 一个 `api_type = third_party` 插件

* 移植的 u-boot GPL-2.0+ 代码**不散落进 WTFPL 插件**, 全部收进一个插件
  `third_party/uboot-mmc/`(`api_type = "third_party"`, `license = "GPL-2.0-or-later"`),
  随附 `LICENSE.GPL-2.0` 与 `NOTICE`(来源 branch/commit/文件清单)。
* **粘合层也在这个插件里**: `brickie` 的规则是 `native` **不得**依赖 `third_party`
  (`rules.rs::api_type_edge_reason`), 所以"WTFPL 的薄胶水包 GPL 库"这种切法在本工具链里
  **不合法**。bdev 对接(实现 `br_bdev_ops`)/PCIe 绑定/中断绑定都写在 `third_party/uboot-mmc`
  内 —— 它依赖 `framework/bdev-core`(init)、`framework/pci-core`(init)是合法的
  (`third_party` 消费者不受 native→third_party 禁则约束)。
* `framework/bdev-core`、`framework/pci-core`、`platform/**` 保持 WTFPL, **不含任何 GPL 代码**。
* ⚠ **残余风险(如实记录, 不粉饰)**: 静态链进同一镜像后, 整镜像在分发时仍构成 GPL 组合作品;
  "目录隔离"解决的是**代码归属清晰**, 不是许可传染。这是主控在 §拍板时明知并接受的取舍。

### D-4 中断驱动 + 数据通路明确 DMA/PIO

* **中断**: SDHCI 走 PCIe INTx(pin A → platform 给的 `intx_intid_base`)。路径 = 
  `br_irq_register()` 的 ISR **只读状态/清源 + `br_work_submit()` 交下半部**, bh 里
  `br_sem_give()` 唤醒在 `br_sem_take()` 上等待的传输线程(设计 `7-02` §5 的形态)。
  与 u-boot 的**轮询**实现是**有意偏离**, 记录在案。
* **数据通路**: 同时实现 **ADMA2** 与 **PIO**; 运行期可选。当前模式必须**可观测**:
  - `br_bdev_geom_t.flags` 里 `BR_BDEV_F_DMA` / `BR_BDEV_F_PIO` 恰好置一;
  - 器件 ioctl `GET_INFO` 回答模式与阈值;
  - 启动与切换时各留一行 `br_log_info`。
  默认策略: 传输大小 ≥ 阈值且缓冲对齐 ⇒ ADMA2; 否则 PIO(非对齐/小传输兜底)。

### D-5 eMMC 的 flush 与错误码

* `flush` v1 = `mmc_poll_for_busy()` + 返回 0(写到卡内的块已落介质); **未实现** EXT_CSD
  `CACHE_FLUSH`, 在头文件与 WORKAROUND 里写明 —— 不假装。
* 补 `BR_ECOMM = 70`(append-only, 与 Linux 编号一致): u-boot 用 `-ECOMM` 表达"命令/状态错",
  用 `-EIO` 代替会丢排障信息。

## 3. 与设计的偏离(逐条)

| # | 偏离 | 理由 | 回归条件 |
|---|---|---|---|
| 1 | bdev 的 `BR_BDEV_F_DMA/PIO` 两位是设计没写的增量 | 主控要求"数据传输需要指出 DMA/PIO" | 设计 `7-02` §1 补该两位 |
| 2 | `framework/pci-core` 依赖 `platform/qemu-aarch64` | ECAM/INTx 是目标事实 | 多平台或 core bus 服务出现时改 `br_pci_host_ops` 注册 |
| 3 | eMMC 用中断而非轮询 | 主控拍板 | — |
| 4 | bdev 由 `third_party` 插件注册(而非 `io/emmc`) | 工具链禁止 native→third_party | 放弃三方隔离时 |
| 5 | `BR_ECOMM` 新增 | u-boot 错误语义 | — |

## 4. 风险与遗留

| 项 | 说明 | 处置 |
|---|---|---|
| 许可传染 | 静态链接下整镜像仍是 GPL 组合作品 | 已记录(D-3), 主控接受 |
| framework→platform 耦合 | pci-core 绑定单一平台 | §3 偏离 2 的回归条件 |
| INTx 电平语义 | 不清源会活锁 | ISR 必须读 `INT_STATUS` 并写回清源; 自检覆盖 |
| DMA 缓存一致性 | 恒等映射下 `dma_addr == vaddr`, 但仍需 clean/invalidate | ADMA2 描述符与缓冲都走 `br_dma_alloc` + `br_mm_cache_*` |
| flush 未落 CACHE_FLUSH | page cache 依赖的"必须转发"语义 | v1 以"等卡不忙"表达; 登记 WORKAROUND |

## 5. 落地顺序

P0 `framework/bdev-core`(本刀先交付, 独立可验证)→ P1 platform PCIe 事实 + `framework/pci-core`
→ P2 `third_party/uboot-mmc` 协议核心(+宿主假 host 用例)→ P3 SDHCI 主机(ADMA2/PIO/中断)
→ P4 bdev 对接 + `emmc-test` 门禁 + 既有门禁回归。

## 6. 落地记录与实测发现(全部已实现)

| 件 | 落点 | 验证 |
|---|---|---|
| bdev 子分类 | `framework/bdev-core/**` | `[BDEVCONF] 8/0`;门禁 `smoke`/`fs-test` |
| 通用 PCI(e) | `framework/pci-core/**`;platform 新导出 `pci-host` | `[PCICONF] 8/0`;门禁 `smoke` |
| eMMC 驱动 | `third_party/uboot-mmc/**`(4 个 .c) | `[EMMCCONF] 8/0`;门禁 `emmc-test` |
| QEMU 参数 | platform `[build.target.qemu]`:`highmem=off` + `-device sdhci-pci,addr=3` + `-device emmc` | 后备镜像由构建后门禁 `emmc-image` 生成 |

实测得到的三条**重要发现**(都已在对应位置修掉/登记):

1. **GIC 的 SPI 段没有归 Group1NS —— 这是"任何 SPI 都收不到中断"的根因。**
   `gicv3_init` 原本只写 `GICR_IGROUPR0`(私有线 0–31 → Group1NS), 而 `GICD_IGROUPR`
   的 SPI 段复位值是 Group0; CPU 侧只放行 Group1(`EnableGrp1NS` + `ICC_IGRPEN1_EL1`)
   ⇒ SPI 永不被投递。原型此前没有真 SPI 源(timer 是 PPI, UART 只被绑定/回读, 不产生中断),
   所以缺口一直没暴露; eMMC 的 PCIe INTx(SPI 38)是第一个真消费者。
   修法 = `platform/qemu-aarch64/src/gicv3.c` 步 5b(按 `nr_irq` 覆盖 SPI 段的
   `GICD_IGROUPR`/`GICD_IGRPMODR`)。**症状特征**: SDHCI `INT_STATUS` 硬件已置位、
   `br_irq_stats_get()` 的 `count` 恒 0(handler 一次没进)——"硬件到了、框架没到"。
2. **BAR 探测不能拿"读回 0"当"没有 BAR"**: 无固件直启时所有内存 BAR 的地址就是 0;
   判据只能是"写全 1 再读回"(见 `framework/pci-core/src/pci_core.c` 的注释与
   TC-PCI-003)。另一个坑: 32 位 BAR 的尺寸掩码只在低 32 位有意义, 用 64 位取反会算出
   `0xFFFFFFFF00000100` 这种假尺寸。
3. **工具链缺 `third_party` 的描述符模板**: `templates/descriptor/` 只交付了
   native/runtime_adapter ⇒ 任何 `api_type = third_party` 的插件都**无法进镜像**。
   本刀补了 `descriptor/third_party/c/plugin_desc.c.tmpl`(native 的逐字节副本)并
   `make tools-prebuilt` 刷新自举种子。登记在 `br-wa-tp-001`。

## 7. 遗留(不假装做完)

* `br-wa-emmc-001`: `flush` 只做"等卡不忙", 未实现 EXT_CSD `CACHE_FLUSH`;分区/RPMB/SDMA 未做。
* `br-wa-tp-001`: 第三方模块的许可残余风险(静态链接下整镜像仍是 GPL 组合作品);
  `brickie new --api third_party` 仍缺骨架模板。
* `br-wa-test-001`: 新增的 `TC-BDEV-*`/`TC-PCI-*`/`TC-EMMC-*` 是自编号, 待 `6-01` 补出对应用例组。
* 设计侧建议: `7-02` §1 的 `BR_BDEV_F_*` 应补 `DMA`/`PIO` 两位(本刀增量);若接受,
  把本节 D-1 的偏离收编回设计。

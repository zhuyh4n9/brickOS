# `adapter/uboot-mmc` — 三方件的**环境**与**对接**(ADR-0026 D-2/D-3)

> **一句话**: `third_party/` 是上游原件(只读、逐字节), `compat/` 是"给上游的**环境**",
> `glue/` 是"我们的**对接**"。三者的边界不靠约定, 而靠 **sha256 锁**(vendor 一字不改)、
> **依赖方向**(上游 `.c` 只经 `compat/` 的头名取环境)与**门禁**
> (`tools/check-third-party-pristine.sh`)。

## 0. 这一刀改了什么(与 ADR-0024 形态的对照)

| | ADR-0024 形态(旧) | **ADR-0026 形态(本件)** |
|---|---|---|
| 上游代码 | `src/mmc_core.c` / `src/sdhci_host.c` —— **改写副本**(K&R→Allman、抽象重命名、裁枝) | `third_party/.../drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c` —— **原件, 原样编译** |
| 副本漂移 | 只能靠人读 diff | **不可能**: 没有副本(`[build].sources` 直接指向 vendor) |
| 上游升级 | 手工重做全部改写 | 换 vendor + 重跑门禁 |
| 环境 | `compat/mmc_compat.h`(只有 8 行 include) | `compat/`(头名薄壳 + `mmc_env.h` 环境实现 + 配置矩阵) |
| 对接 | `glue/emmc_bdev.c` + `src/` 里自造的 `br_mmc_*`/`br_sdhci_t` 抽象 | `glue/` 直接用上游的 `struct mmc` / `struct sdhci_host` |
| 告警 | 改上游让它过我们的 `-Werror` | 按**上游的**告警口径编译(`[build].cflags`) |

**旧 `src/` 那一层已整个删除** —— 包括 `src/mmc_regs.h`:那些寄存器/中断/EXT_CSD 常量
`include/mmc.h` 与 `include/sdhci.h` 本来就有, 摘录一份就是第二处真值。

## 1. 上游有哪些文件参与编译(逐条)

| 上游原件 | 行数 | 为什么需要它 |
|---|---|---|
| `drivers/mmc/mmc.c` | 3352 | MMC 协议核心: CMD0/1/2/3/7/8/9/12/13/16/17/18/24/25、CID/CSD/EXT_CSD、块读写、`mmc_init`/`mmc_start_init` |
| `drivers/mmc/mmc_write.c` | 254 | 块写与擦除(`mmc_bwrite`/`mmc_berase`)。★ 它**无条件定义** `mmc_bwrite`(u-boot 靠 Makefile 决定编不编)⇒ 列进 `sources` 就必须把 `CONFIG_MMC_WRITE` 置 1 |
| `drivers/mmc/mmc_legacy.c` | 260 | **`!DM_MMC` 形态**的设备链与块设备挂钩: `mmc_create`(分配 `struct mmc` + 取 `devnum` 0..3)/`find_mmc_device`/`mmc_get_blk_desc`/`mmc_list_init`/`mmc_list_add`/`mmc_get_dev`/`mmc_select_hwpartp`/`U_BOOT_LEGACY_BLK(mmc)` |
| `drivers/mmc/sdhci.c` | 1050 | 通用 SDHCI 主机: 复位/时钟/IO/命令/数据传输(ADMA2/PIO)/`sdhci_setup_cfg`/`add_sdhci` |
| `drivers/mmc/sdhci-adma.c` | 87 | ADMA2 描述符表构造(`sdhci_prepare_adma_table`/`sdhci_adma_init`) |

**不参与编译**(以及为什么):

| 上游 | 为什么不要 |
|---|---|
| `drivers/mmc/mmc-uclass.c` | DM/uclass 粘合层。本件走 u-boot 自己支持的 **legacy(`!DM_MMC`)** 形态(`struct mmc_config::ops` + `add_sdhci()`), 不要 DM/udevice |
| `drivers/mmc/fsl_esdhc*.c` / `pci_mmc.c` / `dw_mmc.c` … | 各厂商主机。QEMU 的是通用 `sdhci-pci` ⇒ `sdhci.c` + `add_sdhci()` 就够 |
| `include/mmc.h` / `include/sdhci.h` | **参与**(被 include), 但不单独编译: `compat/mmc.h`/`compat/sdhci.h` 转发到它们(原件) |

### 1.1 为了"原样编译"新 vendor 的上游件(6 件, 已补锁)

| 新增 vendor 文件 | 谁需要它 |
|---|---|
| `drivers/mmc/mmc_legacy.c` | 任务书 §2.2 已核实: legacy 形态的必需件(见上表) |
| `include/linux/kconfig.h` | `CONFIG_IS_ENABLED()`/`IS_ENABLED()`/`CONFIG_VAL()` 的**权威实现**(那套"未定义当 0"的粘贴机制)。自己抄一份 = 第二处真值 + 抄错就静默走错分支 ⇒ 转发原件 |
| `include/linux/list.h` | `mmc_legacy.c` **真的遍历**设备链(`INIT_LIST_HEAD`/`list_add_tail`/`list_for_each`/`list_entry`)⇒ 需要**真实现**, 不是"只为布局成立"(与 erofs 那边相反) |
| `include/linux/poison.h` | 上面那份的 include 闭合集(`LIST_POISON1/2`) |
| `include/memalign.h` | `mmc.c:970` 的 `ALLOC_CACHE_ALIGN_BUFFER(u8, test_csd, 512)`(CMD6 的栈上 DMA 安全缓冲) |
| `include/dm/uclass-id.h` | `struct blk_desc::uclass_id` + `UCLASS_MMC`。它是**零 include 闭合的纯枚举** ⇒ 抄一份 140 个值到 `adapter/` 就是"把上游源码抄进 adapter/" |

★ 三份**没有** vendor、而是由 `compat/` 给"忠实但最小"接口类型的头:
`include/blk.h`(它拖进 `<bouncebuf.h>` + `<efi.h>` 整族)、`include/part.h`(它拖进
`<u-boot/uuid.h>` + 整族分区表解析器声明)、`include/cyclic.h`(它拖进 `<u-boot/schedule.h>`
→ `<uthread.h>`, 而本件不启用 `CONFIG_CYCLIC`, 上游那份在关闭时本来就只有空结构 + 三个空
函数)。理由逐条写在 `compat/blk.h`/`compat/part.h`/`compat/cyclic.h` 的文件头。

## 2. 依赖方向(单向)与目录职责

```
   ┌── third_party/uboot-imx/ ─────────────────────────────────────────────────┐
   │  drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c  ← 只读原件    │
   │  include/{mmc,sdhci,memalign}.h  include/linux/{kconfig,list,poison}.h     │
   │  include/dm/uclass-id.h                                        (sha256 锁) │
   └───────────────────────────────┬───────────────────────────────────────────┘
                                   │  上游 .c 只 include **u-boot 头名**
                                   ▼
   ┌── adapter/uboot-mmc/compat/ ──────────────────────────────────────────────┐
   │  28 个**头名薄壳**(log.h / command.h / linux/types.h / asm/cache.h / …)  │
   │      ↓ 全部接到                                                          │
   │  mmc_env.h    ★ **环境实现**: u8/u32/u64、min/max/BIT/ROUND/fls/do_div、    │
   │               printf/sprintf/puts + log_*/pr_*/debug、malloc 家族、         │
   │               readl/writel、cache/DMA、errno、★ udelay/get_timer(见 §4)    │
   │  generated/autoconf.h   ★ **配置矩阵**(见 §3; kconfig.h 的第一行 include)  │
   │  7 个**转发壳** → vendor 原件: mmc.h / sdhci.h / memalign.h /              │
   │               linux/{kconfig,list,poison}.h / dm/uclass-id.h              │
   │  3 个**接口类型壳**: blk.h / part.h / cyclic.h                            │
   │  std-names/{errno.h,time.h}  ★ 标准头名的单元级补丁(见 §6 偏离 12)         │
   └───────────────────────────────┬───────────────────────────────────────────┘
                                   │  compat/ 只**声明**三个环境钩子
                                   ▲
   ┌───────────────────────────────┴── adapter/uboot-mmc/glue/ ────────────────┐
   │  emmc_internal.h  本项目的内部契约(器件数/擦写区/ioctl 布局/观测面)      │
   │  emmc_bdev.c      ★ 三个环境钩子的**实现** + PCIe 绑定 + 静态实例表        │
   │                   + IRQ(ISR/bh) + bdev ops + ioctl + 四阶段钩子           │
   │  emmc_selftest.c  TC-EMMC-001..008(经 bdev 公开面的端到端)                │
   └───────────────────────────────────────────────────────────────────────────┘
```

**单向**: 上游 `.c` **不** include 任何 `<br/...>`(它只 include u-boot 头名, 那些名字由
`compat/` 兑现); `glue/` 可以 include `<br/...>`(它就是本项目的对接层)与上游头。
判据 = `tools/check-third-party-pristine.sh` 判据 ⑤(全树无"直写 third_party 路径"的直连
包含)和"`src/` 那一层不存在"。

★ **`compat/` 只声明、`glue/` 实现**的三个钩子(u-boot 侧没有对应物):

| 钩子 | 为什么它属 glue |
|---|---|
| `uboot_mmc_env_memalign(align, size)` | 上游 `sdhci_adma_init()` 要一张 **DMA 安全**的描述符表 ⇒ 必须来自 DMA 池(`br_dma_alloc`, 见 ADR-0003 的 region 表), 那是本项目的资源面 |
| `uboot_mmc_env_reg_touched(addr)` | 记"刚被访问的 MMIO 地址", 供 §4.3 的"等的是哪块卡"反查 |
| `uboot_mmc_env_wait()` | **本刀的核心手法**: 让出 CPU 到"下一 ms **或**设备中断"(见 §4) |

## 3. 配置矩阵(`compat/generated/autoconf.h`)—— 逐条与陷阱

上游满地的 `CONFIG_IS_ENABLED(X)`/`#ifdef CONFIG_X` **决定上游代码的形状**。本件像 u-boot
的真实构建一样提供 `generated/autoconf.h`, 而 `include/linux/kconfig.h`(vendor 原件)第一行
就是 `#include <generated/autoconf.h>`。逐条理由见该文件里的矩阵表, 这里给摘要:

| 启用 | 值 | 为什么 |
|---|---|---|
| `CONFIG_MMC_WRITE` | 1 | `mmc_write.c` 进了 `sources`; 不给它, `struct mmc` 的 `write_bl_len`/`erase_grp_size`/`ssr` 被裁掉 |
| `CONFIG_MMC_SDHCI_ADMA` | 1 | 要 ADMA2 通路(ADR-0021 D-4); 且 `struct sdhci_host` 才有 `adma_desc_table` |
| `CONFIG_MMC_SDHCI_ADMA_HELPERS` | 1 | `sdhci-adma.c` 在 u-boot 里只在这个开关下编(它自己的 Makefile); 我们列了它 ⇒ `sdhci_ops::adma_write_desc` 必须存在 |
| `CONFIG_MMC_QUIRKS` | 1 | 打开 `mmc.c:2965` 的 `#ifdef`, 让 `mmc_get_op_cond()` 置上三条重试位(set_blocklen/send_cid/app_cmd) |
| `CONFIG_SYS_MMC_MAX_BLK_COUNT` | 64 | **值型**。同时决定 `cfg->b_max` 与 `ADMA_TABLE_SZ`。★ 与 `glue/emmc_internal.h` 的 `BR_EMMC_MAX_RW_SECTORS` **同源**(那里有两条 `_Static_assert` 把它钉成编译期判据) |
| `CONFIG_LOGLEVEL` | 4 | **值型**。`mmc.c:175` 的 `CONFIG_VAL(LOGLEVEL)` 展开成裸标识符, 不定义就 `-Wundef` 报错。4 = WARNING ⇒ 上游的"逐命令 trace"不参与编译 |
| `CONFIG_FS_FAT_MAX_CLUSTSIZE` | 32768 | **值型**, 属**接口面**: `include/part.h`(块层公共面)经它算 `MAX_CLUSTSIZE`。与 `adapter/uboot-fat` 同源同值 |

**不定义**的一族(每条都在 autoconf.h 里写了理由): `DM_MMC`/`DM`/`BLK`/`DM_REGULATOR`/
`DM_GPIO`/`CYCLIC`/`MMC_TINY`/`MMC_UHS_SUPPORT`/`MMC_HS200_SUPPORT`/`MMC_HS400_SUPPORT`/
`MMC_HS400_ES_SUPPORT`/`MMC_SUPPORTS_TUNING`/`MMC_HW_PARTITIONING`/`MMC_IO_VOLTAGE`/
`MMC_SDHCI_SDMA`/`MMC_SDHCI_ADMA_64BIT`/`MMC_SDHCI_IO_ACCESSORS`/`MMC_PWRSEQ`/
`MMC_SPEED_MODE_SET`/`MMC_VERBOSE`/`MMC_TRACE`/`XPL_BUILD`/`SYS_64BIT_LBA`/
`PARTITION_UUIDS`/`FIXED_SDHCI_ALIGNED_BUFFER`。

### ★★ 陷阱一: `#ifdef` 认"定义没定义", 不认值是几

- 要**启用**的: `#define CONFIG_X 1`(`CONFIG_IS_ENABLED` 靠 `__ARG_PLACEHOLDER_1` 的粘贴机制判定);
- 要**关闭**的: **什么都不定义**。
  ⚠ **绝不能**写 `#define CONFIG_DM_MMC 0` —— 那会让 `sdhci.c:200/646/759` 的
  `#ifdef CONFIG_DM_MMC` 变**真**, 上游立刻按 DM(`udevice`)形态编译 ⇒ 直接编不过。
- "值型"配置必须给(它们是裸标识符, 不定义就 `-Wundef -Werror` 直接红)。

`mmc_env.h` 把这条纪律做成了**编译期自检**: `#ifdef CONFIG_DM_MMC` / `#ifdef CONFIG_BLK`
一律 `#error`; `MMC_SDHCI_ADMA`/`MMC_SDHCI_ADMA_HELPERS`/`MMC_WRITE` 缺一个也 `#error`。

### ★★ 陷阱二: `log_*` 与 `pr_*` 是**两种不同的裁剪层**(实测踩到)

| 族 | u-boot 里的裁剪 | 本件 |
|---|---|---|
| `log_err/log_warning/log_info/log_debug` | **不做编译期裁剪**(展开成 `log(cat, level, …)`, 只运行期过滤) | 直接转 `br_log_write(BR_LOG_*, …)` |
| `pr_emerg/pr_alert/pr_crit/pr_err/pr_warn/pr_notice/pr_info/pr_debug` | **按 `CONFIG_LOGLEVEL` 编译期裁剪**(常量条件三元式, 阈值 `>0/>1/…/>7`) | 逐条**同阈值**给出 `UBOOT_MMC_LOG_GATED(cond, level, …)` |
| `debug()` | `debug_cond(_DEBUG, …)`, `_DEBUG = (DEBUG ? 1 : 0)`; 上游版靠 `if (cond)` + DCE 丢实参 | 常量三元式(`_DEBUG == 0`), 与优化级别无关 |

**为什么这不是风格问题**: 上游 `mmc.c:175` 用 `#if CONFIG_IS_ENABLED(MMC_VERBOSE) ||
defined(DEBUG) || CONFIG_VAL(LOGLEVEL) >= LOGL_DEBUG` 把 `mmc_mode_name()` 的**定义**也裁掉,
而它的**全部调用点都在 `pr_debug(...)` 的实参里**(mmc.c 的 232/1748/1884/2228 行)。
若 `pr_debug` 不按 `CONFIG_LOGLEVEL` 裁剪(像 `log_*` 那样无条件转出去), 实参就会被求值 ⇒
**定义被裁掉、引用还在** ⇒ 链接期 `undefined reference to mmc_mode_name`(本层第一版正是
如此, 报告里有原始报错)。

## 4. ★ 核心手法: 上游的**轮询等待** → **中断驱动的睡眠等待**

上游 `sdhci.c` 只通过两个函数让出 CPU(任务书 §2.4):

```c
/* 命令完成 */
start = get_timer(0);
do { stat = sdhci_readl(host, SDHCI_INT_STATUS); …;
     if (get_timer(start) >= SDHCI_READ_STATUS_TIMEOUT) … } while ((stat & mask) != mask);
/* 数据传输 */
do { stat = sdhci_readl(host, SDHCI_INT_STATUS); …; if (timeout-- > 0) udelay(10); … }
while (!(stat & SDHCI_INT_DATA_END));
```

⇒ **环境层把 `get_timer()`/`udelay()` 实现成"让出 CPU 直到'下一 ms 或设备中断'"**,
上游的轮询循环就自动变成"睡下 → 设备中断 → 被唤醒 → 再读一次 `INT_STATUS`"的中断驱动等待,
而**上游一行代码都不用改**。这是本刀的核心手法。

### 4.1 三段式(ADR-0021 D-4 的形态)

| 段 | 代码 | 做什么 |
|---|---|---|
| **ISR** | `emmc_isr()`(glue) | 读 `INT_STATUS`(**只读, 绝不清** —— 上游的循环靠读它判完成); 把 `SIGNAL_ENABLE` 写 0 **去断言电平线**; `br_work_submit()` 交下半部; `isr_count++` |
| **bh** | `emmc_irq_bh()`(glue) | `br_sem_give()` 唤醒等待者; `bh_count++`。bh 契约: 关中断、不可阻塞、**无任何线程面调用** |
| **等待** | `uboot_mmc_env_wait()`(glue) | 睡下**之前**重开本卡 `SIGNAL_ENABLE`(所以每一轮都能被下一次中断唤醒); `br_sem_take(…, win)`; 记账"被中断唤醒"还是"兜底超时" |

### 4.2 为什么 ISR 不能清 `INT_STATUS`(以及这是**电平**中断的对策)

QEMU 的 SDHCI 是**电平**中断, 断言条件 = `INT_STATUS & SIGNAL_ENABLE`(`INT_ENABLE` 是上游
`sdhci_init()` 已经打开的"源使能")。上游要在自己的循环里读到那些位并**自己**清; 我们清了它
就读不到。而**去断言电平线的正统做法是掩掉信号源** —— 这正是 `SIGNAL_ENABLE` 的用途。
等待原语睡下前重开, 于是"一次中断 → 一次唤醒"自洽。

### 4.3 环境层怎么知道"现在等的是哪块卡"

上游的等待循环**没有设备参数**。所以 `compat/mmc_env.h` 的 MMIO 访问器把**刚被访问的地址**
告诉 `uboot_mmc_env_reg_touched()`(一次 store), 由 glue 用它反查"这是哪个已注册的 SDHCI
窗口"(BAR0 起 0x100 字节内)。⇒ 四块卡各有自己的信号量与中断通道(**多实例**, ADR-0022),
不是全局单例。**代价**: 一次额外 store / MMIO 访问; **收益**: 上游一行不改。

### 4.4 等待窗口 = **1 ms**, 不是时钟节拍

`UBOOT_MMC_WAIT_MAX_US = 1000`(任务书 §2.4 的"下一 ms"; 定义在 compat, 是**单次让出窗口的上界**), 而 `udelay(us)` 按调用方要的时长分片(`udelay(100)` ≈ 100 µs)。★ 刻意**不取** `BR_CFG_TICK_PERIOD_US`
(本产品 `hz = 200` ⇒ 5 ms): 上游把 `get_timer()`/`udelay()` 当**轮询节拍器**用
(inhibit 循环 `udelay(100)`、`get_timer(0)`、数据循环 `udelay(10)`), 一次"读时钟"就等价于
"睡一拍"的话, 每条命令要几百毫秒、整个识别被拖成秒级, 而等待窗口远大于设备事件间隔 ⇒
中断驱动退化成"每拍醒一次看看"。取 1 ms 才是任务书要的语义, 且与 `get_timer()` 的毫秒口径
同粒度。也不取更小(如 100 µs): 这个值是**兜底**上界, 设备中断按时到达时根本用不到, 越小越
频繁空转。

### 4.5 ★ 不变量: "没有线程在等 ⇒ INTx 线未断言"

**必须是"线未断言"而不是"`SIGNAL_ENABLE == 0`"**: 电平断言条件 = `INT_STATUS &
SIGNAL_ENABLE`, 而上游 `sdhci_send_command()` 在**每次命令结束时**都
`sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS)` 清干净 ⇒ 命令边界处即使信号还
开着, 线也是平的。判据由 `uboot_mmc_irq_idle()` 直接量这个条件, 自检每卡打印一行
`idle-signal=flat|ASSERTED`。

**打开 `SIGNAL_ENABLE` 的唯一位置 = 等待原语睡下之前**; bring-up/探测结束时**不打开**。

> 实测教训(报告 §1 有原始日志): 第一版在 bring-up 结束时**无条件打开**了信号源, 而当时
> `INT_STATUS` 里压着一个上游"读到了还没清"的 `RESPONSE` 位 ⇒ 这条 INTx 在整个**非传输
> 窗口**(横跨 core 的 IRQCONF 自检期)持续投递, 与 IRQCONF 在 bh 上下文里的 thread-only
> API 执法用例交叉, 炸出 `[sched] br_task_exit(): 不许在中断下半部(bh)里结束线程`。
>
> 也**试过**"等待返回前再掩一次"(更强的"信号恒 0"形态), 实测**代价很大**: 掩掉之后设备在
> "上游正跑代码"窗口里产生的事件没人听, 下一次等待只能靠 1 ms 兜底发现 ⇒ 整轮门禁从 ~2.8 s
> 退化到 29 s 并撞上 30 s 门禁超时。⇒ 保留"信号源开着 + **线**判据"的形态。

## 5. glue 做了什么(逐件)

### 5.1 `glue/emmc_bdev.c`

1. **四阶段钩子**: `early_init`/`init` 空转, 真正的"探测 + 识别 + 注册 bdev"落在 **`start` 相**
   —— 中断驱动意味着 CORE 相里等不到中断(全局中断还没开, ISR/bh 不跑)。这也是
   `adapter/uboot-{fat,erofs}` 必须对本件声明 init 边的原因。
2. **PCIe 绑定**: 遍历 `br_pci_dev_at()`, 认 `1b36:0007`; 取 BAR0 与 `intx_virq`。
3. **静态实例表** `s_dev[BR_EMMC_MAX_DEVS]`: 每卡一份 `struct sdhci_host`(**上游类型**,
   按值内嵌)+ `struct mmc *`(上游 `mmc_create` 从堆里取)+ 几何 + 中断通道 + 计数。
4. **中断**: `br_sem_init` → `br_irq_register(virq, emmc_isr, d, attr{INLINE})` → `br_irq_enable`。
   ★ **先于**上游初始化建立 —— 上游的等待循环从第一条命令起就要能被唤醒。
5. **上游装配**: `add_sdhci(&d->host, 0, 0)` —— 这正是上游自己的职责
   (`sdhci_setup_cfg()` 算 caps/时钟/电压/host_caps/b_max + 建 ADMA2 表; `mmc_create()` 分配
   `struct mmc` 并取 `devnum` 0..3)。**我们一行都不重写**。
6. **上游启动**: `mmc_init(d->mmc)`(CMD0/1/2/3/9/7 + EXT_CSD + 总线宽度/时序切换)。
7. **bdev ops**: `read`/`write`/`flush`/`geometry`/`ioctl`/`suspend`/`resume`。
   ★ 越界在**本层**翻译: 上游 `mmc_bread()` 对越界是"返回 0"(u-boot 块层口径"0 = 出错"),
   而本项目 bdev 契约是"成功 = 传输扇区数 / 失败 = 负 errno"(SD-10)⇒ 0 → `-EIO`
   (这正是 TC-EMMC-004"越界不落卡"的落点: 一条命令都不发给卡)。
8. **`mmc_list_init()`** —— 上游设备链初始化(`cur_dev_num` 归 0 ⇒ 四块卡 `devnum` = 0..3,
   正好对应 `emmc0..emmc3`; 任务书 §2.2)。**不**调 `mmc_initialize()`: 那是 u-boot 的
   "静态板级 mmc 列表"路径, 本件的设备来自 PCIe 枚举。
9. **三个环境钩子的实现**(§2 的表)+ 上游块层挂点(`blk_dselect_hwpart`/
   `blk_select_hwpart_devnum`/`part_init` —— 语义逐条对齐上游 `mmc_legacy.c::mmc_select_hwpartp`)。

**运行期 SET_PATH 怎么切 `host->flags`**(任务书 §2.6): `emmc_set_path()` 只做一件事 ——
置/清 `d->host.flags` 的 `USE_ADMA` 位。上游 `sdhci_send_command()` 里
`if (host->flags & USE_DMA)` 决定是否走 `sdhci_prepare_dma()` + `SDHCI_TRNS_DMA`;
清掉 `USE_ADMA` 就自然落 `sdhci_transfer_data()` 的 PIO 分支。**没有**第二个判据(见 §6 偏离 6)。
`USE_ADMA` vs `USE_DMA` 的区别: `USE_DMA = USE_SDMA|USE_ADMA|USE_ADMA64`, 本件只可能置
`USE_ADMA`(SDMA/64 位不参与编译)。

### 5.2 `glue/emmc_selftest.c`

**语义与判据与旧形态一字未改**(TC-EMMC-001..008), 只经**公开面**驱动
(`br_bdev_get` + `br_dev_*` + ioctl 编码), 一律"先写后读"。★ **只在 `emmc3` 的擦写区写**
(`BR_EMMC_SCRATCH_LBA` 起的 512 KiB, 远在 EROFS 镜像之后) —— `emmc0/1/2` 上是活的 FAT 卷,
而本套件跑在 FAT/EROFS 套件之后。

新增的**可观测面**(任务书 §5 与 ADR-0021 D-4 要求"中断驱动不能只写在注释里"):

- 每卡一行 `IRQ[<用例/通路>] <dev> ISR +N (total M), bh +B, drop +D, wake irq/token/timeout +A/+S/+T`;
- 每卡一行 `/dev/emmcN IRQ count=… (bh=…, dropped=…, wake irq/token/timeout=A/S/T), idle-signal=flat`;
- **三种返回分开数**(★ 这一项做错过一次, 见下):
  - `A = wake irq` —— 睡下之后被**设备中断唤醒**(窗口内 ISR 增长)。最强证据;
  - `S = wake token` —— 进 `br_sem_take` 时**令牌已经在**(ISR/bh 在上一轮就 give 过)⇒ 立即
    返回、根本没睡。**同样是中断捕获了事件**的证据, 只是被下一次调用消费掉;
  - `T = wake timeout` —— 真的睡满 1 ms 窗口、期间一个 ISR 都没有。**这才是兜底**。
  判据是 **`A + S ≫ T`**; 只看 `A` 会在"令牌被下一轮消费"的形态下得出**相反的错误结论**;
- TC-EMMC-004 另有一条: 越界时 ISR 增量**必须为 0**(一条命令都没发)。

## 6. 与上游的偏离(逐条, 必须如实读)

| # | 偏离 | 为什么 / 影响 |
|---|---|---|
| 1 | **ISR 不清 `INT_STATUS`**, 只掩 `SIGNAL_ENABLE` | 上游的等待循环靠读它判完成(§4.2)。**后果**: 冗余中断投递 —— 上游"读到但未清"期间, 等待原语重开信号会立刻再断言一次 ⇒ `isr_count` 比"命令数"大(倍数 = 每条命令内读而未清的次数)。这些**仍是真 INTx 投递**, 只是冗余; 实测 8 扇区 write+read(16 条命令)≈ 23 次投递 |
| 2 | 真正的 1 ms 兜底只占 ~1.5% | 实测(交付版日志): `wake irq/token/timeout = 766/328/16`(emmc0)⇒ **1094/1110 由中断驱动**, 只有 16 次真兜底; 三个传输用例的增量是 `+16/+6/+0`、`+30/+7/+0`、`+18/+8/+0`(**`timeout` 全为 0**)。剩下的兜底来自"上游的 inhibit 循环/时钟稳定循环本来就没有设备事件可等"⇒ 这是"下一 ms **或**设备中断"的语义本身。★ 第一版把"令牌已在、立即返回"也记成 timeout, 于是误得出"完全靠超时"——记账口径的坑记在 `emmc_bdev.c` 的记账注释里 |
| 3 | `U_BOOT_LEGACY_BLK(mmc)` 降级为**普通全局对象** | 上游把它登记进 `.u_boot_list_2_blk_driver_2_mmc` 链接段(供 `blk_driver_lookup_type()` 查); 本件没有那套段枚举(不枚举块驱动, glue 直接持有实例)⇒ 宏展开成普通对象, `mmc_legacy.c` 原文因此仍逐字成立, 但没人遍历(`--gc-sections` 丢掉它)。**接口面由 glue 直接兑现**: `blk_dselect_hwpart`/`blk_select_hwpart_devnum` |
| 4 | `part_init()` 是**显式空实现** | 本件不做软件分区(v1 暴露裸设备; 分区映射是 SD-9/v1.x 的面)。给一个语义明确的空实现是"适配"; 把上游那一行删掉才是"改上游" |
| 5 | **不调** `mmc_initialize()`, 只调 `mmc_list_init()` | 前者走 u-boot 的静态板级列表(`board_mmc_init()` → `cpu_mmc_init()`); 本件设备来自 PCIe 枚举。两个钩子仍以 `__weak` 给出(u-boot 缺省语义: 返回 -1) |
| 6 | **取消** PIO 阈值/对齐兜底 | 旧改写副本有 `BR_SDHCI_PIO_THRESHOLD_SECTORS`(< 4 扇区或缓冲未对齐自动落 PIO)。上游的实现**只有** `if (host->flags & USE_DMA)` ⇒ 运行期 SET_PATH 就是改那个位(任务书 §2.6)。**影响**: 由调用方保证缓冲对齐(ADMA2 只要求字对齐) |
| 7 | **未移植** SDMA / 64 位 ADMA | 矩阵里 `MMC_SDHCI_SDMA`/`MMC_SDHCI_ADMA_64BIT` 不定义 ⇒ 这两条上游分支不参与编译(`sdhci_prepare_dma()` 里它们是 `#if`/运行期位)。QEMU 的 caps 也没有 SDMA |
| 8 | **未做** `CACHE_FLUSH`(flush 只"等卡不忙") | 见 §7 的 `br-wa-emmc-001`: 设计 `7-02` §1 的 `flush` 语义前提是"必须落介质", 而上游写命令返回前已轮询过状态(块已落卡内介质)⇒ v1 的"等卡不忙"满足"不吞 flush"; `CACHE_FLUSH` 只对**卡内易失缓存**有意义, 是可在 P5 补的增量。**也未做** eMMC 硬件分区(boot/GP)/RPMB(只暴露 user area) |
| 9 | flush **不做** `br_mm_cache_flush` | 与 erofs 同口径(ADR-0021 D-5): flush 的语义是"数据到介质", 不是"清 CPU cache"; 且上游的 `dma_map_single`/`dma_unmap_single` 已按方向做 cache 维护 |
| 10 | `mmc_mode_name()` 在运行期**不存在** | 上游把它连定义一起裁掉(§3 陷阱二), 而 `enum bus_mode` 的完整名字表在 `#if` 之外还用不到。glue 的 `eMMC ready` 行给一个**最小映射**(legacy/HS/HS-52MHz/DDR52/other) —— 门禁不依赖它 |
| 11 | 5 个 errno 名字由 `compat/mmc_env.h` 补 | `ECOMM`(70)/`EBADMSG`(74)/`ENOMEDIUM`(123)/`EMEDIUMTYPE`(124)/`ENOTSUPP`(524) —— core 的 `br_error.h` 按 ADR-0027 只收"本仓用到的"第二段名, 而上游原件用到了这 5 个。**数值逐字取自 u-boot `include/linux/errno.h`**, 搬进 core 时一个数值都不用改。**这是欠债, 见报告 §6** |
| 12 | `errno.h`/`time.h` 走 `cflags` 的 `-I` 而**不列 `includes`** | 它们是**标准头名**, 与 `runtime/posix/include/{errno.h,time.h}` **撞名**; 而 brickie 的包含面判据(ADR-0018)是**名字级**的(名字 → 归属插件, 看不到 per-unit `-I`)⇒ 列进去会让 `runtime/posix/src/posix.c` 与 `iface/posix` 被判成"include 了本插件的头"(7 条 BRV-MF-0001, 假依赖)。两个壳不是本插件抛给别人的接口面, 只是两个标准名的**单元级环境补丁**。**回归条件**: 该判据改为按 per-unit `-I` 解析(或排除标准 C 头名)时搬回 `includes` |
| 13 | **vendor 告警口径** | `[build].cflags` 关掉 `-Wshadow -Wpointer-arith -Wmissing-prototypes -Wsign-compare -Wunused-parameter -Wint-to-pointer-cast -Wpointer-to-int-cast -Wunused-function`(ADR-0026 D-4)。**不能**为了过我们的告警去改上游 —— 那正是本刀要消灭的"就地改三方件"。`-Wno-unused-function` 是因为上游按 Makefile 裁**文件**、我们按**配置**裁(矩阵关掉 HS200/tuning 后留下几个无调用者的 `static`) |

### 6.1 ★ 一条"不是偏离、但是坑"的记录(适配层自己的 bug, 已修)

`compat/mmc_env.h` 第一版把 `lower_32_bits` 误写成 `upper_32_bits` 的展开式
(`#define lower_32_bits(n) ((u32)(((n) >> 16) >> 16))`, 而上游 `include/linux/kernel.h:114`
是 `((u32)((n) & 0xffffffff))`)。后果: 每个 ADMA2 描述符的 `addr_lo` 都成了 `addr >> 32 == 0`
⇒ QEMU 的 ADMA 引擎去取 0 号地址 ⇒ `SDHCI_INT_ADMA_ERROR`(`st=0x02008001`,
`adma_err=0x1`)⇒ `mmc_init` 返回 `-ECOMM`(rc=-70)⇒ 四块卡一块都起不来。
**教训**: 环境层的宏不是"随便给个同宽的" —— 这一条与 erofs 那边"`u64` 不能用 `uint64_t`"
是同一类问题(同宽不同语义)。现在 `lower_32_bits`/`upper_32_bits` 逐字取自上游, 并在注释里
留了这条实测记录。

## 7. 工作台账(欠债在源码里的标记点)

| id | 在哪 | 是什么 |
|---|---|---|
| `br-wa-emmc-001` | `glue/emmc_bdev.c::emmc_flush()` 的注释 | flush 只"等卡不忙", 未发 EXT_CSD `CACHE_FLUSH`(CMD6 SET_BIT[32]); 未做 boot/GP/RPMB 分区。★ 本标记从已删除的 `src/mmc_core.c` **搬来**(语义不变), 否则 `tools/check-workarounds.sh` 的双向绑定会红 |
| `br-wa-test-001` | `glue/emmc_selftest.c` 的文件头 | `TC-EMMC-*` 是**自编号**(设计 `6-01` 的用例表里没有 eMMC 族) |
| `br-wa-tp-001` | `plugin.toml` 的文件头 | 第三方模块的许可残余风险 + 工具链的 `third_party` 描述符模板缺口 |

## 8. 判据在哪(全部机械, 都能跑)

| 判据 | 命令 | 它钉住什么 |
|---|---|---|
| vendor 只读 | `bash tools/check-third-party-pristine.sh` | ① 锁元数据齐 ② vendor 逐字节 = 锁 ③ 锁里每条都在磁盘上 ④ vendor 无锁外文件 ⑤ `third_party/` 下无 `plugin.toml` ⑥ 无人**直连**三方件的头 |
| errno 单一真值 | `bash tools/check-errno.sh` | 除 `br_error.h` 外无第二处 errno 定义(§6 偏离 11 的边界) |
| WORKAROUND 双向绑定 | `bash tools/check-workarounds.sh` | §7 的三个标记与 `WORKAROUNDS.md` 登记表一一对应 |
| 上游真在编译 | `brickie build` 后 `ls build/obj/third_party/uboot-imx/drivers/mmc/` | 5 个上游 `.o`(证明编的是 vendor 原件) |
| 端到端 | `brickie test emmc-test` | 见 `tests/gates.toml` 的 `emmc-test` 段: `[SELFTEST] SUMMARY … fails=0 errors=0`、四个 `pci: 00:0N.0 1b36:0007`、`uboot-mmc: eMMC ready`、`/dev/emmc0`+`/dev/emmc3 就绪`、`共接管 4 块 sdhci-pci 卡`、`[PCICONF] SUMMARY … fail=0`、`[EMMCCONF] SUMMARY … fail=0`、`irq_ticks=[1-9]`、8 条 `TC-EMMC-*` 逐条 PASS; 禁 `[PCICONF] FAIL`/`[EMMCCONF] FAIL`/`[PANIC]` |
| 邻居(同一条真链) | `brickie test fat-test` / `brickie test erofs-test` | 三卷 FAT + 一卷 EROFS 都挂在这套块设备之上 ⇒ 它们绿 = 上游 MMC 栈真的在读写介质 |

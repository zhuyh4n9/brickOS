/*
 * adapter/uboot-mmc — **插件内部契约**(glue/; 本项目代码, 上游没有对应物)
 *
 * ============================== 分层(ADR-0026 后的形态) ==============================
 *   third_party/uboot-imx/drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c
 *        上游原件, **原样编译**(没有副本; 锁与判据见 tools/check-third-party-pristine.sh)
 *   compat/   u-boot 运行环境: 头名壳 + mmc_env.h + generated/autoconf.h(配置矩阵)
 *   glue/     本项目的对接: PCIe 绑定 + 静态实例表 + 中断(ISR/bh) + bdev ops + ioctl + 自检
 *
 * ⇒ 本头是 **glue 这一侧**的契约: 上游需要而"环境层不该知道"的东西(器件数、擦写区、
 *   ioctl 载荷布局、IRQ 计数观测面)都在这里。上游的**协议/主机状态**不在这里 ——
 *   那是 `struct mmc`(vendor `include/mmc.h`)与 `struct sdhci_host`(vendor
 *   `include/sdhci.h`), 通过 `compat/` 的转发壳原样使用。
 *
 * ★ 为什么本头可以 include 上游头而 compat/ 也可以: 依赖方向是
 *   `glue → compat → third_party`(单向)。判据 = `src/` 那一层**已删除**,
 *   且全树不出现"尖括号/引号直写 third_party 路径"的直连包含(门禁判据 ⑤)。
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H
#define BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H

#include "mmc_env.h"    /* 适配层: u-boot 运行环境 → brickOS(唯一入口) */
#include "mmc.h"        /* 上游原件(compat/mmc.h 转发): struct mmc / struct mmc_config */
#include "sdhci.h"      /* 上游原件(compat/sdhci.h 转发): struct sdhci_host / 寄存器/中断位 */
/* 上游的**私有**契约(`drivers/mmc/mmc_private.h`): 它声明上游不放进公开头的内部 API ——
 * 本件的 glue 正是那些 API 的调用方(`mmc_bread`/`mmc_bwrite`/`mmc_poll_for_busy`/
 * `mmc_list_init`/`mmc_switch_part`)。这与 `adapter/uboot-erofs/glue/erofs_glue.h` include
 * 上游 `fs/erofs/internal.h` 同款: glue 认识被适配的代码是允许的方向。 */
#include "mmc_private.h"

/* ==================================================================== 数据通路 */
/* 运行期可切(ADR-0021 D-4)。★ 落地方式: 直接改上游 `struct sdhci_host::flags` 的
 * `USE_ADMA` 位 —— 那个字段就是上游"数据通路"的接口(任务书 §2.6), 不另立抽象。 */
#define BR_MMC_PATH_DMA 0u
#define BR_MMC_PATH_PIO 1u

/* ==================================================================== 传输上限
 *
 * ★ **同源**: 单次传输上限就是上游的 `CONFIG_SYS_MMC_MAX_BLK_COUNT`(值型配置, 见
 *   `compat/generated/autoconf.h` 的矩阵表): 它在上游同时决定
 *   `cfg->b_max`(协议核心切开大请求的粒度)与 `ADMA_TABLE_SZ`(ADMA2 描述符表大小)。
 *   本件不另立一个数 —— 两处不一致会造成"描述符表装不下一次传输"的越界写。
 *
 *   下面两条 `_Static_assert` 把这件事变成**编译期判据**(而不是注释里的约定):
 *     ① 上限必须正好是 64(与旧改写副本的 `BR_EMMC_MAX_RW_SECTORS` 同值, 契约不变);
 *     ② 一次 64 扇区传输必须落在**一个** ADMA2 描述符的 len 上限(65532 B)之内。 */
#define BR_EMMC_MAX_RW_SECTORS  CONFIG_SYS_MMC_MAX_BLK_COUNT
#define BR_EMMC_SECTOR_SIZE     512u

_Static_assert(BR_EMMC_MAX_RW_SECTORS == 64u,
               "BR_EMMC_MAX_RW_SECTORS 必须与 CONFIG_SYS_MMC_MAX_BLK_COUNT 同为 64");
_Static_assert((BR_EMMC_MAX_RW_SECTORS * BR_EMMC_SECTOR_SIZE) <= ADMA_MAX_LEN,
               "一次满额传输必须装进一个 ADMA2 描述符");

/* `flush` 的等待上界: eMMC 的 CMD6(GEN_CMD)在 SET_BIT 时可能很慢(u-boot 的
 * `DEFAULT_CMD6_TIMEOUT_MS` 就是 500 ms)。 */
#define BR_EMMC_CMD6_TIMEOUT_MS 500u

/* `udelay()`(忙等)与"让出等待"的分工见 `compat/mmc_env.h` 的文件头:
 * 前者等"寄存器稳定/复位完成"(无设备事件可等 ⇒ 忙等), 后者等"设备事件"(⇒ 让出 + 中断唤醒)。
 * 判据在 `compat` 那一侧表达, 这里不再需要阈值常数。 */

/* ==================================================================== 环境等待窗口
 *
 * ★ 这是"轮询 → 中断驱动"的**关键常数**(任务书 §2.4: "让出 CPU 直到**下一 ms** 或设备中断")。
 *   它是 1 ms, **不是** 时钟节拍(`BR_CFG_TICK_PERIOD_US` = 1/HZ; 本产品 HZ=200 ⇒ 5 ms)。
 *
 * ★★ 实测教训(本刀踩到并修掉, 见 ADR-0029 §4): 它必须只是**单次让出窗口的上界**,
 *   **不能**当成"每次 `udelay()` 都睡这么久"。上游拿 `udelay(100)` 当"轮询节拍器"
 *   (inhibit 循环 / 时钟稳定循环 / `sdhci_reset` 的自旋), 若每次调用都睡满 1 ms,
 *   四块卡的 bring-up 从 **0.037 s 涨到 0.376 s**(实测), 整段自检被推过 `irq-test`
 *   的 6 s 门禁上限(实测: 迁移前 irq-test 2/5 红, 迁移后 5/5 红)。
 *   ⇒ 现在 `udelay(us)` 按**调用方要的时长**让出(`min(剩余, 1 ms)` 分片), 于是
 *   `udelay(100)` ≈ 100 µs(与 u-boot 的语义一致), 而 `get_timer()` 的 1 ms 让出窗口
 *   仍然可被设备中断提前唤醒(那才是命令完成等待的路径)。
 *
 * 常数本身定义在 `compat/mmc_env.h`(`UBOOT_MMC_WAIT_MAX_US`)—— 它是**环境层**的参数
 * (compat 声明等待原语时要用), 不在 glue 这一侧再定义一份。 */


/* ==================================================================== 器件契约(私有) */

/* 本插件最多同时驱动的卡数(= `sdhci-pci` 端点数上界)。取值来自**平台事实**而不是拍脑袋:
 * QEMU virt 的 GPEX 只把 4 根 INTx 线接到 GIC(SPI 3..6), 而 core 的 IRQ 框架规定
 * "每根物理线恰一属主"(`irq_core.c` INV-A)⇒ 第 5 块卡的 INTx 会与第 1 块撞线而注册失败。
 * 所以 4 是本平台**能挂满**的卡数。 */
#define BR_EMMC_MAX_DEVS     4u

/* 设备名形如 `emmc<d>`(dev-core 命名规则 [a-z][a-z0-9]*, 见 7-02 §1)。 */
#define BR_EMMC_DEV_NAME     "emmc0"
#define BR_EMMC_NAME_MAX     8u

/*
 * 设备级用例的**擦写区**(ADR-0022): 四块卡各有用途(0/1/2 = FAT12/16/32 卷, 3 = EROFS),
 * FAT 卷被文件系统占满, 而 EROFS 是只读的 —— 它的镜像只占卡首(实测 ≤ 1 MiB), 卡尾
 * 留给"写特征模式再读回"的设备级用例。**不与任何文件系统的数据区重叠**这条性质由
 * `tools/mk-fs-images.sh` 断言(EROFS 镜像 > 1 MiB 就直接失败), 不是靠约定。
 */
#define BR_EMMC_ASSETS_DEV        "emmc3"
#define BR_EMMC_SCRATCH_LBA       4096u    /* 2 MiB 起 */
#define BR_EMMC_SCRATCH_SECTORS   1024u    /* 512 KiB 擦写区(2 MiB..2.5 MiB) */

#define BR_EMMC_IOC_MAGIC    'e'
#define BR_EMMC_IOC_GET_INFO 0u
#define BR_EMMC_IOC_SET_PATH 1u

/* GET_INFO 的回答(ioctl 载荷)。生产(emmc_bdev.c)与自检(emmc_selftest.c)共用一份布局。 */
typedef struct br_emmc_info {
    br_uintptr_t base;              /* SDHCI 寄存器窗口(BAR0) */
    br_u64       capacity_sectors;
    br_u32       sector_size;
    br_u32       max_rw;
    br_u32       bus_width;
    br_u32       clock_hz;
    br_u32       path;              /* BR_MMC_PATH_DMA / _PIO */
    br_u32       card_type;
} br_emmc_info_t;

/* ==================================================================== 观测面(自检用) */
/*
 * 每张卡的**等待/中断计数**(ADR-0021 D-4 的"中断驱动"必须可观测, 不能只写在注释里)。
 *
 * `wake` 是本刀**新增的判据面**, 它把"到底是不是中断驱动的"从注释变成数字 ——
 * 只统计 `isr > 0` 是不够的: 若等待主要靠 1 ms 兜底超时返回, `isr` 仍会缓慢增长,
 * 看上去"有中断"。所以等待原语**每次返回都记一笔账**:
 *
 *   irq     进入该卡 INTx ISR 的次数(设备真的投递了电平中断);
 *   bh      由 ISR 经 `br_work_submit()` 交到下半部、并在 bh 里 `br_sem_give()` 的次数;
 *   drop    `br_work_submit()` 因队列满失败(IR-10 回滚路径: 在 ISR 里直接唤醒)的次数;
 *   wake_irq    等待原语**睡下之后被设备中断唤醒**的次数(本次窗口内 `isr` 有增长);
 *   wake_sem    进 `br_sem_take` 时**令牌已经在**(ISR/bh 在上一轮就 give 过)⇒ 立即返回。
 *               ★ 这同样是"事件由中断捕获"的证据, 只是被下一次调用消费掉 —— **不能**当超时读;
 *   wake_to     真的睡满 1 ms 窗口、期间一个 ISR 都没有的次数 —— ★ 这才是**兜底**;
 *   wake_spin   **忙等**的次数(`udelay()` 走这条 —— 等寄存器稳定/复位完成, 没有设备
 *               事件可等, 所以不让出 CPU; 见 `compat/mmc_env.h` 文件头的分工)。
 *
 * ★ 判据: `wake_irq + wake_sem ≫ wake_to` 才算"中断驱动"; 只看 `wake_irq` 会在
 *   "令牌被下一轮消费"的形态下得出错误结论(实测踩到, 见 emmc_bdev.c 的记账注释)。
 *
 * ★ 另有一处**如实记录的冗余**: 因为 ISR 只能掩 `SIGNAL_ENABLE` 去断言电平线(不能清
 *   `INT_STATUS` —— 上游的等待循环要读它), 而等待原语睡下前又必须重开 `SIGNAL_ENABLE`,
 *   于是当上游"读到了状态但还没清"时, 重开信号会让电平线**立刻再断言一次** ⇒ `irq`
 *   会比"命令数"大(倍数 = 每条命令内上游读状态而未清的次数)。这些仍是**真的 INTx 投递**,
 *   但属于冗余投递; 数量关系见 emmc_selftest.c 的对照行与 README 的"与上游的偏离"。 */
void uboot_mmc_irq_counts(br_u32 idx, br_u32 *irq, br_u32 *bh, br_u32 *drop,
                          br_u32 *wake_irq, br_u32 *wake_sem, br_u32 *wake_to);

/* `udelay()`(忙等)那条路的次数 —— 它**不睡**, 所以不计入上面三类。 */
br_u32 uboot_mmc_wake_spin(br_u32 idx);

/*
 * **"没有线程在等 ⇒ 不投递"这条不变量的可判据化**(见 emmc_bdev.c 里唯一打开
 * `SIGNAL_ENABLE` 的那一处)。
 *
 * 电平中断的断言条件 = `INT_STATUS & SIGNAL_ENABLE`, 而上游会留下"读到了但还没清"的位
 * ⇒ 只要在无人等待时把信号打开, 这条 INTx 就会在整个非传输期间持续投递。实测后果:
 * 它与 core 的 IRQCONF 自检(bh 上下文里的 thread-only API 执法用例)交叉, 炸出
 * `[sched] br_task_exit(): 不许在中断下半部(bh)里结束线程`。
 *
 * 判据: 传输结束后(无人在等)`SIGNAL_ENABLE == 0`。自检在每块卡上打印这一条。
 */
br_bool uboot_mmc_irq_idle(br_u32 idx);

#endif /* BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H */

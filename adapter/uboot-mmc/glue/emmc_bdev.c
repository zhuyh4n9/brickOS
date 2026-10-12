/*
 * adapter/uboot-mmc — bdev 对接 + PCIe 绑定 + 实例表 + 中断(ISR/bh) + 运行环境钩子
 *
 * 设计依据: `docs/7-storage/7-02-bdev.md` §1/§4(形状与"DMA/PIO 要能指出")/§5(ISR→bh→唤醒)、
 * `docs/decisions/0021-bdev-pci-emmc.md` D-3(三方隔离)/D-4(中断 + ADMA2/PIO)/D-5(flush 口径)、
 * `docs/decisions/0022-erofs-fat.md`(多卡: emmc0/1/2 = FAT12/16/32 卷, emmc3 = EROFS)、
 * `docs/decisions/0026-third-party-vendor-and-adapter.md`(本刀的架构: 上游原样编译)。
 *
 * ============================== 本文件在架构里的位置 ==============================
 * 上游 `drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c` 是**逐字节原件**,
 * 由 `plugin.toml` 的 `sources` 直接点名参与编译。它们需要两样东西:
 *   ① **运行环境**(类型/宏/MMIO/时间/日志/errno…)—— 由 `compat/` 兑现;
 *   ② **行为对接**(哪块卡、中断线、bdev、实例表)—— 就是本文件。
 *
 * 上游协议核心与主机层的**状态**不再由我们自定义: `struct mmc`(vendor `mmc.h`)与
 * `struct sdhci_host`(vendor `sdhci.h`)就是它。旧改写副本里的 `br_mmc_t`/`br_sdhci_t`
 * 抽象**整层删除** —— 那正是 ADR-0026 要消灭的"上游派生件"。
 *
 * ============ ★ 中断驱动怎么在"上游原样编译"下成立(ADR-0021 D-4 的形态) ============
 * 上游 `sdhci.c` 是**轮询**完成的: 它只通过 `get_timer()` / `udelay()` 让出 CPU(见任务书
 * §2.4 的两条循环)。`compat/mmc_env.h` 把这两个函数实现成"让出 CPU 直到'下一 ms 或**设备
 * 中断**'", 于是上游的循环自动变成中断驱动的等待。配套的三段式(本文件 + 环境层):
 *
 *   { ISR }  读 `INT_STATUS`(**只读, 绝不清** —— 上游的等待循环正是靠读它判完成),
 *            把 `SIGNAL_ENABLE` 写 0 **去断言电平中断线**, `br_work_submit()` 交下半部;
 *   { bh }   `br_sem_give()` 唤醒等待者;
 *   { 环境等待原语 }  睡下**之前**重新打开本卡的 `SIGNAL_ENABLE`(见 `emmc_env_wait`),
 *            于是每一轮都还能被下一次中断唤醒。
 *
 * ★ 为什么 ISR 不能清 `INT_STATUS`: QEMU 的 SDHCI 是**电平**中断, 断言条件 =
 *   `INT_STATUS & SIGNAL_ENABLE`(`INT_ENABLE` 是上游 `sdhci_init()` 已经打开的"源使能")。
 *   上游要在自己的循环里读到那些位并**自己**清, 我们清了它就读不到; 而去断言电平线的
 *   正统做法是掩掉信号源 —— 这正是 `SIGNAL_ENABLE` 的用途。掩掉之后由等待原语在睡下前
 *   重开, 于是"一次中断 → 一次唤醒"是自洽的(重开时电平线若仍有效会立刻再中断一次,
 *   而等待者已经先读过 `INT_STATUS`, 不会丢事件)。
 *
 * ★ 环境层怎么知道"现在等的是哪块卡": 上游的等待循环没有设备参数。`compat/mmc_env.h` 的
 *   MMIO 访问器把**刚被访问的地址**告诉 `uboot_mmc_env_reg_touched()`, 本文件用它反查
 *   "这是哪个已注册的 SDHCI 窗口"。⇒ 四块卡各有自己的信号量与中断通道(**多实例**,
 *   ADR-0022), 而不是一个全局单例。
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>
#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>
#include <br/pci/br_pci.h>

#include "emmc_internal.h"

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物 `plugin_desc.c` 引用) */
int uboot_mmc_early_init(void);
int uboot_mmc_init(void);
int uboot_mmc_start(void);

/* QEMU `-device sdhci-pci` 的身份(与 framework/pci-core 的自检同源; 实测取得) */
#define BR_EMMC_PCI_VENDOR  0x1b36u
#define BR_EMMC_PCI_DEVICE  0x0007u

/* 我们要让 SDHCI **信号**哪些中断: 与上游 `sdhci_init()` 写给 `INT_ENABLE` 的集合一致
 * (`SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK`)—— 命令响应/数据就绪/数据结束/各项错误。
 * `INT_STATUS & SIGNAL_ENABLE` 是电平线的断言条件(见文件头)。 */
#define BR_EMMC_INT_OURS  (SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK)

/* SDHCI 寄存器窗口的大小上界(BAR0 的最小合法尺寸, 我们只反查这个范围内的地址)。 */
#define BR_EMMC_BAR_WINDOW  0x100u

/* ==================================================================== 实例表 */

/*
 * 一块卡的运行期状态。**没有全局单例**(ADR-0022): 四块卡各有自己的上游主机实例
 * (`struct sdhci_host`)、协议核心(`struct mmc *`, 上游 `mmc_create` 分配)、几何、中断通道
 * 与设备名; `bdev ops` 的 `priv` 就指向它。
 */
typedef struct emmc_dev {
    struct sdhci_host  host;        /* ★ 上游主机状态(寄存器窗口/flags/adma 表) */
    struct mmc        *mmc;         /* ★ 上游协议核心(上游 mmc_create 出来的) */
    br_bdev_geom_t     geom;        /* n_sectors/max_rw 在 start 里填; flags 每次现算 */
    br_uintptr_t       base;        /* SDHCI 寄存器窗口(BAR0), 供 GET_INFO 回答 */
    br_u32             path;        /* BR_MMC_PATH_* (落地 = host.flags 的 USE_ADMA 位) */
    br_u32             intx_virq;
    br_bool            chan_ready;  /* 中断通道已注册并使能(之前只能靠 tick 让出) */
    br_sem_t           sem;         /* ISR → bh → 等待者 的唤醒通道 */
    volatile br_u32    pending;     /* ISR 累积的 INT_STATUS 快照(诊断用) */
    volatile br_u32    isr_count;   /* 进 ISR 的次数(可观测性判据, 见 emmc_internal.h) */
    volatile br_u32    bh_count;    /* bh 里 give 的次数 */
    volatile br_u32    bh_drop;     /* br_work_submit 队列满 ⇒ 改在 ISR 里直接唤醒 */
    volatile br_u32    wake_irq;    /* 等待被设备中断唤醒的次数 */
    volatile br_u32    wake_sem;    /* 进 take 时令牌已在(ISR/bh 已给)⇒ 立即返回, 没睡 */
    volatile br_u32    wake_to;     /* 真的睡满窗口且期间无 ISR 的次数(这才是"兜底") */
    volatile br_u32    wake_spin;   /* `udelay()`(忙等)的调用次数 —— 这条路不睡, 不计入上面三类 */
    char               name[BR_EMMC_NAME_MAX];
} emmc_dev_t;

static emmc_dev_t s_dev[BR_EMMC_MAX_DEVS];
static br_u32     s_dev_n;          /* **在册实例数**(探测期只增不减; 见 emmc_bring_up) */

/* ==================================================================== 中断通道 */

static void emmc_signal_enable(emmc_dev_t *d)
{
    sdhci_writel(&d->host, (u32)BR_EMMC_INT_OURS, SDHCI_SIGNAL_ENABLE);
}

/* 下半部: 只做"唤醒"(bh 契约: 关中断、不可阻塞, 见 br_work.h)。 */
static void emmc_irq_bh(void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)arg;

    d->bh_count++;
    (void)br_sem_give(&d->sem);
}

static void emmc_isr(void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)arg;

    if (d == BR_NULL) {
        return;
    }

    const u32 st = sdhci_readl(&d->host, SDHCI_INT_STATUS);

    /*
     * ★ 只读 `INT_STATUS`, **不清** —— 上游的等待循环靠读它判完成; 去断言电平线靠把
     *   `SIGNAL_ENABLE` 写 0(下一次睡下前由 `emmc_env_wait` 重开)。
     */
    sdhci_writel(&d->host, 0u, SDHCI_SIGNAL_ENABLE);

    d->pending |= st;
    d->isr_count++;

    if (br_work_submit(emmc_irq_bh, d) != 0) {
        /* IR-10: 这次提交是"中断事件"的唯一载体 ⇒ 交不出下半部就必须自己把它交给等待者,
         * 否则事件静默(等待者只能靠 tick 超时兜底)。 */
        d->bh_drop++;
        (void)br_sem_give(&d->sem);
    }
}

/* ==================================================================== 运行环境钩子
 * 这三个函数是 `compat/mmc_env.h` **声明、本文件兑现**的(u-boot 侧没有对应物):
 *   - memalign: 上游 `sdhci_adma_init()` 要一张 DMA 安全的 ADMA2 描述符表;
 *   - reg_touched: 记录刚被访问的 MMIO 地址(供下面反查"等的是哪块卡");
 *   - wait: 让出 CPU 到"下一 ms 或当前设备的设备中断"(上游轮询 → 中断驱动的核心)。
 * 依赖方向因此是 `compat`(声明) ← `glue`(实现), 与 `malloc` → core 同款。
 */

/* ---- memalign: 上游的 DMA 对齐分配 ------------------------------------------------
 * u-boot 的 `memalign.h` 契约是"返回至少 align 对齐、可用于 DMA 的缓冲"。
 * ★ 为什么接到 `br_dma_alloc`(DMA 池)而不是 `br_malloc`: ADMA2 描述符表要**给设备读**,
 *   必须物理连续且属性正确(见 ADR-0003 的 region 表: DMA 池在 CACHED 背景下,
 *   `sdhci_prepare_adma_table()` 末尾的 `flush_cache()` 因此是真的 cache 维护)。
 * 上游只在一处调它(`sdhci_adma_init`, 每卡一次, 8 字节), 所以用一张小的静态登记表;
 * 没有"释放"语义(表随卡的寿命, 永不释放 —— 与上游一致)。 */
#define BR_EMMC_ALIGN_SLOTS  8u

static br_dma_buf_t s_align_slot[BR_EMMC_ALIGN_SLOTS];
static br_u32       s_align_n;

void *uboot_mmc_env_memalign(br_size_t align, br_size_t size)
{
    if (size == 0u || s_align_n >= BR_EMMC_ALIGN_SLOTS) {
        return BR_NULL;
    }

    /* 至少一个 cache 行: 上游会对 `ROUND(表长, ARCH_DMA_MINALIGN)` 做 flush,
     * 让分配块覆盖那个范围(免得 flush 踩到相邻分配)。 */
    br_size_t need = (size < (br_size_t)ARCH_DMA_MINALIGN) ? (br_size_t)ARCH_DMA_MINALIGN : size;

    br_dma_buf_t b;
    if (br_dma_alloc(need, BR_DMA_F_ALIGN4K, &b) != 0) {
        return BR_NULL;
    }

    /* `br_dma_alloc` 给 4 KiB 对齐 ≫ 上游会请求的 ARCH_DMA_MINALIGN(64)。 */
    const br_uintptr_t want = (align == 0u) ? 1u : (br_uintptr_t)align;
    if (((br_uintptr_t)b.vaddr % want) != 0u) {
        (void)br_dma_free(&b);
        return BR_NULL;
    }

    s_align_slot[s_align_n] = b;
    s_align_n++;
    return b.vaddr;
}

/* ---- MMIO 地址 → 实例 反查 ------------------------------------------------------ */
static br_uintptr_t s_last_reg;

void uboot_mmc_env_reg_touched(const volatile void *reg)
{
    s_last_reg = (br_uintptr_t)reg;
}

static emmc_dev_t *emmc_dev_for_reg(br_uintptr_t addr)
{
    for (br_u32 i = 0u; i < s_dev_n; i++) {
        const br_uintptr_t b = s_dev[i].base;
        if ((b != 0u) && (addr >= b) && (addr < (b + BR_EMMC_BAR_WINDOW))) {
            return &s_dev[i];
        }
    }
    return BR_NULL;
}

/* ---- wait: "让出 CPU 到下一 ms 或设备中断" ---------------------------------------
 *
 * ★ 这是本刀的核心手法(任务书 §2.4/§2.5): 上游的 `get_timer()`/`udelay()` 都落到这里,
 *   于是它的轮询循环变成"睡下 → 设备中断 → 被 bh 唤醒 → 再读一次 INT_STATUS"。
 *
 * 三条分支:
 *   ① 地址不属于任何 SDHCI 窗口(例如 platform 早于本件用时间设施)或调度器未就位
 *      ⇒ 退回"让出到一拍/忙等一拍"; 环境层因此**永远可用**。
 *   ② 实例在册但中断通道还没注册好(探测早期) ⇒ 同上, tick 兜底。
 *   ③ 正常路径 ⇒ **先重开本卡的 `SIGNAL_ENABLE`**(ISR 用它去断言电平线), 再睡;
 *      睡的上界是一拍(`BR_CFG_TICK_PERIOD_US`), 所以**不会因为丢一次中断就永远睡下去**
 *      —— 最坏情况退化成"每拍重读一次 INT_STATUS", 正确性不依赖中断, 但快路径由中断驱动。
 */
/* ---- spin: **忙等**(u-boot 的 `udelay` 语义) -------------------------------------
 *
 * 上游只拿 `udelay()` 等"寄存器稳定/复位完成"(10/100/1000/2000 µs, 实测穷举过上游的取值),
 * 那些循环没有设备事件可等; 而调度器的睡眠粒度是一拍(5 ms) ⇒ 接到睡眠路径会把 µs 级自旋
 * 放大成 5 ms(bring-up 0.037 s → 0.35 s, 并把整段自检推过 `irq-test` 的 6 s 上限)。
 * 所以这里**不让出 CPU**; 计数进 `wake_spin` 以便日志里能看出"这条路走了多少次"。
 */
void uboot_mmc_env_spin(br_u32 us)
{
    emmc_dev_t *d = emmc_dev_for_reg(s_last_reg);

    br_delay_us((br_time_t)((us != 0u) ? us : 1u));
    if (d != BR_NULL) {
        d->wake_spin++;
    }
}

void uboot_mmc_env_wait(br_u32 max_us)
{
    emmc_dev_t *d = emmc_dev_for_reg(s_last_reg);
    const br_u32 win = (max_us != 0u) ? max_us : UBOOT_MMC_WAIT_MAX_US;

    /* 分支①②: 地址不属于任何 SDHCI 窗口 / 中断通道尚未注册 / 调度器未就位
     * ⇒ 退回"按调用方要的时长让出一次"(环境层永远可用; 见 emmc_internal.h 的窗口常数说明)。 */
    if ((d == BR_NULL) || (d->chan_ready != BR_TRUE) || (br_task_self() == BR_NULL)) {
        if (br_task_self() != BR_NULL) {
            (void)br_task_sleep((br_time_t)win);
        } else {
            br_delay_us((br_time_t)win);
        }
        return;
    }

    const br_u32 isr_before = d->isr_count;
    const br_time_t t0 = br_clock_now();

    /*
     * ★★ 睡下之前打开信号源 —— 这一行就是"中断驱动"的来源, 不能省。
     *
     * 三条事实决定了这个写法(全部有实测依据):
     *
     *   ① 电平中断的断言条件 = `INT_STATUS & SIGNAL_ENABLE` ⇒ 要"能被下一次中断唤醒",
     *      睡下之前必须把信号源打开;
     *   ② 我们**不能**清 `INT_STATUS`(上游的等待循环正是靠读它判完成);
     *   ③ 我们等的那个位(DATA_END / DATA_AVAIL / SPACE_AVAIL…)常常**已经**挂在
     *      `INT_STATUS` 里 —— 也就是"信号源打开后线会立刻断言, 我们随即被唤醒"。
     *
     * ★ 试过但**否决**的两种写法(都留下实测数字, 免得下次再走一遍):
     *   (a) 醒来后再掩 `SIGNAL_ENABLE = 0`: 设备在"上游正跑代码"窗口里产生的事件没有任何
     *       东西在听 ⇒ 下一次等待只能靠 1 ms 兜底发现 ⇒ 整段自检 2.6 s → 26~29 s,
     *       `wake irq/timeout = 0/N`(**中断驱动在日志里失去证据**), 并贴着门禁 30 s 上限。
     *   (b) 只打开"还没挂在 `INT_STATUS` 里"的源(`OURS & ~pending`): 看似聪明, 实则把
     *       我们要等的那个位正好掩掉了(见 ③)⇒ 同样退化成 0/N 与 20 s+。
     *   ⇒ 唯一正确的形态是**全开**(`SIGNAL_ENABLE = BR_EMMC_INT_OURS`), 醒来后不掩。
     *
     * 那"没有线程在等时线会不会一直被断言"? —— 断言条件仍由硬件决定, 而上游
     * `sdhci_send_command()` 在**每次命令结束时**都
     * `sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS)` 清干净 ⇒ 命令边界处线是
     * 平的。且**打开信号的唯一位置就是这里**(bring-up 结束时**不**打开)⇒ 非传输窗口里
     * 信号源保持上游 `sdhci_init()` 留下的 0。判据是**线**而不是信号源, 见
     * `uboot_mmc_irq_idle()`(自检每卡打印 `idle-signal=flat|ASSERTED`)。
     */
    emmc_signal_enable(d);

    (void)br_sem_take(&d->sem, (br_time_t)win);

    /*
     * 记账(见 emmc_internal.h)。★ 三种返回, 必须分开数 —— 第一版只按"本次调用期间有没有
     * ISR"二分, 于是把下面第 ② 种误记成"兜底超时", 读日志的人会以为中断驱动没生效:
     *
     *   ① `wake_irq`  —— 睡下之后被设备中断唤醒(本次窗口内 `isr_count` 增长)。**最强证据**。
     *   ② `wake_sem`  —— 进 `br_sem_take` 时**令牌已经在**(ISR/bh 在上一轮就把 give 记在
     *                    `sem.count` 上了)⇒ 立即返回、根本没睡。这**同样是中断投递的证据**
     *                    (事件由 ISR/bh 捕获并给了令牌), 只是被下一次调用消费掉。
     *                    判据 = 实际耗时远小于窗口(取窗口的一半)。
     *   ③ `wake_to`   —— 真的睡满了窗口、期间一个 ISR 都没有 ⇒ 这才是**兜底**。
     *
     * 为什么会大量出现 ②: 上游的等待循环在**一次** `uboot_mmc_env_wait()` 之后往往要读好几
     * 轮寄存器才决定再等一次; 而 ISR 已经把令牌放好了 ⇒ 下一轮的 take 直接命中。若把 ② 当
     * 超时读, 就会得出"等待完全靠 1 ms 兜底"的错误结论(实测踩到过, 见交付报告 §5)。
     */
    if (d->isr_count != isr_before) {
        d->wake_irq++;
    } else if ((br_clock_now() - t0) < ((br_time_t)win / 2u)) {
        d->wake_sem++;
    } else {
        d->wake_to++;
    }
}

/* ---- 上游要的"板级"弱钩子 -------------------------------------------------------
 * u-boot 的 `mmc_initialize()` 会调 `board_mmc_init()` → 失败再 `cpu_mmc_init()`。
 * 本件的探测由 glue 自己做(PCIe 枚举), **不**走 `mmc_initialize()`(只调
 * `mmc_list_init()`); 这两个符号是给"上游那份 `mmc_probe()` 万一被链进来"兜底,
 * 语义与 u-boot 的缺省一致(返回 -1 = 本板没有静态 mmc 列表)。 */
__weak int board_mmc_init(struct bd_info *bis)
{
    (void)bis;
    return -1;
}

__weak int cpu_mmc_init(struct bd_info *bis)
{
    (void)bis;
    return -1;
}

/* ==================================================================== 上游块层挂点
 * 上游 `mmc.c`/`mmc_write.c` 会在块路径上调用它们(`blk.h`/`part.h` 的接口面)。
 * 语义**逐条对齐上游** `drivers/mmc/mmc_legacy.c` 的 `mmc_select_hwpartp()`:
 * 上游把它注册进 `U_BOOT_LEGACY_BLK(mmc)` 的驱动表, 而本件没有那套段枚举
 * (见 `compat/blk.h` 的说明)⇒ 由 glue 直接兑现同名行为。
 */
int blk_dselect_hwpart(struct blk_desc *desc, int hwpart)
{
    if (desc == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    struct mmc *mmc = find_mmc_device(desc->devnum);
    if (mmc == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    if ((int)mmc->block_dev.hwpart == hwpart) {
        return 0;                       /* 已经是该硬件分区(本件的常态: 恒 0) */
    }
    if (mmc->part_config == MMCPART_NOAVAILABLE) {
        return BR_ERR(EMEDIUMTYPE);
    }
    return mmc_switch_part(mmc, (unsigned int)hwpart);
}

int blk_select_hwpart_devnum(enum uclass_id uclass_id, int devnum, int hwpart)
{
    if (uclass_id != UCLASS_MMC) {
        return BR_ERR(ENODEV);
    }

    struct mmc *mmc = find_mmc_device(devnum);
    if (mmc == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    return blk_dselect_hwpart(&mmc->block_dev, hwpart);
}

/*
 * `part_init()`: 上游 `mmc_startup()` 末尾在 `!DM_MMC` 时会调它(扫描 MBR/GPT 并建
 * 软件分区表)。
 * ★ 本件**有意不做软件分区**(v1 暴露裸设备: bdev 的几何就是整卡; 分区映射是 SD-9/v1.x
 *   的面)。这里给一个**显式的空实现**, 而不是把上游那一行删掉 —— 删上游那一行才是
 *   "改上游"; 给一个语义明确的环境实现是"适配"。
 */
void part_init(struct blk_desc *desc)
{
    (void)desc;
}

/*
 * ★ **本件不定义** `blk_dread` / `blk_dwrite`(合并两件三方插件时踩到的实测问题:
 *   重复符号 —— 见下)。
 *
 *   它们是 u-boot 的**块层接口**, 在本组合里**唯一的调用方**是 `adapter/uboot-fat` 编译进来的
 *   上游 `fs/fat/fat.c`/`fat_write.c`(`disk_read/disk_write` → `blk_dread/blk_dwrite`),
 *   于是定义归它的 glue(`adapter/uboot-fat/glue/fat_blk.c`)。本插件的上游(`mmc.c`)只把
 *   `mmc_bread/mmc_bwrite` 挂进 `blk_desc` 的函数指针,**一个 `blk_dread` 调用点都没有**
 *   (非 BLK 形态的上游就是这么设计的) —— 定义在这里是死代码, 而且与 fat 的那份撞名。
 *
 *   实测症状(本刀合并 prototype-mmc 后第一次整镜像链接):
 *     ld: emmc_bdev.o: multiple definition of `blk_dread'; fat_blk.o: first defined here
 *   ⇒ 处置 = 删掉本件这两份定义(而不是把 fat 的那份改弱): "谁的上游调用它, 谁来定义"。
 *   `compat/part.h` 仍**声明**它们(那是上游头文件的接口面, 删声明才是改上游);
 *   本插件的编译单元不引用 ⇒ 不产生未定义符号。
 */

/* ==================================================================== bdev ops */

static int emmc_read(void *priv, br_u64 lba, void *buf, br_size_t n_lba)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;

    if ((d == BR_NULL) || (buf == BR_NULL) || (n_lba == 0u)) {
        return BR_ERR(EINVAL);
    }

    struct blk_desc *bdesc = mmc_get_blk_desc(d->mmc);
    if (bdesc == BR_NULL) {
        return BR_ERR(ENODEV);
    }

    /*
     * ★ 越界守门在本层: 上游 `mmc_bread()` 对越界是**返回 0**(它按 u-boot 的块层口径
     * "0 = 出错"), 而本项目的 bdev 契约是"成功 = 传输的扇区数 / 失败 = 负 errno"
     * (SD-10)⇒ 0 必须翻译成 `-EIO`。这也正是 TC-EMMC-004 的判据("越界不落卡": 一条
     * 命令都不发给卡)。
     */
    if ((lba >= (br_u64)bdesc->lba) || ((lba + (br_u64)n_lba) > (br_u64)bdesc->lba)) {
        return BR_ERR(EIO);
    }

    const ulong got = mmc_bread(bdesc, (lbaint_t)lba, (lbaint_t)n_lba, buf);
    if (got != (ulong)n_lba) {
        return BR_ERR(EIO);
    }
    return (int)n_lba;
}

static int emmc_write(void *priv, br_u64 lba, const void *buf, br_size_t n_lba)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;

    if ((d == BR_NULL) || (buf == BR_NULL) || (n_lba == 0u)) {
        return BR_ERR(EINVAL);
    }

    struct blk_desc *bdesc = mmc_get_blk_desc(d->mmc);
    if (bdesc == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    if ((lba >= (br_u64)bdesc->lba) || ((lba + (br_u64)n_lba) > (br_u64)bdesc->lba)) {
        return BR_ERR(EIO);
    }

    const ulong got = mmc_bwrite(bdesc, (lbaint_t)lba, (lbaint_t)n_lba, buf);
    if (got != (ulong)n_lba) {
        return BR_ERR(EIO);
    }
    return (int)n_lba;
}

static int emmc_flush(void *priv)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /*
     * WORKAROUND(br-wa-emmc-001): v1 的 flush 只做"等卡不忙"。
     * 写命令返回前已经轮询过状态, 所以语义上"写到卡内部的块已落介质"; 但**没有**发
     * EXT_CSD CACHE_FLUSH(CMD6 SET_BIT[32]), 也没有判定 CACHE_CTRL 是否使能。
     * 设计与还债动作见 `WORKAROUNDS.md` 的 `br-wa-emmc-001`(ADR-0021 D-5)。
     *
     * ★ ADR-0026 之后这一条仍由本件承载: 上游没有 "flush" 这个入口, 所以"等卡不忙"
     *   直接用上游导出的 `mmc_poll_for_busy()`(它的 `mmc_wait_dat0` 在本件是 -ENOSYS
     *   ⇒ 走 SEND_STATUS 轮询那一支, 与旧改写副本同口径)。
     */
    const int rc = mmc_poll_for_busy(d->mmc, (int)BR_EMMC_CMD6_TIMEOUT_MS);
    return (rc == 0) ? 0 : rc;
}

static int emmc_geometry(void *priv, br_bdev_geom_t *g)
{
    const emmc_dev_t *d = (const emmc_dev_t *)priv;
    if ((d == BR_NULL) || (g == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    br_bdev_geom_t x = d->geom;
    x.flags = (d->path == BR_MMC_PATH_PIO) ? BR_BDEV_F_PIO : BR_BDEV_F_DMA;
    *g = x;
    return 0;
}

/* 运行期 SET_PATH: ★ 落地就是改上游 `host.flags` 的 `USE_ADMA` 位(任务书 §2.6)。
 *   `sdhci_send_command()` 里 `if (host->flags & USE_DMA)` 决定是否走
 *   `sdhci_prepare_dma()` + `SDHCI_TRNS_DMA`; 清掉 `USE_ADMA` 就自然落 PIO
 *   (`sdhci_transfer_data()` 的 PIO 分支)。**没有**额外的"阈值/对齐兜底"逻辑 ——
 *   通路由运行期标志唯一决定(旧改写副本的 `BR_SDHCI_PIO_THRESHOLD_SECTORS` 因此取消;
 *   见 README 的偏离一节)。 */
static void emmc_set_path(emmc_dev_t *d, br_u32 path)
{
    if (path == BR_MMC_PATH_PIO) {
        d->host.flags &= ~(int)USE_ADMA;
    } else {
        d->host.flags |= (int)USE_ADMA;
    }
    d->path = path;
}

static int emmc_ioctl(void *priv, br_u32 cmd, void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    if (BR_IOC_TYPE(cmd) != (br_u32)BR_EMMC_IOC_MAGIC) {
        return BR_ERR(ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_EMMC_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }
        br_emmc_info_t *info = (br_emmc_info_t *)arg;
        info->base             = d->base;
        info->capacity_sectors = (br_u64)d->geom.n_sectors;
        info->sector_size      = d->geom.sector_size;
        info->max_rw           = d->geom.max_rw;
        info->bus_width        = d->mmc->bus_width;
        info->clock_hz         = d->mmc->clock;
        info->path             = d->path;
        info->card_type        = d->mmc->cardtype;
        return 0;
    }
    case BR_EMMC_IOC_SET_PATH: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }
        const br_u32 path = *(const br_u32 *)arg;
        if ((path != BR_MMC_PATH_DMA) && (path != BR_MMC_PATH_PIO)) {
            return BR_ERR(EINVAL);
        }
        emmc_set_path(d, path);
        br_log_info("uboot-mmc: %s 数据通路切到 %s", d->name,
                    (path == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
        return 0;
    }
    default:
        return BR_ERR(ENOTSUP);
    }
}

static int emmc_suspend(void *priv) { (void)priv; return BR_ERR(ENOTSUP); }   /* D22 预留 */
static int emmc_resume (void *priv) { (void)priv; return BR_ERR(ENOTSUP); }

static const br_bdev_ops_t s_emmc_ops = {
    .read     = emmc_read,
    .write    = emmc_write,
    .flush    = emmc_flush,
    .geometry = emmc_geometry,
    .ioctl    = emmc_ioctl,
    .suspend  = emmc_suspend,
    .resume   = emmc_resume,
};

/* ==================================================================== 观测面 */

void uboot_mmc_irq_counts(br_u32 idx, br_u32 *irq, br_u32 *bh, br_u32 *drop,
                          br_u32 *wake_irq, br_u32 *wake_sem, br_u32 *wake_to)
{
    const br_bool bad = (idx >= BR_EMMC_MAX_DEVS) ? BR_TRUE : BR_FALSE;

    if (irq      != BR_NULL) { *irq      = bad ? 0u : s_dev[idx].isr_count; }
    if (bh       != BR_NULL) { *bh       = bad ? 0u : s_dev[idx].bh_count; }
    if (drop     != BR_NULL) { *drop     = bad ? 0u : s_dev[idx].bh_drop; }
    if (wake_irq != BR_NULL) { *wake_irq = bad ? 0u : s_dev[idx].wake_irq; }
    if (wake_sem != BR_NULL) { *wake_sem = bad ? 0u : s_dev[idx].wake_sem; }
    if (wake_to  != BR_NULL) { *wake_to  = bad ? 0u : s_dev[idx].wake_to; }
}

br_u32 uboot_mmc_wake_spin(br_u32 idx)
{
    return (idx >= BR_EMMC_MAX_DEVS) ? 0u : s_dev[idx].wake_spin;
}

br_bool uboot_mmc_irq_idle(br_u32 idx)
{
    if (idx >= BR_EMMC_MAX_DEVS) {
        return BR_FALSE;
    }
    /*
     * 不变量: **没有线程在等 ⇒ INTx 线未断言**。断言条件就是 SDHCI 的硬件定义
     * (`INT_STATUS & SIGNAL_ENABLE != 0`, 电平型)⇒ 这一条直接量它, 而不是量"信号源是否为 0"
     * (信号源在上游命令结束清掉 `INT_STATUS` 之后即使还开着, 线也是平的; 见环境等待原语的说明)。
     */
    const u32 st = sdhci_readl(&s_dev[idx].host, SDHCI_INT_STATUS);
    const u32 se = sdhci_readl(&s_dev[idx].host, SDHCI_SIGNAL_ENABLE);

    return ((st & se) == 0u) ? BR_TRUE : BR_FALSE;
}

/* ==================================================================== 生命周期 */

int uboot_mmc_early_init(void)
{
    return 0;   /* 此刻无关中断/无堆; 且寄存器窗口要等 pci-core 的 CORE 枚举 */
}

int uboot_mmc_init(void)
{
    return 0;   /* 见文件头与 README: 真正的初始化在 start(全局中断已开) */
}

/* 把设备序号拼成 `emmc<d>`(无 snprintf, 逐个字符填; 只可能是 0..9)。 */
static void emmc_make_name(char *out, br_u32 idx)
{
    out[0] = 'e';
    out[1] = 'm';
    out[2] = 'm';
    out[3] = 'c';
    out[4] = (char)('0' + (char)(idx % 10u));
    out[5] = '\0';
}

/* 探测并注册**一块**卡。任何一步失败都返回负 errno(首败即停机是刻意的)。 */
static int emmc_bring_up(const br_pci_dev_t *pci, emmc_dev_t *d)
{
    if ((pci->bar[0] == 0u) || (pci->bar_size[0] < BR_EMMC_BAR_WINDOW)) {
        br_log_error("uboot-mmc: %s 的 sdhci BAR0 未分配或过小(bar0=0x%lx size=%u)",
                     d->name, (unsigned long)pci->bar[0], (unsigned)pci->bar_size[0]);
        return BR_ERR(ENODEV);
    }
    if (pci->intx_virq == 0u) {
        br_log_error("uboot-mmc: %s 没有 INTx virq", d->name);
        return BR_ERR(ENODEV);
    }

    d->base      = pci->bar[0];
    d->intx_virq = pci->intx_virq;
    d->path      = BR_MMC_PATH_DMA;

    /*
     * ① 上游主机实例的静态接口字段。`add_sdhci()`(上游)会补上别的
     *    (`version`/`max_clk`/`clk_mul`/`flags`/`cfg`)—— 那是上游 `sdhci_setup_cfg()`
     *    的职责, 我们不重写它。
     */
    d->host.name    = d->name;          /* 上游把它当 cfg->name(诊断用) */
    d->host.ioaddr  = (void *)d->base;
    d->host.mmc     = BR_NULL;
    d->host.ops     = BR_NULL;          /* 本件不用 SDHCI 的厂商 ops 表 */
    d->host.quirks  = 0u;               /* 卡级 quirks 由 CONFIG_MMC_QUIRKS 在 mmc.c 里置 */

    /*
     * ② 中断通道**先于**上游初始化建立: 上游的等待循环从第一条命令起就要能被唤醒
     *    (`sdhci_init()` 会把 SIGNAL_ENABLE 写 0, 而我们的等待原语每次睡下前重开它)。
     */
    int rc = br_sem_init(&d->sem, 0u);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s 信号量初始化失败 rc=%d", d->name, rc);
        return rc;
    }

    const br_irq_attr_t attr = {
        .prio    = BR_IRQ_PRIO_DEFAULT,
        .trigger = BR_IRQ_TRIG_DEFAULT,
        .flags   = 0u,                  /* INLINE 分发: ISR 在中断上下文, 由它交 bh */
    };
    rc = br_irq_register(d->intx_virq, emmc_isr, d, &attr);
    if (rc != 0) {
        br_log_error("uboot-mmc: br_irq_register(virq=%u) 失败 rc=%d",
                     (unsigned)d->intx_virq, rc);
        return rc;
    }
    rc = br_irq_enable(d->intx_virq);
    if (rc != 0) {
        br_log_error("uboot-mmc: br_irq_enable(virq=%u) 失败 rc=%d",
                     (unsigned)d->intx_virq, rc);
        return rc;
    }
    d->chan_ready = BR_TRUE;

    /*
     * ③ 上游的"主机 → mmc 设备"装配。这正是上游 `add_sdhci()` 的职责:
     *    `sdhci_setup_cfg()`(caps/时钟/电压/host_caps/b_max/ADMA2 描述符表) +
     *    `mmc_create()`(分配 struct mmc、登记块设备号 0/1/2/3)。
     *    ★ 我们一行都不重写 —— 这就是 ADR-0026 的落地形态。
     */
    rc = add_sdhci(&d->host, 0u, 0u);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s add_sdhci 失败 rc=%d", d->name, rc);
        return rc;
    }
    d->mmc = d->host.mmc;
    if (d->mmc == BR_NULL) {
        br_log_error("uboot-mmc: %s mmc_create 没有交回设备", d->name);
        return BR_ERR(ENOMEM);
    }

    /*
     * ★ 这里**不开** `SIGNAL_ENABLE`(这是本文件最容易被写错的一处, 实测踩过):
     *   电平中断的断言条件 = `INT_STATUS & SIGNAL_ENABLE`, 而 `INT_STATUS` 里常有上游
     *   "读到了但还没清"的位(RESPONSE 等)。只要在没有线程等待时把它打开, 这条线就会在
     *   **整个非传输期间持续投递** —— 而 core 的 IRQCONF 自检正是在那段窗口里用 bh 上下文
     *   做 thread-only API 的执法用例, 两条路径交叉就会炸出
     *   `[sched] br_task_exit(): 不许在中断下半部(bh)里结束线程`。
     *   ⇒ **不变量: 任何时刻只要没有线程在等, `SIGNAL_ENABLE` 就是 0。**
     *     打开它的**唯一**位置是 `uboot_mmc_env_wait()` 睡下之前那一处, 由 ISR 负责掩掉。
     */

    br_log_info("uboot-mmc: SDHCI @ 0x%lx (caps=0x%08x, max_clk=%u Hz, intx virq=%u)",
                (unsigned long)d->base, (unsigned)sdhci_readl(&d->host, SDHCI_CAPABILITIES),
                (unsigned)d->host.max_clk, (unsigned)d->intx_virq);

    /*
     * ④ 上游协议核心启动: CMD0/CMD1/CMD2/CMD3/CMD9/CMD7 + EXT_CSD + 总线宽度/时序切换。
     *    上游 `mmc_get_op_cond()` 会调 `cfg->ops->init`(= `sdhci_init()`), 那里面会把
     *    SIGNAL_ENABLE 清 0 ⇒ 之后再打开一次(之后由等待原语维持)。
     */
    rc = mmc_init(d->mmc);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s mmc_init 失败 rc=%d", d->name, rc);
        return rc;
    }
    /* ★ 上游 `sdhci_init()`(经 `mmc_get_op_cond()` → `cfg->ops->init`)会把
     *   `SIGNAL_ENABLE` 写 0 —— 正是我们要的不变量(没有线程在等 ⇒ 不投递)。**不再打开**。 */

    struct mmc      *mmc   = d->mmc;
    struct blk_desc *bdesc = mmc_get_blk_desc(mmc);
    if ((bdesc == BR_NULL) || (bdesc->lba == 0u) || (mmc->read_bl_len == 0u)) {
        return BR_ERR(EIO);           /* 容量读不出来 = 识别没完成, 不往下走 */
    }

    d->geom.sector_size = (br_u32)mmc->read_bl_len;
    d->geom.n_sectors   = (br_u64)bdesc->lba;
    d->geom.max_rw      = BR_EMMC_MAX_RW_SECTORS;
    d->geom.align       = 4u;         /* ADMA2 描述符地址要求字对齐(PIO 无对齐要求) */
    d->geom.flags       = BR_BDEV_F_DMA;

    /* ★ 门禁判据行(措辞与旧形态逐字一致; 值全部来自**上游**状态)。
     * ★ 模式名: 上游的 `mmc_mode_name()` 被它自己的 `#if CONFIG_LOGLEVEL >= LOGL_DEBUG`
     *   裁掉了(见 compat/mmc_env.h 的"口径区分"), 链接期**不存在** ⇒ 这里按上游
     *   `enum bus_mode` 的前几档给一个**最小映射**, 并附上游的枚举原值(判据不依赖它)。 */
    const char *mode_name;
    switch (mmc->selected_mode) {
    case MMC_LEGACY: mode_name = "legacy"; break;
    case MMC_HS:     mode_name = "HS";     break;
    case MMC_HS_52:  mode_name = "HS-52MHz"; break;
    case MMC_DDR_52: mode_name = "DDR52";  break;
    default:         mode_name = "other";  break;
    }
    br_log_info("uboot-mmc: eMMC ready (rca=%u, %lu sectors = %lu MiB, %u-bit, %s, %u Hz)",
                (unsigned)mmc->rca, (unsigned long)bdesc->lba,
                (unsigned long)(bdesc->lba / 2048u),
                (unsigned)mmc->bus_width, mode_name,
                (unsigned)mmc->clock);

    rc = br_bdev_register(d->name, &s_emmc_ops, d);
    if (rc != 0) {
        br_log_error("uboot-mmc: bdev 注册失败 rc=%d (name=%s)", rc, d->name);
        return rc;
    }

    br_log_info("uboot-mmc: /dev/%s 就绪 (%lu sectors, %u-bit, %u Hz, path=%s)",
                d->name, (unsigned long)d->geom.n_sectors,
                (unsigned)mmc->bus_width, (unsigned)mmc->clock,
                (d->path == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
    return 0;
}

int uboot_mmc_start(void)
{
    const br_u32 total = br_pci_dev_count();
    br_u32 found = 0u;

    /* 上游的设备链要初始化一次(它把 `cur_dev_num` 归 0 ⇒ 四块卡的 devnum = 0..3,
     * 正好对应 emmc0..emmc3; 见任务书 §2.2)。不走 `mmc_initialize()`: 那是 u-boot 的
     * "静态板级 mmc 列表"路径, 本件的设备来自 PCIe 枚举(见 board_mmc_init 的说明)。 */
    mmc_list_init();

    for (br_u32 i = 0u; i < total; i++) {
        const br_pci_dev_t *d = br_pci_dev_at(i);
        if (d == BR_NULL) {
            continue;
        }
        if ((d->vendor != BR_EMMC_PCI_VENDOR) || (d->device != BR_EMMC_PCI_DEVICE)) {
            continue;
        }
        if (s_dev_n >= BR_EMMC_MAX_DEVS) {
            br_log_error("uboot-mmc: 卡数超上限 %u(第 %lu 个 sdhci-pci 未被接管)",
                         (unsigned)BR_EMMC_MAX_DEVS, (unsigned long)(found + 1u));
            return BR_ERR(ENOSPC);
        }

        emmc_dev_t *dev = &s_dev[s_dev_n];
        emmc_make_name(dev->name, s_dev_n);     /* 名字先定: 失败日志里要能指名道姓 */
        s_dev_n++;                              /* 占位: 失败也不回收(启动期即停机) */

        const int rc = emmc_bring_up(d, dev);
        if (rc != 0) {
            return rc;
        }
        found++;
    }

    if (found == 0u) {
        br_log_error("uboot-mmc: 组合里有 pci-core, 但没找到 sdhci-pci (%04x:%04x)",
                     (unsigned)BR_EMMC_PCI_VENDOR, (unsigned)BR_EMMC_PCI_DEVICE);
        return BR_ERR(ENODEV);
    }

    br_log_info("uboot-mmc: 共接管 %lu 块 sdhci-pci 卡", (unsigned long)found);
    return 0;
}

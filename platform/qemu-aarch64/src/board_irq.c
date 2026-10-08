/*
 * brickOS prototype v0.1.0 — QEMU virt 板级中断数据 + 平台中断初始化 + arch timer 装弹
 *
 * 设计依据(权威): `docs/3-os-core/3-02-int.md`
 *   §3.3  绑定表(virq ↔ hwirq + 板级静态 prio/trigger)—— 这是 **platform 数据**
 *   §14.3 初始化单向链: 步 1–3 在 `br_plat_early_init()` 内, 且"任一步失败 = 启动失败,
 *         绝不静默降级"; 步 4–7(register/enable)属驱动/框架, 不在这里
 *   §8.2  电平型必须"设备侧清源", 否则线持续 assert ⇒ 活锁 —— timer 的 rearm
 *         就是 arch timer 的"清源 + 装下一个期限"
 *   §13.1 SGI/PPI/SPI 的 kind 与亲和性边界(见 board_irq.h 的 INTID 注释)
 *
 * ★ 层次: 本文件是**平台数据与特化**层(基址/绑定/频率换算)。控制器方言在 gicv3.c,
 *   语义与机制在 core(br_irq.c)。本文件里出现的地址是 QEMU virt 的事实, 不属 ISA 层。
 */
#include <br/board_irq.h>
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/platform/br_gicv3.h>
#include <br/platform/br_plat.h>

/* =====================================================================
 * 1. 板级 INTID(唯一真值的注释在 br/board_irq.h; 这里只是把数字命名)
 * ===================================================================== */
#define BR_BOARD_INTID_TIMER        30u  /* EL1 物理 timer(PPI; GIC INTID 30)   */
/* 心跳周期: 100 ms —— 1 秒里 ~10 次中断, 日志上一眼可见"中断真的在跑" */
#define BR_BOARD_TIMER_PERIOD_US    100000u
#define BR_BOARD_INTID_UART0        33u  /* PL011 UART0(SPI 1 ⇒ INTID 32+1=33)  */
#define BR_BOARD_INTID_SGI_TEST      0u  /* SGI 0: 软件触发自检(§8.4)           */
#define BR_BOARD_INTID_SGI_STORM     1u  /* SGI 1: 风暴用例载体                 */
#define BR_BOARD_INTID_SGI_DOMAIN    2u  /* SGI 2: 假 FAST 级联域的父线         */
/* 刻意不绑定 INTID 3(SGI 3): "无属主却有硬件投递"的样本(TC-IRQ-008) */

/*
 * 绑定表(编译进 .rodata; 设计 §3.3/§14.4)。
 * 逐条: virq 与 hwirq 的映射、静态默认 prio、静态触发方式、分发形态。
 *   - TIMER: INTID 30, **电平**(arch timer 的 ISTATUS/条件线), prio 0x80,
 *            PERCPU(PPI 每核私有; §13.1 —— 绝不能"均衡到别的核")
 *   - UART0: INTID 33, 电平(PL011 的 UARTINTR 是电平型), prio 0xA0; v0.1 只轮询
 *            console, 中断未使用 —— 绑定在表里用于验证 SPI 的 mask/prio/readback 路径
 *   - SGI:   恒**边沿**(架构固定), prio 0xC0, 软件触发通道
 *   - DOMAIN_PARENT: SGI 2, 边沿, 假 FAST 域的父线(域成员在 virq 56.. 窗口)
 * dispatch 全为 INLINE: Stage 1 只接受内联分发(3-02 §1.1/§12.1)。
 * pic_id = 0(EARLY 注册顺序里的第一个/唯一 PIC); dom_id = -1(直连, 不属任何域)。
 */
static const br_irq_binding_t s_board_irq[BR_IRQ_BOARD_NR] = {
    [BR_IRQ_TIMER] = {
        .virq     = BR_IRQ_TIMER,
        .pic_id   = 0u,
        .flags    = BR_IRQB_F_PERCPU,
        .hwirq    = BR_BOARD_INTID_TIMER,
        .dom_id   = -1,
        .prio     = 0x80u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_UART0] = {
        .virq     = BR_IRQ_UART0,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_UART0,
        .dom_id   = -1,
        .prio     = 0xA0u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_TEST_SGI] = {
        .virq     = BR_IRQ_TEST_SGI,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_TEST,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_TEST_SGI_STORM] = {
        .virq     = BR_IRQ_TEST_SGI_STORM,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_STORM,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_DOMAIN_PARENT] = {
        .virq     = BR_IRQ_DOMAIN_PARENT,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_DOMAIN,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
};

/* 表长必须与 br/board_irq.h 的 BR_IRQ_BOARD_NR 一致: 表不能静默漂移 */
_Static_assert((br_size_t)BR_ARRAY_SIZE(s_board_irq) == (br_size_t)BR_IRQ_BOARD_NR,
               "绑定表长度与 BR_IRQ_BOARD_NR 漂移");

/* =====================================================================
 * 2. 平台中断初始化(设计 §14.3 步 1–3; EARLY 相, 全局关中断)
 * ===================================================================== */
int br_plat_irq_init(void)
{
    /* QEMU virt 的 GICv3 布局(平台数据):
     *   GICD 0x0800_0000; GICR 0x080A_0000; 每核帧跨度 2 × 64 KiB = 0x2_0000。
     *   nr_irq = 1024 = INTID 上界(不用于分配表; §14.2 明说不用直接索引表)。 */
    static const br_gicv3_cfg_t cfg = {
        .gicd_base       = 0x08000000ul,
        .gicr_base       = 0x080A0000ul,
        .gicr_frame_size = 0x20000u,
        .gicr_max_frames = 8u,
        .nr_irq          = 1024u,
    };
    int pic_id;
    int rc;

    /* 步 1–2: 注册 PIC 实例(内部完成 GICv3 的 CPU 接口/分发器/重分发器初始化)。
     * 失败 = 启动失败, 负 errno 上抛(绝不"静默返回 0 但没配好", §14.3 义务)。 */
    pic_id = br_gicv3_register(&cfg);
    if (pic_id < 0) {
        return pic_id;
    }

    /* 步 3: 提交绑定表 —— 之后 virq↔hwirq 才可解析。
     * 重复提交由 core 报 -EBUSY(EARLY 恰一次)。 */
    rc = br_irq_bindings_set(s_board_irq, BR_ARRAY_SIZE(s_board_irq));
    if (rc != 0) {
        return rc;
    }
    return 0;
}

/* =====================================================================
 * 3. arch timer(EL1 物理 timer, PPI INTID 30)的装弹/清源
 *   (ISR 与"中断开跑"见 §4; 周期常量 BR_BOARD_TIMER_PERIOD_US)
 *
 * 为什么这算"设备侧清源"(§8.2): arch timer 的条件线是**电平**型 —— ISR 只
 * ack/eoi 而不重装 TVAL, 条件持续成立 ⇒ 立刻重新 pending ⇒ 活锁。
 * 所以 rearm(重写 CNTP_TVAL_EL0)不是一个可选优化, 而是 level 中断的义务。
 * ===================================================================== */
#define BR_CNTP_CTL_ENABLE  0x1u   /* CNTP_CTL_EL0.ENABLE(bit0); IMASK(bit1)=0 表示不屏蔽 */
#define BR_CNTP_CTL_DISABLE 0x0u   /* ENABLE=0 */

static void br_timer_write_tval(br_u64 ticks)
{
    __asm__ volatile("msr cntp_tval_el0, %0" ::"r"(ticks) : "memory");
}

static void br_timer_write_ctl(br_u64 ctl)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" ::"r"(ctl) : "memory");
}

/*
 * 微秒 → tick。**向上取整**: `((us * freq) + 999999) / 1000000`。
 * 向下取整会把期限缩短(比请求更早到期) —— 对 tickless 的"最早期限"语义是
 * 不安全方向(可能提前唤醒), 所以宁可多等不到 1 个 tick。
 * freq 来自 CNTFRQ_EL0(QEMU virt = 62.5 MHz), 由 br_plat_ticks_freq() 提供。
 */
static br_u64 br_timer_us_to_ticks(br_u64 period_us)
{
    br_u64 freq = br_plat_ticks_freq();
    return ((period_us * freq) + 999999u) / 1000000u;
}

static void br_timer_rearm(br_u64 period_us)
{
    /* ISR 内调用: 重装 TVAL = 清掉当前条件 + 排下一个期限(§8.2) */
    br_timer_write_tval(br_timer_us_to_ticks(period_us));
}

void br_plat_timer_stop(void)
{
    br_timer_write_ctl(BR_CNTP_CTL_DISABLE);
}

/* ---------------------------------------------------------------------
 * 4. "中断开跑": timer PPI 的 ISR + 使能 + 装弹 + 全局开中断
 *
 * ★ 为什么 ISR 在 **platform** 而不是 APP(P-IRQ-17, 权限模型决定):
 *   设计 3-01 §13.6 的特权分级把"**中断控制**"归 **P3**(仅
 *   `ability.subkind=scheduler` 与 `platform` 可声明), "IRQ 线"归 P1;
 *   而 APP 是 **P0**(只有任务/时间/锁/服务注册表/内存分配那些普通 native API)。
 *   ⇒ 让 app/hello 调 `br_irq_register` 会是一处**声明面与实现不一致**:
 *     它在 P0 的位置上做了 P3 的事, 而 `brickie check` 的 priv 域(声明合法性)
 *     看不到源码从而不会报红 —— 这类"工具看不见"的越权必须靠**放置**来避免。
 *   设计 3-02 §11.1 也把 timer PPI 的 ISR 归 core.init(核心自己注册);
 *   v0.1 没有 core.init, 由同属 P4 的 platform 代注册, 并记入 br-wa-boot-001。
 * ------------------------------------------------------------------- */
static volatile br_u32 s_timer_ticks;

static void br_plat_timer_isr(void *arg)
{
    (void)arg;
    s_timer_ticks++;
    br_timer_rearm(BR_BOARD_TIMER_PERIOD_US);
}

br_u32 br_plat_timer_ticks(void)
{
    return s_timer_ticks;
}

int br_plat_irq_start(void)
{
    const br_irq_attr_t attr = {
        .prio    = BR_IRQ_PRIO_DEFAULT,   /* 用绑定表的静态默认值 */
        .trigger = BR_IRQ_TRIG_DEFAULT,   /* 同上(PPI 是电平型, 由板级数据决定) */
        /* PERCPU 必须与绑定表一致(§3.4); NO_STORM_GUARD = 设计 §8.3 的"逃生门" ——
         * arch timer 是**已知且合法**的周期性高频线, 而本原型几乎不产生别的事 ⇒
         * 它在"最近 256 次中断"里的占比远超 25%, 会周期性触发风暴告警。
         * 用按线豁免(而不是调阈值/关检测器)表达"这是已知合法的高频线"。 */
        .flags   = BR_IRQ_F_PERCPU | BR_IRQ_F_NO_STORM_GUARD,
    };

    int r = br_irq_register(BR_IRQ_TIMER, br_plat_timer_isr, BR_NULL, &attr);
    if (r != 0) {
        return r;
    }
    r = br_irq_enable(BR_IRQ_TIMER);
    if (r != 0) {
        return r;
    }

    br_timer_rearm(BR_BOARD_TIMER_PERIOD_US);
    br_timer_write_ctl(BR_CNTP_CTL_ENABLE);   /* IMASK=0: 条件满足就发中断 */

    /* 全局开中断 = 设计 3-02 §14.3 初始化链的最后一步("全部插件 init 之后")。
     * v0.1 没有 core.init / 阶段机 ⇒ 由 platform 在"设备已就绪"这一刻执行
     * (WORKAROUND br-wa-boot-001)。 */
    br_irq_cpu_enable();
    return 0;
}

/* =====================================================================
 * 5. conformance 自检通道(设计 §8.4; 实现归 irq_conf.c, 这里只给两个原语)
 * ===================================================================== */

/* 软件触发一个"绑定为 SGI"的 virq: 找不到绑定 ⇒ -ENODEV; 非 SGI ⇒ -ENOTSUP */
int br_plat_irq_trigger(br_u32 virq)
{
    for (br_size_t i = 0; i < BR_ARRAY_SIZE(s_board_irq); i++) {
        if ((br_u32)s_board_irq[i].virq == virq) {
            if (s_board_irq[i].hwirq > BR_GICV3_INTID_SGI_MAX) {
                return BR_ERR(BR_ENOTSUP);   /* 只有 SGI(0–15)能软件触发 */
            }
            return br_gicv3_sgi_trigger(s_board_irq[i].hwirq);
        }
    }
    return BR_ERR(BR_ENODEV);
}

/* virq → 绑定表里的 hwirq(只给 conformance 做影子 vs 硬件交叉校验, §4.3 第 5 条) */
int br_plat_irq_hwirq(br_u32 virq, br_u32 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    for (br_size_t i = 0; i < BR_ARRAY_SIZE(s_board_irq); i++) {
        if ((br_u32)s_board_irq[i].virq == virq) {
            *out = s_board_irq[i].hwirq;
            return 0;
        }
    }
    return BR_ERR(BR_ENODEV);
}

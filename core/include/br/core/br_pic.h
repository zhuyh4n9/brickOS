/*
 * brickOS prototype v0.1.0 — PIC(中断控制器)契约: core ↔ platform 的填表面
 *
 * 权威设计: `docs/3-os-core/3-02-int.md` §4.1(`br_pic_ops`)/ §4.2(能力位与降级)/
 * §4.1.1(GICv3 EOImode 义务)/ §14.3(platform 侧契约, **非 native 面**)。
 *
 * 三层归属(3-02 §1.3, 本原型的落点):
 *   语义与机制   core(本头的消费者: 号空间/生命周期/屏蔽语义)
 *   控制器方言   本头由 ISA 层实现(GICv3 = platform/qemu-aarch64/src/gicv3.c)
 *   数据与特化   platform 提供(PIC 实例/基址/绑定表/优先级位数)
 *
 * ⚠ 这不是 native API 面(3-02 §14.3 末段): native API 是"全部插件作者的编码对象",
 * 而 `br_pic_ops` 只有 platform/ISA 作者面对。混进 native 面会让 CA-5 的面统计失真。
 *
 * 热路径纪律(3-02 §4.1/§14.3):
 *   ack / eoi / mask / unmask / cpu_mask / cpu_unmask **必须只碰寄存器**(ISR 内可调用);
 *   任何"要过总线"的屏蔽实现都不许放进这里 —— 那种控制器走级联域(§9)让总线操作
 *   发生在 demux/线程上下文。
 */
#ifndef BR_CORE_BR_PIC_H
#define BR_CORE_BR_PIC_H

#include <br/core/br_types.h>

/* platform 拥有的 PIC 实例存储(不透明; core 只经 ops 访问) */
typedef struct br_pic br_pic_t;

/*
 * `ack()` 的"无中断可领"返回值(3-02 §14.6)。
 * ★ 方言义务: GICv3 的 `ICC_IAR1_EL1` 会返回 1020–1023(含 1023 = spurious),
 *   这些**不是合法 INTID**, 必须**整个区间**折算为本值 —— 否则 core 会把它们
 *   当合法 hwirq 查绑定表、未命中、于是写一次 EOI, 而对特殊 INTID 写 EOI 是
 *   未定义/被弃用的行为(3-02 §5.3 的"方言义务")。
 */
#define BR_PIC_NO_IRQ   0xFFFFFFFFu

/* PIC 实例数上限(3-02 §3.2: 描述符里 pic_id 是 int8_t, 负数留给域成员) */
#define BR_PIC_MAX      128

/* ---------------- 能力位(3-02 §4.2) ----------------
 * 缺失时的 core 行为是**确定性降级**(绝不静默改变语义):
 * 要么降级到"更弱但确定"的行为(并留 trace), 要么 `-ENOTSUP`。 */
#define BR_PIC_CAP_MASK        0x0001u  /* 硬件单线屏蔽; 缺失 ⇒ 纯软件影子屏蔽 */
#define BR_PIC_CAP_LATCH       0x0002u  /* masked 期间仍锁存 pending */
#define BR_PIC_CAP_READBACK    0x0004u  /* 可回读配置/状态寄存器 */
#define BR_PIC_CAP_PRIO        0x0008u  /* 支持优先级 */
#define BR_PIC_CAP_NEST        0x0010u  /* 支持优先级嵌套(v1 恒不嵌套) */
#define BR_PIC_CAP_AFFINITY    0x0020u  /* 可路由到指定核(v2b) */
#define BR_PIC_CAP_TRIG_EDGE   0x0040u  /* 支持边沿触发 */
#define BR_PIC_CAP_TRIG_BOTH   0x0080u  /* 支持双边沿 */
#define BR_PIC_CAP_SW_TRIGGER  0x0100u  /* 支持软件触发(SGI/自测通道) */
#define BR_PIC_CAP_PER_CPU     0x0200u  /* 有每核私有中断(PPI) */
#define BR_PIC_CAP_RESYNC      0x0400u  /* 支持 save/restore(PM, v2) */

typedef struct {
    br_u8  prio_bits;   /* 有效优先级位数(0 = 不支持优先级) */
    br_u32 nr_irq;      /* 本实例 hwirq 上界 */
    br_u32 caps;        /* BR_PIC_CAP_* 位图 */
} br_pic_caps_t;

typedef struct br_pic_ops {
    const char *name;   /* "gicv3" / "gicv2" / "plic" / "nvic" / "soft" */

    /* ---- 生命周期(EARLY 相, 全局关中断) ---- */
    int (*init)    (br_pic_t *pic);                          /* 控制器复位/初始化(关中断) */
    int (*caps_get)(br_pic_t *pic, br_pic_caps_t *out);      /* 能力上报(IR-3) */

    /* ---- 取走与结束(热路径) ---- */
    br_u32 (*ack)(br_pic_t *pic);                            /* 无中断可领 ⇒ BR_PIC_NO_IRQ */
    void   (*eoi)(br_pic_t *pic, br_u32 hwirq);              /* Active → Inactive(EOImode 义务在此收敛) */

    /* ---- 单线屏蔽(ISR 内可调用: 只碰 MMIO) ---- */
    void (*mask)  (br_pic_t *pic, br_u32 hwirq);
    void (*unmask)(br_pic_t *pic, br_u32 hwirq);

    /* ---- 状态查询(诊断, 非热路径) ---- */
    int (*pending)(br_pic_t *pic, br_u32 hwirq, int *out);
    int (*active) (br_pic_t *pic, br_u32 hwirq, int *out);

    /* ---- 配置(thread-only) ---- */
    int (*set_prio)   (br_pic_t *pic, br_u32 hwirq, br_u8 prio);     /* prio = 逻辑优先级(小 = 高);
                                                                      * ★ 逻辑→硬件编码的**量化**是方言义务(3-02 §7.1):
                                                                      *   位宽不足时向**低优先级**方向饱和, 绝不静默提权 */
    int (*set_trigger)(br_pic_t *pic, br_u32 hwirq, br_u8 trigger);
    int (*trigger)    (br_pic_t *pic, br_u32 hwirq);                 /* 软件触发(仅 CAP_SW_TRIGGER) */

    /* ---- CPU 侧屏蔽(PSTATE.I 的方言包装; 3-02 §6.4 的 L2 就落在这里) ---- */
    br_u32 (*cpu_mask)  (br_pic_t *pic);                             /* 关本地 IRQ, 返回进入前状态 */
    void   (*cpu_unmask)(br_pic_t *pic, br_u32 state);               /* 按进入前状态恢复 I 位 */

    /* ---- 优先级屏蔽/嵌套(仅 CAP_NEST; v1 无人调用) ---- */
    void (*set_running_prio)(br_pic_t *pic, br_u8 prio);

    /* ---- 亲和性(仅 CAP_AFFINITY; v2b) ---- */
    int (*set_affinity)(br_pic_t *pic, br_u32 hwirq, br_u32 cpu_mask);

    /* ---- PM 状态同步(仅 CAP_RESYNC; v2) ---- */
    int (*save)   (br_pic_t *pic, void *buf, br_size_t n);
    int (*restore)(br_pic_t *pic, const void *buf, br_size_t n);
} br_pic_ops_t;

/*
 * 注册一个 PIC 实例(EARLY 相; 每实例一次)。
 * 返回 `pic_id`(>= 0)或负 errno(`-EINVAL` 参数非法 / `-ENOSPC` 实例池满 /
 * `-EBUSY` 重复注册同一实例)。
 *
 * ★ IR-3 推论(3-02 §4.2): **每个 CPU 恰有一个"CPU 接口控制器"** —— 它经
 *   `ack()` 取中断, 是中断的唯一入口。core 把**第一个注册成功的 PIC** 记为
 *   CPU 接口(PIC 未就绪则无中断可用 ⇒ 初始化顺序第 1 步, §14.3)。其后注册的
 *   实例只能经级联域接入(core 不为"哪个控制器在响"引入判定)。
 */
int br_pic_register(br_pic_t *pic, const br_pic_ops_t *ops);

/*
 * 换出 CPU 接口 PIC 的 caps 快照(平台写入自己的副本, 供 caps 相关决策使用)。
 * 未注册任何 PIC 时返回 -ENODEV。
 */
int br_pic_caps_of(br_u32 pic_id, br_pic_caps_t *out);

#endif /* BR_CORE_BR_PIC_H */

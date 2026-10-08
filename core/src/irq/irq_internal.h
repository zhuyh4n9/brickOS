/*
 * brickOS prototype v0.1.0 — 中断子系统内部真值(core 私有; **不导出**, CA-10)
 *
 * 这里放的是 3-02 §3.3 / §14.4 的关键数据结构与 core 内部函数。
 * 布局约束来自设计(`_Static_assert` 把它钉住):
 *   br_irq_desc_t  = 32 B(字段顺序"按宽度降序", 消除填充; §3.3 的尺寸推导)
 *   br_irq_rt_t    =  8 B
 *   br_irq_binding_t = 16 B(在 br_irq.h, platform 数据)
 *
 * ★ 描述符与运行期计数是**两个平行数组、同一 virq 下标**(§3.3):
 *   前者 = "配置与身份"(布局入 golden, 长期不动), 后者 = "运行期计数"(可增删)。
 *   分开后加计数器不会破 golden; 且**风暴检测是安全特性**(§8.3), 不能挂在
 *   诊断开关后面 ⇒ 两个池都恒在。
 */
#ifndef BR_IRQ_INTERNAL_H
#define BR_IRQ_INTERNAL_H

#include <br/core/br_irq.h>
#include <br/core/br_pic.h>
#include <br/core/br_trace.h>

/* =====================================================================
 * 描述符(3-02 §3.3; 下标 = virq, 热路径 O(1), 无查找表)
 * ===================================================================== */
typedef struct br_irq_desc {
    void   (*isr)(void *);   /* [0x00] ISR 入口(thread-only 的 register 写入) */
    void    *arg;            /* [0x08] */
    br_u32   hwirq;          /* [0x10] 缓存绑定表的 hwirq(热路径省一次查表) */
    br_u16   flags;          /* [0x14] BR_IRQ_F_* 运行期属性 */
    br_u16   depth;          /* [0x16] br_irq_disable 嵌套计数(IR-5; 注册后 = 1) */
    br_u8    prio;           /* [0x18] 逻辑优先级(数值小 = 高) */
    br_u8    trigger;        /* [0x19] BR_IRQ_TRIG_* */
    br_s8    pic_id;         /* [0x1A] 直连: PIC 实例; 域成员: 父 PIC */
    br_s8    dom_id;         /* [0x1B] -1 = 直连; >= 0 = 域 id */
    br_u16   sub;            /* [0x1C] dom_id >= 0 时的子中断号 */
    br_u16   storm;          /* [0x1E] 预留(§3.3 的 rsv 槽位): 交错的 2 B 槽位 */
} br_irq_desc_t;

_Static_assert(sizeof(br_irq_desc_t) == 32u, "描述符必须 32 B(3-02 §3.3 的尺寸推导)");

/* 运行期计数表(3-02 §3.3/§14.4; 与描述符平行同下标) */
typedef struct br_irq_rt {
    br_u32 count;   /* 进入次数(仅 BR_IRQ_STATS=1 时维护) */
    br_u16 storm;   /* 本线"窗口内被服务"次数(§8.3; 恒在 —— 安全特性) */
    br_u16 epoch;   /* storm 复位用的 isr_seq 快照 */
} br_irq_rt_t;

_Static_assert(sizeof(br_irq_rt_t) == 8u, "运行期表项必须 8 B");

/* =====================================================================
 * CPU-local(3-02 §14.4; TPIDR_EL1 指向它, BSS 零初始化 ⇒ 无运行期构造)
 * ===================================================================== */
typedef struct br_irq_cpu {
    void   *current;          /* 当前线程(Stage 1 = NULL: 无线程) */
    br_u32  irq_depth;        /* br_irq_lock 嵌套深度(INV-C 的执法对象) */
    br_u32  saved_daif;       /* 最外层 lock 保存的 DAIF(以 br_irq_state_t 编码) */
    br_u32  isr_nest;         /* ISR 嵌套(v1 恒 ≤ 1, IR-6) */
    br_u32  in_fault;         /* fault 嵌套(≥2 = double fault, §10.6) */
    br_u16  curr_virq;        /* 当前 ISR 的 virq(fault 归因: "哪个 ISR 踩了内存") */
    br_u16  in_handler;       /* 正在执行的 fault handler 下标(v2; v1 恒 0xFFFF) */
    br_u32  isr_seq;          /* 全局 ISR 序号(任何线进入 +1; 风暴窗口时基, §8.3) */
    br_u32  storm_total;      /* 本核风暴事件累计(每次越过阈值 +1) */
    br_u32  spurious_owned;   /* 有硬件屏蔽能力却仍到达(真 bug 信号) */
    br_u32  spurious_nocap;   /* 无屏蔽能力时的预期丢弃(降级行为) */
    br_u32  prev_fault_ec;    /* 上一次 fault 的 EC(§10.6 打印用) */
    br_u64  prev_fault_elr;   /* 上一次 fault 的 ELR */
} br_irq_cpu_t;

#define BR_IRQ_NO_HANDLER  0xFFFFu

/* =====================================================================
 * 级联域(3-02 §9.2)
 * ===================================================================== */
struct br_irq_domain {
    const char                *name;
    const br_irq_domain_ops_t *ops;
    void                      *priv;
    br_u16 parent_virq;    /* 父物理线的 virq(★ 必须是 PIC 直连线) */
    br_u16 virq_base;      /* 子中断 virq 窗口起点 */
    br_u16 n_sub;
    br_u16 flags;          /* BR_IRQ_DOMAIN_F_FAST / _SLOW */
    br_irq_desc_t            *sub_desc;  /* [n_sub] 指向描述符池窗口 */
    br_u32                   *bitmap;    /* [nwords] pending 缓冲(core 提供) */
    br_u16 nwords;
    br_u8  id;             /* 域下标(域池) */
    br_u8  rsv;
    br_u16 busy;           /* SLOW 域: demux 单飞(§9.4; Stage 1 不可达) */
    br_u16 drop_count;     /* SLOW 域: 提交失败的推迟/丢弃计数 */
};

/* =====================================================================
 * core 内部接口(各 .c 之间共享; 均**不导出**)
 * ===================================================================== */

/* 本核 CPU-local(BSS 静态实例; br_irq_cpu_init 后 TPIDR_EL1 指向同一处) */
br_irq_cpu_t *br_irq_cpu(void);

/* virq → 描述符(**含边界校验**: 越界/BR_IRQ_INVALID ⇒ NULL) */
br_irq_desc_t *br_irq_desc_of(br_u32 virq);

/* 运行期计数表项 */
br_irq_rt_t *br_irq_rt_of(br_u32 virq);

/* 是否在 ISR 上下文(thread-only API 的运行期执法, 3-02 §14.1) */
int br_irq_in_isr(void);

/* ---- PIC 注册表(irq_pic.c) ---- */
br_pic_t                *br_irq_pic(br_u32 pic_id);
const br_pic_ops_t      *br_irq_pic_ops(br_u32 pic_id);
const br_pic_caps_t     *br_irq_pic_caps(br_u32 pic_id);
br_u32                   br_irq_pic_count(void);
br_u32                   br_irq_cpu_pic_id(void);   /* 第一个注册的 = CPU 接口(IR-3 推论) */

/* hwirq → virq(热路径; v1 = 有序索引表 + 二分, §14.2)。命中返回 0 并写 *out。 */
int br_irq_virq_of(br_u32 pic_id, br_u32 hwirq, br_u32 *out);

/* ---- 屏蔽(irq_core.c) ---- */
void br_irq_pgm_mask  (br_irq_desc_t *d);
void br_irq_pgm_unmask(br_irq_desc_t *d);

/* ---- 风暴检测(irq_core.c; §8.3 的窗口化判据) ---- */
void br_irq_storm_check(br_irq_desc_t *d);

/* ---- 域(irq_domain.c) ---- */
void br_irq_demux_fast(br_irq_domain_t *dom);   /* ISR 上下文, §9.3 */

#endif /* BR_IRQ_INTERNAL_H */

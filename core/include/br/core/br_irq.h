/*
 * brickOS prototype v0.1.0 — 中断管理(int 框架)native API
 *
 * 权威设计: `docs/3-os-core/3-02-int.md`(机制/数据结构/不变量/分期);
 * 符号面与冻结计划: `docs/3-os-core/3-01-core-api-list.md` §8/§8.1/§15。
 *
 * 分期(3-02 §1.1): 本篇实现 **Stage 1**(调度器交付前, M0)的机制,
 * 并在 ADR-0011(本原型)里**提前落下 Stage 2 的第一件** —— **bottom half(bh)**:
 *   ① 中断号管理与抽象(§3)   ② PIC 抽象与能力降级(§4, 见 br_pic.h)
 *   ③ 生命周期(Pending/Active/Inactive, §5, 实现见 src/irq/irq_core.c)
 *   ④ fault 路径(同步异常分槽/分类/extable/panic, §10, 见 br_fault.h;
 *      **`br_fault_handler_register` 注册 API 属 v2**, 本篇不实现)
 *   ⑤ 优先级与抢占(§7)       ⑥ 屏蔽与使能三层(§6)
 *   ⑦ 触发方式(§8)           ⑧ 级联中断域(§9)
 *
 * ★ **bh 已落地(ADR-0011)**: `BR_IRQ_F_DISPATCH_BH` 不再是"置位忽略", 而是真的
 *   把该线的 ISR 推迟到下半部(workqueue, `br/core/br_work.h`)执行; SLOW 级联域
 *   (其存在前提是 bh)随之可用。**仍未做**: `BR_IRQ_DISPATCH_THREAD`(专属/共享线程)
 *   与 §12 的动态升级/§13 的亲和性与负载均衡(SMP, v2b)⇒ THREAD 置位**不报错但忽略**
 *   并留 trace(§3.4), 那段注释不改。v1 coop/preempt 都**无线程化 IRQ**。
 *   ★ 全局约束(3-02 §1.3): **core 不含任何控制器型号名**。本头里不出现
 *   "gic"/"pl011" 之类的字眼 —— 型号差异全在 ISA 层(br_pic.h 的填表)。
 */
#ifndef BR_CORE_BR_IRQ_H
#define BR_CORE_BR_IRQ_H

#include <br/core/br_types.h>

/* =====================================================================
 * 1. 编译期常量(3-02 §14.6: 全部由 manifest/平台配置裁剪, 无运行期协商)
 * ===================================================================== */

/* virq 空间布局(3-02 §3.2):
 *   0 .. BR_IRQ_NR_PIC-1        直连 PIC 线(§3.2)
 *   BR_IRQ_IPI_BASE .. +N_IPI-1 IPI 窗口(v2b 预留; v1 未用)
 *   BR_IRQ_DOMAIN_BASE .. MAX-1 级联域子中断窗口(按域连续切片)
 */
#define BR_IRQ_MAX              64u
#define BR_IRQ_IPI_BASE         48u
#define BR_IRQ_NR_IPI            8u
#define BR_IRQ_DOMAIN_BASE      56u
#define BR_IRQ_DOMAIN_MAX        2u   /* 域池容量 */
#define BR_IRQ_DOMAIN_WINDOW    (BR_IRQ_MAX - BR_IRQ_DOMAIN_BASE)

/* 无效 virq(公共签名用 uint32 表达"无效"; **绝不写进内部字段**) */
#define BR_IRQ_INVALID          0xFFFFFFFFu

/* 域 demux 单次轮数上限(域级风暴保护, 3-02 §9.3) */
#define BR_IRQ_DEMUX_MAX_ROUND  8u

/* 中断风暴保护(3-02 §8.3: 窗口化判据, 与频率无关, 只看占比) */
#define BR_IRQ_STORM_LIMIT      64u
#define BR_IRQ_STORM_WINDOW     256u
#define BR_IRQ_STORM_GUARD      0     /* 1 = 越限时自动 mask(默认关: 可观测的持续错误优于静默失效) */

/* 每线计数开关(3-02 §14.6: 槽位恒在, 开关只决定是否维护 count —— 不改变 RAM) */
#define BR_IRQ_STATS            1

/* =====================================================================
 * 2. 优先级 / 触发 / 属性(3-02 §7.1 / §8.1 / §3.4)
 * ===================================================================== */

/* 逻辑优先级: **数值小 = 高**(GIC/NVIC 惯例); 解释权在 platform */
#define BR_IRQ_PRIO_HIGHEST   0x00u
#define BR_IRQ_PRIO_LOWEST    0xFFu
#define BR_IRQ_PRIO_DEFAULT   0x80u

/* 触发方式编码(3-02 §8.1; 解释权在 platform, core 只透传) */
#define BR_IRQ_TRIG_LEVEL_HIGH 0u
#define BR_IRQ_TRIG_LEVEL_LOW  1u
#define BR_IRQ_TRIG_EDGE_RISE  2u
#define BR_IRQ_TRIG_EDGE_FALL  3u
#define BR_IRQ_TRIG_EDGE_BOTH  4u
#define BR_IRQ_TRIG_DEFAULT    0xFFu   /* 用绑定表的静态值(板级信息, 归 platform) */

/* `br_irq_attr_t.flags`(3-02 §3.4; append-only, D14)
 * ★ 未知位置位 ⇒ register 返回 `-EINVAL`(**刻意不静默忽略**: 静默忽略会让
 *   用新头文件编译的插件在老 core 上悄悄失去它请求的属性)。 */
#define BR_IRQ_F_SHARED          0x0001u  /* 共享线 = v1 非目标 ⇒ -ENOTSUP */
#define BR_IRQ_F_PERCPU          0x0002u  /* 每核私有(PPI); 与硬件不符 ⇒ -EINVAL */
#define BR_IRQ_F_DISPATCH_BH     0x0004u  /* 该线分发到 bh(下半部)。★ ADR-0011 起**真的生效**:
                                           * ISR 不在中断上下文里跑, 而是被推迟到
                                           * workqueue(bh); 处理期间硬件线被 mask,
                                           * handler 返回后放行(§12.4/§9.4 的骨架) */
#define BR_IRQ_F_DISPATCH_THREAD 0x0008u  /* Stage 2: 分发到专属/共享线程; **仍未实现** ⇒
                                           * 置位不报错但忽略 + trace(§3.4) */
#define BR_IRQ_F_MIGRATABLE      0x0010u  /* v2b: 允许运行时迁移 */
#define BR_IRQ_F_STATS           0x0020u  /* 该线开启统计(需 BR_IRQ_STATS) */
#define BR_IRQ_F_NO_STORM_GUARD  0x0040u  /* 该线豁免风暴保护(已知合法高频线) */
#define BR_IRQ_F_KNOWN_MASK      0x007Fu  /* v1 已知位全集 */

typedef struct {
    br_u8  prio;      /* BR_IRQ_PRIO_*; 超出控制器分辨率 ⇒ 向低优先级饱和 + trace */
    br_u8  trigger;   /* BR_IRQ_TRIG_* */
    br_u32 flags;     /* BR_IRQ_F_* */
} br_irq_attr_t;

/* =====================================================================
 * 3. native API(9 件; 3-01 §8/§8.1 冻结签名, 3-02 §14.1 语义规格)
 * =====================================================================
 * thread-only 的准确定义(3-02 §14.1 末注): 它是**上下文禁令**("禁止在 ISR 内调用"),
 * **不要求系统里已有线程对象** —— M0 的主上下文与插件 init 相本身就是 thread 上下文。
 *
 * 直连 ↔ 域成员**双向互斥**(3-02 §14.1): 对域成员调直连 API(或反之)一律 `-EINVAL`,
 * 且 `register` 期就报出(不是等到 disable 才出错)。
 */

/* 绑定 ISR 到 virq: 校验绑定表 → 填描述符 → 配 prio/trigger(经 pic_ops, 关中断)
 * → **不使能**(depth = 1, 硬件已 mask)。 */
int br_irq_register(br_u32 irq, void (*isr)(void *), void *arg,
                    const br_irq_attr_t *attr);

/* depth 减到 0 才 pic_ops.unmask; 已在放行态 ⇒ 幂等返回 0。 */
int br_irq_enable(br_u32 irq);

/* depth++; 0→1 时 pic_ops.mask(顺序: **先 L2 后 L1**, 3-02 §6.5)。 */
int br_irq_disable(br_u32 irq);

/* ---- L2: CPU 本地屏蔽(ISR-safe, 白名单四件之一, CA-3) ---- */

/* `br_irq_state_t` 位布局**进 golden**(3-02 §6.4); 未用位保留为 0。 */
typedef br_u32 br_irq_state_t;

#define BR_IRQ_ST_I_MASK       0x00000001u  /* [0]     进入前 PSTATE.I */
#define BR_IRQ_ST_F_MASK       0x00000002u  /* [1]     进入前 PSTATE.F(v1 不管理) */
#define BR_IRQ_ST_A_MASK       0x00000004u  /* [2]     进入前 PSTATE.A(v1 不管理) */
#define BR_IRQ_ST_D_MASK       0x00000008u  /* [3]     进入前 PSTATE.D(v1 不管理) */
#define BR_IRQ_ST_DEPTH_SHIFT  4
#define BR_IRQ_ST_DEPTH_MASK   0x0003FFF0u  /* [17:4]  进入前 irq_depth(14 位, 调试校验) */

/* 保存 DAIF → 关本地 IRQ → irq_depth++ → 返回凭据。 */
br_irq_state_t br_irq_lock(void);

/* irq_depth--; 归零**且**进入前 I 位为 0 时才恢复 PSTATE.I。
 * ★ 下溢护栏(3-02 §6.4): depth 已是 0 时**不得** `--`(会回绕 ⇒ 中断永久关闭)。 */
void br_irq_unlock(br_irq_state_t st);

/* =====================================================================
 * 4. 级联中断域(3-01 §8.1 冻结签名; 3-02 §9)
 * =====================================================================
 * v1 单层级联; 嵌套域与共享线是非目标。**父线必须是 PIC 直连线**(SLOW 域
 * 在 ISR 内 mask 父线的前提, 3-02 §9.4)。
 * ★ ADR-0011 后 **FAST 与 SLOW 都可用**: SLOW 域的 demux 走 core 的 workqueue
 *   (`br/core/br_work.h`), 父线在提交前被 mask、demux 完成后放行(§9.4)。
 *   只有"bh 不可用"(core 未提供 workqueue)时 `br_irq_domain_create(..., F_SLOW, ...)`
 *   才返回 NULL —— 在 v0.2.0 的 core 里 bh 恒在, 故 SLOW 恒可用。
 */

typedef struct br_irq_domain br_irq_domain_t;

typedef struct br_irq_domain_ops {
    /* 挂起查询: core 提供位图缓冲(容量 = ceil(n_sub/32) 字), 驱动填当前 pending。
     * ★ 二段式注册(3-02 §9.2): 域创建后、子 handler 注册前若发生该子中断,
     *   core 的处理是 **mask 该子中断**(不留"无属主却反复触发"的风暴口)。 */
    void (*pending)(void *priv, br_u32 *bits, br_size_t nwords);
    void (*mask)  (void *priv, br_u32 sub);   /* 子中断屏蔽 */
    void (*unmask)(void *priv, br_u32 sub);
    void (*ack)   (void *priv, br_u32 sub);   /* ★ core 在子 handler **返回后**调用 */
} br_irq_domain_ops_t;

#define BR_IRQ_DOMAIN_F_FAST 0x0u   /* 状态寄存器内存映射 ⇒ demux + 子 handler 在 ISR 上下文 */
#define BR_IRQ_DOMAIN_F_SLOW 0x1u   /* 状态读取需总线事务 ⇒ demux 在 bh(下半部, ADR-0011 起可用)。
                                     * ★ 子 handler 的上下文 = **bh 契约**(比设计的"线程上下文"
                                     * 更严: 关中断、不可阻塞)。见 br_work.h 的文件头。 */

/* 从域池分配域 + 子描述符窗口 + bitmap; 注册父线 ISR = 域 demux。
 * 返回 NULL(池满 / 父线非法或非直连 / n_sub 越界或窗口不足 / ops->pending 缺失 /
 * 请求 SLOW 而 bh 不可用)。 */
br_irq_domain_t *br_irq_domain_create(const char *name, br_u32 parent_irq,
                                      br_u32 n_sub, br_u32 flags,
                                      const br_irq_domain_ops_t *ops, void *priv);

/* 填子描述符; 成功后子 depth = 1(屏蔽态, 与 br_irq_register 对称)。 */
int br_irq_register_child(br_irq_domain_t *dom, br_u32 sub,
                          void (*isr)(void *), void *arg, const br_irq_attr_t *attr);

int br_irq_enable_child (br_irq_domain_t *dom, br_u32 sub);
int br_irq_disable_child(br_irq_domain_t *dom, br_u32 sub);

/* =====================================================================
 * 5. 平台侧: virq ↔ hwirq 绑定表(3-02 §3.3; platform 数据, 编译进 .rodata)
 * ===================================================================== */

/* 绑定表条目静态属性 */
#define BR_IRQB_F_PERCPU       0x01u   /* 每核私有(PPI) */
#define BR_IRQB_F_ACTIVE_LOW   0x02u   /* 低有效(板级极性信息) */
#define BR_IRQB_F_EXCLUSIVE    0x04u   /* 独占(共享线为 v1 非目标) */

typedef struct {
    br_u16 virq;      /* 全局虚拟号(下标语义; 必须 < BR_IRQ_MAX) */
    br_u8  pic_id;    /* 挂在哪个 PIC 实例(EARLY 注册顺序) */
    br_u8  flags;     /* BR_IRQB_F_* */
    br_u32 hwirq;     /* PIC 实例内硬件号(INTID / IRQ number / pin) */
    br_s8  dom_id;    /* -1 = 直连 PIC; >= 0 = 级联域成员 */
    br_u8  prio;      /* 静态默认优先级(attr.prio == 0xFF 时生效) */
    br_u8  trigger;   /* 静态默认触发方式(attr.trigger == 0xFF 时生效) */
    br_u8  dispatch;  /* 静态分发形态(Stage 2; Stage 1 只接受 INLINE) */
    br_u8  rsv[3];    /* append-only 预留(D14) */
} br_irq_binding_t;   /* 16 B */

#define BR_IRQ_DISPATCH_INLINE 0u
#define BR_IRQ_DISPATCH_BH     1u
#define BR_IRQ_DISPATCH_THREAD 2u

/* 平台侧一次性提交绑定表(EARLY 相, 恰一次; 重复提交 ⇒ -EBUSY)。
 * core 在此构造 hwirq→virq 解析表(§14.2: v0.1 用"有序表 + 二分", 4 B/绑定 —— 
 * 不采用 `uint16_t virq_of[nr_irq]` 直接索引表: GICv3 的 1024 INTID 要 2 KB)。 */
int br_irq_bindings_set(const br_irq_binding_t *tbl, br_size_t n);

/* =====================================================================
 * 6. 跨模块观测契约(不能 hidden: 消费方是 debug bridge / ramdump 插件, 3-02 §1.4)
 * ===================================================================== */

/* 诊断快照(3-02 §14.4; 非热路径, 不进 golden 布局承诺)
 * ★ ADR-0011 追加 bh 三项(append-only; 该结构明说不进 golden)。*/
typedef struct {
    br_u32 count;          /* 进入次数(BR_IRQ_STATS; 关闭时恒 0) */
    br_u16 storm;          /* 当前窗口内该线计数(直接取 rt->storm, 不另存峰值) */
    br_u16 depth;          /* 当前屏蔽深度 */
    br_u32 spurious_owned; /* "有硬件屏蔽却仍到达" —— 真 bug 信号(§4.2) */
    br_u32 spurious_nocap; /* "无屏蔽能力时的预期丢弃" —— 降级行为(§4.2) */
    br_u32 drop;           /* SLOW 域提交失败次数(域成员, §9.4) */
    br_u16 flags;          /* 描述符 flags 快照(★ 含 ADR-0011 后真正生效的 DISPATCH_BH 位) */
    br_u8  prio, trigger;
    /* ---- 下半部(bh)三项: 本核累计, v1 单核 ⇒ 即全局(ADR-0011 §3) ---- */
    br_u32 bh_deferred;        /* BH 形态: 成功推迟到下半部的次数 */
    br_u32 bh_dropped;         /* BH 形态: 队满 ⇒ 回滚未提交的次数(IR-10) */
    br_u32 suppressed_dispatch;/* §11.4.1: 派发前已 disable ⇒ 抑制 handler 的次数 */
} br_irq_stat_t;

/* ★ `spurious_owned`/`spurious_nocap` 在 CPU-local(3-02 §14.4), 故本调用返回的是
 *   **本核**的计数(v1 单核 ⇒ 即全局)。取不到(virq 越界/无绑定)⇒ out 清零。 */
void br_irq_stats_get(br_u32 virq, br_irq_stat_t *out);

/*
 * 执行上下文查询(ADR-0011; §12.1 的三种形态在运行期的可观测面)。
 *
 * 为什么进对外头而不是 core 私有头: **域方言**要能自证"我的状态寄存器没有在 ISR 里被读"
 * (`SLOW` 域存在的全部理由, §9.4), 而那是插件(platform/驱动)侧写的代码 —— 它拿不到
 * core 私有头。`br_work_in_bh()` 同理(在 `br_work.h`)。
 */
br_bool br_irq_in_isr(void);        /* 在 ISR 体内(中断上下文) */
br_bool br_irq_in_atomic(void);     /* ISR **或** bh —— thread-only API 的禁令范围 */

/* =====================================================================
 * 7. core 内部符号(3-02 §1.4/§14.2: asm 桩调用与 M0 替身; **不导出**)
 * ===================================================================== */

/*
 * CPU-local 中断状态初始化(BSS 零初始化 ⇒ 无运行期构造; 这里把 TPIDR_EL1 指向本核的
 * `br_irq_cpu_t`, 并把描述符池的 pic_id/dom_id 置 -1 —— 见 P-IRQ-5)。
 *
 * 设计侧这一步属 **core.init**, 由 core 的入口 `br_core_main()` 在启动链阶段 ① 调用
 * (BSS 清零之后、任何 C 子系统之前)。
 * ★ **硬约束**: 必须排在 platform 提交 PIC 绑定表(`br_irq_bindings_set`)之前 ——
 *   本函数会把**全部**描述符的 pic_id/dom_id 置 -1, 排在绑定表之后会抹掉已落的绑定。
 *   见 `core/src/main.c` 的阶段 ① 与 ADR-0008。
 */
void br_irq_cpu_init(void);

/*
 * 全局开中断(只放行 PSTATE.I; F/A/D 保持)。
 * 设计侧 = "全部插件 init 之后"的那一步(主文档 §6.2 / 3-02 §14.3 第 7 步之后),
 * 由插件管理器在 LATE 相之后、START 相之前调用(见 plugin_mgr.c 的相位驱动)。
 */
void br_irq_cpu_enable(void);

/* 中断入口(asm 桩调用, 3-02 §5.2): ack → 分发 → ISR → 统一出口 eoi。 */
struct br_exc_frame;
void br_irq_enter(struct br_exc_frame *f);

/*
 * 当前 L2 临界区嵌套深度(ADR-0011 裁定 P-1)。**core 内部符号**(同样不进 golden):
 * 调度框架在"新线程首次进入"时要判断"我是不是在一个 L2 临界区里被切过来的" ——
 * 若是(深度 > 0), 说明有人在临界区里让出了 CPU(既有禁令), 那就**不动**中断状态;
 * 若否(深度 == 0), 线程必须跑在"中断放开"的形态 —— 这一条在 preempt 调度器从
 * **IRQ 出口**换栈(此刻 PSTATE.I == 1)时是必需的, 否则新线程会带着关中断一路跑下去。
 */
br_u32 br_irq_lock_depth(void);

#endif /* BR_CORE_BR_IRQ_H */

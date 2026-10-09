/*
 * brickOS prototype v0.1.0 — 级联中断域: 域池 / 窗口切片 / FAST demux / 父线 ISR
 *
 * 权威设计: `docs/3-os-core/3-02-int.md` §9
 *   §9.1  模型与边界: 一根物理线承载 N 个子中断源; v1 **单层级联**; 共享线 = 非目标
 *   §9.2  域池 + 子描述符窗口 + bitmap 全部来自 core 静态池(不来自堆);
 *         子中断注册是**二段式**(create 域 → register_child), 无属主时 demux 里 mask
 *   §9.3  FAST demux 伪码: 轮询读空 + **显式 exhausted 标志**(不用 round == MAX_ROUND,
 *         否则"最后一轮读到空"会误报 overflow); core 在**子 handler 返回后** ack
 *   §9.4  SLOW 域: 状态读取需总线事务 ⇒ 父线 ISR 只做"mask 整线 + 单飞 + 提交 bh";
 *         demux 在**下半部**执行, 完成后清 busy + 放行父线; **`br_work_submit` 失败是
 *         这套方案唯一的死锁入口, 必须回滚**(IR-10)。
 *         ★ ADR-0011: bh 已落地(core 的 workqueue)⇒ SLOW 域**可用**(此前直接返回 NULL)。
 *   §9.6  域事件: IRQ_DEMUX / IRQ_DEMUX_OVERFLOW / IRQ_DOMAIN_ORPHAN / IRQ_DOMAIN_DROP /
 *         IRQ_DOMAIN_NOBH(后者此后只在"bh 不可用"时出现)
 *
 * 上下文(INV-F): FAST 域的 demux 与子 handler 全在 ISR 内(纯寄存器访问);
 * SLOW 域的 demux 与子 handler 全在 **bh**(ADR-0011)—— 父线的状态寄存器只能在
 * 那里读, 这正是 SLOW 域存在的理由。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_trace.h>
#include <br/core/br_work.h>

#include "irq_internal.h"

/*
 * §9.2: 域池 + 每域一个 pending 位图字。
 * v1 只支持 nwords == 1(窗口 ≤ 32), 故"每域一个 uint32"就够 —— 这是把
 * `uint32_t *bitmap` 指针指进来的最小静态池。_Static_assert 把窗口上限钉住,
 * 将来放宽到 >32 时必须把这里换成按域切片的字数组。
 */
_Static_assert(BR_IRQ_DOMAIN_WINDOW <= 32u,
               "v0.1 的域 bitmap 只支持单字(nwords == 1); 放宽窗口需同时改静态池形态");

static struct br_irq_domain s_dom[BR_IRQ_DOMAIN_MAX];
static br_u32               s_dom_bitmap[BR_IRQ_DOMAIN_MAX];
static br_u32               s_dom_n;
static br_u32               s_dom_virq_next;   /* 域窗口切片的游标(§3.2: 按域连续切片) */

/* 父线 ISR 的共享蹦床(§9.3/§9.4): arg = 域指针。static —— 只在本 TU 经函数指针使用,
 * 连 irq_internal.h 都不必声明(比 hidden 更 hidden)。 */
static void br_irq_domain_parent_isr(void *arg);

/* SLOW 域的 demux 工作项体(arg = 域指针): 在 **bh** 上下文执行(ADR-0011)。 */
static void br_irq_domain_bh_demux(void *arg);

/* 子描述符窗口: 从 core 描述符池切 [virq_base, virq_base + n_sub)(§3.2/§9.2) */
static br_irq_desc_t *domain_sub_desc(const struct br_irq_domain *dom, br_u32 sub)
{
    return &dom->sub_desc[sub];
}

/* =====================================================================
 * 创建(§9.2; EARLY/CORE 相, thread 上下文)
 * ===================================================================== */

br_irq_domain_t *br_irq_domain_create(const char *name, br_u32 parent_irq,
                                      br_u32 n_sub, br_u32 flags,
                                      const br_irq_domain_ops_t *ops, void *priv)
{
    /* ops->pending 是域方言的最小契约: 没有它, demux 无从知道"谁挂起了"(§9.2) */
    if (name == BR_NULL || ops == BR_NULL || ops->pending == BR_NULL) {
        return BR_NULL;
    }

    /* flags 只能是 FAST(0) 或 SLOW(1); 其余位是保留(append-only) */
    if ((flags & ~(br_u32)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        return BR_NULL;
    }

    /* ★ ADR-0011: SLOW 域的存在前提是 bh(下半部) —— core 的 workqueue 恒在
     *   (`BR_WORKQ_DEPTH > 0`), 所以这一族**可用**。
     *   下面这段"bh 被裁掉 ⇒ 拒绝 + 留痕"的守卫保留为**编译期**分支: 它是"分期边界"
     *   的可执行表述 —— 将来若把 workqueue 裁剪成可选件(深度 0), 这里就是正确的降级点
     *   (而不是"接受但永远不 demux"的静默失效, §9.4/§1.1.1)。 */
#if BR_WORKQ_DEPTH == 0
    if ((flags & (br_u32)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        br_trace_emit(BR_TRACE_IRQ_DOMAIN_NOBH, flags, 0u);
        return BR_NULL;
    }
#endif

    if (s_dom_n >= (br_u32)BR_IRQ_DOMAIN_MAX) {
        return BR_NULL;                     /* §9.2: 池满 ⇒ NULL(签名只能这么表达错误) */
    }
    if (n_sub == 0u || n_sub > (br_u32)BR_IRQ_DOMAIN_WINDOW) {
        return BR_NULL;
    }
    if (s_dom_virq_next + n_sub > (br_u32)BR_IRQ_DOMAIN_WINDOW) {
        return BR_NULL;                     /* 子中断窗口切不出这么长的一片 */
    }

    /* ---- 父线校验(§9.2/§9.4): 必须是 PIC **直连线**, 且有绑定、未被占用 ---- */
    br_irq_desc_t *parent = br_irq_desc_of(parent_irq);
    if (parent == BR_NULL) {
        return BR_NULL;
    }
    /* 嵌套域(父线本身是域成员)是 v1 非目标(§9.5): 父线 mask/unmask 会不再是 MMIO,
     * §9.4 的 mask 前提失效。 */
    if (parent->dom_id >= 0) {
        return BR_NULL;
    }
    if (parent->pic_id < 0) {
        return BR_NULL;                     /* 无绑定: 初始化顺序违反(§14.3), 大声失败 */
    }
    if (parent->isr != BR_NULL) {
        return BR_NULL;                     /* INV-A: 父线恰一属主(已被别的驱动占用) */
    }

    const br_u32 dom_id = s_dom_n;
    const br_u32 virq_base = (br_u32)BR_IRQ_DOMAIN_BASE + s_dom_virq_next;

    struct br_irq_domain *dom = &s_dom[dom_id];
    dom->name       = name;
    dom->ops        = ops;
    dom->priv       = priv;
    dom->parent_virq = (br_u16)parent_irq;
    dom->virq_base  = (br_u16)virq_base;
    dom->n_sub      = (br_u16)n_sub;
    dom->flags      = (br_u16)flags;
    dom->id         = (br_u8)dom_id;
    /* (n_sub+31)/32; _Static_assert 保证 BR_IRQ_DOMAIN_WINDOW <= 32 ⇒ 恒为 1, 与
     * "每域一个 uint32"的静态 bitmap 池一致。放宽窗口时两处必须同时改。 */
    dom->nwords     = (br_u16)((n_sub + 31u) / 32u);
    dom->busy       = 0u;
    dom->drop_count = 0u;
    dom->bitmap     = &s_dom_bitmap[dom_id];
    dom->bitmap[0]  = 0u;
    /* 描述符池是连续数组(§3.3), 故窗口切片 = 取起点指针; 不跨 TU 直接摸 s_desc */
    dom->sub_desc   = br_irq_desc_of(virq_base);
    if (dom->sub_desc == BR_NULL) {
        return BR_NULL;
    }

    /* ---- 填子描述符窗口(身份字段; isr/arg 留空等待 register_child) ---- */
    for (br_u32 sub = 0u; sub < n_sub; sub++) {
        br_irq_desc_t *sd = domain_sub_desc(dom, sub);
        sd->isr     = BR_NULL;              /* 二段式: 还没属主(§9.2) */
        sd->arg     = BR_NULL;
        sd->hwirq   = sub;                  /* 域内子号 */
        sd->pic_id  = parent->pic_id;       /* 域成员: 记住父 PIC(供诊断/统计) */
        sd->dom_id  = (br_s8)dom_id;
        sd->sub     = (br_u16)sub;
        sd->flags   = 0u;
        sd->depth   = 0u;
        sd->prio    = 0u;
        sd->trigger = 0u;
    }

    /*
     * ---- 注册父线: ISR = 域 demux 蹦床(§9.2/§9.3) ----
     * 走 br_irq_register() 而不是手写: "register 后 depth=1 + 硬件已 mask" 这条
     * 安全默认(§6.2)只需一处实现。attr = NULL ⇒ 全用绑定表静态 prio/trigger。
     * 失败(如 prio/trigger 配置被 PIC 拒绝)⇒ 不提交域, 返回 NULL。
     */
    {
        const int r = br_irq_register(parent_irq, br_irq_domain_parent_isr, dom, BR_NULL);
        if (r != BR_OK) {
            return BR_NULL;
        }
    }

    /* 提交: 只有到这里域才占住 id 与窗口切片 */
    s_dom_n++;
    s_dom_virq_next += n_sub;

    return dom;
}

/* =====================================================================
 * 域 demux 主循环(§9.3; FAST = ISR 上下文 / SLOW = bh 上下文, ADR-0011)
 *
 * 循环本身**与上下文无关** —— 这正是"SLOW 域的 ops->pending() 必须在非 ISR 上下文里
 * 读状态寄存器"这条契约的落点: 换的不是循环, 是**调用者**。
 * ===================================================================== */

void br_irq_demux(br_irq_domain_t *dom)
{
    if (dom == BR_NULL || dom->ops == BR_NULL || dom->ops->pending == BR_NULL) {
        return;
    }

    /*
     * ★ 判据用**显式 exhausted 标志**, 不用 `round == BR_IRQ_DEMUX_MAX_ROUND`:
     *   若最后一轮恰好读到空位图, 循环会先 round++ 再退出, 此时
     *   round == MAX_ROUND 但位图已空 ⇒ 会误报 overflow(§9.3 伪码的易错点注释)。
     *   语义: exhausted 初值 1 = "假定没跑完", 只有读到空才清零。
     */
    br_bool exhausted = BR_TRUE;

    for (br_u32 round = 0u; round < (br_u32)BR_IRQ_DEMUX_MAX_ROUND; round++) {
        /* 状态寄存器是"快照": 处理子中断期间可能有新子中断置位 ⇒ 每轮重读(§9.3) */
        dom->bitmap[0] = 0u;
        dom->ops->pending(dom->priv, dom->bitmap, (br_size_t)dom->nwords);

        if (dom->bitmap[0] == 0u) {
            exhausted = BR_FALSE;
            break;
        }

        const br_u32 bits = dom->bitmap[0];
        for (br_u32 sub = 0u; sub < (br_u32)dom->n_sub; sub++) {
            if ((bits & (1u << sub)) == 0u) {
                continue;
            }

            br_irq_desc_t *sd = domain_sub_desc(dom, sub);
            if (sd->isr != BR_NULL) {
                sd->isr(sd->arg);           /* 子 handler 契约(FAST=ISR, INV-F) */
            } else {
                /* 二段式的空窗(§9.2): 无属主 ⇒ 立即挡住, 不留"反复触发"的风暴口 */
                if (dom->ops->mask != BR_NULL) {
                    dom->ops->mask(dom->priv, sub);
                }
                br_trace_emit(BR_TRACE_IRQ_DOMAIN_ORPHAN, (br_u32)dom->id, (br_u64)sub);
            }

            /* §9.3: core 在**子 handler 返回后** ack(与父线"eoi 只在出口一处"同构:
             * 集中管理 ⇒ 驱动无法忘记 ack) */
            if (dom->ops->ack != BR_NULL) {
                dom->ops->ack(dom->priv, sub);
            }
        }
    }

    /* 位图摘要: 干净退出 == 0; 轮数用尽仍有位 ⇒ 非 0(overflow 的直接证据) */
    br_trace_emit(BR_TRACE_IRQ_DEMUX, (br_u32)dom->id, (br_u64)dom->bitmap[0]);
    if (exhausted == BR_TRUE) {
        /* 域级风暴保护: 上限 = BR_IRQ_DEMUX_MAX_ROUND; 剩余 pending 由父线 eoi 后
         * 重新触发 —— level 型状态寄存器天然保证不会丢(§9.3)。 */
        br_trace_emit(BR_TRACE_IRQ_DEMUX_OVERFLOW, (br_u32)dom->id, 0u);
    }
}

/* =====================================================================
 * 父线 ISR 蹦床(§9.3 FAST 直达 / §9.4 SLOW 转 bh)+ SLOW 的 bh 工作项体
 * ===================================================================== */

/*
 * SLOW 域的 bh 工作项体(ADR-0011): 在**下半部**读状态位图 → 逐子 handler → ack,
 * 然后清单飞 + 放行父线。
 *
 * 顺序即正确性:
 *   1. 清 busy 必须**在放行父线之前**? —— 不。父线在 bh 期间是 mask 的, 不可能重入;
 *      放行之后 level 线可能立刻重新 assert 并进入父线 ISR —— 那时 busy 必须已经是 0,
 *      否则新的一轮会被"单飞"挡掉、而线已被放行 ⇒ 事件要等下一次 assert 才处理。
 *      ⇒ 本实现: **先清 busy, 再放行**(顺序与 §9.4 的伪码一致)。
 *   2. demux 本身按 §9.3 的轮询+exhausted 语义跑(读空为止或轮数用尽)。
 */
static void br_irq_domain_bh_demux(void *arg)
{
    br_irq_domain_t *dom = (br_irq_domain_t *)arg;

    if (dom == BR_NULL) {
        return;
    }

    br_irq_demux(dom);

    dom->busy = 0u;
    br_irq_pgm_unmask(br_irq_desc_of((br_u32)dom->parent_virq));
}

static void br_irq_domain_parent_isr(void *arg)
{
    br_irq_domain_t *dom = (br_irq_domain_t *)arg;
    if (dom == BR_NULL) {
        return;
    }

    if ((dom->flags & (br_u16)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        /*
         * ---- §9.4: 父线 ISR 只做三件事: 单飞、mask 整线、提交 demux ----
         * 为什么单飞: 父线虽然马上被 mask, 但"mask 之前已经 pending 的那一次"仍可能
         * 进来(level 型 + 边沿敏感的控制器); 没有单飞就会为同一份状态排两次 demux。
         * 为什么 mask 必须在提交**之前**: 否则 bh 还没读状态寄存器, 线又 assert,
         * 队列被同一域刷爆(深度是静态预算)。
         */
        if (dom->busy != 0u) {
            return;                     /* 已有一份 demux 在排队/在跑 */
        }
        dom->busy = 1u;
        br_irq_pgm_mask(br_irq_desc_of((br_u32)dom->parent_virq));

        if (br_work_submit(br_irq_domain_bh_demux, dom) != 0) {
            /*
             * ★ IR-10: **这套方案唯一的死锁入口**。父线已经被 mask, 若不立即回滚,
             *   该域的**全部子中断永久静默失效**且没有任何报错。
             *   回滚 = 清 busy + 放行父线 + 计数 + 留痕(§9.6 的 IRQ_DOMAIN_DROP)。
             *   level 型状态寄存器在放行后立刻重新 assert ⇒ 事件不丢;
             *   边沿/读清型会丢这一份(§6.3 的诚实条款)。
             */
            dom->busy = 0u;
            dom->drop_count++;
            br_irq_pgm_unmask(br_irq_desc_of((br_u32)dom->parent_virq));
            br_irq_bh_note_drop();
            br_trace_emit(BR_TRACE_IRQ_DOMAIN_DROP, (br_u32)dom->id, 0u);
            return;
        }

        /* 推迟计数与按线的 BH 分发**共用一份真值**(br_irq_bh_note_defer 的注释)。 */
        br_irq_bh_note_defer();
        br_trace_emit(BR_TRACE_IRQ_BH_DEFER, (br_u32)dom->parent_virq, (br_u64)dom->id);
        return;
    }

    br_irq_demux(dom);
}

/* SLOW 域成员所属域的 drop_count(br_irq_stats_get 的只读访问器; §9.4)。 */
br_u32 br_irq_domain_drop_of(const br_irq_desc_t *d)
{
    if (d == BR_NULL || d->dom_id < 0) {
        return 0u;                      /* 直连线: 没有域, 也就没有域级丢弃 */
    }
    const br_u32 id = (br_u32)d->dom_id;
    if (id >= (br_u32)BR_IRQ_DOMAIN_MAX) {
        return 0u;
    }
    return (br_u32)s_dom[id].drop_count;
}

/* =====================================================================
 * 子中断 API(§14.1; 与直连族**双向互斥**的域侧; 全部 thread-only)
 * ===================================================================== */

/* 域子 API 的公共前置: thread-only(ISR/bh 都拒) + dom/sub 边界 + 属主存在 */
static int child_api_guard(br_irq_domain_t *dom, br_u32 sub, br_irq_desc_t **out)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(BR_EINVAL);
    }
    if (dom == BR_NULL || dom->ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (sub >= (br_u32)dom->n_sub) {
        return BR_ERR(BR_EINVAL);
    }
    br_irq_desc_t *sd = domain_sub_desc(dom, sub);
    if (sd->isr == BR_NULL) {
        return BR_ERR(BR_EINVAL);           /* 未注册的子中断 */
    }
    *out = sd;
    return BR_OK;
}

int br_irq_register_child(br_irq_domain_t *dom, br_u32 sub,
                          void (*isr)(void *), void *arg, const br_irq_attr_t *attr)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(BR_EINVAL);
    }
    if (dom == BR_NULL || dom->ops == BR_NULL || isr == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (sub >= (br_u32)dom->n_sub) {
        return BR_ERR(BR_EINVAL);
    }

    br_irq_desc_t *sd = domain_sub_desc(dom, sub);
    if (sd->isr != BR_NULL) {
        return BR_ERR(BR_EBUSY);            /* INV-A */
    }

    const br_u32 child_virq = (br_u32)dom->virq_base + sub;

    br_u32 flags = 0u;
    /* 域成员没有绑定表的静态 prio/trigger(§3.2: 子中断不是物理线)⇒ 默认就是
     * "未指定", 由域方言(domain_ops)自行决定落地的编码。 */
    br_u8 prio = (br_u8)BR_IRQ_PRIO_DEFAULT;
    br_u8 trigger = (br_u8)BR_IRQ_TRIG_DEFAULT;

    if (attr != BR_NULL) {
        flags = attr->flags;

        if ((flags & ~(br_u32)BR_IRQ_F_KNOWN_MASK) != 0u) {
            return BR_ERR(BR_EINVAL);       /* 未知位 ⇒ 拒绝(§3.4) */
        }
        if ((flags & BR_IRQ_F_SHARED) != 0u) {
            return BR_ERR(BR_ENOTSUP);      /* 共享线 = v1 非目标 */
        }
        /*
         * PERCPU 对域成员没有来源: 该位的权威是绑定表(§3.4), 而子中断没有绑定条目。
         * ⇒ 显式拒绝(绝不静默忽略一个请求的属性 —— 那正是 §3.4 反对的做法)。
         * 记 P-IRQ-12(实现裁定)。
         */
        if ((flags & BR_IRQ_F_PERCPU) != 0u) {
            return BR_ERR(BR_EINVAL);
        }
        /* 逐子的分发形态: **上下文由域类型决定**(FAST ⇒ ISR / SLOW ⇒ bh, ADR-0011),
         * 子中断的 `DISPATCH_BH/_THREAD` 没有额外语义 ⇒ 置位即忽略 + 留痕, 落库时清掉
         * (绝不静默: 静默会让"我明明请求了 bh"变成一个看不见的谎)。 */
        if ((flags & (BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD)) != 0u) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED, child_virq,
                          (br_u64)(flags & (br_u32)(BR_IRQ_F_DISPATCH_BH |
                                                    BR_IRQ_F_DISPATCH_THREAD)));
            flags &= ~(br_u32)(BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD);
        }
        if ((flags & BR_IRQ_F_STATS) != 0u && (BR_IRQ_STATS == 0)) {
            return BR_ERR(BR_ENOTSUP);
        }

        /* 域成员不经 PIC caps 校验 prio/trigger: 子中断的逐子配置全在 domain_ops
         * (mask/unmask/ack)一侧(§9.2), core 只记录逻辑值。 */
        if (attr->prio != (br_u8)BR_IRQ_PRIO_DEFAULT) {
            prio = attr->prio;
        }
        if (attr->trigger != (br_u8)BR_IRQ_TRIG_DEFAULT) {
            trigger = attr->trigger;
        }
    }

    /*
     * 编码合法域(§8.1)。
     * ★ 域成员与直连线的一个必要差别: 子中断**没有绑定表条目**(§3.2), 因此
     *   attr 未指定时 trigger 就停在 `BR_IRQ_TRIG_DEFAULT` —— 这表示"由域方言自行决定"
     *   (逐子配置全在 domain_ops 一侧, core 只记录逻辑值), 不是非法值。
     *   ⇒ 只对"显式给出的具体编码"做范围校验。记 P-IRQ-13。
     */
    if (trigger != (br_u8)BR_IRQ_TRIG_DEFAULT && trigger > (br_u8)BR_IRQ_TRIG_EDGE_BOTH) {
        return BR_ERR(BR_EINVAL);
    }

    /* 与直连 register 对称(§6.2): 注册 = depth 1 + 硬件屏蔽。
     * 父线在 enable 前本身也是屏蔽的, 这里仍按 L2 临界区写 depth/mask,
     * 保持与 enable_child/disable_child 的原子性一致。 */
    const br_irq_state_t st = br_irq_lock();
    sd->isr     = isr;
    sd->arg     = arg;
    sd->flags   = (br_u16)flags;
    sd->prio    = prio;
    sd->trigger = trigger;
    sd->depth   = 1u;
    if (dom->ops->mask != BR_NULL) {
        dom->ops->mask(dom->priv, sub);
    }
    br_irq_unlock(st);

    return BR_OK;
}

int br_irq_enable_child(br_irq_domain_t *dom, br_u32 sub)
{
    br_irq_desc_t *sd = BR_NULL;
    const int guard = child_api_guard(dom, sub, &sd);
    if (guard != BR_OK) {
        return guard;
    }

    /* depth 语义与直连族完全一致(§6.2): 只有 1→0 才真正 unmask */
    const br_irq_state_t st = br_irq_lock();
    if (sd->depth == 0u) {
        br_irq_unlock(st);
        return BR_OK;                       /* 已放行: 幂等成功 */
    }
    sd->depth--;
    if (sd->depth == 0u && dom->ops->unmask != BR_NULL) {
        dom->ops->unmask(dom->priv, sub);
    }
    br_irq_unlock(st);
    return BR_OK;
}

int br_irq_disable_child(br_irq_domain_t *dom, br_u32 sub)
{
    br_irq_desc_t *sd = BR_NULL;
    const int guard = child_api_guard(dom, sub, &sd);
    if (guard != BR_OK) {
        return guard;
    }

    const br_irq_state_t st = br_irq_lock();
    if (sd->depth == 0u && dom->ops->mask != BR_NULL) {
        dom->ops->mask(dom->priv, sub);     /* 0 → 1: 真正下线 */
    }
    sd->depth++;
    br_irq_unlock(st);
    return BR_OK;
}

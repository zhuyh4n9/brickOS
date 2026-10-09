/*
 * brickOS prototype v0.1.0 — 中断核心: 描述符池 / CPU-local / 生命周期 / 屏蔽 / 风暴
 *
 * 权威设计: `docs/3-os-core/3-02-int.md`
 *   §3.3 / §14.4  描述符池与运行期计数表(两个平行数组、同一 virq 下标)
 *   §5.2 / §5.3   生命周期: ack → ISR → **唯一 eoi 点**("已领到 ⇒ 必 eoi, 未领到 ⇒ 绝不 eoi")
 *   §6.1–§6.4     三层屏蔽; L1 depth 嵌套; L2 lock/unlock + 下溢护栏 + INV-C 出口断言
 *   §7.1 / IR-7   优先级语义(数值小 = 高; 位宽不足向**低优先级**饱和 + 留痕)
 *   §8.3          窗口化风暴判据(isr_seq 时基; 与频率无关, 只看占比)
 *   §4.2 / §4.3   能力协商与确定性降级; CAP_MASK=0 时"影子是最后一道闸门"的豁免
 *   §11.2         调度接缝 br_sched_irq_epilogue(Stage 1 = 空实现)
 *   §14.1         thread-only 的运行期执法(设计侧由静态扫描 + conformance 执法)
 *
 * 本文件不含任何控制器型号名(§1.3); 所有硬件动作都经 br_pic_ops(br_pic.h)。
 */
#include <br/core/br_error.h>
#include <br/core/br_exc.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_pic.h>
/* `br_sched_irq_epilogue()` 的声明与实现归调度侧(br_sched.h / core/src/sched/sched_core.c):
 * 本文件只在 IRQ 退出路径上**调用**它(调用点不动)。 */
#include <br/core/br_sched.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/* =====================================================================
 * 静态池(§3.3: 全部 BSS, 无运行期构造; RAM = 2 KB + 512 B @ 64 线)
 * ===================================================================== */

static br_irq_desc_t s_desc[BR_IRQ_MAX];   /* 配置与身份(布局入 golden) */
static br_irq_rt_t   s_rt[BR_IRQ_MAX];     /* 运行期计数(风暴游标恒在) */
static br_irq_cpu_t  s_cpu;                /* 本核中断状态(TPIDR_EL1 指向它) */

/* 累计计数器的统一约定(§14.4): **饱和不回绕** —— 回绕会让"计数变小"被误读成
 * "问题消失"; 饱和只需一条比较。连续量游标(rt->storm)不适用该规则。 */
static br_u32 sat_inc_u32(br_u32 v)
{
    return (v == 0xFFFFFFFFu) ? v : (v + 1u);
}

/* br_u32 → br_u16 的饱和收窄(仅用于 storm/epoch 这类窗口内小量; 见 br_irq_storm_check) */
static br_u16 sat_u16(br_u32 v)
{
    return (v > 0xFFFFu) ? (br_u16)0xFFFFu : (br_u16)v;
}

/* =====================================================================
 * CPU-local(§14.4; TPIDR_EL1 归 core 独占, 3-01 §2.2)
 * ===================================================================== */

br_irq_cpu_t *br_irq_cpu(void)
{
    return &s_cpu;
}

void br_irq_cpu_init(void)
{
    /*
     * ★ 调用时序(头文件 br_irq.h §7): BSS 清零之后、platform 提交绑定表之前。
     * 设计侧这一步属 core.init —— v0.2.0 由 core 的入口 `br_core_main()` 在启动链
     * 阶段 ① 调用(见 ADR-0008); 在这一刀之前它由 Platform Entry 的汇编直接调。
     * ★ **硬约束**: 本函数把**全部**描述符的 pic_id/dom_id 置 -1(P-IRQ-5), 所以必须
     *   排在 platform 的 `br_irq_bindings_set()` **之前** —— 顺序颠倒会把绑定抹掉。
     *
     * P-IRQ-5(实现裁定, 必须说明): "BSS 已清零 ⇒ 无其他构造"对 br_irq_cpu_t 成立,
     * 对**描述符池不成立** —— pic_id 与 dom_id 的 0 值都是**合法编码**
     * (0 号 PIC / 0 号域), 不是"无效"。若不显式初始化, 未绑定的描述符会
     * dom_id == 0 ⇒ br_irq_register 把"无绑定"误判成"域成员"返回 -EINVAL,
     * 而 §14.3 要求的 -ENODEV(初始化顺序违反要报对错)永远不会命中。
     * ⇒ 这里把两个索引字段置为内部约定的无效值 -1(§3.2: "内部用 -1/越界值表示无效")。
     */
    for (br_u32 v = 0u; v < BR_IRQ_MAX; v++) {
        s_desc[v].pic_id = (br_s8)-1;
        s_desc[v].dom_id = (br_s8)-1;
    }

    /* 冻结头 irq_internal.h 注明 in_handler 在 v1 恒 0xFFFF(BSS 零是 0)——
     * v1 无 fault handler 链(§10.5 注册 API 属 v2), 这里只为让诊断读数诚实。 */
    s_cpu.in_handler = BR_IRQ_NO_HANDLER;

    /* 把本核 CPU-local 挂到 TPIDR_EL1(3-02 §14.4「初始化时序」) */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&s_cpu) : "memory");
}

void br_irq_cpu_enable(void)
{
    /*
     * 全局开中断: 只放行 PSTATE.I(§14.3 链尾"全部插件 init 之后"; F/A/D 保持)。
     * 调用点是插件管理器在 LATE 相之后、START 相之前(见 plugin_mgr.c 的相位驱动)。
     */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

/* =====================================================================
 * 描述符 / 运行期表访问(§3.3; 越界与 BR_IRQ_INVALID 一律 NULL)
 * ===================================================================== */

br_irq_desc_t *br_irq_desc_of(br_u32 virq)
{
    if (virq >= BR_IRQ_MAX) {          /* BR_IRQ_INVALID(UINT32_MAX)也落在这里 */
        return BR_NULL;
    }
    return &s_desc[virq];
}

br_irq_rt_t *br_irq_rt_of(br_u32 virq)
{
    if (virq >= BR_IRQ_MAX) {
        return BR_NULL;
    }
    return &s_rt[virq];
}

int br_irq_in_isr(void)
{
    return (s_cpu.isr_nest > 0u) ? 1 : 0;
}

/* =====================================================================
 * L2: br_irq_lock / br_irq_unlock(§6.4; ISR-safe —— 不取锁/不 trace/不读时钟)
 * ===================================================================== */

/* br_irq_state_t 的位布局是 golden(§6.4): bit0..3 = 进入前 I/F/A/D, [17:4] = depth 副本 */
static br_irq_state_t encode_state(br_u32 raw_daif, br_u32 depth)
{
    br_irq_state_t st = 0u;

    if ((raw_daif & BR_DAIF_I) != 0u) {
        st |= BR_IRQ_ST_I_MASK;
    }
    if ((raw_daif & BR_DAIF_F) != 0u) {
        st |= BR_IRQ_ST_F_MASK;
    }
    if ((raw_daif & BR_DAIF_A) != 0u) {
        st |= BR_IRQ_ST_A_MASK;
    }
    if ((raw_daif & BR_DAIF_D) != 0u) {
        st |= BR_IRQ_ST_D_MASK;
    }

    st |= (br_irq_state_t)((depth & 0x3FFFu) << BR_IRQ_ST_DEPTH_SHIFT);
    return st;
}

/* 从 st 的 4 个屏蔽位**重建**方言 raw(下溢护栏仍要尊重 st 的 I 位, §6.4)。
 * ★ 这一步只对"raw DAIF 的位位置 = PSTATE 位位置"的方言(aarch64)成立;
 *   st 的 4 位按定义就是 BR_DAIF_* 的映射(见 encode_state), 故重建是自洽的。 */
static br_u32 raw_from_state(br_irq_state_t st)
{
    br_u32 raw = 0u;

    if ((st & BR_IRQ_ST_I_MASK) != 0u) {
        raw |= BR_DAIF_I;
    }
    if ((st & BR_IRQ_ST_F_MASK) != 0u) {
        raw |= BR_DAIF_F;
    }
    if ((st & BR_IRQ_ST_A_MASK) != 0u) {
        raw |= BR_DAIF_A;
    }
    if ((st & BR_IRQ_ST_D_MASK) != 0u) {
        raw |= BR_DAIF_D;
    }
    return raw;
}

static void restore_irq_raw(br_u32 raw)
{
    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);

    if (pic != BR_NULL && ops != BR_NULL && ops->cpu_unmask != BR_NULL) {
        ops->cpu_unmask(pic, raw);
    }
}

br_irq_state_t br_irq_lock(void)
{
    br_irq_cpu_t *cpu = &s_cpu;
    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    br_u32 raw;

    if (pic != BR_NULL && ops != BR_NULL && ops->cpu_mask != BR_NULL) {
        raw = ops->cpu_mask(pic);          /* L2 的真身 = 方言对 PSTATE.I 的包装(§6.1) */
    } else {
        /*
         * P-IRQ-6(实现裁定): PIC 尚未注册时(EARLY 相 §14.3 步 1 之前)没有方言可调,
         * 无法真正关中断。该窗口按设计就是"全局关中断"相, 故此处的合成状态取
         * **全部屏蔽位为 1**: 既符合事实(IRQ 确实关着), 又保证随后的 unlock
         * 不会把中断误开(若合成 I=0, unlock 会试图"恢复放行", 语义就反了)。
         */
        raw = BR_DAIF_I | BR_DAIF_F | BR_DAIF_A | BR_DAIF_D;
    }

    const br_irq_state_t st = encode_state(raw, cpu->irq_depth);

    /*
     * 最外层(0→1)记 raw: unlock 用它做"精确按进入前状态"的恢复(§6.4)。
     *
     * ★ P-IRQ-14(实现裁定/接口口径): 冻结头 irq_internal.h 对该字段的注释是
     *   "以 br_irq_state_t 编码", 但 br_pic_ops.cpu_unmask() 的入参是**方言 raw**
     *   (aarch64 = PSTATE.DAIF 位), 而 v1 的恢复动作就是把它原样交还方言
     *   ⇒ 这里存 raw 才能保证"恢复 = 精确回放"(存 st 编码还得再解码一次, 多一处错源)。
     *   字段宽度(u32)两者都装得下, 故不改冻结布局; 该注释的口径以本实现为准。
     */
    if (cpu->irq_depth == 0u) {
        cpu->saved_daif = raw;
    }
    cpu->irq_depth++;

    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    br_irq_cpu_t *cpu = &s_cpu;

    /* ★ 下溢护栏必须在最前(§6.4): depth 已是 0 时若再 --, uint32 会回绕成
     * 0xFFFFFFFF ⇒ 此后永远到不了 0 ⇒ unlock 再不恢复 ⇒ 该核中断**永久关闭**且无报错。 */
    if (cpu->irq_depth == 0u) {
        /* 留痕: 事件族里没有独立的"下溢"项(冻结 br_trace.h), 用 st 的 depth 副本作 a ——
         * 它正是判断"陈旧 unlock(副本非 0)"还是"纯多余 unlock(副本 0)"的依据(§6.4)。 */
        const br_u32 extra = (br_u32)((st & BR_IRQ_ST_DEPTH_MASK) >> BR_IRQ_ST_DEPTH_SHIFT);
        br_trace_emit(BR_TRACE_IRQ_UNLOCK_UNDERFLOW, extra, 0u);
        restore_irq_raw(raw_from_state(st));    /* 仍尊重 st 的 I 位, 不留悬挂状态 */
        return;
    }

    cpu->irq_depth--;
    if (cpu->irq_depth != 0u) {
        return;                                 /* ★ 内层 unlock 绝不开中断(TC-IRQ-004) */
    }

    /* v1 只管理 I(§6.4: F/A/D 原样保留); 用最外层 lock 捕获的 raw 精确恢复 */
    restore_irq_raw(cpu->saved_daif);
}

/* =====================================================================
 * 单线屏蔽(L1; §6.2 / §4.3)
 * ===================================================================== */

/*
 * ★ 只对**直连线**成立: 域成员的 pic_id 指"父 PIC"、hwirq 是域内子号,
 *   对它调本函数的后果是把子号当父 PIC 的 hwirq ⇒ **屏蔽错一根物理线**(§14.1)。
 *   故这里显式拒绝域成员; 域子中断的屏蔽走 br_irq_domain_ops.mask(irq_domain.c)。
 */
void br_irq_pgm_mask(br_irq_desc_t *d)
{
    if (d == BR_NULL || d->pic_id < 0 || d->dom_id >= 0) {
        return;
    }
    const br_u32 pic_id = (br_u32)d->pic_id;
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || caps == BR_NULL) {
        return;
    }
    /* §4.3 豁免条款: 无硬件屏蔽能力 ⇒ 纯软件影子是最后一道闸门, 这里什么都不做 */
    if ((caps->caps & BR_PIC_CAP_MASK) == 0u) {
        return;
    }
    if (ops->mask != BR_NULL) {
        ops->mask(pic, d->hwirq);
    }
}

void br_irq_pgm_unmask(br_irq_desc_t *d)
{
    if (d == BR_NULL || d->pic_id < 0 || d->dom_id >= 0) {
        return;
    }
    const br_u32 pic_id = (br_u32)d->pic_id;
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || caps == BR_NULL) {
        return;
    }
    if ((caps->caps & BR_PIC_CAP_MASK) == 0u) {
        return;
    }
    if (ops->unmask != BR_NULL) {
        ops->unmask(pic, d->hwirq);
    }
}

/* =====================================================================
 * §7.1 / IR-7: 能力降级与优先级量化
 * ===================================================================== */

/* 该触发方式能否被本控制器表达(§4.2: 缺失能力绝不静默改写, 宁 -ENOTSUP) */
static br_bool trigger_supported(const br_pic_caps_t *caps, br_u8 trigger)
{
    switch (trigger) {
    case BR_IRQ_TRIG_LEVEL_HIGH:
    case BR_IRQ_TRIG_LEVEL_LOW:
        return BR_TRUE;
    case BR_IRQ_TRIG_EDGE_RISE:
    case BR_IRQ_TRIG_EDGE_FALL:
        return ((caps->caps & BR_PIC_CAP_TRIG_EDGE) != 0u) ? BR_TRUE : BR_FALSE;
    case BR_IRQ_TRIG_EDGE_BOTH:
        return ((caps->caps & BR_PIC_CAP_TRIG_BOTH) != 0u) ? BR_TRUE : BR_FALSE;
    default:
        return BR_FALSE;
    }
}

/*
 * 优先级量化留痕(§7.1 的"两个方向都报"; 这里只可能向下量化)。
 *
 * ★ P-IRQ-3(实现裁定): 本处的 `step = 1 << (8 - prio_bits)` 假定**硬件编码 MSB 对齐**
 *   (GICv3: 3 位有效 ⇒ 可达值 {0x00,0x20,…,0xE0}), 因此"向上取整到 step 的倍数"
 *   = 逻辑优先级**向低优先级方向**饱和(绝不提权)。LSB 对齐的方言(NVIC: 连续 0..2^n-1)
 *   不能套这个式子 —— 它的 core 侧量化/编码应由方言自持, core 只看"小 = 高"的逻辑序
 *   (设计 §7.1: 逻辑→硬件编码的映射是方言义务)。v0.1 只接 GICv3(MSB 对齐), 故在此裁定。
 */
static void trace_prio_quantized(br_u32 virq, br_u8 prio, const br_pic_caps_t *caps)
{
    /* 前置: 调用者只在 CAP_PRIO 已置位时调用(无 CAP_PRIO 的降级留痕在 register 里做) */
    if (caps->prio_bits < 1u || caps->prio_bits > 7u) {
        return;     /* 位宽 0/≥8: step 无定义或恒为 1 ⇒ 不存在量化 */
    }

    const br_u32 step = 1u << (8u - (br_u32)caps->prio_bits);
    if (((br_u32)prio & (step - 1u)) == 0u) {
        return;     /* 已落在可实现档位上 */
    }

    /*
     * 向**数值大 = 优先级低**方向取整(取整到 step 的倍数), 绝不向下取整(那会提权)。
     * 注意 enc 用 br_u32 承载并**不窄化到 u8**: 当 prio 靠近 0xFF 时(如 prio=0xFF,
     * step=2)取整结果是 256, 已超出 8 位表示域 —— 这正说明"请求的档位在最低优先级之上
     * 仍不可表达"; 事件里如实记 256, 而不是回绕成 0(0 会读成"最高优先级", 是静默提权)。
     */
    const br_u32 enc = ((br_u32)prio + step - 1u) & ~(step - 1u);

    /* b = (encoded << 8) | logical(§br_trace.h 的事件语义) */
    br_trace_emit(BR_TRACE_IRQ_PRIO_QUANTIZED, virq,
                  ((br_u64)enc << 8) | (br_u64)prio);
}

/* =====================================================================
 * native API: register / enable / disable(§14.1; 全部 thread-only)
 * ===================================================================== */

/*
 * thread-only 的运行期执法(§14.1)。
 * 设计侧这条禁令由"静态扫描 + conformance 矩阵"执法(ISR 白名单, CA-3);
 * 原型在此**额外**加一道运行期守卫: 在 ISR 内调用直接拒绝 -EINVAL。
 * 理由: 单线屏蔽的配置路径可能持有 core 内部锁, 在 ISR 内取用即自锁(§5.3)。
 */
static int direct_api_guard(br_u32 irq, br_irq_desc_t **out)
{
    if (br_irq_in_isr() != 0) {
        return BR_ERR(BR_EINVAL);
    }
    br_irq_desc_t *d = br_irq_desc_of(irq);
    /* 越界 / 域成员(两族互斥, §14.1)/ 未注册 一律 -EINVAL */
    if (d == BR_NULL || d->dom_id >= 0 || d->isr == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    *out = d;
    return BR_OK;
}

int br_irq_register(br_u32 irq, void (*isr)(void *), void *arg,
                    const br_irq_attr_t *attr)
{
    if (br_irq_in_isr() != 0) {
        return BR_ERR(BR_EINVAL);
    }

    br_irq_desc_t *d = br_irq_desc_of(irq);
    if (d == BR_NULL || isr == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    /* 两族互斥(§14.1): 对域成员调直连 register 必须在**注册期**就拒绝,
     * 否则后面会把 sub 当 hwirq 去 mask 一根毫不相干的物理线。 */
    if (d->dom_id >= 0) {
        return BR_ERR(BR_EINVAL);
    }
    /* INV-A: 每根物理线恰一属主 */
    if (d->isr != BR_NULL) {
        return BR_ERR(BR_EBUSY);
    }
    /* §14.3: 无绑定 ⇒ -ENODEV(初始化顺序违反必须报对错, 不能"注册成功却永不响应") */
    if (d->pic_id < 0) {
        return BR_ERR(BR_ENODEV);
    }

    const br_u32 pic_id = (br_u32)d->pic_id;
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);
    if (pic == BR_NULL || ops == BR_NULL || caps == BR_NULL) {
        return BR_ERR(BR_ENODEV);
    }

    /* 绑定表带来的静态事实(§3.4: PERCPU "通常不用手填, 由绑定表推导") */
    const br_bool bound_percpu = ((d->flags & (br_u16)BR_IRQ_F_PERCPU) != 0u);

    br_u32 flags = 0u;
    br_u8  prio = d->prio;            /* 绑定表静态默认(attr.prio == DEFAULT 时生效) */
    br_u8  trigger = d->trigger;      /* 绑定表静态默认(attr.trigger == DEFAULT 时生效) */
    br_bool trigger_requested = BR_FALSE;

    if (attr != BR_NULL) {
        flags = attr->flags;

        /* ★ §3.4: 未知位置位 ⇒ -EINVAL(刻意不静默忽略: 否则新头文件编的插件
         * 在老 core 上会悄悄失去它请求的属性, 属最难查的兼容性 bug)。 */
        if ((flags & ~(br_u32)BR_IRQ_F_KNOWN_MASK) != 0u) {
            return BR_ERR(BR_EINVAL);
        }
        /* 共享线 = v1 非目标(§3.4) */
        if ((flags & BR_IRQ_F_SHARED) != 0u) {
            return BR_ERR(BR_ENOTSUP);
        }
        /* §3.4: 手填 PERCPU 与硬件(绑定表)不符 ⇒ -EINVAL(防"把 PPI 当 SPI 去均衡");
         * 反方向(绑定是 PPI 而驱动没填)合法 —— 该位由绑定表推导后补进描述符。 */
        if ((flags & BR_IRQ_F_PERCPU) != 0u && bound_percpu == BR_FALSE) {
            return BR_ERR(BR_EINVAL);
        }
        /* Stage 1 无 bh ⇒ BH/THREAD 分发"置位不报错但忽略", trace 提示(§3.4/§12.2)。
         * 这里同时把这两位从落库 flags 里清掉: 描述符不应声称一个 v0.1 不生效的形态。 */
        if ((flags & (BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD)) != 0u) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED, irq,
                          (br_u64)(flags & (br_u32)(BR_IRQ_F_DISPATCH_BH |
                                                    BR_IRQ_F_DISPATCH_THREAD)));
            flags &= ~(br_u32)(BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD);
        }
        if ((flags & BR_IRQ_F_STATS) != 0u && (BR_IRQ_STATS == 0)) {
            return BR_ERR(BR_ENOTSUP);       /* §3.4: 统计开关未开时置位 ⇒ -ENOTSUP */
        }

        /* `prio`/`trigger` 的解释权在 platform(§7.1/§8.1): DEFAULT ⇒ 用绑定静态值 */
        if (attr->prio != (br_u8)BR_IRQ_PRIO_DEFAULT) {
            prio = attr->prio;
        }
        if (attr->trigger != (br_u8)BR_IRQ_TRIG_DEFAULT) {
            trigger = attr->trigger;
            trigger_requested = BR_TRUE;
        }
    }

    /* 触发方式编码合法域(§8.1) */
    if (trigger > (br_u8)BR_IRQ_TRIG_EDGE_BOTH) {
        return BR_ERR(BR_EINVAL);
    }

    /*
     * ---- 能力降级(§4.2: 绝不静默改变语义, 要么"更弱但确定" + 留痕, 要么 -ENOTSUP)----
     *
     * ★ 触发方式的 caps 校验只针对**驱动显式请求**(attr.trigger != DEFAULT):
     *   §4.2 的原话是"attr.trigger 只能给 DEFAULT; 非 DEFAULT 请求 → -ENOTSUP"。
     *   绑定表里的静态 trigger 是 platform 对自己硬件的声明(§8.1: "板级信息"), 属可信数据 ——
     *   若对它也套 caps 校验, 一个 level-only 的控制器会因为绑定表写了 LEVEL_LOW 而
     *   连注册都做不了, 那就把"平台数据"与"驱动请求"混为一谈了。
     */
    if (trigger_requested == BR_TRUE &&
        (caps->caps & BR_PIC_CAP_TRIG_EDGE) == 0u) {
        return BR_ERR(BR_ENOTSUP);           /* 非 DEFAULT 请求而控制器无可编程触发 ⇒ 拒绝 */
    }
    if (trigger_requested == BR_TRUE && trigger == (br_u8)BR_IRQ_TRIG_EDGE_BOTH &&
        (caps->caps & BR_PIC_CAP_TRIG_BOTH) == 0u) {
        return BR_ERR(BR_ENOTSUP);
    }

    const br_bool caps_prio = ((caps->caps & BR_PIC_CAP_PRIO) != 0u) ? BR_TRUE : BR_FALSE;
    if (caps_prio == BR_FALSE) {
        /*
         * §4.2: 控制器不支持优先级 ⇒ attr.prio 被忽略, 但仍记录进描述符 + 留痕(不静默);
         * caps_prio==FALSE 时下面不调 set_prio。
         * ★ P-IRQ-7(实现裁定): 冻结的事件族里没有独立的"prio 不支持"项 ⇒ 复用
         *   PRIO_QUANTIZED 并把 encoded 记为 0(硬件不表达该优先级)。消费者应按
         *   "encoded == 0 且 logical != 0"识别出这是**能力缺失**而非量化。
         */
        br_trace_emit(BR_TRACE_IRQ_PRIO_QUANTIZED, irq, ((br_u64)0u << 8) | (br_u64)prio);
    }

    /* ---- 硬件配置必须在 L2 临界区内(§14.1: "配置 pic_ops.set_* 期间不得被本线中断打断";
     *      顺序遵循 §6.5 的"先 L2 后 L1") ---- */
    const br_irq_state_t st = br_irq_lock();
    int r = BR_OK;

    if (caps_prio == BR_TRUE && ops->set_prio != BR_NULL) {
        r = ops->set_prio(pic, d->hwirq, prio);
    }
    if (r == BR_OK && ops->set_trigger != BR_NULL &&
        trigger_supported(caps, trigger) == BR_TRUE) {
        r = ops->set_trigger(pic, d->hwirq, trigger);
    }
    br_irq_unlock(st);

    if (r != BR_OK) {
        return r;       /* 硬件配置失败 ⇒ 传播负 errno, 不注册(不留"半注册"状态) */
    }

    /* ---- §7.1 / IR-7: 优先级量化留痕(命中即报, 两个方向都报) ---- */
    if (caps_prio == BR_TRUE) {
        trace_prio_quantized(irq, prio, caps);
    }

    /* ---- 最后落实注册(§14.1; 此时才置 isr, 上面的失败路径不会留下属主) ----
     * hwirq/pic_id 在 br_irq_bindings_set 已填(§3.3 的绑定缓存), 此处不重复写。 */
    d->isr     = isr;
    d->arg     = arg;
    d->flags   = (br_u16)(flags | (bound_percpu == BR_TRUE ? (br_u32)BR_IRQ_F_PERCPU : 0u));
    d->prio    = prio;
    d->trigger = trigger;
    d->sub     = 0u;                 /* 直连线: 域子号恒 0 */
    d->depth   = 1u;                 /* §6.2 初值: 注册 = 屏蔽态 */

    /* §6.2: register 后硬件必须处于屏蔽态, 否则 enable 看到 depth==0 会直接返回 0,
     * 硬件永远不会被 unmask(经典回归点)。 */
    br_irq_pgm_mask(d);

    return BR_OK;
}

int br_irq_enable(br_u32 irq)
{
    br_irq_desc_t *d = BR_NULL;
    const int guard = direct_api_guard(irq, &d);
    if (guard != BR_OK) {
        return guard;
    }

    const br_irq_state_t st = br_irq_lock();
    if (d->depth == 0u) {
        br_irq_unlock(st);
        return BR_OK;                /* 已在放行态: 幂等成功(§6.2, 驱动 cleanup 友好) */
    }
    d->depth--;
    if (d->depth == 0u) {
        br_irq_pgm_unmask(d);        /* 1 → 0: 只有最外层才真正写硬件 */
    }
    br_irq_unlock(st);
    return BR_OK;
}

int br_irq_disable(br_u32 irq)
{
    br_irq_desc_t *d = BR_NULL;
    const int guard = direct_api_guard(irq, &d);
    if (guard != BR_OK) {
        return guard;
    }

    const br_irq_state_t st = br_irq_lock();   /* §6.5: 先 L2 挡住本核, 再改硬件 */
    if (d->depth == 0u) {
        br_irq_pgm_mask(d);                    /* 0 → 1: 真正下线 */
    }
    d->depth++;
    br_irq_unlock(st);
    return BR_OK;
}

/* =====================================================================
 * §8.3: 窗口化风暴检测(安全特性, 恒在 —— 不挂诊断开关)
 * ===================================================================== */

void br_irq_storm_check(br_irq_desc_t *d)
{
    if (d == BR_NULL) {
        return;
    }

    br_irq_cpu_t *cpu = &s_cpu;

    /* 逃生门(§3.4/§8.3): 已知且合法的高频线可豁免 */
    if ((d->flags & (br_u16)BR_IRQ_F_NO_STORM_GUARD) != 0u) {
        return;
    }

    /* 描述符指针 → virq(调用者只可能传池内描述符, 见 §14.2 的 hidden 契约) */
    const br_u32 virq = (br_u32)(d - &s_desc[0]);
    if (virq >= BR_IRQ_MAX) {
        return;
    }
    br_irq_rt_t *rt = &s_rt[virq];

    /*
     * ★ 窗口时基: br_irq_enter 的入口处已对 cpu->isr_seq 做一次饱和 +1(见 P-IRQ-8),
     *   本函数**只采样**, 不再递增。
     *   P-IRQ-8(实现裁定): 设计 §8.3 的伪码在**两处**都写了 `cpu->isr_seq++`
     *   (入口一处、每线窗口块一处); 若两处都做, 每次中断序号 +2, 窗口 256 会被压缩成
     *   ~128 次中断的等效窗口 —— 判据就从设计 §8.3 的"最近 256 次中断里占 64 次(25%)"
     *   漂成 ~50%。故裁定:**唯一递增点在 br_irq_enter 顶部**(它是"任何线进入"的
     *   单一事实点), 本函数保持"check"的语义。
     *
     * ★ epoch 是 uint16(冻结布局 §3.3), isr_seq 是 uint32 ⇒ 比较必须回到 16 位模运算域:
     *   P-IRQ-9(实现裁定): `(br_u16)(isr_seq - epoch)` 给出"自上次窗口起点的中断数(mod 2^16)",
     *   窗口仅 256 ≪ 32768 ⇒ 模运算结果无歧义。若按 32 位直接比较(isr_seq - epoch),
     *   epoch 被当成"绝对小数字", 一旦 isr_seq 超过 ~65792 条件恒真 ⇒ storm 每次都被
     *   复位成 1 ⇒ **检测器永久失效**(正是 §8.3 警告的那类"恒失效")。
     */
    const br_u16 now = sat_u16(cpu->isr_seq);
    if ((br_u16)(now - rt->epoch) > (br_u16)BR_IRQ_STORM_WINDOW) {
        rt->epoch = now;                 /* 窗口过期: 重新开始计数 */
        rt->storm = 1u;
    } else {
        rt->storm++;                     /* 窗口内的第 N 次 */
    }

    /* 只在"越过阈值的那一次"上报(rt->storm == LIMIT + 1), 不是每次越限都刷屏 */
    if (rt->storm == (br_u16)(BR_IRQ_STORM_LIMIT + 1u)) {
        cpu->storm_total = sat_inc_u32(cpu->storm_total);
        br_trace_emit(BR_TRACE_IRQ_STORM, virq, (br_u64)rt->storm);
#if BR_IRQ_STORM_GUARD
        /* 默认关(§8.3/IR-9): 自动 mask 把"活锁"变成"该中断静默失效", 更难查。
         * "可观测的持续错误优于静默的功能丧失" ⇒ 默认只计数 + 留痕。 */
        br_irq_pgm_mask(d);
#endif
    }
}

/* =====================================================================
 * 中断入口(§5.2: ack → 分发 → ISR → **统一出口 eoi**; asm 桩调用)
 * ===================================================================== */

void br_irq_enter(br_exc_frame_t *frame)
{
    /* v0.1 不使用异常帧(ISR 一律返回被打断的现场, 不切栈);
     * Stage 2 的"IRQ 出口切栈"(§11.2)才会用到 frame 里的现场。 */
    (void)frame;

    br_irq_cpu_t *cpu = &s_cpu;

    /* 窗口时基(§8.3): 每次 IRQ 入口饱和 +1 —— 唯一递增点(见 br_irq_storm_check 的 P-IRQ-8) */
    cpu->isr_seq = sat_inc_u32(cpu->isr_seq);

    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || ops->ack == BR_NULL) {
        /* 没有 CPU 接口 PIC: 无中断可领(初始化顺序违反, §14.3)⇒ 绝不 eoi */
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_NOCAP, BR_PIC_NO_IRQ, 0u);
        return;
    }

    const br_u32 hwirq = ops->ack(pic);      /* Pending → Active(IAR 读一次即够, §4.1.1) */
    if (hwirq == BR_PIC_NO_IRQ) {
        /* ★ 未领到 ⇒ 绝不 eoi(误写会破坏其他线的状态机, §5.3 判据表第一行) */
        cpu->spurious_nocap = sat_inc_u32(cpu->spurious_nocap);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_NOCAP, hwirq, 0u);
        return;
    }

    br_u32 virq = 0u;
    br_irq_desc_t *d = BR_NULL;
    br_u32 served_virq = BR_IRQ_NO_HANDLER;

    if (br_irq_virq_of(pic_id, hwirq, &virq) != BR_OK) {
        /* 领到了但无绑定(§5.3 判据表第三行)⇒ 跳过 ISR, 仍必 eoi */
        cpu->spurious_owned = sat_inc_u32(cpu->spurious_owned);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_OWNED, hwirq, 0u);
        goto EOI;
    }

    d = br_irq_desc_of(virq);
    if (d == BR_NULL || d->isr == BR_NULL) {
        cpu->spurious_owned = sat_inc_u32(cpu->spurious_owned);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_OWNED, hwirq, 0u);
        goto EOI;
    }

    /*
     * ★ §4.3 豁免条款 —— 唯一允许影子参与"是否调用 ISR"的位置:
     *   仅当控制器**无硬件屏蔽能力**(CAP_MASK=0)且该线处于软件屏蔽态(depth>0)。
     *   有硬件屏蔽能力时"已屏蔽却仍到达"才是真 bug 信号(spurious_owned)。
     *   该分支**必须 eoi**(已经 ack 过; 否则线永久 Active ⇒ running priority 抬高
     *   ⇒ 全系统 IRQ 饥饿)。
     */
    if (((caps == BR_NULL) || ((caps->caps & BR_PIC_CAP_MASK) == 0u)) && d->depth > 0u) {
        cpu->spurious_nocap = sat_inc_u32(cpu->spurious_nocap);
        br_trace_emit(BR_TRACE_IRQ_SUPPRESSED, virq, 0u);
        goto EOI;
    }

#if BR_IRQ_STATS
    {
        br_irq_rt_t *rt = br_irq_rt_of(virq);
        if (rt != BR_NULL) {
            rt->count = sat_inc_u32(rt->count);      /* §14.4: 累计计数器饱和 */
        }
    }
#endif

    cpu->curr_virq = (br_u16)virq;
    cpu->isr_nest++;
    served_virq = virq;
    br_trace_emit(BR_TRACE_IRQ_ENTER, virq, (br_u64)hwirq);

    d->isr(d->arg);                     /* ISR 契约: 最小工作 + 禁阻塞/malloc/持锁返回(§1.2) */

    cpu->isr_nest--;
    cpu->curr_virq = BR_IRQ_NO_HANDLER;
    br_irq_storm_check(d);
    br_trace_emit(BR_TRACE_IRQ_EXIT, virq, 0u);

EOI:
    /* ★ 唯一 eoi 点: 凡"已领到"必到这里(正常 / 无效 virq / 未注册 / 被抑制);
     *   "未领到"在上面两处已直接返回 ⇒ 结构上免除"驱动忘了 eoi"(§5.3/INV-H)。 */
    ops->eoi(pic, hwirq);

    /* ---- OUT(§5.2 出口): INV-C 断言 + 调度接缝 ---- */
    if (cpu->irq_depth != 0u) {
        /*
         * INV-C(§6.4): "禁持锁跨 ISR 返回"的运行时执法。v1 不嵌套(IR-6)⇒ 进入 ISR 时
         * 必为 0(持锁期间 IRQ 被屏蔽), 出口非 0 只可能来自"本 ISR 内取了锁没还"。
         */
#ifdef BR_DEBUG
        br_panic_bare("INV-C violated: irq_depth=%u leaked across IRQ exit (virq=%u)",
                      (br_u32)cpu->irq_depth, (br_u32)served_virq);
#else
        /*
         * release 自愈(§6.4 的 IR-15 修订): 泄漏的 L2 锁留在**每核**的 irq_depth,
         * 不 panic 也会静默让此后所有线程在关中断下跑 ⇒ 正确动作是"修好并报告":
         * 强制归零 + 恢复中断 + 留痕。
         */
        const br_u32 leaked = cpu->irq_depth;
        cpu->irq_depth = 0u;
        /* v0.1 只恢复 I: raw=0 ⇒ 放行 IRQ(cpu_unmask 只碰 I 位, §6.4/§4.1)。
         * ★ P-IRQ-10(实现裁定): 设计说"按入口 DAIF 恢复", 帧里的 daif 正是为此准备的;
         *   但 v0.1 把 frame 标为未用(ERET 由 SPSR 恢复现场), 故此处置 raw=0。
         *   等 Stage 2 真用帧时改为 frame->daif 的低 4 位即可(布局已留好)。 */
        if (ops->cpu_unmask != BR_NULL) {
            ops->cpu_unmask(pic, 0u);
        }
        /* 事件族没有独立的 INV-C 项(冻结 br_trace.h)⇒ 借"下溢"这一 depth 违约事件记录;
         * b 记下"泄漏发生在哪条线的 ISR 里"(curr_virq 已按 §5.2 复位, 故用局部副本)。 */
        br_trace_emit(BR_TRACE_IRQ_UNLOCK_UNDERFLOW, leaked, (br_u64)served_virq);
#endif
    }

    br_sched_irq_epilogue();
}

/* =====================================================================
 * 跨模块观测契约(§14.2; 消费方 = debug bridge / ramdump 插件, 故不能 hidden)
 * ===================================================================== */

void br_irq_stats_get(br_u32 virq, br_irq_stat_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    /* 先清零: 取不到(virq 越界 / 无绑定)就返回全 0(§14.4 的 br_irq_stat_t 语义) */
    out->count          = 0u;
    out->storm          = 0u;
    out->depth          = 0u;
    out->spurious_owned = 0u;
    out->spurious_nocap = 0u;
    out->drop           = 0u;
    out->flags          = 0u;
    out->prio           = 0u;
    out->trigger        = 0u;

    br_irq_desc_t *d = br_irq_desc_of(virq);
    if (d == BR_NULL || d->pic_id < 0) {
        return;                         /* 越界 或 无绑定 ⇒ 全 0 */
    }

    br_irq_rt_t *rt = br_irq_rt_of(virq);

    out->count = (BR_IRQ_STATS != 0 && rt != BR_NULL) ? rt->count : 0u;
    out->storm = (rt != BR_NULL) ? rt->storm : 0u;
    out->depth = d->depth;

    /* §4.2: 两个 spurious 计数在 CPU-local(§14.4); v1 单核 ⇒ 即全局 */
    out->spurious_owned = s_cpu.spurious_owned;
    out->spurious_nocap = s_cpu.spurious_nocap;

    /* drop 只对 SLOW 域成员有意义(§9.4), 而 SLOW 域在 Stage 1 被 domain_create 拒绝
     * ⇒ v0.1 恒 0(域池是 irq_domain.c 的静态对象, 这里也不必跨 TU 取它)。 */
    out->drop = 0u;

    out->flags   = d->flags;
    out->prio    = d->prio;
    out->trigger = d->trigger;
}

/*
 * brickOS prototype v0.1.0 — PIC 注册表 + 绑定表 + hwirq→virq 索引
 *
 * 权威设计: `docs/3-os-core/3-02-int.md`
 *   §4.1 / §4.2  PIC ops 填表与能力位(IR-3); "每 CPU 恰一个 CPU 接口控制器"的推论
 *   §14.3        platform 侧契约与**初始化单向链**(1 注册 PIC → 2 caps → 3 绑定表 → 4-7 驱动)
 *   §14.2        热路径 hwirq→virq: **有序表 + 二分**(4 B/绑定), 不用 virq_of[nr_irq] 直接表
 *   §3.3/§3.4    绑定表条目(platform .rodata)与 attr.flags 语义
 *   §3.2         宽度不变量: 公共签名 br_u32, 内部字段 int8/uint16
 *
 * 本文件只做"注册 + 解析表"两件事, 不含任何控制器型号名(§1.3 铁律)。
 * 描述符池归 irq_core.c; 这里经 br_irq_desc_of()(irq_internal.h)访问它。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_pic.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/* =====================================================================
 * PIC 实例池(§4.1: core 只持指针, 实例存储归 platform)
 * ===================================================================== */

static struct {
    br_pic_t           *pic;
    const br_pic_ops_t *ops;
    br_pic_caps_t       caps;
} s_pics[BR_PIC_MAX];

static br_u32  s_pic_n;
/* ★ IR-3 推论(§4.2): ack() 无参数 ⇒ 每个 CPU 恰一个"CPU 接口控制器"。
 * 第一个注册成功的 PIC 记为它, 此后**永不改变**(其余实例只能经级联域接入)。 */
static br_u32  s_cpu_pic = BR_IRQ_INVALID;
static br_bool s_bindings_set;

/*
 * hwirq→virq 有序索引(§14.2 的"有序表 + 二分"形态)。
 * 排序键 = (pic_id, hwirq); pic_id 入键是因为多实例共存(§4.2 的多控制器接入规则)。
 *
 * P-IRQ-4(实现裁定): 表项 = 8 B 而不是设计 §14.2 写的 4 B —— 因为 v0.1 的表项
 * 把 pic_id 与 pad 一起放进来了(2+1+1+4 = 8)。设计那句 4 B 对应的是
 * "只存 (virq, hwirq)"的最小形态, 而它要求"每实例一张表"或 sort 键里不含 pic_id。
 * v0.1 选择单表 + (pic_id, hwirq) 复合键: 表项翻倍, 但绑定数 ≤ BR_IRQ_MAX 时
 * 最多 512 B, 且省掉"每个 PIC 一张表"的静态池管理。
 */
static struct {
    br_u16 virq;     /* 命中后写出的 virq */
    br_u8  pic_id;   /* ★ 复合键第一段 */
    br_u8  pad;      /* 显式对齐(消填充歧义) */
    br_u32 hwirq;    /* ★ 复合键第二段 */
} s_index[BR_IRQ_MAX];

static br_u32 s_index_n;

/* =====================================================================
 * 小工具(无 libc: 自持字符串比较)
 * ===================================================================== */

static br_bool str_eq(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

/* 按 (pic_id, hwirq) 升序插入(n 很小, 插入排序足够; §14.2 只说"有序"不说算法) */
static void index_insert(br_u32 pic_id, br_u32 hwirq, br_u32 virq)
{
    br_u32 pos = s_index_n;

    while (pos > 0u) {
        const br_u32 pp = (br_u32)s_index[pos - 1u].pic_id;
        const br_u32 ph = s_index[pos - 1u].hwirq;
        if (pp < pic_id || (pp == pic_id && ph <= hwirq)) {
            break;
        }
        s_index[pos] = s_index[pos - 1u];
        pos--;
    }

    s_index[pos].virq   = (br_u16)virq;
    s_index[pos].pic_id = (br_u8)pic_id;
    s_index[pos].pad    = 0u;
    s_index[pos].hwirq  = hwirq;
    s_index_n++;
}

/* =====================================================================
 * 池访问器(irq_internal.h; 越界/未注册一律 NULL)
 * ===================================================================== */

br_pic_t *br_irq_pic(br_u32 pic_id)
{
    return (pic_id < s_pic_n) ? s_pics[pic_id].pic : BR_NULL;
}

const br_pic_ops_t *br_irq_pic_ops(br_u32 pic_id)
{
    return (pic_id < s_pic_n) ? s_pics[pic_id].ops : BR_NULL;
}

const br_pic_caps_t *br_irq_pic_caps(br_u32 pic_id)
{
    return (pic_id < s_pic_n) ? &s_pics[pic_id].caps : BR_NULL;
}

br_u32 br_irq_pic_count(void)
{
    return s_pic_n;
}

br_u32 br_irq_cpu_pic_id(void)
{
    return s_cpu_pic;
}

/* =====================================================================
 * 注册(§14.3 初始化链第 1-2 步; EARLY 相, 全局关中断)
 * ===================================================================== */

int br_pic_register(br_pic_t *pic, const br_pic_ops_t *ops)
{
    /* ---- 必填槽位(§4.1 的 ops 表): 缺一个都说明方言实现不完整, 必须大声失败 ---- */
    if (pic == BR_NULL || ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (ops->name == BR_NULL || ops->ack == BR_NULL || ops->eoi == BR_NULL ||
        ops->caps_get == BR_NULL || ops->cpu_mask == BR_NULL ||
        ops->cpu_unmask == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    if (s_pic_n >= (br_u32)BR_PIC_MAX) {
        return BR_ERR(BR_ENOSPC);
    }

    /* 重复注册: 同一实例指针, 或同一方言名(§4.1: "每实例一次") */
    for (br_u32 i = 0; i < s_pic_n; i++) {
        if (s_pics[i].pic == pic || str_eq(s_pics[i].ops->name, ops->name) == BR_TRUE) {
            return BR_ERR(BR_EBUSY);
        }
    }

    /* init/caps_get 可能失败 ⇒ EARLY 相失败 = 启动失败(§14.3 错误处理义务):
     * 这里**传播**负 errno, 绝不"静默返回 0 但没配好"。 */
    if (ops->init != BR_NULL) {
        const int r = ops->init(pic);
        if (r != BR_OK) {
            return r;
        }
    }

    br_pic_caps_t caps;
    caps.prio_bits = 0u;
    caps.nr_irq    = 0u;
    caps.caps      = 0u;
    {
        const int r = ops->caps_get(pic, &caps);
        if (r != BR_OK) {
            return r;
        }
    }
    /* nr_irq 的"合理"判据: 至少一根线。0 线的控制器注册进来只会让
     * br_irq_bindings_set 的 hwirq < caps.nr_irq 校验变得不可满足 ⇒ 早报错。 */
    if (caps.nr_irq == 0u) {
        return BR_ERR(BR_EINVAL);
    }

    /* mask/unmask 只在 CAP_MASK 会被置位时才必填(§4.2: 无硬件屏蔽能力 ⇒
     * 退化为纯软件影子屏蔽, 那时这两个槽位没有意义)。⇒ 该校验必须在 caps_get 之后。 */
    if ((caps.caps & BR_PIC_CAP_MASK) != 0u &&
        (ops->mask == BR_NULL || ops->unmask == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u32 pic_id = s_pic_n;
    s_pics[pic_id].pic  = pic;
    s_pics[pic_id].ops  = ops;
    s_pics[pic_id].caps = caps;
    s_pic_n++;

    /* ★ 第一个成功注册的 PIC = CPU 接口(IR-3 推论); 之后不再改。 */
    if (s_cpu_pic == BR_IRQ_INVALID) {
        s_cpu_pic = pic_id;
    }

    return (int)pic_id;
}

int br_pic_caps_of(br_u32 pic_id, br_pic_caps_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (pic_id >= s_pic_n) {
        return BR_ERR(BR_ENODEV);
    }
    *out = s_pics[pic_id].caps;
    return BR_OK;
}

/* =====================================================================
 * 绑定表(§3.3; platform 一次性提交, EARLY 相恰一次)
 * ===================================================================== */

int br_irq_bindings_set(const br_irq_binding_t *tbl, br_size_t n)
{
    if (s_bindings_set == BR_TRUE) {
        return BR_ERR(BR_EBUSY);                    /* 恰一次(§14.3) */
    }
    if (tbl == BR_NULL || n == 0u || n > (br_size_t)BR_IRQ_MAX) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u32 cnt = (br_u32)n;

    /* ---- 第一遍: 全量校验(全成功才写状态, 避免"半张表生效") ---- */
    for (br_u32 i = 0u; i < cnt; i++) {
        const br_irq_binding_t *b = &tbl[i];

        /* 公共签名是 br_u32, 内部 virq 是 br_u16 ⇒ 入口必须校验合法域(§3.2 宽度不变量) */
        if ((br_u32)b->virq >= BR_IRQ_MAX) {
            return BR_ERR(BR_EINVAL);
        }
        /*
         * P-IRQ-1(实现裁定): v0.1 的绑定表**只覆盖直连线**。域成员的描述符由
         * br_irq_domain_create() 的窗口切片填充(§3.2: 子中断"不是物理线",
         * 语义上是域的成员)⇒ 绑定条目带 dom_id >= 0 属 schema 误用, 必须拒绝。
         */
        if (b->dom_id != (br_s8)-1) {
            return BR_ERR(BR_EINVAL);
        }
        /* §14.3: 初始化顺序违反要**大声失败**(PIC 未注册 ⇒ 引用的 pic_id 无意义),
         * 绝不静默接受一个永远解析不到控制器的绑定。 */
        if ((br_u32)b->pic_id >= s_pic_n) {
            return BR_ERR(BR_ENODEV);
        }
        if (b->hwirq >= s_pics[b->pic_id].caps.nr_irq) {
            return BR_ERR(BR_EINVAL);
        }
        /* 重复: 同一 virq(INV-E/INV-A), 或同一 (pic_id, hwirq) 被绑两次(INV-A:
         * 每根物理线恰一属主) */
        for (br_u32 j = 0u; j < i; j++) {
            if (tbl[j].virq == b->virq) {
                return BR_ERR(BR_EINVAL);
            }
            if (tbl[j].pic_id == b->pic_id && tbl[j].hwirq == b->hwirq) {
                return BR_ERR(BR_EINVAL);
            }
        }
    }

    /* ---- 第二遍: 落实(填描述符 + 硬件默认屏蔽 + 建有序索引) ---- */
    for (br_u32 i = 0u; i < cnt; i++) {
        const br_irq_binding_t *b = &tbl[i];
        br_irq_desc_t *d = br_irq_desc_of((br_u32)b->virq);
        if (d == BR_NULL) {
            return BR_ERR(BR_EINVAL);               /* 第一遍已排除, 纯防御 */
        }

        /* 身份与绑定缓存(§3.3): isr/arg 留 NULL(未注册), depth/sub 留 0,
         * dom_id 保持 -1(直连; 由 br_irq_cpu_init 初始化为 -1 —— 见 P-IRQ-5)。
         *
         * ★ flags 的特例(§3.4): PERCPU 的权威来源是**绑定表**(“通常不用手填”)。
         *   描述符没有独立的"静态绑定属性"位域, 所以这里把静态 PERCPU 折进运行期
         *   flags 的对应位; br_irq_register 再拿它与 attr 请求做一致性校验。
         *   其余 flags 位仍留 0(等待 register 填)。 */
        d->hwirq  = b->hwirq;
        d->pic_id = (br_s8)b->pic_id;
        d->prio   = b->prio;
        d->trigger = b->trigger;
        /* 绑定条目只允许直连线(上面的 P-IRQ-1 校验)⇒ 内部域索引归一化为 -1,
         * 使 br_irq_register/desc 的"直连"判据不依赖 br_irq_cpu_init 是否先跑。 */
        d->dom_id = (br_s8)-1;
        d->flags  = ((b->flags & (br_u8)BR_IRQB_F_PERCPU) != 0u)
                        ? (br_u16)BR_IRQ_F_PERCPU : (br_u16)0u;

        /* 安全默认(§14.3 第 7 步"7 之前的线一律收不到中断"): 绑定即硬件屏蔽。
         * 无 CAP_MASK 的控制器由 br_irq_pgm_mask 自己跳过(§4.3 豁免)。 */
        br_irq_pgm_mask(d);

        /*
         * 分发形态(§12.2: 默认载体是**绑定表**(platform 数据, flash))。
         *   INLINE ⇒ 常态(热路径直接调 ISR);
         *   BH     ⇒ ★ ADR-0011: 折进运行期 flags 的 DISPATCH_BH 位当**真值**, 该线的
         *            ISR 会被推迟到下半部执行(处理期间硬件线被 mask);
         *   THREAD / 其它 ⇒ 仍未实现(线程化 IRQ 属 Stage 2)⇒ 接受但忽略 + 留痕(§3.4)。
         */
        if (b->dispatch == (br_u8)BR_IRQ_DISPATCH_BH) {
            d->flags |= (br_u16)BR_IRQ_F_DISPATCH_BH;
        } else if (b->dispatch != (br_u8)BR_IRQ_DISPATCH_INLINE) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED,
                          (br_u32)b->virq, (br_u64)b->dispatch);
        }

        index_insert((br_u32)b->pic_id, b->hwirq, (br_u32)b->virq);
    }

    s_bindings_set = BR_TRUE;
    return BR_OK;
}

/* =====================================================================
 * 热路径: hwirq → virq(§5.2 入口桩的 lookup; 只读, ISR-safe)
 * ===================================================================== */

int br_irq_virq_of(br_u32 pic_id, br_u32 hwirq, br_u32 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    /* 在按 (pic_id, hwirq) 升序的 s_index 上做下界二分(lower_bound) */
    br_u32 lo = 0u;
    br_u32 hi = s_index_n;
    while (lo < hi) {
        const br_u32 mid = lo + ((hi - lo) >> 1);
        const br_u32 mp  = (br_u32)s_index[mid].pic_id;
        const br_u32 mh  = s_index[mid].hwirq;
        if (mp < pic_id || (mp == pic_id && mh < hwirq)) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    if (lo < s_index_n && (br_u32)s_index[lo].pic_id == pic_id &&
        s_index[lo].hwirq == hwirq) {
        *out = (br_u32)s_index[lo].virq;
        return BR_OK;
    }

    /* 未命中 ⇒ §5.2 的 spurious 分槽(有硬件屏蔽 ⇒ spurious_owned, 真 bug 信号) */
    return BR_ERR(BR_ENODEV);
}

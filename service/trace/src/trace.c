/*
 * service/trace — trace 观测服务(实现)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md`
 *   §1 trace 插件: 定长 16B 事件 + 静态环形缓冲 + **热路径零字符串**
 *      ("id→名字的映射表随插件元数据导出, 离线解码");
 *   §1 "泄放途径: panic 时 console 倒出 / bridge 实时流 / host 工具解码"
 *      —— 本插件就是"倒出"这一侧的消费端;
 *   §1 API 一节把事件宏与 id 分配留给 manifest, 本实现对**动态 id** 取
 *      32 槽 hash 表(声明面已冻结, 见 include/br/debug/br_trace_svc.h)。
 * 归属裁定(ADR-0003): 16B 事件的**环与 ISR 发射点在 core**
 *   (`br/core/br_trace.h` 的 `br_trace_emit`), 本插件是它的唯一消费方:
 *   取走(drain)、id→名字解码、计数、成行呈现。理由见 plugin.toml 的抬头注释。
 *
 * ★ 名字表的前提与风险(声明面明确写成 `const char *`, 这里如实落地):
 *   槽位里**只存调用方给的指针, 不拷贝字符串** —— 契约是"tag 的生命周期 = 映像"
 *   (调用方通常传字符串字面量或静态数组)。若调用方传栈上/堆上的临时缓冲,
 *   指针会悬空; 本原型没有插件管理器来强制这条前提, 所以由调用纪律承担。
 *   身份判据用**指针相等**: 同一字面量在同一编译单元内会被合并(-fmerge-constants),
 *   所以"同一处 tag 重复 marker"幂等; 而"内容相同但指针不同"说明调用方给了两个
 *   不同缓冲, 身份不可判定(也破坏了上面的生命周期前提) ⇒ -ENOSPC。
 *   **地址撞槽不报错**: 从 `hash % SLOTS` 起**向后线性探测**空槽(环绕, 最多扫全表)。
 *   为什么不是"撞车即错": 32 槽表上一次撞车就失败, 会让**谁先注册**决定别人能不能
 *   注册 —— 别的插件的 marker 能凭 hash 挤掉本插件 selftest 的槽位, 让用例无辜
 *   变红(测试依赖哈希槽位是设计味道)。线性探测只在撞车时退化为最多 32 步扫描,
 *   寻址仍是 hash O(1), 对 32 槽的表完全可接受。见 README 的"关键裁定"。
 *
 * ISR 安全: 本文件全部函数 **thread-only**(drain 会推进消费游标)。ISR/fault
 *   路径只允许直接调 core 的 `br_trace_emit`。
 */
#include <br/debug/br_trace_svc.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>

#include "trace_internal.h"    /* 自检用的内部视图(见该头; 生产路径不用它) */

/* =====================================================================
 * 常量与内部状态
 * ===================================================================== */

/* FNV-1a 32 位: 与 5-01 §1 的"热路径零字符串"配套 —— 事件里只带 32 位 hash,
 * 名字留在这张表里, 供在线呈现或随元数据离线解码。 */
#define TRC_FNV_OFFSET   2166136261u
#define TRC_FNV_PRIME    16777619u

/* 每次 drain 的批量(固定静态占用; 16B × 16 = 256B) */
#define TRC_DRAIN_BATCH  16u

/* "已消费"计数表: 64 项线性表; 表满后的余量并入 s_other 桶(summary 尾随一行体现) */
#define TRC_SEEN_SLOTS   64u

/* 动态名字表: 只在槽位放下指针, 不拥有字符串(前提见文件头) */
static const char *s_names[BR_TRACE_SVC_ID_SLOTS];

/* 按 id 累计的"已消费"计数(线性查找; 观测与用例判据) */
static br_u32 s_seen_id[TRC_SEEN_SLOTS];
static br_u32 s_seen_cnt[TRC_SEEN_SLOTS];
static br_u32 s_seen_n;      /* 已登记的不同 id 数(= summary 的 ids) */
static br_u32 s_other;       /* 计数表满后并入"其它"桶的条数 */

/* 本插件累计取走的条数; 同时充当 [TRACE] 行的消费序号 */
static br_u32 s_drained;

/* drain 批量缓冲(线程上下文单消费者, 无需加锁) */
static br_trace_evt_t s_batch[TRC_DRAIN_BATCH];

/* 本插件自有事件名(静态数组 ⇒ 指针恒等, init 可重复调用) */
static const char TRC_NAME_REPORT[] = "trace.report";
static const char TRC_NAME_RESET[]  = "trace.reset";

/* =====================================================================
 * 小工具(不引 libc)
 * ===================================================================== */

/* FNV-1a 32 位; 按字节走, 故先转 br_u8(避免 char 的有符号性影响结果) */
static br_u32 trc_fnv1a32(const char *s)
{
    br_u32 h = TRC_FNV_OFFSET;

    for (const char *p = s; *p != '\0'; p++) {
        h ^= (br_u32)(br_u8)*p;
        h *= TRC_FNV_PRIME;
    }
    return h;
}

static br_u32 trc_slot_of(const char *name)
{
    return trc_fnv1a32(name) % BR_TRACE_SVC_ID_SLOTS;
}

static br_bool trc_streq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b);   /* 同时停在 '\0' 才相等 */
}

/* =====================================================================
 * 名字注册 / 解码
 * ===================================================================== */

/*
 * 取槽: 从 `hash % SLOTS` 起**向后线性探测**(环绕, 最多 SLOTS 步)。
 *   - 空槽 ⇒ 占位(只存指针, 不拷贝字符串)并返回槽位下标;
 *   - 命中同指针 ⇒ 幂等, 返回既有槽位;
 *   - 命中**内容相同但指针不同** ⇒ -ENOSPC(调用方给了两个不同缓冲, 身份不可判定,
 *     也破坏了"tag 生命周期 = 映像"的前提);
 *   - 扫完全表仍无空槽 ⇒ -ENOSPC。
 * ★ 为什么不是"撞车即 -ENOSPC": 32 槽表上一次撞车就失败, 会让**谁先注册**决定
 *   别人能不能注册 —— 别的插件的 marker 能凭 hash 挤掉本插件 selftest 的槽位,
 *   让用例无辜变红(测试依赖哈希槽位是设计味道)。线性探测只在撞车时退化为最多
 *   32 步扫描, 寻址仍是 hash O(1), 对 32 槽的表完全可接受。
 * 返回槽位下标(0..SLOTS-1)或负 errno。
 */
static int trc_acquire_slot(const char *name)
{
    if (name == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u32 start = trc_slot_of(name);

    for (br_u32 step = 0u; step < BR_TRACE_SVC_ID_SLOTS; step++) {
        const br_u32 slot = (start + step) % BR_TRACE_SVC_ID_SLOTS;

        if (s_names[slot] == BR_NULL) {
            s_names[slot] = name;          /* 只存指针, 不拷贝 */
            return (int)slot;
        }
        if (s_names[slot] == name) {       /* 同指针 ⇒ 幂等 */
            return (int)slot;
        }
        if (trc_streq(s_names[slot], name)) {
            return BR_ERR(BR_ENOSPC);      /* 同名(内容)但指针不同 */
        }
    }
    return BR_ERR(BR_ENOSPC);              /* 全表满 */
}

/* 槽位下标 → 事件 id(唯一一处换算, marker/register 共用) */
static int trc_slot_to_id(int slot)
{
    return (slot < 0) ? slot : (int)(BR_TRACE_SVC_ID_BASE + (br_u32)slot);
}

int br_trace_svc_marker(const char *tag)
{
    const int slot = trc_acquire_slot(tag);

    if (slot < 0) {
        return slot;
    }

    const int id = trc_slot_to_id(slot);

    /* a = tag 的 32 位 hash(离线/在线解码的键), b = 当前时钟(us) */
    br_trace_emit((br_u32)id, trc_fnv1a32(tag), (br_u64)br_clock_now());
    return id;
}

int br_trace_svc_register(const char *name)
{
    return trc_slot_to_id(trc_acquire_slot(name));
}

const char *br_trace_svc_name(br_u32 id)
{
    if ((id >= BR_TRACE_SVC_ID_BASE)
        && (id < (BR_TRACE_SVC_ID_BASE + BR_TRACE_SVC_ID_SLOTS))) {
        const char *n = s_names[id - BR_TRACE_SVC_ID_BASE];
        if (n != BR_NULL) {
            return n;
        }
    }
    return br_trace_name(id);   /* 退回 core 的固定事件名表(未知 ⇒ "?") */
}

/* =====================================================================
 * 消费计数
 * ===================================================================== */

static void trc_seen_add(br_u32 id)
{
    for (br_u32 i = 0u; i < s_seen_n; i++) {
        if (s_seen_id[i] == id) {
            s_seen_cnt[i]++;
            return;
        }
    }
    if (s_seen_n < TRC_SEEN_SLOTS) {
        s_seen_id[s_seen_n]  = id;
        s_seen_cnt[s_seen_n] = 1u;
        s_seen_n++;
        return;
    }
    s_other++;   /* 表满: 只保证"总数不丢", 不再保 id 粒度 */
}

br_u32 br_trace_svc_seen(br_u32 id)
{
    for (br_u32 i = 0u; i < s_seen_n; i++) {
        if (s_seen_id[i] == id) {
            return s_seen_cnt[i];
        }
    }
    return 0u;
}

/* =====================================================================
 * 取走 + 呈现
 * ===================================================================== */

br_u32 br_trace_svc_report(br_u32 max_events)
{
    /* max_events == 0 = "不限", 但必须有硬上界, 否则消费期间 ISR 还在补货时
     * 单次调用可能永不收敛 —— 取环容量作上界(多轮调用即可取净)。 */
    const br_u32 limit = (max_events == 0u) ? (br_u32)BR_TRACE_RING_SIZE : max_events;
    br_u32 taken = 0u;

    while (taken < limit) {
        br_u32 want = limit - taken;
        if (want > TRC_DRAIN_BATCH) {
            want = TRC_DRAIN_BATCH;
        }

        const br_u32 n = br_trace_drain(s_batch, want);
        if (n == 0u) {
            break;   /* 环已取空 */
        }

        for (br_u32 i = 0u; i < n; i++) {
            s_drained++;
            br_log_info("[TRACE] #%u id=%u name=%s a=0x%x b=0x%lx",
                        s_drained, s_batch[i].id,
                        br_trace_svc_name(s_batch[i].id),
                        s_batch[i].a, s_batch[i].b);
            trc_seen_add(s_batch[i].id);
        }
        taken += n;
    }

    return taken;
}

br_u32 br_trace_svc_summary(void)
{
    const br_u32 total   = br_trace_total();
    const br_u32 overrun = br_trace_overrun();
    /* live 下界 0: 环被覆盖时生产者计数与消费计数会短暂错位 */
    const br_u32 live    = (total >= s_drained) ? (total - s_drained) : 0u;

    br_log_info("[TRACE] total=%u drained=%u overrun=%u ids=%u live=%u",
                total, s_drained, overrun, s_seen_n, live);

    /* "其它"桶: 逐 id 计数表(64 项)满后无法保 id 粒度的余量, 在这里体现,
     * 免得 summary 的 drained 与逐 id 计数看起来"对不上"。 */
    if (s_other > 0u) {
        br_log_info("[TRACE] other=%u (逐 id 计数表 %u 项已满, 余量并入此桶)",
                    s_other, TRC_SEEN_SLOTS);
    }

    return s_drained;
}

void br_trace_svc_reset(void)
{
    for (br_u32 i = 0u; i < TRC_SEEN_SLOTS; i++) {
        s_seen_id[i]  = 0u;
        s_seen_cnt[i] = 0u;
    }
    s_seen_n  = 0u;
    s_other   = 0u;
    s_drained = 0u;
    /* ★ 不动 core 环的读写游标, 也不清动态名字表(注册是"映像级"的, 与计数无关) */
}

int br_trace_svc_init(void)
{
    br_trace_svc_reset();

    /* 本插件的两个自有事件名。起始槽位由 hash 决定(trace.report→26,
     * trace.reset→31), 实际落点由线性探测决定。v0.1 只登记名字, 不发这两个 id 的
     * 事件(事件留痕仍走 marker/core 的固定事件), 目的是让离线解码能认出它们。
     * 注: 线性探测之后, 别的插件撞槽不会再挤掉 selftest 的 slot 2 —— 只有全表
     * 32 槽都被占满才会出现 -ENOSPC(契约变更, 见 trc_acquire_slot 的注释)。 */
    (void)br_trace_svc_register(TRC_NAME_REPORT);
    (void)br_trace_svc_register(TRC_NAME_RESET);

    return 0;
}

/* =====================================================================
 * 自检用的内部视图
 *
 * 见 `src/trace_internal.h` 的说明: 这两条**只**为 `trace_selftest.c` 存在 —— 用例要
 * 复核"撞槽前提"与"解码出的名字逐字节相等", 用公开 API 表达不出来。★ 一律**转调**
 * 生产实现, 不复制逻辑: 用例必须跑真算法, 否则验证的是副本。
 * 原型在该头文件里(-Wmissing-prototypes), 入口名不是本插件的对外 API。
 * ===================================================================== */

br_u32 br_trace_internal_slot_of(const char *name)
{
    return trc_slot_of(name);
}

br_bool br_trace_internal_streq(const char *a, const char *b)
{
    return trc_streq(a, b);
}


/* =====================================================================
 * 插件生命周期钩子(名字 = symbol_prefix(short) + 相; 由生成物
 * `build/gen/service/trace/plugin_desc.c` 引用; 设计 1-01 §9 / 3-05 §2)
 *
 * 自带原型满足 -Wmissing-prototypes: 钩子名由**生成器**推导(不是本插件的对外 API),
 * 所以不写进 `include/br/debug/br_trace_svc.h`、也不进 plugin.toml 的 [[export]]
 * (那会改接口 hash)。
 * ===================================================================== */
int trace_early_init(void);
int trace_init(void);
int trace_start(void);

/* EARLY 相: 无动作(不用堆/无线程/关中断; 这里没有要注册的东西)。 */
int trace_early_init(void)
{
    return 0;
}

/* LATE 相(Service 类别 ⇒ ② 完成点): 现有 init 的转调。 */
int trace_init(void)
{
    return br_trace_svc_init();
}

/* START 相: 本服务没有需要"中断可用/可建线程"之后才做的事。 */
int trace_start(void)
{
    return 0;
}


/*
 * service/dump — 现场倾倒 + 调试域 conformance 编排者(实现体)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md`
 *   §2 `MEMRD`/`TRACE_READ`/`GETINFO`(debug bridge 的读命令);
 *   §3 mini ramdump 的捕获集("寄存器组、trace 环、**内存 region 表**、arena 统计")
 *      与约束"捕获路径**只用静态缓冲**, 不碰堆/调度器"。
 *
 * 归属边界(`br_dump.h` 抬头, ADR-0003 §2.6): 本插件是**编排者**, 不是又一份实现 ——
 *   呈现原语  → `service/hexdump`(边界判定不归它: 谁读内存谁问 region 表);
 *   栈回溯    → `service/backtrace`(只捕获, 符号化归 host 离线工具);
 *   环的消费  → `service/trace`(环本身在 core, 插件是唯一消费方);
 *   泄漏/红区账 → `service/memleak`(只读 core 的堆观测契约)。
 *   本插件自己只做三件事: ① 按 region 表做边界判定; ② 把上面几件编排成一份现场;
 *   ③ 自身状态的 LATE 相 init + 启动快照。
 *
 * ★ 调试域 conformance 的旧编排入口**已不在本文件**(ADR-0010): 拆分前 dump 那个对外发的
 *   conformance 函数
 *   既跑 dump 自己的用例, 又代调 trace/backtrace/hexdump/memleak 四家的 `*_selftest()`
 *   与 `*_init()`(v0.1 没有阶段机时的替身, WORKAROUND(br-wa-boot-001))。现在:
 *     - 各插件的自检入口在各自的 `src/<short>_selftest.c` 里, 由描述符钩子 `.selftest` 挂出,
 *       **core 在一个 pass 里驱动**(`br_plugin_manager_selftest`);
 *     - init 由 plugin_manager 按 `[[dep]]` 拓扑序驱动(本插件**不再**代调别家的 init)。
 *   dump 自己的用例搬到 `src/dump_selftest.c`; 台账口径见该文件抬头。
 *
 * ★ 呈现落点欠债: 现在直写早期 console, 未经 `5-01 §2` 的 debug bridge
 *   (COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面, M3)。`WORKAROUND(br-wa-debug-002)`
 *   登记的还债动作 = 把呈现层改为经 bridge 成帧输出(内容与边界判定不变, 只换落点)。
 *
 * 线程纪律: 全部 **thread-only**(会打印、会走 region 表与堆观测)。
 */
#include <br/debug/br_dump.h>

#include <br/debug/br_bt.h>
#include <br/debug/br_hexdump.h>
#include <br/debug/br_memleak.h>
#include <br/debug/br_trace_svc.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>

/* =====================================================================
 * 小工具: 无 libc 的定长串拼接 + region 属性解码
 *
 * attribute 的可读串按"位串 + 种类"给, 例: `RO|CACHED|kind=heap`;
 * 四个属性位一个都没置 ⇒ 编成 `RW`(RW 不是 attrs 的位, 只是缺省可写可执行的读法)。
 * ===================================================================== */

static void dump_buf_add(char *buf, br_size_t cap, br_size_t *len, const char *s)
{
    if (cap == 0u) {
        return;
    }
    while ((*s != '\0') && ((*len + 1u) < cap)) {
        buf[*len] = *s;
        (*len)++;
        s++;
    }
    buf[*len] = '\0';
}

static void dump_attr_add(char *buf, br_size_t cap, br_size_t *len,
                          br_bool *first, const char *tok)
{
    if (!*first) {
        dump_buf_add(buf, cap, len, "|");
    }
    *first = BR_FALSE;
    dump_buf_add(buf, cap, len, tok);
}

/* BR_MM_KIND_* → 名字(与设计 3-01 §13.6 的 `regions` 轴取值一一对应) */
static const char *dump_kind_name(br_u32 attrs)
{
    switch (BR_MM_KIND(attrs)) {
    case BR_MM_KIND_NONE:     return "none";
    case BR_MM_KIND_IMAGE:    return "image";
    case BR_MM_KIND_STACK:    return "stack";
    case BR_MM_KIND_HEAP:     return "heap";
    case BR_MM_KIND_CONTIG:   return "contig";
    case BR_MM_KIND_PAGE:     return "page";
    case BR_MM_KIND_DMA:      return "dma";
    case BR_MM_KIND_MMIO:     return "mmio";
    case BR_MM_KIND_RESERVED: return "reserved";
    default:                  return "?";
    }
}

static void dump_attrs_str(char *buf, br_size_t cap, br_u32 attrs)
{
    br_size_t len   = 0u;
    br_bool   first = BR_TRUE;

    if (cap == 0u) {
        return;
    }
    buf[0] = '\0';

    if ((attrs & BR_MM_RO) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "RO");
    }
    if ((attrs & BR_MM_NX) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "NX");
    }
    if ((attrs & BR_MM_DEVICE) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "DEVICE");
    }
    if ((attrs & BR_MM_CACHED) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "CACHED");
    }
    if (first) {
        dump_attr_add(buf, cap, &len, &first, "RW");
    }
    dump_attr_add(buf, cap, &len, &first, "kind=");
    dump_buf_add(buf, cap, &len, dump_kind_name(attrs));
}

/* =====================================================================
 * LATE 相 init
 * ===================================================================== */

int br_dump_init(void)
{
    /*
     * 注册本插件自己的两个事件名("一次现场倾倒的开/末")。
     * 返回值可能是负的(trace 服务未 init 时 `br_trace_svc_register` 会拒), 但
     * **各插件的 init 由调用方按相位顺序负责**, dump 不代它 init(见 README
     * 「归属边界」)⇒ 这里失败不致命, 继续; id 也丢弃(v1 不在 dump 路径发自有
     * 事件, 名表只为离线解码可达)。
     */
    (void)br_trace_svc_register("dump.begin");
    (void)br_trace_svc_register("dump.end");
    return 0;
}

/* =====================================================================
 * ① region 清单(设计 §3 捕获集的"内存 region 表")
 * ===================================================================== */

br_u32 br_dump_regions(void)
{
    const br_u32 count = br_mm_region_count();
    br_u32       printed = 0u;

    for (br_u32 i = 0u; i < count; i++) {
        const br_mm_region_t *region = br_mm_region_get(i);
        char                  attrs[64];

        /* 观测契约: 越界 ⇒ NULL。条数对不上就是 core 的观测面不自洽, 由用例报红 */
        if (region == BR_NULL) {
            continue;
        }
        dump_attrs_str(attrs, sizeof attrs, region->attrs);
        br_log_info("[DUMP] region[%u] kind=%s attrs=0x%x (%s) base=0x%lx size=%lu",
                    i, dump_kind_name(region->attrs), region->attrs,
                    attrs, region->base, region->size);
        printed++;
    }

    br_log_info("[DUMP] regions=%u", count);
    return printed;
}

/* =====================================================================
 * ② 边界受检的内存 dump
 * ===================================================================== */

/*
 * ★ 关键裁定: `BR_DUMP_F_FORCE` + **未声明地址** ⇒ 只打表头、**不读内存**。
 *
 * 理由(ADR-0003 §2.4/§2.5): 页表是按 region 表快照构造的, 表外地址是 invalid;
 * 例如 `0x42000000` 就在 RAM 窗口(`0x40000000 + 8 MiB`)之外 —— 它在
 * `irq_conf.c` 里正是"确定可取 translation fault"的锚点。而 hexdump 是纯呈现原语,
 * **没有** extable 保护的读原语(设计 §2 的 MEMRD 完整形态要它, v1 未交付)⇒
 * 在这里真读一次就是一次不可恢复的 data abort, 把"调试服务"变成"崩溃源"。
 *
 * 所以本原型里 FORCE 的语义收敛为: **绕过 region 表判定**(不返回 -EINVAL)、
 * 如实打印表头与"未读取"说明, 返回 0。要让它真读, 需要 extable 保护的 probe
 * (回灌项, 见 README「待回灌」)—— 不是把风险藏进 dump。
 */
int br_dump_memory(br_uintptr_t addr, br_size_t len, br_u32 flags)
{
    const br_mm_region_t *region = br_mm_region_find(addr);
    char       note[96];
    br_size_t  note_len = 0u;
    br_size_t  n        = len;
    br_bool    cap_hit  = BR_FALSE;
    br_bool    end_hit  = BR_FALSE;
    br_bool    unread   = BR_FALSE;
    const char *kind;

    note[0] = '\0';

    /* 缺省(STRICT): 未声明的地址一律拒绝 */
    if ((region == BR_NULL) && ((flags & BR_DUMP_F_FORCE) == 0u)) {
        return BR_ERR(BR_EINVAL);
    }

    kind = (region != BR_NULL) ? dump_kind_name(region->attrs) : "none";

    /* 上界截断: 防"一个 dump 打满 UART" */
    if (n > BR_DUMP_MAX_BYTES) {
        n       = BR_DUMP_MAX_BYTES;
        cap_hit = BR_TRUE;
    }

    if (region != BR_NULL) {
        /* 跨出首条 region 尾 ⇒ 截到 region 尾(不碰第二条 region) */
        const br_size_t off   = addr - region->base;
        const br_size_t avail = (off < region->size) ? (region->size - off) : 0u;

        if (n > avail) {
            n       = avail;
            end_hit = BR_TRUE;
        }
    } else {
        unread = BR_TRUE;
        n      = 0u;
    }

    if (cap_hit) {
        dump_buf_add(note, sizeof note, &note_len, " (truncated to BR_DUMP_MAX_BYTES)");
    }
    if (end_hit) {
        dump_buf_add(note, sizeof note, &note_len, " (truncated to region end)");
    }
    if (unread) {
        dump_buf_add(note, sizeof note, &note_len, " (force: region unknown, not read)");
    }

    br_log_info("[DUMP] memory base=0x%lx len=%lu region=%s flags=0x%x%s",
                addr, n, kind, flags, note);

    if (n > 0u) {
        (void)br_hexdump((const void *)addr, n);
    }
    return (int)n;
}

/* =====================================================================
 * ③ 堆统计 + 池几何(设计 §3 的 "arena 统计")
 * ===================================================================== */

/*
 * 返回**打印的行数**(头文件只要求"打印堆统计 + 池几何", 未规定返回值;
 * 这里选行数, 便于 `br_dump_all()` 累计"确实全打了"的判据)。
 * 任一子观测失败时也会打一行说明并计入 —— 返回值恒等于本函数打出的行数。
 */
br_u32 br_dump_heap(void)
{
    br_u32 lines = 0u;
    br_mem_layout_t lay;

    if (br_mem_layout(&lay) == 0) {
        br_log_info("[DUMP] heap pool base=0x%lx size=%lu",
                    lay.heap.base, lay.heap.size);
        lines++;
        br_log_info("[DUMP] heap pool.contig base=0x%lx size=%lu",
                    lay.contig.base, lay.contig.size);
        lines++;
        br_log_info("[DUMP] heap pool.page base=0x%lx size=%lu page_size=%u page_total=%u",
                    lay.page.base, lay.page.size, lay.page_size, lay.page_total);
        lines++;
        br_log_info("[DUMP] heap pool.dma base=0x%lx size=%lu",
                    lay.dma.base, lay.dma.size);
        lines++;
    } else {
        br_log_info("[DUMP] heap not ready (br_mem_layout)");
        lines++;
    }

    {
        br_heap_stats_t st;

        if (br_heap_stats_get(&st) == 0) {
            br_log_info("[DUMP] heap stats total=%lu used=%lu free=%lu max_free=%lu high_water=%lu",
                        st.total, st.used, st.free_bytes, st.max_free_block, st.high_water);
            lines++;
            br_log_info("[DUMP] heap alloc n_alloc=%u n_free=%u n_live=%u n_fail=%u",
                        st.n_alloc, st.n_free, st.n_live, st.n_fail);
            lines++;
            br_log_info("[DUMP] heap guard redzone=%u canary=%u double_free=%u bad_free=%u owners=%u",
                        st.redzone_hits, st.canary_hits, st.double_free,
                        st.bad_free, st.owner_count);
            lines++;
        } else {
            br_log_info("[DUMP] heap stats unavailable (br_heap_stats_get)");
            lines++;
        }
    }

    {
        br_page_stats_t pg;

        if (br_page_stats(&pg) == 0) {
            br_log_info("[DUMP] page stats total=%u used=%u free=%u fail=%u max_run=%u",
                        pg.total, pg.used, pg.free, pg.fail, pg.max_run);
            lines++;
        } else {
            br_log_info("[DUMP] page stats unavailable (br_page_stats)");
            lines++;
        }
    }

    return lines;
}

/* =====================================================================
 * ④ 三个转调口(本插件不重复实现它们)
 * ===================================================================== */

br_u32 br_dump_trace(br_u32 max_events)
{
    /* max_events 语义同 `br_trace_svc_report`(0 = 不限, 用尽为止) */
    return br_trace_svc_report(max_events);
}

br_u32 br_dump_backtrace(void)
{
    return br_bt_print();
}

br_u32 br_dump_leaks(void)
{
    return br_memleak_report();
}

/* =====================================================================
 * ⑤ 一份完整现场
 * ===================================================================== */

/*
 * 行数累计口径(头文件要求"返回打印的行数, 便于用例断言确实全打了"):
 *   固定标题 2(首/末) + [regions: 条数 + 1(regions=<n> 行)]
 *   + heap 段(br_dump_heap 自报行数)
 *   + leaks 段(折算口径: 1 行 summary + 每存活块折算 1 行)
 *   + trace 段(条数) + backtrace 段(帧数)。
 *
 * ★ 为什么 leaks 段是"折算口径"而不是精确行数: `br_memleak_report()` 的声明面
 *   只回传**存活块数**; 它的真实输出行数 = 1 + 归属数 + Σ min(8, 每归属存活块),
 *   其中"归属分布"与"每归属 8 条明细上限"都不在回传面上(声明面已冻结, 不许加函数)。
 *   所以折算值与真实行数**同阶但两端都可能偏**(归属多则低估 归属数, 单归属块多则
 *   因 8 条上限而高估)。调用方只用它判"全打了"(>= 阈值), **不做等值断言**。
 *   要精确到行, 需要 memleak 回传行数 —— 记入 README「待回灌」。
 */
br_u32 br_dump_all(void)
{
    br_u32 lines = 0u;
    br_u32 nreg;
    br_u32 live;
    br_u32 events;
    br_u32 frames;

    br_log_info("[DUMP] ===== snapshot start =====");
    lines++;

    nreg    = br_dump_regions();
    lines  += nreg + 1u;

    lines  += br_dump_heap();

    live    = br_dump_leaks();
    lines  += live + 1u;          /* 折算口径: summary 一行 + 每存活块折算一行 */

    events  = br_dump_trace(0u);
    lines  += events;

    frames  = br_dump_backtrace();
    lines  += frames;

    br_log_info("[DUMP] ===== snapshot end =====");
    lines++;

    return lines;
}

/* =====================================================================
 * 插件生命周期钩子(名字 = symbol_prefix(short) + 相; 由生成物
 * `build/gen/service/dump/plugin_desc.c` 引用; 设计 1-01 §9 / 3-05 §2)
 * 自带原型满足 -Wmissing-prototypes(钩子名由生成器推导, 不进对外头与 [[export]])。
 *
 * ★ LATE init 只做**本插件自己的**事(自身 init + 启动快照) —— 调试域一致性用例已按
 *   ADR-0010 搬到 `src/dump_selftest.c`, 其驱动者不再是 init, 而是**插件管理器**在全部
 *   START 之后统一调描述符的 `.selftest` 钩子。于是 `app/hello → service/dump` 那条 M0
 *   豁免**已从 `product.toml [lint].allow_edges` 删掉**(删掉后 `brickie check` 仍 0 错误
 *   = 证据), 而 dump 也**不再代调** trace/backtrace/hexdump/memleak 四家的 init:
 *   plugin.toml 的四条 `kind = "init"` 边保证 LATE 相里它们的 init 已由 plugin_manager
 *   按拓扑序调过(相位单调由它执法; 非 0 rc ⇒ `[PLUGIN] FAIL` + 停机, ADR-0005 裁定 G6)。
 * ===================================================================== */
int dump_early_init(void);
int dump_init(void);
int dump_start(void);

/* EARLY 相: 无动作(不用堆/无线程/关中断; dump 自己要读堆, 必须等 LATE)。 */
int dump_early_init(void)
{
    return 0;
}

/* LATE 相(Service 类别 ⇒ ② 完成点): 自身 init → 启动快照。
 *
 * 这里**没有** conformance 调用: 用例由 core 的自检 pass 驱动(见 dump_selftest.c 抬头)。
 * 也不代调别家 init —— 那正是 ADR-0010 拆掉的编排(四家的 init 由 plugin_manager 在
 * 本函数**之前**按 `[[dep]]` 拓扑序调完)。 */
int dump_init(void)
{
    const int rc = br_dump_init();
    if (rc != 0) {
        return rc;   /* 首败即停机由 plugin_manager 执行(裁定 G6) */
    }

    /* 启动现场一份(三套门禁截取证据的地方; 行数口径见 br_dump.h) */
    const br_u32 dump_lines = br_dump_all();
    br_log_info("dbg: boot snapshot lines=%lu", (br_u64)dump_lines);

    /* init 的返回值只表达"本插件的初始化成不成"(见 ADR-0005 §2.5); 用例的红绿由门禁
     * ([DBGCONF] FAIL 的 forbid)判, 两者不混。 */
    return 0;
}

/* START 相: 本服务没有需要"中断可用/可建线程"之后才做的事。 */
int dump_start(void)
{
    return 0;
}


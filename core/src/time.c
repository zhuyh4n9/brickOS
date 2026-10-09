/*
 * brickOS prototype v0.1.0 — 时钟换算与忙等延迟
 *
 * 设计对应: 3-01 §4(br_clock_now / br_deadline_from_now, 组 br-sched)。
 * v0.1.0 只实现"读数"这一半; 超时表与唤醒属于调度器(M1), 不在这里假装实现。
 *
 * 换算为什么不用简单的 "ticks / (freq/1e6)":
 *   QEMU virt 的 arch timer 是 62.5 MHz = 每微秒 62.5 拍, 不是整数。
 *   `freq/1000000` 取整会得到 62 或 63, 无论哪个都会带来 ~1% 的**系统性时间漂移**
 *   (向上取整 63 -> 读数比真实时间慢 0.8%, 跑 1000 秒差 8 秒)。
 *   这里以**毫秒**为换算基准(freq/1000 = 62500, 对 62.5 MHz 是精确的),
 *   再用"整数部分 + 余数部分"拆开算, 既无漂移也不溢出。
 *
 * WORKAROUND(br-wa-boot-001) ① **已还清**(v0.2.0): 调度器就位后, "睡眠"由
 * `br_task_sleep()` 真阻塞切换; 本文件的 `br_delay_*` 从此只是**忙等原语**
 * (当前全树无调用点, 保留给早期相/调试用)。
 * 设计里没有 br_delay_*(设计只有 br_task_sleep, 因为它必然阻塞切换)。
 *
 * 本文件另持有 **IRQ 心跳计数**(`br_clock_tick_notify` / `br_clock_tick_count`):
 * platform 的 timer ISR 每拍通知, 计数归 core —— 这样 P0 的 APP 不必直读 platform
 * 插件接口(还 `br-wa-boot-001` ②; 见 docs/decisions/0016-core-timer-heartbeat.md)。
 */
#include <br/core/br_time.h>
#include <br/platform/br_plat.h>

/* 平台时钟频率缺省值: QEMU virt 的 arch timer 是 62.5 MHz。
 * 只在 CNTFRQ_EL0 读出 0/异常小值时兜底, 不是正常路径。 */
#define BR_CLOCK_FALLBACK_HZ   62500000u

/* 换算基准: 每毫秒多少拍。用 1000 而不是 1000000, 因为 62.5 MHz 在毫秒上才精确。 */
static br_u64 s_freq_hz      = 0;
static br_u64 s_ticks_per_ms = BR_CLOCK_FALLBACK_HZ / 1000u;

void br_clock_init(void)
{
    br_u64 freq = br_plat_ticks_freq();

    /* 频率不可信(< 1 kHz 或读出 0)时才用兜底值; 正常路径读 CNTFRQ_EL0。 */
    if (freq < 1000u) {
        freq = BR_CLOCK_FALLBACK_HZ;
    }

    s_freq_hz      = freq;
    s_ticks_per_ms = freq / 1000u;
}

br_u64 br_clock_freq_hz(void)
{
    return s_freq_hz;
}

br_u64 br_clock_ticks_per_ms(void)
{
    return s_ticks_per_ms;
}

/* ------------------------------------------------------------------ IRQ 心跳计数
 *
 * 单一真值在 core: platform 的 timer ISR 每拍调 `br_clock_tick_notify()`, 消费者
 * (APP/服务/调试)读 `br_clock_tick_count()`。收进 core 的理由见 br_time.h 的注释与
 * `docs/decisions/0016-core-timer-heartbeat.md` —— 一句话: 不让 P0 的 APP 为了读一个
 * 诊断计数去认识 platform 插件的接口(还 `br-wa-boot-001` ②)。
 *
 * 无锁: 单核原型 + `volatile` 自增; 它与多核/PM 的形态(per-CPU 计数或原子)要到
 * 设计的 PM/IPI 落地时再谈, 现在不假装。
 *
 * 节拍频率 = `BR_CFG_TICK_HZ`(来自 `product.toml [kernel].hz`, 缺省 200 ⇒ 5 ms 一拍;
 * 见 ADR-0017)。platform 的 timer 装弹周期读同一份配置, core 这里只负责计数。
 */
static volatile br_u32 s_tick_count;

void br_clock_tick_notify(void)
{
    s_tick_count++;
}

br_u32 br_clock_tick_count(void)
{
    return s_tick_count;
}

/* 配置的节拍频率(Hz)。宏由 `product.toml [kernel].hz` 经 `-DBR_CFG_TICK_HZ` 带进来
 * (缺省 200, 见 br_time.h); 这里是它唯一的运行期读点。 */
br_u32 br_clock_tick_hz(void)
{
    return (br_u32)BR_CFG_TICK_HZ;
}

br_time_t br_clock_now(void)
{
    const br_u64 ticks = br_plat_ticks_now();

    /*
     * us = ticks * 1000 / ticks_per_ms, 拆成整数+余数两步:
     *   ticks*1000 整体算会在长时间运行后溢出 64 位;
     *   拆开后 (ticks % ticks_per_ms) < ticks_per_ms(~6e4), 乘 1000 也不会溢出。
     * 这是精确的整数换算(对 62.5 MHz: 62500 拍 = 1000 us, 一一对应)。
     */
    const br_u64 ms  = ticks / s_ticks_per_ms;
    const br_u64 rem = ticks % s_ticks_per_ms;

    return (br_time_t)((ms * 1000u) + ((rem * 1000u) / s_ticks_per_ms));
}

/*
 * 相对微秒 → 绝对期限(设计 `3-01` §14 CA-4: 阻塞 API 收相对值, 内部统一成**绝对
 * 期限**, tickless 框架只比较期限)。两处刻意:
 *   - `BR_TIMEOUT_INF` **原样返回**: 它表示"没有期限", 不是"很远的期限";
 *   - 溢出**饱和到 `BR_TIMEOUT_INF`**(而不是回绕成过去的时间点 —— 那会把"等到天荒地老"
 *     变成"立刻超时", 是静默的语义反转; 见 6-01 TC-TIME-003 的"饱和不回绕")。
 */
br_time_t br_deadline_from_now(br_time_t rel_us)
{
    if (rel_us == BR_TIMEOUT_INF) {
        return BR_TIMEOUT_INF;
    }

    const br_time_t now = br_clock_now();

    if (rel_us > (BR_TIMEOUT_INF - now)) {
        return BR_TIMEOUT_INF;
    }

    return now + rel_us;
}

void br_delay_us(br_time_t us)
{
    const br_u64 start = br_plat_ticks_now();

    /*
     * 换算成拍数并**向上取整** —— 这是"不早醒"语义的落点
     * (设计 3-01 §2.1: 到期唤醒不早醒, 晚到无上界)。
     * us 先按 1000 拆开, 避免 us * ticks_per_ms 溢出。
     */
    const br_u64 ticks = ((us / 1000u) * s_ticks_per_ms)
                       + ((((us % 1000u) * s_ticks_per_ms) + 999u) / 1000u);

    /*
     * 无符号回绕安全的差值比较: 写成 (now - start) < ticks 而不是
     * (now < start + ticks), 前者的正确性不依赖 start + ticks 不回绕。
     */
    while ((br_plat_ticks_now() - start) < ticks) {
        /* 忙等。没有调度器, 没有别的线程可跑 —— 连 idle 都还没有。 */
    }
}

void br_delay_ms(br_time_t ms)
{
    br_delay_us(ms * BR_US_PER_MS);
}

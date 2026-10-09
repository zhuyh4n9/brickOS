/*
 * brickOS prototype v0.1.0 — 中断子系统一致性用例(in-image conformance)
 *
 * 设计依据: `docs/6-test/6-01-test.md` §3.7(TC-IRQ 组, target-only)与
 * `docs/3-os-core/3-02-int.md` §17.1/§17.2(用例 ↔ 设计点的映射)。
 *
 * 为什么是 **in-image** 而不是 host 用例:
 *   设计 6-01 §3.7 已定"中断组 = target-only"; 而且 Stage 1 的中断路径
 *   (异常向量桩 → ack → 分发 → ISR → eoi → ERET)在 host 上根本不存在。
 *   所以本文件把用例编进镜像, 由 `qemu_aarch64_selftest()`(src/selftest.c)在**全部
 *   start 之后**经 core 的自检 pass 驱动(ADR-0010; 本文件此前是从 platform 的 start
 *   里被调的),
 *   每项打一行 `[IRQCONF] PASS/FAIL <用例 id> <说明>`, 末尾打
 *   `[IRQCONF] SUMMARY pass=N fail=M` —— 于是红绿可被 `make smoke` / `make irq-test`
 *   的 grep 判定, 不靠人眼。
 *
 * 本文件属 **platform 插件**(与 GICv3/绑定表同层): 它需要用软件触发(SGI)、
 * 硬件回读等 ISA 级手段构造场景; 用例本身调用的全是 core 的 native API。
 *
 * 覆盖(与 3-02 §17.1 的"既有 7 例" + §17.2 的"建议补充"对齐):
 *   TC-IRQ-001 register(SGI)→enable→自触发; arg 传递; ack/eoi 配对(INV-H)
 *   TC-IRQ-002 重复 register → -EBUSY(INV-A)
 *   TC-IRQ-003/013 disable 期间触发不达; enable 后补上(CAP_LATCH); 幂等 enable
 *   TC-IRQ-004 lock 嵌套: 内层 unlock 不开中断; st 位布局(IR-15)
 *   TC-IRQ-008 无绑定 hwirq → spurious_owned, 不 panic, 不调 ISR
 *   TC-IRQ-010 extable 命中: 非法访存降级为 -EFAULT
 *   TC-IRQ-012/021 风暴保护: ISR 内自我重触发(活锁的软件等价物)⇒ 窗口化判据检出
 *   TC-IRQ-014 优先级量化: 向低优先级饱和 + 描述符保留逻辑值 + 留痕(IR-7)
 *   TC-IRQ-016 多余 unlock 被下溢护栏拦截, 且中断仍能恢复
 *   TC-IRQ-018 对域成员调直连 API → -EINVAL, 且父线未被误 mask
 *   TC-IRQ-019 最低优先级线也能被投递(PMR 的 init 义务, §7.1)
 *   TC-IRQ-022 extable 命中后 in_fault 已复位(第二次 fault 仍走正常修复路径)
 *   TC-IRQ-101 FAST 级联域: 分发/逐子 ack/无属主子中断被 mask
 *   TC-IRQ-102 SLOW 域: Stage 1 无 bh ⇒ 明确拒绝(分期边界, §1.1.1)
 *   GIC-*      方言事实: EOImode/PRIbits/SPI 线数/绑定表(平台数据)核对
 */
#include <br/core/br_irq.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_fault.h>
#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include <br/platform/br_plat.h>
#include <br/platform/br_gicv3.h>
#include <br/board_irq.h>

/* 域窗口必须装进一个 32 位 pending 字(v0.1 的静态 bitmap 池按此裁剪) */
_Static_assert(BR_IRQ_DOMAIN_WINDOW <= 32u, "域窗口超出单字位图容量");

/*
 * extable 负控制用的"确定 fault"地址: 0x42000000 在 RAM 段(0x40000000, -m 128M)之内,
 * 但在页表的 RAM 映射窗口(0x40000000 + 8 MiB = 0x40800000)之外 ⇒ 页表项 invalid
 * ⇒ 访问取 translation fault。为什么不取 0x1000 那种"看起来没映射"的地址: QEMU virt
 * 把低地址区间实现成"返回 0 的 unassigned/flash", 根本不 fault(实测过一次, P-IRQ-16)。
 * 这个字面量与 br_mmu.h 的 BR_PLAT_CONF_UNMAPPED 同值; irq_conf.c 不依赖 MMU 头,
 * 故此处自带一份并注明出处(改动窗口边界时两处一起改)。
 */
#define CONF_UNMAPPED_ADDR   0x42000000ul

/* =====================================================================
 * 断言与统计
 * ===================================================================== */
static br_u32 s_pass;
static br_u32 s_fail;

static void conf_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_pass++;
        br_log_info("[IRQCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[IRQCONF] FAIL %s %s", tag, what);
    }
}

/* trace 事件留痕的观察(按 id 位图累计; 每次轮询把环取空) */
static br_u32 s_trace_seen;

static void conf_trace_poll(void)
{
    br_trace_evt_t ev[16];
    br_u32 n;

    while ((n = br_trace_drain(ev, 16u)) > 0u) {
        for (br_u32 i = 0; i < n; i++) {
            if (ev[i].id < 32u) {
                s_trace_seen |= (1u << ev[i].id);
            }
        }
    }
}

static br_bool conf_trace_has(br_u32 id)
{
    conf_trace_poll();
    return (s_trace_seen & (1u << id)) != 0u;
}

/* 有界等待: 期间中断照常投递(这是"等待 ISR 真的跑了"的判据, 不靠固定 sleep) */
static int conf_wait_count(volatile br_u32 *counter, br_u32 target, br_time_t timeout_us)
{
    const br_time_t t0 = br_clock_now();

    while (*counter < target) {
        if ((br_clock_now() - t0) > timeout_us) {
            return BR_ERR(BR_ETIMEDOUT);
        }
    }
    return 0;
}

/* 纯忙等(不断言); 期间中断照常投递 —— 用于"屏蔽期间不该投递"的负向判据 */
static void conf_idle_us(br_time_t us)
{
    const br_time_t t0 = br_clock_now();

    while ((br_clock_now() - t0) < us) {
        /* 忙等 */
    }
}

/* =====================================================================
 * ISR 与测试用"假设备"(FAST 域方言)
 * ===================================================================== */
static volatile br_u32 s_test_isr_count;
static volatile br_u32 s_test_isr_arg;

static void conf_test_isr(void *arg)
{
    s_test_isr_count++;
    s_test_isr_arg = (br_u32)(br_uintptr_t)arg;
}

/* 风暴用例: ISR 内自我重触发 ⇒ 电平活锁的软件等价物(3-02 §8.3)。
 * 之所以能这样构造: SGI 是**边沿**且 GIC 会锁存 pending —— 在本次 Active 被 eoi
 * 之前写 ICC_SGI1R 就会让同一条线立刻重新 pending。 */
static volatile br_u32 s_storm_isr_count;
static volatile br_u32 s_storm_left;

static void conf_storm_isr(void *arg)
{
    (void)arg;
    s_storm_isr_count++;

    if (s_storm_left > 0u) {
        s_storm_left--;
        (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI_STORM);
    }
}

/* FAST 域的"假引脚控制器": 状态位图在 RAM 里, 于是 demux 是纯 ISR 内的(§9.3) */
static volatile br_u32 s_fake_pending;
static volatile br_u32 s_fake_masked;
static volatile br_u32 s_fake_ack_count;
static volatile br_u32 s_child_count[3];

static void fake_pending(void *priv, br_u32 *bits, br_size_t nwords)
{
    (void)priv;
    for (br_size_t i = 0; i < nwords; i++) {
        bits[i] = 0u;
    }
    /* 屏蔽态不出现在 pending 快照里(域方言自己的影子) */
    bits[0] = s_fake_pending & ~s_fake_masked;
}

static void fake_mask(void *priv, br_u32 sub)
{
    (void)priv;
    s_fake_masked |= (1u << sub);
}

static void fake_unmask(void *priv, br_u32 sub)
{
    (void)priv;
    s_fake_masked &= ~(1u << sub);
}

static void fake_ack(void *priv, br_u32 sub)
{
    (void)priv;
    s_fake_ack_count++;
    s_fake_pending &= ~(1u << sub);
}

static void conf_child0_isr(void *arg) { (void)arg; s_child_count[0]++; }
static void conf_child2_isr(void *arg) { (void)arg; s_child_count[2]++; }

static const br_irq_domain_ops_t s_fake_ops = {
    .pending = fake_pending,
    .mask    = fake_mask,
    .unmask  = fake_unmask,
    .ack     = fake_ack,
};

static br_irq_domain_t *s_dom;

/* =====================================================================
 * extable 探针(3-02 §10.4)
 *
 * ★ 返回值的**唯一来源**是 core 按表项 `reg`/`errno_val` 写进异常帧的那个寄存器
 *   (arm64 的 x0); 修复标签本身只做跳转, 不自己造错误码 —— 否则这个用例就没有
 *   验证"表项必须带 reg + errno_val"这条设计结论(§10.4 的易错点)。
 * ===================================================================== */
static int conf_probe_read32(br_uintptr_t addr, br_u32 *out)
{
    register br_u32       val __asm__("x1");
    register br_s32       ret __asm__("x0");
    register br_uintptr_t a   __asm__("x2");

    a = addr;
    __asm__ volatile(
        ".Lconf_insn" "%=:\n\t"
        "   ldr %w[val], [%[addr]]\n\t"
        "   mov %w[ret], wzr\n\t"
        "   b   .Lconf_done" "%=\n\t"
        ".Lconf_fix" "%=:\n\t"
        "   b   .Lconf_done" "%=\n\t"
        ".Lconf_done" "%=:\n\t"
        BR_EXTABLE_ENTRY(.Lconf_insn%=, .Lconf_fix%=, 0, -14)
        : [val] "=&r"(val), [ret] "=&r"(ret)
        : [addr] "r"(a)
        : "memory");

    if (ret == 0 && out != BR_NULL) {
        *out = val;
    }
    return ret;
}

/* =====================================================================
 * 用例
 * ===================================================================== */
static void conf_gicv3_facts(void)
{
    br_gicv3_info_t info;
    br_gicv3_info(&info);

    conf_report(info.eoimode == 0u, "GIC-EOIMODE",
                "EOImode == 0(单次 eoi 即完整; 3-02 §4.1.1 的硬前提)");
    conf_report(info.prio_bits >= 5u && info.prio_bits <= 8u, "GIC-PRIBITS",
                "ICC_CTLR_EL1.PRIbits 解出有效优先级位数");
    conf_report(info.spi_lines >= 32u, "GIC-LINES",
                "GICD_TYPER 报出 SPI 线数(平台数据核对)");

    br_u32 hwirq = 0u;
    const int r = br_plat_irq_hwirq(BR_IRQ_TIMER, &hwirq);
    conf_report(r == 0 && hwirq == 30u, "GIC-TIMERID",
                "BR_IRQ_TIMER 绑定到 INTID 30(EL1 物理 timer PPI)");
}

static void conf_extable(void)
{
    static const br_u32 probe_src = 0x5A5A1234u;
    br_u32 v = 0u;

    const int r1 = conf_probe_read32((br_uintptr_t)&probe_src, &v);
    br_log_info("[IRQCONF] info probe(valid) ret=%d val=0x%x", r1, v);
    conf_report(r1 == 0 && v == probe_src, "TC-IRQ-010",
                "extable 正控制: 合法地址读出真值");

    /*
     * ★ 怎么造出"确定可恢复的访存 fault"(P-IRQ-16; 随内存映射子系统一起改过一次):
     *   旧手法是 MMU-off 时的**非对齐访问**(全部访存按 Device-nGnRnE ⇒ Alignment
     *   fault, EC=0x25/DFSC=0x21)。平台内存映射子系统落地后 MMU 已开, 且 SCTLR_EL1 的
     *   A/SA/SA0 被显式清 0(见 mmu.c 的 MMU_SCTLR_A 注释: 字段号按 ARMv8 更正过)
     *   ⇒ 对 Normal 内存的非对齐访问**不再** fault(`-mstrict-align` 因此从硬要求
     *   退化为防御性旋钮, 见 Makefile 的注释)。
     *   现在改用**真正未映射的地址**: CONF_UNMAPPED_ADDR 落在页表的 RAM 映射窗口
     *   (0x40000000 + 8 MiB)之外, 对应 L1/L2 项为 invalid ⇒ **Translation fault**
     *   (EC=0x25/DFSC=0x04..0x07 一类), 与旧手法同样确定、同样由 extable 修复。
     */
    v = 0u;
    const int r2 = conf_probe_read32(CONF_UNMAPPED_ADDR, &v);
    br_log_info("[IRQCONF] info probe(unmapped=0x%lx) ret=%d", (br_u64)CONF_UNMAPPED_ADDR, r2);
    conf_report(r2 == BR_ERR(BR_EFAULT), "TC-IRQ-010",
                "extable 命中: 未映射地址(translation fault)降级为 -EFAULT, 系统存活");

    /* 第二个 fault: 若 in_fault 没复位, 这一次会被当成 double fault(裸 panic) */
    v = 0u;
    const int r3 = conf_probe_read32(CONF_UNMAPPED_ADDR, &v);
    conf_report(r3 == BR_ERR(BR_EFAULT), "TC-IRQ-022",
                "命中修复后 in_fault 已复位(第二个 fault 仍走修复路径, 不是 double fault)");

    conf_report(conf_trace_has(BR_TRACE_FAULT_FIXUP), "TC-IRQ-010",
                "修复留痕(TRACE_FAULT_FIXUP)");
}

static void conf_sgi_path(void)
{
    const br_irq_attr_t attr = {
        .prio = BR_IRQ_PRIO_DEFAULT, .trigger = BR_IRQ_TRIG_EDGE_RISE, .flags = 0u
    };

    const int r = br_irq_register(BR_IRQ_TEST_SGI, conf_test_isr,
                                  (void *)(br_uintptr_t)0xC0FFEEu, &attr);
    conf_report(r == 0, "TC-IRQ-001", "register(SGI virq) -> 0");

    conf_report(br_irq_enable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-001", "enable -> 0");

    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    const br_bool ran = (conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0);
    conf_report(ran && s_test_isr_arg == 0xC0FFEEu, "TC-IRQ-001",
                "ISR 运行且 arg 传递正确");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_TEST_SGI, &st);
    conf_report(st.count == s_test_isr_count, "TC-IRQ-001",
                "ack/eoi 恰各一次: 描述符计数 == ISR 次数(INV-H)");
}

static void conf_duplicate_register(void)
{
    const br_irq_attr_t attr = { BR_IRQ_PRIO_DEFAULT, BR_IRQ_TRIG_DEFAULT, 0u };
    const int r = br_irq_register(BR_IRQ_TEST_SGI, conf_test_isr, BR_NULL, &attr);

    conf_report(r == BR_ERR(BR_EBUSY), "TC-IRQ-002",
                "重复 register -> -EBUSY(每根物理线恰一属主, INV-A)");
}

static void conf_priority_readback(void)
{
    /* 用一条尚未注册的线(PL011 UART0 = SPI 33)做"量化 + 硬件回读"交叉校验。
     * 请求逻辑 prio = 0x01: GICv3 的有效位 **MSB 对齐**(5 位 ⇒ 步长 8),
     * 必须向**低优先级**量化为 0x08 —— 若量化方向反了会得到 0x00(静默提权, IR-7 禁止)。 */
    br_u32 hwirq = 0u;
    if (br_plat_irq_hwirq(BR_IRQ_UART0, &hwirq) != 0) {
        conf_report(BR_FALSE, "TC-IRQ-014", "取 UART0 的 hwirq 失败(绑定表缺项)");
        return;
    }

    /*
     * ★ 先验"最低可实现档被 PMR 永久屏蔽"这条架构性质的处理(P-IRQ-15):
     *   5 位优先级时 PMR 只实现高 5 位(写 0xFF 读回 0xF8), 投递判据是 `prio < PMR`
     *   ⇒ 量化到 0xF8 的线**永不被投递**; 请求 0xFF 必须被判为"无法表达"
     *   ⇒ `-ENOTSUP`(§4.2/§7.1), 而**不是**悄悄降到 0xF0(那是提权, IR-7 禁止)。
     */
    {
        const br_irq_attr_t too_low = { .prio = BR_IRQ_PRIO_LOWEST,
                                        .trigger = BR_IRQ_TRIG_DEFAULT, .flags = 0u };
        const int rr_low = br_irq_register(BR_IRQ_UART0, conf_test_isr, BR_NULL, &too_low);
        conf_report(rr_low == BR_ERR(BR_ENOTSUP), "TC-IRQ-014",
                    "请求逻辑最低档(0xFF)落在'被 PMR 永久屏蔽'的硬件档 -> -ENOTSUP(不静默提权)");
    }

    const br_irq_attr_t attr = { .prio = 0x01u, .trigger = BR_IRQ_TRIG_DEFAULT, .flags = 0u };
    conf_report(br_irq_register(BR_IRQ_UART0, conf_test_isr, BR_NULL, &attr) == 0,
                "TC-IRQ-014", "register(UART0, 逻辑 prio 0x01) -> 0");

    br_u8 hw_prio = 0u;
    const int rr = br_gicv3_readback_prio(hwirq, &hw_prio);
    conf_report(rr == 0 && hw_prio == 0x08u, "TC-IRQ-014",
                "硬件优先级 = 0x08(向低优先级饱和, 绝不静默提权)");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_UART0, &st);
    conf_report(st.prio == 0x01u, "TC-IRQ-014",
                "描述符保留逻辑优先级(硬件编码不外泄到 core)");

    conf_report(conf_trace_has(BR_TRACE_IRQ_PRIO_QUANTIZED), "TC-IRQ-014",
                "量化留痕(两个方向都要报, §7.1)");

    int en = 1;
    (void)br_gicv3_readback_enabled(hwirq, &en);
    conf_report(en == 0, "TC-IRQ-014",
                "register 后硬件为屏蔽态(CAP_READBACK 交叉校验影子 vs 硬件)");
}

static void conf_mask_depth(void)
{
    br_u32 hwirq = 0u;
    (void)br_plat_irq_hwirq(BR_IRQ_TEST_SGI, &hwirq);

    const br_u32 before = s_test_isr_count;
    conf_report(br_irq_disable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-003", "disable -> 0");

    int en = 1;
    (void)br_gicv3_readback_enabled(hwirq, &en);
    conf_report(en == 0, "TC-IRQ-003", "disable 后硬件已 mask(depth 0→1 才真正写硬件)");

    /* 屏蔽期间触发: 硬件 pending 会锁存(CAP_LATCH=1), 但 ISR 不得**开始** */
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_idle_us(2000u);
    conf_report(s_test_isr_count == before, "TC-IRQ-003",
                "屏蔽期间触发不达(INV-D: disable 返回后不再开始新的 handler)");

    conf_report(br_irq_enable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-003", "enable -> 0");
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0,
                "TC-IRQ-003", "enable 后锁存的 pending 被补上一次(CAP_LATCH)");

    conf_report(br_irq_enable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-013",
                "已放行态重复 enable 幂等返回 0(§6.2)");
    conf_idle_us(1000u);
    conf_report(s_test_isr_count == before + 1u, "TC-IRQ-013",
                "重复 enable 不产生额外 unmask/投递");
}

static void conf_lock_nesting(void)
{
    const br_irq_state_t st1 = br_irq_lock();
    const br_irq_state_t st2 = br_irq_lock();

    conf_report((st1 & BR_IRQ_ST_I_MASK) == 0u, "TC-IRQ-004",
                "进入前 I 位为放行(凭据第 0 位 == 0)");
    conf_report(((st2 >> BR_IRQ_ST_DEPTH_SHIFT) & 0x3FFFu) == 1u, "TC-IRQ-004",
                "凭据里的 depth 副本 == 1(IR-15 的 golden 位布局)");

    br_irq_unlock(st2);   /* 内层: 绝不开中断 */
    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_idle_us(2000u);
    conf_report(s_test_isr_count == before, "TC-IRQ-004",
                "内层 unlock 后中断仍屏蔽(不投递)");

    br_irq_unlock(st1);   /* 最外层: 恢复进入前的 I 位 */
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0,
                "TC-IRQ-004", "最外层 unlock 后恢复投递");
}

static void conf_unlock_underflow(void)
{
    const br_irq_state_t st = br_irq_lock();

    br_irq_unlock(st);   /* 正确配对 */
    br_irq_unlock(st);   /* ★ 多余的一次: 必须被下溢护栏拦住(否则 depth 回绕 ⇒ 永久关中断) */

    conf_report(conf_trace_has(BR_TRACE_IRQ_UNLOCK_UNDERFLOW), "TC-IRQ-016",
                "多余的 unlock 被下溢护栏拦截并留痕");

    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0,
                "TC-IRQ-016", "护栏之后中断仍能恢复(没有永久关中断)");
}

static void conf_spurious_unbound(void)
{
    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_TIMER, &st);
    const br_u32 owned_before = st.spurious_owned;
    const br_u32 isr_before   = s_test_isr_count;

    /*
     * INTID 3 故意**不绑定**。但"无属主"要能被 core 看见, 硬件上必须先**使能**这条线
     * —— 否则它连 CPU 都到不了(没绑定/没注册的线本来就是 disabled 的, §6.2 的"注册前
     * 天然安全")。这里用 conformance 专用的 raw 钩子模拟"固件留下一条已使能却无人认领
     * 的线", 这恰是 spurious_owned 想抓的集成期故障。
     */
    conf_report(br_gicv3_raw_line_enable(BR_IRQ_TEST_SGI_UNBOUND, 1) == 0, "TC-IRQ-008",
                "conformance 钩子: 让无属主线在硬件上使能(模拟固件遗留)");
    conf_report(br_gicv3_sgi_trigger(BR_IRQ_TEST_SGI_UNBOUND) == 0, "TC-IRQ-008",
                "触发无绑定的 SGI(INTID 3)");

    conf_idle_us(3000u);
    br_irq_stats_get(BR_IRQ_TIMER, &st);
    conf_report(st.spurious_owned > owned_before, "TC-IRQ-008",
                "无绑定 hwirq ⇒ 计入 spurious_owned(真 bug 信号)");
    conf_report(s_test_isr_count == isr_before, "TC-IRQ-008",
                "无属主的投递不调用任何 ISR");
    conf_report(conf_trace_has(BR_TRACE_IRQ_SPURIOUS_OWNED), "TC-IRQ-008",
                "spurious 留痕");
    (void)br_gicv3_raw_line_enable(BR_IRQ_TEST_SGI_UNBOUND, 0);   /* 收尾: 关掉这条线的硬件使能 */
}

static void conf_storm(void)
{
    /* 最低优先级注册: 它能否被投递 = PMR 的 init 义务是否履行(§7.1 / TC-IRQ-019) */
    /*
     * ★ 用"最低**可投递**档"0xF0(P-IRQ-15): 5 位优先级下 PMR 只实现高 5 位
     *   (写 0xFF 读回 0xF8), 而投递判据是 `prio < PMR` ⇒ 0xF8 档永远被屏蔽。
     *   TC-IRQ-019 要验的是"init 把 PMR 放行到**所有可投递**优先级", 所以取 0xF0;
     *   "0xFF 会被拒绝"由 TC-IRQ-014 的 -ENOTSUP 断言覆盖。
     */
    const br_irq_attr_t attr = {
        .prio = 0xF0u, .trigger = BR_IRQ_TRIG_EDGE_RISE, .flags = 0u
    };

    conf_report(br_irq_register(BR_IRQ_TEST_SGI_STORM, conf_storm_isr, BR_NULL, &attr) == 0,
                "TC-IRQ-019", "register(逻辑 prio = 最低可投递档 0xF0) -> 0");
    conf_report(br_irq_enable(BR_IRQ_TEST_SGI_STORM) == 0, "TC-IRQ-019", "enable -> 0");

    {
        br_u32 shw = 0u;
        br_u8  sprio = 0u;
        int    sen = -1;
        (void)br_plat_irq_hwirq(BR_IRQ_TEST_SGI_STORM, &shw);
        (void)br_gicv3_readback_prio(shw, &sprio);
        (void)br_gicv3_readback_enabled(shw, &sen);
        br_log_info("[IRQCONF] info storm line intid=%u prio=0x%x enabled=%d", shw, (br_u32)sprio, sen);
        conf_report(sprio == 0xF0u && sen != 0, "TC-IRQ-019",
                    "硬件回读: prio==0xF0(最低可投递档) 且线已 enabled");
    }

    s_storm_left = 1u;
    const br_u32 base = s_storm_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI_STORM);
    conf_report(conf_wait_count(&s_storm_isr_count, base + 1u, 20000u) == 0, "TC-IRQ-019",
                "最低优先级线被投递(PMR 已放行全部可实现优先级)");

    /* 风暴: ISR 内自我重触发 80 次(电平活锁的软件等价物) */
    const br_u32 storm_base = s_storm_isr_count;
    s_storm_left = 80u;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI_STORM);
    conf_report(conf_wait_count(&s_storm_isr_count, storm_base + 81u, 200000u) == 0,
                "TC-IRQ-012", "ISR 内自我重触发链跑完(80 次软件触发)");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_TEST_SGI_STORM, &st);
    conf_report(st.storm > (br_u16)BR_IRQ_STORM_LIMIT, "TC-IRQ-012",
                "窗口内计数越过 BR_IRQ_STORM_LIMIT");
    conf_report(conf_trace_has(BR_TRACE_IRQ_STORM), "TC-IRQ-021",
                "风暴留痕: 窗口化判据(占比, 与频率/相邻性无关)");
    br_log_info("[IRQCONF] info trace ring: emitted=%u overrun=%u seen=0x%x (storm=%u)",
                br_trace_total(), br_trace_overrun(), s_trace_seen, (br_u32)st.storm);

    /* 默认动作 = trace + 继续(不自动 mask): 别的线照常工作 */
    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0, "TC-IRQ-012",
                "风暴之后系统继续运行(其他线正常投递)");
}

static void conf_domain_stage1_boundary(void)
{
    /* SLOW 域的存在前提是 bh(Stage 2); Stage 1 必须**明确拒绝**而不是假装支持 */
    br_irq_domain_t *d = br_irq_domain_create("conf-slow", BR_IRQ_DOMAIN_PARENT, 3u,
                                              BR_IRQ_DOMAIN_F_SLOW, &s_fake_ops, BR_NULL);
    conf_report(d == BR_NULL, "TC-IRQ-102",
                "SLOW 级联域在 Stage 1 被拒绝(返回 NULL)");
    conf_report(conf_trace_has(BR_TRACE_IRQ_DOMAIN_NOBH), "TC-IRQ-102",
                "拒绝原因留痕(TRACE_IRQ_DOMAIN_NOBH)");
}

static void conf_domain_fast(void)
{
    s_dom = br_irq_domain_create("conf-gpio", BR_IRQ_DOMAIN_PARENT, 3u,
                                BR_IRQ_DOMAIN_F_FAST, &s_fake_ops, BR_NULL);
    conf_report(s_dom != BR_NULL, "TC-IRQ-101", "domain_create(FAST, n_sub=3) 成功");
    if (s_dom == BR_NULL) {
        return;
    }

    {
        const int rc0 = br_irq_register_child(s_dom, 0u, conf_child0_isr, BR_NULL, BR_NULL);
        const int rc2 = br_irq_register_child(s_dom, 2u, conf_child2_isr, BR_NULL, BR_NULL);
        br_log_info("[IRQCONF] info register_child rc(sub0)=%d rc(sub2)=%d", rc0, rc2);
        conf_report(rc0 == 0, "TC-IRQ-101", "register_child(sub=0) -> 0");
        conf_report(rc2 == 0, "TC-IRQ-101", "register_child(sub=2) -> 0");
    }
    conf_report(br_irq_register_child(s_dom, 0u, conf_child0_isr, BR_NULL, BR_NULL) == BR_ERR(BR_EBUSY),
                "TC-IRQ-101", "重复 register_child -> -EBUSY");
    conf_report(br_irq_register_child(s_dom, 9u, conf_child0_isr, BR_NULL, BR_NULL) == BR_ERR(BR_EINVAL),
                "TC-IRQ-101", "sub 越界 -> -EINVAL");

    conf_report(br_irq_enable_child(s_dom, 0u) == 0, "TC-IRQ-101", "enable_child(0)");
    conf_report(br_irq_enable_child(s_dom, 2u) == 0, "TC-IRQ-101", "enable_child(2)");
    conf_report(br_irq_enable(BR_IRQ_DOMAIN_PARENT) == 0, "TC-IRQ-101",
                "enable(父线: 它是 PIC 直连线)");

    /* 位图: 子 0/2 有属主, 子 1 **无属主** ⇒ demux 必须 mask 它(防风暴口, §9.3) */
    s_fake_masked    = 0u;
    s_fake_ack_count = 0u;
    s_child_count[0] = 0u;
    s_child_count[2] = 0u;
    s_fake_pending   = (1u << 0) | (1u << 1) | (1u << 2);

    (void)br_plat_irq_trigger(BR_IRQ_DOMAIN_PARENT);

    conf_report(conf_wait_count(&s_child_count[0], 1u, 20000u) == 0, "TC-IRQ-101",
                "子 0 的 handler 在 demux(ISR 上下文)内运行");
    conf_report(s_child_count[2] == 1u, "TC-IRQ-101",
                "子 2 的 handler 在同一次 demux 内运行");
    conf_report(s_fake_ack_count == 3u, "TC-IRQ-101",
                "core 在**子 handler 返回后**逐子 ack(3 个子位各一次)");
    conf_report((s_fake_masked & (1u << 1)) != 0u, "TC-IRQ-101",
                "无属主子中断被 mask(不留'无属主却反复触发'的风暴口)");
    conf_report(conf_trace_has(BR_TRACE_IRQ_DOMAIN_ORPHAN), "TC-IRQ-101",
                "无属主留痕(TRACE_IRQ_DOMAIN_ORPHAN)");
}

static void conf_domain_api_misuse(void)
{
    if (s_dom == BR_NULL) {
        conf_report(BR_FALSE, "TC-IRQ-018", "域未创建, 无法验证两族互斥");
        return;
    }

    const br_u32 child_virq = BR_IRQ_DOMAIN_BASE;   /* 窗口的第一个子中断 */
    const br_irq_attr_t attr = { BR_IRQ_PRIO_DEFAULT, BR_IRQ_TRIG_DEFAULT, 0u };

    conf_report(br_irq_register(child_virq, conf_child0_isr, BR_NULL, &attr) == BR_ERR(BR_EINVAL),
                "TC-IRQ-018", "对域成员调直连 register -> -EINVAL(register 期就报出)");
    conf_report(br_irq_disable(child_virq) == BR_ERR(BR_EINVAL),
                "TC-IRQ-018", "对域成员调直连 disable -> -EINVAL");
    conf_report(br_irq_enable_child(s_dom, 7u) == BR_ERR(BR_EINVAL),
                "TC-IRQ-018", "域 API 反向: sub 越界 -> -EINVAL");

    br_u32 parent_hw = 0u;
    int  en = 0;
    const int got_hw = br_plat_irq_hwirq(BR_IRQ_DOMAIN_PARENT, &parent_hw);
    const int got_en = (got_hw == 0) ? br_gicv3_readback_enabled(parent_hw, &en) : -1;
    conf_report(got_hw == 0 && got_en == 0 && en != 0, "TC-IRQ-018",
                "硬件回读: 父线仍 enabled(误操作没有 mask 到毫不相干的物理线)");
}

static void conf_stats(void)
{
    br_irq_stat_t st;

    br_irq_stats_get(BR_IRQ_TEST_SGI, &st);
    conf_report(st.count > 0u && st.depth == 0u && st.flags == 0u, "TC-IRQ-STATS",
                "stats_get 快照自洽(count>0 / depth==0 / flags==0)");

    br_irq_stats_get(BR_IRQ_INVALID, &st);
    conf_report(st.count == 0u && st.depth == 0u, "TC-IRQ-STATS",
                "越界 virq 的快照清零(不越界读)");

    br_irq_stats_get(BR_IRQ_UART0, &st);
    conf_report(st.depth == 1u && st.prio == 0x01u, "TC-IRQ-STATS",
                "UART0: register 后 depth==1(屏蔽态)且保留逻辑 prio");
}

/* =====================================================================
 * 入口(由 platform 的 `qemu_aarch64_start()` 在 START 相调用 —— 那时全局中断已开; 见 br_plat.h)
 * ===================================================================== */
int br_plat_irq_conformance(void)
{
    s_pass = 0u;
    s_fail = 0u;
    s_trace_seen = 0u;
    s_dom = BR_NULL;

    br_log_info("[IRQCONF] int framework conformance (Stage 1, GICv3, no scheduler)");

    /* 全局开中断: 设计 §14.3 的最后一步(全部插件 init 之后)。
     * 管理器已在 LATE 之后开过(见 plugin_mgr.c), 这里是**幂等**的保底 —— 用例本身
     * 需要"中断真的放行"这一前提(软件触发 SGI 才收得到)。 */
    br_irq_cpu_enable();

    conf_gicv3_facts();
    conf_extable();
    conf_sgi_path();
    conf_duplicate_register();
    conf_priority_readback();
    conf_mask_depth();
    conf_lock_nesting();
    conf_unlock_underflow();
    conf_spurious_unbound();
    conf_storm();
    conf_domain_stage1_boundary();
    conf_domain_fast();
    conf_domain_api_misuse();
    conf_stats();

    br_log_info("[IRQCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}

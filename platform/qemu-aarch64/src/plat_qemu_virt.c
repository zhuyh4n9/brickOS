/*
 * QEMU virt (aarch64) platform plugin: PL011 + arch timer + GICv3 bring-up, plus the plugin
 * lifecycle hooks. Hook name = symbol_prefix + phase, referenced by the generated
 * plugin_desc.c; the manifest is ../brick.toml.
 */
#include <br/core/br_console.h>
#include <br/core/br_log.h>
#include <br/core/br_time.h>        /* br_clock_tick_hz / BR_CFG_TICK_PERIOD_US (tick config) */
#include <br/board_irq.h>            /* BR_IRQ_TIMER (the virq in the binding table) */
#include <br/platform/br_mmu.h>
#include <br/platform/br_plat.h>

/* Descriptor name/ISA strings. */
#define BR_PLAT_NAME_STR   "qemu-aarch64/virt"
#define BR_PLAT_ISA_STR    "aarch64"

/* Hook prototypes for -Wmissing-prototypes. Hook names come from the generator, not this plugin's
 * exported API; listing them in brick.toml [[export]] would change the interface hash. */
int qemu_aarch64_early_init(void);
int qemu_aarch64_init(void);
int qemu_aarch64_start(void);

/* EARLY phase, first step of plugin_manager for plugin_type == platform: console, PIC
 * register/caps, binding table, region table, pool claim, identity map + MMU on.
 * br_plat_early_init() parks internally on failure, so returning here means all steps passed. */
int qemu_aarch64_early_init(void)
{
    br_plat_early_init();
    return 0;
}

/* CORE phase: no extra action -- console/PIC/binding table/region table/page tables/MMU are all
 * EARLY, before any plugin init. Kept so the declared completion point (brick.toml phase) is
 * visible in the boot order. */
int qemu_aarch64_init(void)
{
    return 0;
}

/* START phase, after global interrupt enable: br_plat_irq_start() registers the timer PPI ISR,
 * enables that line, arms the first deadline (1/HZ) and is an idempotent global IRQ enable.
 * The conformance suites run later, in this plugin's selftest (qemu_aarch64_selftest), after all
 * starts. The return value covers device bring-up only; selftest pass/fail is judged by the gates
 * ([IRQCONF]/[MEMCONF] FAIL lines). */
int qemu_aarch64_start(void)
{
    /* The platform reports its own identity (APP no longer reads br_plat_name/isa); the platform
     * runs first in the START phase, so this precedes the APP banner. */
    br_log_info("platform: %s (%s)", br_plat_name(), br_plat_isa());

    const int r = br_plat_irq_start();
    if (r != 0) {
        br_log_error("int: platform irq start failed: %d", r);
        return r;   /* stop-on-first-failure is enforced by plugin_manager (ruling G6) */
    }
    br_log_info("int: timer PPI armed by platform (virq=%u INTID=%u, %u Hz = %u us)",
                (br_u32)BR_IRQ_TIMER, 30u, br_clock_tick_hz(),
                (br_u32)BR_CFG_TICK_PERIOD_US);
    return 0;
}

const char *br_plat_name(void)
{
    return BR_PLAT_NAME_STR;
}

const char *br_plat_isa(void)
{
    return BR_PLAT_ISA_STR;
}

void br_plat_early_init(void)
{
    /*
     * Order matters: early console first, else any init-chain failure is silent. Then the three
     * platform-side interrupt steps (PIC register -> caps_get -> binding table commit), then the
     * memory pieces: memmap init only declares the region table, mmu_ops_register only registers
     * page-table ops. Pool claiming and MMU activation are a core.init slot after platform plugin
     * init. EARLY failure = boot failure: interrupts are off, so park instead of degrading.
     * GIC registers are reached directly before MMU activation and via the Device mapping after
     * (both in the low 1 GiB 2 MiB blocks).
     */
    br_console_init();

    if (br_plat_irq_init() != 0) {
        br_console_puts("[FATAL] platform IRQ init failed\n");
        br_plat_park_forever();
    }

    if (br_plat_memmap_init() != 0) {
        br_console_puts("[FATAL] mem region declare failed\n");
        br_plat_park_forever();
    }

    if (br_plat_mmu_ops_register() != 0) {
        br_console_puts("[FATAL] page-table ops register failed\n");
        br_plat_park_forever();
    }

    /* CNTFRQ_EL0 is preset by firmware; reload writes CNTP_TVAL_EL0 / CNTP_CTL_EL0. */
}

BR_NORETURN void br_plat_park_forever(void)
{
    for (;;) {
        /* WFE: lowest-power spin without a scheduler; interrupts are masked so it never wakes. */
        __asm__ volatile("wfe" ::: "memory");
    }
}

/*
 * Landing point for the vector table: under fault/double fault do not call core facilities that
 * may be corrupted (no logging, locking or allocation); emit via the polling console and halt.
 * Shout-once level, not core's br_panic_bare(fmt, ...) observation contract.
 */
BR_NORETURN void br_plat_unexpected_exception(br_u64 vector)
{
    char digits[20];
    br_u32 n = 0;

    br_console_puts("\n[FATAL] unhandled exception -- vector=");

    if (vector == 0) {
        digits[n++] = '0';
    }
    while (vector != 0 && n < sizeof(digits)) {
        digits[n++] = (char)('0' + (br_u32)(vector % 10u));
        vector /= 10u;
    }
    while (n > 0) {
        br_console_putc(digits[--n]);
    }

    br_console_puts(" -- parked\n");

    br_plat_park_forever();
}

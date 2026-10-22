/*
 * hal/int — plugin wrapper. The interrupt framework has no init hook on purpose: its state is BSS
 * (`br_irq_cpu_init()` is called by the boot chain's phase 1, before any plugin phase can run) and
 * the IRQ exit hooks are installed by `hal/sched`. This file exists so the brick has a descriptor
 * with the mandatory `early_init`, mirroring every other plugin.
 */

/* Hooks the generated descriptor references. Declared here because a `.c` may only use what it
 * declares or includes (ADR-0038 / R1), and these names are derived by the generator from
 * `symbol_prefix`, so no public header carries them. */
int hal_int_early_init(void);
int hal_int_start(void);

int hal_int_early_init(void)
{
    return 0;
}

int hal_int_start(void)
{
    return 0;
}

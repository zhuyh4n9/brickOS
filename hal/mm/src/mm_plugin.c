/*
 * hal/mm — plugin wrapper. `br_mem_init()` (claim the three pools per the platform region table) and
 * `br_mm_activate()` (identity map + MMU on) are called by the boot chain's phase 3, between the
 * platform's `early_init` and the plugin phase machine: the order is hard-coded in `br_core_main`
 * (ADR-0008), not derived from init edges, so this brick declares no init hook.
 */

/* Hooks the generated descriptor references (see hal/int/src/int_plugin.c for why they are here). */
int hal_mm_early_init(void);
int hal_mm_start(void);

int hal_mm_early_init(void)
{
    return 0;
}

int hal_mm_start(void)
{
    return 0;
}

/*
 * hal/sched — plugin wrapper (lifecycle + IRQ-exit hook installation).
 *
 * The scheduling *mechanism* is `sched_core.c` / `switch.S` / `sync.c` / `work_core.c`; this file is
 * the only part that knows it is a plugin. Its `early_init` installs `br_irq_hooks_t` (ADR-0039):
 * the IRQ core must not depend on the scheduler brick, so the mechanism owner registers instead.
 *
 * Why EARLY is the right phase: the hooks have to be in place before the first interrupt can be
 * taken (IRQs are enabled only after every init returned), and this runs before that. No `init`
 * hook: the framework has no state to initialise at a phase boundary -- its BSS is the state.
 */
#include <br/hal/br_irq.h>
#include <br/hal/br_sched.h>
#include <br/hal/br_work.h>

/* Hooks the generated descriptor references. Declared here because a `.c` may only use what
 * it declares or includes (ADR-0038 / R1), and these names are derived by the generator
 * from `symbol_prefix`, so no public header carries them. */
int hal_sched_early_init(void);
int hal_sched_start(void);

/* `br_sched_irq_epilogue()` / `br_work_*` are declared by <br/hal/br_sched.h> and <br/hal/br_work.h>
 * (already included): the definitions live in sched_core.c / work_core.c of this same brick. */

static const br_irq_hooks_t s_hooks = {
    .irq_exit   = br_sched_irq_epilogue,
    .bh_submit  = br_work_submit,
    .bh_drain   = br_work_drain,
    .in_bh      = br_work_in_bh,
};

int hal_sched_early_init(void)
{
    br_irq_set_hooks(&s_hooks);
    return 0;
}

int hal_sched_start(void)
{
    return 0;
}

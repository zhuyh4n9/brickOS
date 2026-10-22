/*
 * Plugin meta-contract and plugin manager. Plugins export no symbols, only the `.br_plugins`
 * section; the generated artifact (build/gen/<plugin>/plugin_desc.c) instantiates a br_plugin_t,
 * so this hand-written header is the contract's only source of truth. `.br_plugins` is aligned(8)
 * because the struct holds pointers; br_res_t budget fields are diagnostic only (budget truth
 * lives in the manifest).
 */
#ifndef BR_CORE_BR_PLUGIN_H
#define BR_CORE_BR_PLUGIN_H

#include <br/core/br_types.h>

/* ---- phases ---- */

/* Four phases: EARLY (no heap/threads, IRQs off) -> CORE -> LATE -> APP. `rank` is monotonic. */
#define BR_PHASE_EARLY  0u
#define BR_PHASE_CORE   1u
#define BR_PHASE_LATE   2u
#define BR_PHASE_APP    3u
/* "no completion-point constraint" in a dependency declaration (`[[dep]].phase` default). */
#define BR_PHASE_NONE   0xFFu

/* ---- deps ---- */

/* The three kinds of structural dependency edge. */
#define BR_DEP_INIT     0u   /* init order: acyclic, part of topological sort */
#define BR_DEP_RUNTIME  1u   /* runtime call: not part of topology */
#define BR_DEP_TYPE     2u   /* type/header: not part of topology */

/* One dependency edge. `deps` ends at `.name == BR_NULL` (not by length). */
typedef struct br_dep {
    const char *name;      /* full name of the dependency (`runtime/qemu-aarch64`) */
    br_u8       kind;      /* BR_DEP_* */
    br_u8       phase;     /* assertion: provider's completion point (meaningful only for kind=INIT) */
    br_u16      compat_gen;
} br_dep_t;

/* ---- sched class ---- */

/* `[plugin].sched_class`: runtime records only, never enforces (composition-time check). */
#define BR_SCHED_CLASS_SAFE_PREEMPT  0u
#define BR_SCHED_CLASS_COOP_ONLY     1u
#define BR_SCHED_CLASS_TT_SAFE       2u

/* ---- plugin type ---- */

/* plugin_type / subkind go into the descriptor: the manager does not guess the category from
 * naming conventions, and knowing who is the target runtime is required to run its early_init
 * first (ADR-0040: the type used to be called `platform`). */
#define BR_PLUGIN_TYPE_APP       0u
#define BR_PLUGIN_TYPE_INTERFACE 1u
#define BR_PLUGIN_TYPE_ABILITY   2u
#define BR_PLUGIN_TYPE_RUNTIME   3u
/* The core: mandatory and singular (exactly one per image). It defines no lifecycle -- it is the
 * plugin that drives the others (boot chain + plugin manager), so early_init/init/start are all
 * BR_PLUGIN_NO_HOOK in its descriptor and it never enters the phase machine it runs. */
#define BR_PLUGIN_TYPE_CORE      4u

#define BR_SUBKIND_NONE      0u
#define BR_SUBKIND_SCHEDULER 1u
#define BR_SUBKIND_FRAMEWORK 2u
#define BR_SUBKIND_IO        3u
#define BR_SUBKIND_FS        4u
#define BR_SUBKIND_SERVICE   5u

/* `[plugin].phase` self-report (diagnostics only; the actual init phase is decided by category). */

/* ---- resources ---- */

/* Runtime mirror of `[[res]]` (diagnostics only). */
typedef struct br_res {
    br_u32 ram_kib;
    br_u32 stack_kib;
} br_res_t;

/* ---- descriptor ---- */

/* Plugin descriptor; the generated artifact instantiates it (`build/gen/<plugin>/plugin_desc.c`)
 * and hand-written plugins neither need nor should write their own. */
typedef struct br_plugin {
    const char *name;                 /* [plugin].name from brick.toml */
    br_u32      plugin_type;          /* BR_PLUGIN_TYPE_* */
    br_u32      subkind;              /* BR_SUBKIND_* */
    const char *phase_self;           /* [plugin].phase self-report ("early"/"core"/"late"/"app") */
    br_u16      ver[4];               /* COMPAT_GEN.MAJOR.MINOR.REVISE */
    br_u16      api_rev;
    br_u32      sched_class;          /* BR_SCHED_CLASS_* */
    const br_dep_t *deps;             /* ends at .name == BR_NULL; BR_NULL = no edges */
    const char *abi_id;
    const char *const *api_syms;      /* only Interface plugins own a symbol family (1-01 §7.3) */
    br_res_t    res;
    int (*early_init)(void);          /* nullable (use BR_PLUGIN_NO_HOOK); **always** NO_HOOK for APP (ADR-0037) */
    int (*init)(void);                /* runs in the phase its category decides; APP => the APP phase, last */
    int (*start)(void);
    /*
     * Selftest hook, the only entry to interface tests (nullable). Returns the number of failing
     * items (0 = all green) and logs each verdict (`[XXXCONF] PASS/FAIL <id>`); negative means the
     * hook itself failed. Not a fifth phase: phases are capability completion points, selftest is
     * verification, run once for all plugins. It is not an `[[export]]` API (composition-time
     * contract, like early_init/init/start): exposing it would put the test surface into the golden
     * interface. With product.toml [selftest] off the generated artifact emits BR_PLUGIN_NO_HOOK,
     * so --gc-sections strips the test code from the image entirely.
     */
    int (*selftest)(void);
} br_plugin_t;

/* Empty hook spelling, so the manager need not distinguish two kinds of empty (0 vs non-0). */
#define BR_PLUGIN_NO_HOOK  ((int (*)(void))0)

/* Section name and linker boundary symbols (defined by `platform/.../src/link.ld` with KEEP).
 * The name contains '.', so GNU ld will not auto-generate `__start_/__stop_` for it. */
#define BR_PLUGIN_SECTION_NAME ".br_plugins"

/* Declare a descriptor (expansion macro for generated artifacts; `deps` takes `const br_dep_t[]`
 * or BR_NULL). aligned(8) because the struct holds pointers. */
#define BR_PLUGIN_SECTION  __attribute__((used, section(BR_PLUGIN_SECTION_NAME), aligned(8)))

#define BR_PLUGIN_DEFINE(sym, nm, v0, v1, v2, v3, rev, cls, dp, abi, syms, res_, ei, in, st) \
    static const br_plugin_t BR_PLUGIN_SECTION sym = {                                       \
        .name = (nm),                                                                        \
        .ver = {(v0), (v1), (v2), (v3)},                                                     \
        .api_rev = (rev),                                                                    \
        .sched_class = (cls),                                                                \
        .deps = (dp),                                                                        \
        .abi_id = (abi),                                                                     \
        .api_syms = (syms),                                                                  \
        .res = (res_),                                                                       \
        .early_init = (ei),                                                                  \
        .init = (in),                                                                        \
        .start = (st),                                                                       \
    }
/* BR_PLUGIN_DEFINE omits `selftest` (parameter list frozen), so the field is zero-initialized per
 * C rules = BR_PLUGIN_NO_HOOK. A descriptor that wants a selftest should use a named initializer
 * and append `.selftest = function_name`. */

/* ---- manager ---- */

/*
 * The manager's three entries = the boot chain's slots: init (core, runtime-independent),
 * runtime_init (the runtime plugin), run (topology + phase drive). They are separate because
 * "scan the section / find the runtime" must precede any C code, the runtime's early_init is the
 * second bootstrap slot, and the phase drive waits for core's heap and address mapping.
 */

/*
 * 1 Runtime-independent static preparation: scan `.br_plugins` into a table and find the runtime
 * plugin. Silent throughout (console comes up in that plugin's early_init; earlier PL011 writes are
 * dropped). Empty section / over the static bound / no runtime plugin => panic.
 */
void br_plugin_manager_init(void);

/*
 * 2 Runtime plugin init: take the entry with plugin_type == BR_PLUGIN_TYPE_RUNTIME and run its
 * early_init (early console / PIC / region table / page table ops). By field, not by topological
 * luck: PIC, page tables and console are not ready yet (TC-PLUG-003). Precondition:
 * br_plugin_manager_init() has run. First failure halts.
 */
void br_plugin_manager_runtime_init(void);

/*
 * 4 Build init edges -> Kahn topological sort (cycle => panic) -> EARLY (early_init of the rest)
 *   -> CORE (category-decided init) -> LATE -> APP (the APP's init, last of all)
 *   -> global IRQs on -> START (topological, APP last) -> SELFTEST pass -> br_sched_run().
 * The APP sits at the tail of both the init and the start sequence, and emits no `early_init`
 * (ADR-0037): it is the payload, so everything it may consume is up before it acts.
 * Precondition: 1 and 2 have run. Does not return.
 */
BR_NORETURN void br_plugin_manager_run(void);

/*
 * Selftest pass. Not a fifth phase: it is verification, run once for all plugins in init's
 * topological order. It runs after START and before br_sched_run(), when the timer is armed, IRQs
 * are on and devices/mounts are in place; control flow never returns to single-threaded testing
 * afterwards. Plugins with selftest == BR_PLUGIN_NO_HOOK are skipped, and the failure count is
 * returned without halting (red/green is judged by the gate), unlike init/start. Ends with
 * `[SELFTEST] SUMMARY plugins=N ran=M fails=K`.
 */
void br_plugin_manager_selftest(void);

/* ---- observation ---- */

/* `[plugin].phase` self-report string (`p` empty => "?"). */
const char *br_plugin_phase_self(const br_plugin_t *p);

/* Total descriptor count (entries in the section). */
br_u32 br_plugin_count(void);

/* Get the i-th descriptor in **topological order** (i >= count => BR_NULL). */
const br_plugin_t *br_plugin_at(br_u32 index);

/* Look up by name (linear over the static table built at boot). */
const br_plugin_t *br_plugin_find(const char *name);

/* Phase name ("EARLY"/"CORE"/"LATE"/"APP") and the phase reached so far (observation). */
const char *br_plugin_phase_name(br_u32 phase);
br_u32 br_plugin_phase_reached(void);

/* init failure count (prototype convention: first failure halts, so 0 or 1). */
br_u32 br_plugin_init_failures(void);

/* The plugin manager selftest suite is declared in core/c_core/selftest/, not here (test surface). */

/*
 * ---- mount plan ----
 * Mounting is decided by the APP: FS plugins only register drivers with vfs-core (fstype -> ops),
 * and the APP declares what to mount and where through [[mount]] in its own brick.toml, which
 * brickie generates into this descriptor; the APP start phase runs POSIX mount() one by one. That
 * keeps one source of truth (br-wa-fs-001: with two, changing one silently mounts the wrong
 * device). Fields map to mount(source, target, fstype, flags, data): `source` (BR_NULL for
 * tmpfs/devfs), `target` (mount point), `fstype` (driver), `order` = build-time sort only.
 */
typedef struct br_mount_plan {
    const char *source;     /* BR_NULL = no medium */
    const char *target;
    const char *fstype;
    br_u32      order;
} br_mount_plan_t;

#endif /* BR_CORE_BR_PLUGIN_H */

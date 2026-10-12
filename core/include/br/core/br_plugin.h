/*
 * brickOS prototype v0.2.0 — plugin meta-contract and plugin manager (core side)
 *
 * Design basis (on the brickOS-Design branch):
 *   - `1-01` §6.1 (meta-contract br_plugin_t / BR_PLUGIN macro), §13.3 (section collection), §9 (boot sequence)
 *   - `3-05` §2 (plugin_manager: scan section -> topology -> phase drive), §2.2 (__br_plugins_start/stop)
 *   - `1-01` §6.1's "near-zero export surface" (CA-10): plugins export **no symbols**, only the `.br_plugins` section
 *
 * Who writes this header: **core hand-written** (ruling G8). It is not generated — the generated
 * artifact is each plugin's `build/gen/<plugin>/plugin_desc.c`, which `#include <br/core/br_plugin.h>`
 * and instantiates a `br_plugin_t`. So the meta-contract has exactly one source of truth: this file.
 *
 * **Deviations** from the design (each registered in `docs/decisions/0005-plugin-manager.md`):
 *   - `aligned(4)` -> `aligned(8)`: the struct holds pointers (`1-01` §13.3's 4 is for scalar-only
 *     tables, too small for a struct with pointers — aligning `.br_plugins` to 8 is what is safe).
 *   - `br_res_t` budget fields are **diagnostic only** at runtime (budget truth lives in the manifest,
 *     enforced by `brickie check`; `[[res]]` of `plugin_type = "platform"` is a capacity) — see ADR-0005 §2.
 */
#ifndef BR_CORE_BR_PLUGIN_H
#define BR_CORE_BR_PLUGIN_H

#include <br/core/br_types.h>

/* ==================================================================== phases */

/* Four phases (1-01 §9): EARLY (no heap/threads/IRQs off) -> CORE -> LATE -> APP.
 * `rank` is monotonic (1-01 §9's completion-point assertions rest on it). */
#define BR_PHASE_EARLY  0u
#define BR_PHASE_CORE   1u
#define BR_PHASE_LATE   2u
#define BR_PHASE_APP    3u
/* "no completion-point constraint" in a dependency declaration (`[[dep]].phase` default). */
#define BR_PHASE_NONE   0xFFu

/* ==================================================================== deps */

/* The three kinds of structural dependency edge (4-04 / contract §7.1 [[dep]].kind). */
#define BR_DEP_INIT     0u   /* init order: acyclic, part of topological sort */
#define BR_DEP_RUNTIME  1u   /* runtime call: not part of topology */
#define BR_DEP_TYPE     2u   /* type/header: not part of topology */

/* One dependency edge. `deps` ends at `.name == BR_NULL` (not by length). */
typedef struct br_dep {
    const char *name;      /* full name of the dependency (`platform/qemu-aarch64`) */
    br_u8       kind;      /* BR_DEP_* */
    br_u8       phase;     /* assertion: provider's completion point (meaningful only for kind=INIT) */
    br_u16      compat_gen;
} br_dep_t;

/* ==================================================================== sched class */

/* `[plugin].sched_class` (1-01 §9 combination-legality matrix; runtime **records only, does not
 * enforce**, ruling G10 — enforcement is at composition time in `brickie check`). */
#define BR_SCHED_CLASS_SAFE_PREEMPT  0u
#define BR_SCHED_CLASS_COOP_ONLY     1u
#define BR_SCHED_CLASS_TT_SAFE       2u

/* ==================================================================== plugin type */

/*
 * `plugin_type` / `subkind` **go into the descriptor**: the plugin manager does not guess the
 * category from naming conventions (`1-01` §6.1's table lacks these two fields; they are a necessary
 * addition of this prototype — see ADR-0005 §2: "the first step of the EARLY phase must be
 * platform's early_init, otherwise console is not up"; knowing who is platform requires a field,
 * and that was already written in brick.toml anyway).
 */
#define BR_PLUGIN_TYPE_APP       0u
#define BR_PLUGIN_TYPE_INTERFACE 1u
#define BR_PLUGIN_TYPE_ABILITY   2u
#define BR_PLUGIN_TYPE_PLATFORM  3u

#define BR_SUBKIND_NONE      0u
#define BR_SUBKIND_SCHEDULER 1u
#define BR_SUBKIND_FRAMEWORK 2u
#define BR_SUBKIND_IO        3u
#define BR_SUBKIND_FS        4u
#define BR_SUBKIND_SERVICE   5u

/* `[plugin].phase` self-report (presentation/diagnostics only — which phase init lands in is
 * decided by **category**, see `1-01` §9: non-Service/Interface => CORE, Service/Interface => LATE).
 * The accessor is declared after `br_plugin_t` (see the "observation" section). */

/* ==================================================================== resources */

/* Runtime mirror of `[[res]]` (diagnostics; budget truth is in the manifest, see the file header). */
typedef struct br_res {
    br_u32 ram_kib;
    br_u32 stack_kib;
} br_res_t;

/* ==================================================================== descriptor */

/*
 * Plugin descriptor. **The generated artifact instantiates it** (`build/gen/<plugin>/plugin_desc.c`);
 * hand-written plugins neither need nor should write their own — the only truth is `brick.toml` plus
 * each plugin's header.
 */
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
    int (*early_init)(void);          /* nullable (use BR_PLUGIN_NO_HOOK) */
    int (*init)(void);
    int (*start)(void);
    /*
     * Selftest hook (the **only** entry to interface tests; nullable).
     *
     * Semantics: returns the **number of failing items** (0 = all green), and logs each item's
     * verdict (`[XXXCONF] PASS/FAIL <id> ...`); a negative value = the hook itself failed (recorded
     * separately from the failure count; see plugin_mgr's rc convention).
     * It is **not** a fifth phase: phases are "capability completion points" (1-01 §9's four phases),
     * while selftest is **verification**, happening at the same moment for all plugins (after all
     * starts, before the scheduler takes over — see ADR-0010).
     *
     * * Why it is not an API in `[[export]]`: it is of the same origin as early_init/init/start
     *   (ADR-0005 ruling 9) — selftest is a **composition-time contract** (the generated artifact
     *   references it), not a capability the plugin offers to others. Letting consumers call selftest
     *   would put the "test surface" into the golden interface, so changing one case becomes an
     *   interface change.
     *
     * * Generation-time switch (ADR-0010 §2.3): when `product.toml [selftest]` turns selftest off,
     *   the generated artifact emits `BR_PLUGIN_NO_HOOK` here => nothing references the test code
     *   => `--gc-sections` strips it out of the image entirely. So a "selftest-disabled image"
     *   contains **no test code**, not merely "does not run tests".
     */
    int (*selftest)(void);
} br_plugin_t;

/* Empty hook spelling: used by the generated artifact when "this plugin has no such phase", so
 * plugin_manager need not distinguish two kinds of empty (0 vs non-0). */
#define BR_PLUGIN_NO_HOOK  ((int (*)(void))0)

/* Section name and **linker boundary symbols** (defined by `platform/.../src/link.ld` with KEEP).
 * The name contains `.`, so GNU ld will not auto-generate `__start_/__stop_` => the boundary symbols
 * must be given explicitly. */
#define BR_PLUGIN_SECTION_NAME ".br_plugins"

/*
 * Declare a descriptor (expansion macro for generated artifacts; `deps` takes `const br_dep_t[]` or
 * BR_NULL). `aligned(8)`: the struct holds pointers — see the deviation note in the file header.
 */
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
/*
 * `BR_PLUGIN_DEFINE` does **not include `selftest`** (parameter list frozen, same origin as ADR-0005
 * ruling 1: the generated artifact now instantiates the struct directly). A macro user omitting
 * `.selftest` gets that field zero-initialized per C rules = `BR_PLUGIN_NO_HOOK` => "no selftest".
 * Hand-written descriptors that want a selftest should use a named initializer directly and append
 * `.selftest = function_name` at the end.
 */

/* ==================================================================== manager */

/*
 * The manager's three entries = the boot chain's three slots (see `br/core/br_main.h` and ADR-0008):
 *
 *   1 br_plugin_manager_init()            core platform-independent init (bootstrap stage 1)
 *   2 br_plugin_manager_platform_init()   platform plugin init (bootstrap stage 2)
 *   4 br_plugin_manager_run()             topology + phase drive (bootstrap stage 4)
 *
 * Why three slots instead of one big function: "scan section/find platform" must happen before any C
 * code (and console is not up yet then), "platform's early_init" is the boot chain's second slot, and
 * "phase drive" must wait until core's heap and address mapping are ready (stage 3) — the three have
 * different preconditions (ADR-0008 §2).
 */

/*
 * 1 Platform-independent static preparation: scan the `.br_plugins` section to build the plugin table
 * + explicitly find the platform plugin.
 *
 * **Silent throughout** (early console is not initialized yet — that belongs to platform's
 * `early_init`; before then, writes to PL011's DR are dropped by QEMU). Empty section / over the
 * static bound / no `plugin_type = platform` in the section => immediate panic (the image cannot be
 * valid anyway).
 *
 * Called by core's entry `br_core_main()`; edge building/topology/phase drive happen in 4.
 */
void br_plugin_manager_init(void);

/*
 * 2 Platform plugin init: explicitly take the entry with `plugin_type == BR_PLUGIN_TYPE_PLATFORM` and
 * run its `early_init` (early console / PIC registration and binding tables / region table / page
 * table ops).
 *
 * * Taking platform by **field** rather than by a coincidence of topological order: at this point PIC,
 *   page tables and console are not ready, so "who comes first" has nothing to do with "what can be
 *   observed". This is also TC-PLUG-003's criterion ("the first early_init that actually runs belongs
 *   to platform").
 *
 * Precondition: `br_plugin_manager_init()` has run. First failure halts (same discipline as 4's
 * init/start).
 */
void br_plugin_manager_platform_init(void);

/*
 * 4 Build init edges -> Kahn topological sort (cycle => panic, reporting the full cycle path)
 *   -> EARLY (early_init of the remaining plugins) -> CORE (category-decided init) -> LATE -> enable IRQs globally
 *   -> START (topological order, but APP is last by explicit rule)
 *   -> **SELFTEST pass** (per-plugin `selftest`; see below)
 *   -> `br_sched_run()` (core's idle/first schedule).
 *
 * Precondition: 1 and 2 have run (boot chain order, see `br/core/br_main.h`). Does not return.
 */
BR_NORETURN void br_plugin_manager_run(void);

/*
 * Selftest pass (ADR-0010). **Not a fifth phase**: phases are "capability completion points", selftest
 * is **verification** — it happens at the same moment for all plugins; the order merely "reuses init's
 * topological order" for determinism.
 *
 * Why at this moment (after START, before `br_sched_run()`): the timer is armed, IRQs are on, devices
 * are registered and mounts are in place — all preconditions of the existing consistency suites hold;
 * and once the scheduler takes over, control flow never returns to "run tests single-threaded".
 *
 * Convention:
 *   - plugins with `selftest == BR_PLUGIN_NO_HOOK` are skipped (**true for most images**: when the
 *     generation-time switch disables selftest, the generated artifact emits NO_HOOK);
 *   - returns the **number of failing items** (0 = all green); negative = the hook itself failed.
 *     Neither halts — selftest is **observation**, red/green is judged by the gate (`[XXXCONF] FAIL`
 *     is in gates.toml's forbid list); this is a **different discipline** from init/start's
 *     "first failure halts" (ruling G6), see ADR-0010 §2.4.
 *   - at the end of the pass, print one line `[SELFTEST] SUMMARY plugins=N ran=M fails=K` (boot evidence).
 */
void br_plugin_manager_selftest(void);

/* ==================================================================== observation */

/* `[plugin].phase` self-report string (`p` empty => "?"). */
const char *br_plugin_phase_self(const br_plugin_t *p);

/* Total descriptor count (entries in the section). */
br_u32 br_plugin_count(void);

/* Get the i-th descriptor in **topological order** (i >= count => BR_NULL). */
const br_plugin_t *br_plugin_at(br_u32 index);

/* Look up by name (linear; the name table is a static array built at boot). */
const br_plugin_t *br_plugin_find(const char *name);

/* Phase name ("EARLY"/"CORE"/"LATE"/"APP") and current progress (which phase was reached; observation). */
const char *br_plugin_phase_name(br_u32 phase);
br_u32 br_plugin_phase_reached(void);

/* init failure count (0 = all succeeded; prototype convention: first failure halts, so it is only 0 or 1). */
br_u32 br_plugin_init_failures(void);

/*
 * The plugin manager selftest suite has moved to `core/selftest/plugin_selftest.c` (ADR-0010), so it is
 * not declared here. Why not keep it in the public header: selftest is a **test surface**, not a plugin
 * capability — declaring it in `br_*.h` would turn "change one case" into an interface change (the
 * golden interface hash covers exactly the public declaration surface); implementation and declarations
 * both live in core/selftest/.
 */

/*
 * ==================================================================== mount plan (ADR-0030)
 *
 * **Mounting is decided by the APP**: FS plugins only register **drivers** with vfs-core (fstype name
 * -> ops); the APP **declares** "what to mount and where" in its own `brick.toml` via `[[mount]]`, and
 * brickie generates that table into this plugin's descriptor; the APP's start phase executes POSIX
 * `mount()` one by one.
 *
 * So the "mount plan" has exactly **one** source of truth (the APP's declaration surface), rather than
 * "one copy in the FS plugin + one in the manifest" (see br-wa-fs-001: with two truths, changing one
 * does not error, it just silently mounts the wrong device).
 *
 * Fields correspond one-to-one with POSIX `mount(source, target, fstype, flags, data)`:
 *   `source` = mount()'s 1st arg (device name for media-backed FS; BR_NULL for tmpfs/devfs)
 *   `target` = 2nd arg (mount point)
 *   `fstype` = 3rd arg (driver name, e.g. "tmpfs"/"fat"/"erofs")
 *   `order`  = **generation-time sorting only** (the APP must mount in ascending order: "/" before
 *              "/dev"...); it is not a POSIX argument, so it never enters the mount() call.
 */
typedef struct br_mount_plan {
    const char *source;     /* BR_NULL = no medium */
    const char *target;
    const char *fstype;
    br_u32      order;
} br_mount_plan_t;

#endif /* BR_CORE_BR_PLUGIN_H */

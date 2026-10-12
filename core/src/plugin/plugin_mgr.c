/*
 * brickOS prototype v0.2.0 — plugin manager
 *
 * Design basis (design repo):
 *   - `1-01` §9 boot sequence, §6.2 the four lifecycle phases and "two completion points per plugin", §6.5 init-DAG (cycle = hard error)
 *   - `3-05` §2 (plugin_manager: scan section -> topology -> phase drive), §2.2 (`__br_plugins_start/stop`)
 *   - `3-01` §13.3 (section collection: plugins export almost nothing, enumerated via the `.br_plugins` section + linker boundary symbols)
 *
 * **Everything** this file does, split by the three cells of the core boot chain (see `br/core/br_main.h` / ADR-0008):
 *
 *   `br_plugin_manager_init()`           [boot chain 1] scan `.br_plugins` -> plugin table + explicitly find platform
 *   `br_plugin_manager_platform_init()`  [boot chain 2] run the platform plugin's `early_init`
 *   `br_plugin_manager_run()`            [boot chain 4] topology + phase drive:
 *     1 section-count self-check + duplicate-name self-check (duplicates should have been caught at compose time; prove it at runtime too)
 *     2 build init edges (only `kind == BR_DEP_INIT`) -> **Kahn topological sort**
 *        * cycle ⇒ print the **full cycle path** and `br_panic` (a cycle is a compose-time hard error; runtime must prove it too)
 *     3 EARLY: the remaining plugins in topological order (**platform already ran in boot chain 2**, skipped here)
 *     4 CORE: category-decided `init` (non Service/Interface; `1-01` §9/§6.2)
 *     5 LATE: `init` of Service / Interface
 *     6 **globally enable IRQs** (core's `br_irq_cpu_enable()`)
 *     7 START: topological order, but **APP last is an explicit rule** (in topological order app/hello may come first)
 *     8 `br_sched_registered()` ⇒ `br_sched_run()`; if not registered, let the last start() hold the
 *        CPU itself (transition bridge: before the scheduler lands, APP's start does not return -- see ADR-0005 §2.5)
 *
 * ★ Clock/logging (where core.init goes) and heap/address mapping are **not in this file**: they belong to
 *   `core/src/main.c`'s four-stage boot chain -- ADR-0008 paid off ADR-0005 §2.6's debt of "the manager does core.init",
 *   so the plugin manager now does only the three plugin-related things: scan sections / sort / drive.
 *
 * Failure granularity (ruling G6): **first failure stops the machine** -- print `[PLUGIN] FAIL <name> phase=<phase> rc=<n>` then
 * `br_panic`. init failure = boot failure, no degradation (same obligation as design `3-02` §14.3).
 *
 * Why the phase names in logs are **lowercase** (early/core/late/start): the smoke gate in `tests/gates.toml`
 * uses `forbid = ["EARLY"]` to catch "a delay self-test printed EARLY" (delay shorter than requested); using uppercase
 * phase names in the plugin manager would **falsely trip** that forbidden pattern. The public phase name
 * (`br_plugin_phase_name`) still uses the design doc's uppercase form; only the log takes lowercase.
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include "plugin_mgr_internal.h"

/* ==================================================================== section bounds */

/*
 * Boundary symbols from the linker script (`platform/…/src/link.ld`, same pattern as `.br_extable`: the section name
 * contains `.`, so GNU ld does not auto-generate `__start_/__stop_` -- they must be assigned explicitly). `KEEP`
 * stops `--gc-sections` from dropping the descriptors.
 */
extern const br_plugin_t __br_plugins_start[];
extern const br_plugin_t __br_plugins_stop[];

/*
 * **Weak reference** to the core self-test suite entry (`core/selftest/`, ADR-0010).
 *
 * Why it must be weak: with self-test off, the whole `core/selftest/` directory **is not compiled** (see build.rs) ⇒ the
 * symbol does not exist. A weak reference still lets core link on its own (same trick as `br_plugin_gen_total`'s weak
 * reference in `plugin_mgr_internal.h`), and
 * a NULL pointer is the **only criterion** for "self-test not built into this image" -- no second switch needed and no
 * inconsistency where "the switch says on but the code was not compiled in".
 */
extern int br_core_selftest(void) __attribute__((weak));

/* ==================================================================== static bounds */

/* `BR_PLUGIN_MAX` (upper bound on descriptor count) is also in plugin_mgr_internal.h -- self-test uses it to size a temporary array. */
#define BR_PLUGIN_MAX_EDGES  128u  /* upper bound on init edges */
#define BR_PLUGIN_CYCLE_BUF  512u  /* cycle-path text buffer */
#define BR_DEP_TERM_GUARD    512u  /* hard bound when walking deps (no overrun even if a bad table does not terminate) */

/* The truth for `plug_edge_t` is in plugin_mgr_internal.h (the self-test's negative fixtures build fake edge sets). */

/* ==================================================================== state */
/*
 * ★ The entries marked `[self-test readable]` dropped `static` (contract in `plugin_mgr_internal.h`):
 *   the plugin manager's self-test suite (`core/selftest/plugin_selftest.c`, ADR-0010) uses **real state** as its
 *   criterion rather than mirroring a parallel ledger. They are still core-internal symbols only -- not part of
 *   `br_plugin.h`'s interface. The rest (`s_order` / `s_init_done` / start counters) stay private: self-test does not read them.
 */

const br_plugin_t *s_plugins[BR_PLUGIN_MAX];           /* [self-test readable] section order */
br_u32  s_count;                                       /* [self-test readable] entry count in the section */
plug_edge_t s_edges[BR_PLUGIN_MAX_EDGES];              /* [self-test readable] contains only kind == BR_DEP_INIT */
br_u32  s_edge_count;                                  /* [self-test readable] */
static br_u16  s_order[BR_PLUGIN_MAX];                 /* topological order (section indices) */
br_u16  s_pos[BR_PLUGIN_MAX];                          /* [self-test readable] section index -> topological rank */
br_bool s_sorted;                                      /* [self-test readable] */
static br_bool s_init_done;                            /* boot chain 1 has run (section scanned) */

static br_u32  s_phase_reached = BR_PHASE_EARLY;
static br_u32  s_init_failures;
br_u32  s_plat_index = (br_u32)-1;                     /* [self-test readable] section index of the platform plugin */

/* ---- self-test: trace of "ordering invariants" ---- */
const char *s_first_early_name;   /* [self-test readable] who owned the first early_init that actually ran */
static br_u32  s_non_app_start_done;     /* number of non-APP starts executed */
static br_u32  s_non_app_start_total;    /* plugins that are non-APP and have a start hook */
br_bool s_app_start_last_ok;      /* [self-test readable] before calling the first APP start: all non-APP ones already ran */
br_bool s_in_app_start;           /* [self-test readable] currently inside an APP start */

/* ==================================================================== section enumeration */

static br_u32 section_count(void)
{
    const br_u8 *lo = (const br_u8 *)(const void *)__br_plugins_start;
    const br_u8 *hi = (const br_u8 *)(const void *)__br_plugins_stop;

    if (hi <= lo) {
        return 0u;
    }
    return (br_u32)(hi - lo) / (br_u32)sizeof(br_plugin_t);
}

static const br_plugin_t *section_at(br_u32 i)
{
    return &__br_plugins_start[i];
}

static void scan(void)
{
    s_count = section_count();
    if (s_count > BR_PLUGIN_MAX) {
        br_panic("plugin_manager: .br_plugins has %u descriptors, over the static bound %u",
                 s_count, (br_u32)BR_PLUGIN_MAX);
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        s_plugins[i] = section_at(i);
    }
}

static br_u32 index_of_name(const char *name)
{
    if (name == BR_NULL) {
        return (br_u32)-1;
    }
    /* The section itself is the "name table" (built by the linker at boot); do not depend on whether scan() ran --
     * an observation entry should answer "is this name in the section" even before the manager starts. */
    for (br_u32 i = 0u; i < section_count(); i++) {
        const char *n = section_at(i)->name;
        if (n == BR_NULL) {
            continue;
        }
        const char *a = n;
        const char *b = name;
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return (br_u32)-1;
}

/* ==================================================================== phases and completion points */

/* Phase name for logs (lowercase, see the file header). */
static const char *phase_key(br_u32 phase)
{
    switch (phase) {
    case BR_PHASE_EARLY: return "early";
    case BR_PHASE_CORE:  return "core";
    case BR_PHASE_LATE:  return "late";
    default:             return "start";
    }
}

static br_bool is_service_like(const br_plugin_t *p)
{
    if (p->plugin_type == BR_PLUGIN_TYPE_INTERFACE) {
        return BR_TRUE;
    }
    return (p->plugin_type == BR_PLUGIN_TYPE_ABILITY) && (p->subkind == BR_SUBKIND_SERVICE);
}

/*
 * Which phase `init` lands in is decided by **category** (`1-01` §9: non Service/Interface ⇒ CORE,
 * Service/Interface ⇒ LATE). No init hook ⇒ completion point = EARLY (steps 2 and 1 coincide).
 *
 * Non-static: self-test uses it to re-check "every init edge is phase-monotonic" (TC-PLUG-003) --
 * see plugin_mgr_internal.h.
 */
br_u32 init_phase_of(const br_plugin_t *p)
{
    if (p->init == BR_PLUGIN_NO_HOOK) {
        return BR_PHASE_EARLY;
    }
    return is_service_like(p) ? BR_PHASE_LATE : BR_PHASE_CORE;
}

/* ==================================================================== topological sort */

/*
 * Kahn topological sort (pure algorithm: touches nothing outside plugin declarations, so negative cases can run it on a fake graph).
 *
 * Semantics: `edges[e] = {from, to}` = "from depends on to" ⇒ to must be dequeued first.
 * Returns BR_TRUE = sorted (first n entries of `order` valid); BR_FALSE = cycle (`cycle` holds the **closed** path,
 * first and last index equal, `cycle_len` includes the repeated closing entry).
 *
 * Non-static: TC-PLUG-002b's **negative case** must run this real thing (hand-built cycle fed in); otherwise it only proves a copy reports cycles
 * -- see plugin_mgr_internal.h.
 */
br_bool topo_sort(br_u32 n, const plug_edge_t *edges, br_u32 m,
                         br_u16 *order, br_u16 *cycle, br_u32 *cycle_len)
{
    br_u16 indeg[BR_PLUGIN_MAX];
    br_u16 queue[BR_PLUGIN_MAX];
    br_u32 head = 0u;
    br_u32 tail = 0u;
    br_u32 k = 0u;

    for (br_u32 i = 0u; i < n; i++) {
        indeg[i] = 0u;
    }
    for (br_u32 e = 0u; e < m; e++) {
        if (edges[e].from < n) {
            indeg[edges[e].from]++;
        }
    }
    for (br_u32 i = 0u; i < n; i++) {
        if (indeg[i] == 0u) {
            queue[tail++] = (br_u16)i;
        }
    }
    while (head < tail) {
        const br_u16 u = queue[head++];
        order[k++] = u;
        for (br_u32 e = 0u; e < m; e++) {
            if (edges[e].to != u) {
                continue;
            }
            const br_u16 v = edges[e].from;
            if (v >= n || indeg[v] == 0u) {
                continue;   /* already queued/dequeued: do not decrement again */
            }
            indeg[v]--;
            if (indeg[v] == 0u) {
                queue[tail++] = v;
            }
        }
    }
    if (k == n) {
        return BR_TRUE;
    }

    /* ---- cycle: DFS over "not dequeued" nodes to extract the **full cycle path** ---- */
    {
        br_u8  state[BR_PLUGIN_MAX];
        br_u16 stack[BR_PLUGIN_MAX];

        for (br_u32 i = 0u; i < n; i++) {
            state[i] = 0u;
        }
        *cycle_len = 0u;

        /* Iterative DFS (explicit stack), avoiding recursion and stack-depth worries for an n=32 graph. */
        for (br_u32 root = 0u; root < n; root++) {
            if (state[root] != 0u) {
                continue;
            }
            br_u32 depth = 0u;
            stack[depth++] = (br_u16)root;
            state[root] = 1u;
            while (depth > 0u) {
                const br_u16 u = stack[depth - 1u];
                br_u16 next = (br_u16)n;
                for (br_u32 e = 0u; e < m; e++) {
                    if (edges[e].from != u) {
                        continue;
                    }
                    const br_u16 v = edges[e].to;
                    if (v >= n) {
                        continue;
                    }
                    if (state[v] == 1u) {
                        /* back edge ⇒ the cycle is from v's position in the stack up to the top */
                        br_u32 at = 0u;
                        while (at < depth && stack[at] != v) {
                            at++;
                        }
                        br_u32 len = 0u;
                        for (br_u32 j = at; j < depth; j++) {
                            cycle[len++] = stack[j];
                        }
                        cycle[len++] = v;   /* close the cycle */
                        *cycle_len = len;
                        return BR_FALSE;
                    }
                    if (state[v] == 0u) {
                        next = v;
                        break;
                    }
                }
                if (next != (br_u16)n) {
                    state[next] = 1u;
                    stack[depth++] = next;
                    continue;
                }
                state[u] = 2u;
                depth--;
            }
        }
    }
    return BR_FALSE;
}

/* ==================================================================== failure stop */

static BR_NORETURN void plugin_fail(const char *name, br_u32 phase, int rc)
{
    s_init_failures++;
    br_log_error("[PLUGIN] FAIL %s phase=%s rc=%d",
                 (name != BR_NULL) ? name : "(null)", phase_key(phase), rc);
    br_panic("plugin_manager: first failure stops the machine (ruling G6): %s phase=%s rc=%d",
             (name != BR_NULL) ? name : "(null)", phase_key(phase), rc);
}

/* ==================================================================== manager */

static void build_edges(void)
{
    s_edge_count = 0u;
    for (br_u32 i = 0u; i < s_count; i++) {
        const br_dep_t *d = s_plugins[i]->deps;
        if (d == BR_NULL) {
            continue;
        }
        for (br_u32 k = 0u; k < BR_DEP_TERM_GUARD; k++, d++) {
            if (d->name == BR_NULL) {
                break;                      /* terminator: `.name == BR_NULL` */
            }
            if (d->kind != BR_DEP_INIT) {
                continue;                   /* only init edges participate in topology */
            }
            const br_u32 to = index_of_name(d->name);
            if (to == (br_u32)-1) {
                /* Compose-time closure should guarantee the provider is present; missing means a hard error (reporting beats silently misordering). */
                br_panic("plugin_manager: `%s` declares an init dependency on `%s`, which is not in the .br_plugins section",
                         s_plugins[i]->name, d->name);
            }
            if (s_edge_count >= BR_PLUGIN_MAX_EDGES) {
                br_panic("plugin_manager: init edges exceed the bound %u", (br_u32)BR_PLUGIN_MAX_EDGES);
            }
            s_edges[s_edge_count].from = (br_u16)i;
            s_edges[s_edge_count].to   = (br_u16)to;
            s_edge_count++;
        }
    }
}

/* Cycle path text (bounded; truncates long names but still shows "there is a cycle + who is on it"). */
static void cycle_text(const br_u16 *cycle, br_u32 cycle_len, char *out, br_u32 cap)
{
    br_u32 n = 0u;
    out[0] = '\0';
    for (br_u32 i = 0u; i < cycle_len; i++) {
        const char *name = (cycle[i] < s_count) ? s_plugins[cycle[i]]->name : "?";
        if (i > 0u) {
            if (n + 4u >= cap) {
                break;
            }
            out[n++] = ' ';
            out[n++] = '-';
            out[n++] = '>';
            out[n++] = ' ';
            out[n] = '\0';
        }
        for (const char *p = name; *p != '\0' && n + 1u < cap; p++) {
            out[n++] = *p;
        }
        out[n] = '\0';
    }
}

/* ==================================================================== boot chain 1 */

void br_plugin_manager_init(void)
{
    /* Scanning the section is pure computation with **no output**: the early console belongs to the boot chain 2 platform
     * plugin (see br_main.h), and writing PL011's DR now would be dropped by QEMU (CR.UARTEN = 0). */
    scan();

    /* ★ Find platform via the `plugin_type` field (explicit), not via a topological coincidence: PIC/page tables/
     *   console are not ready yet, so who comes first in topological order is unrelated to "can we observe". */
    br_u32 plat = (br_u32)-1;
    for (br_u32 i = 0u; i < s_count; i++) {
        if (s_plugins[i]->plugin_type == BR_PLUGIN_TYPE_PLATFORM) {
            plat = i;
            break;
        }
    }
    if (plat == (br_u32)-1) {
        br_panic("plugin_manager: no plugin with plugin_type = platform in the section; "
                 "the first step of the EARLY phase cannot happen (design 1-01 §9)");
    }
    s_plat_index = plat;
    s_init_done  = BR_TRUE;
}

/* ==================================================================== boot chain 2 */

void br_plugin_manager_platform_init(void)
{
    if (s_init_done == BR_FALSE || s_plat_index == (br_u32)-1) {
        /* This line is also dropped by QEMU (console not up), but "running on with an unscanned section" is worse */
        br_panic("plugin_manager: br_plugin_manager_init() must be called before platform_init()");
    }

    const br_plugin_t *p = s_plugins[s_plat_index];
    if (p->early_init == BR_PLUGIN_NO_HOOK) {
        br_panic("plugin_manager: platform plugin `%s` has no early_init hook; "
                 "console/PIC/page tables must be built by it first (design 1-01 §9)", p->name);
    }

    /* First step of the EARLY phase = platform's early_init (hard constraint). This is also where TC-PLUG-003's criterion
     * ("the first early_init that actually runs belongs to platform") is sampled. */
    s_phase_reached    = BR_PHASE_EARLY;
    s_first_early_name = p->name;

    const int rc = p->early_init();
    br_log_info("[PLUGIN] phase=%s name=%s rc=%d", phase_key(BR_PHASE_EARLY), p->name, rc);
    if (rc != 0) {
        plugin_fail(p->name, BR_PHASE_EARLY, rc);
    }
}

/* ==================================================================== boot chain 4 */

BR_NORETURN void br_plugin_manager_run(void)
{
    if (s_init_done == BR_FALSE) {
        br_panic("plugin_manager: br_plugin_manager_init() must be called before br_plugin_manager_run()");
    }

    /* ---- 1 section-count self-check: console is up from boot chain 2's platform.early_init,
     *      so logs are meaningful from this line on ---- */
    br_log_info("[PLUGIN] manager: .br_plugins section has %u descriptors", s_count);

    /* Duplicate-name self-check: duplicates should have been caught at compose time (globally unique). */
    for (br_u32 i = 0u; i < s_count; i++) {
        for (br_u32 j = i + 1u; j < s_count; j++) {
            const char *a = s_plugins[i]->name;
            const char *b = s_plugins[j]->name;
            const char *pa = a;
            const char *pb = b;
            if (a == BR_NULL || b == BR_NULL) {
                continue;
            }
            while (*pa != '\0' && *pa == *pb) {
                pa++;
                pb++;
            }
            if (*pa == '\0' && *pb == '\0') {
                br_panic("plugin_manager: duplicate name `%s` in .br_plugins (global uniqueness is a compose-time hard constraint)", a);
            }
        }
    }

    /* ---- 2 init edges + topological sort ---- */
    build_edges();
    br_log_info("[PLUGIN] manager: %u init edges (only kind=init)", s_edge_count);

    {
        br_u16 cycle[BR_PLUGIN_MAX];
        br_u32 cycle_len = 0u;
        char   text[BR_PLUGIN_CYCLE_BUF];

        if (!topo_sort(s_count, s_edges, s_edge_count, s_order, cycle, &cycle_len)) {
            cycle_text(cycle, cycle_len, text, (br_u32)sizeof(text));
            br_log_error("[PLUGIN] cycle: %s", text);
            br_panic("plugin_manager: init-DAG has a cycle (compose-time hard error, proven at runtime): %s", text);
        }
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        s_pos[s_order[i]] = (br_u16)i;
    }
    s_sorted = BR_TRUE;

    /* ---- 3 EARLY: remaining plugins in topological order (platform ran in boot chain 2, skipped here) ---- */
    for (br_u32 k = 0u; k < s_count; k++) {
        const br_u32 i = s_order[k];
        const br_plugin_t *p = s_plugins[i];
        if (i == s_plat_index || p->early_init == BR_PLUGIN_NO_HOOK) {
            continue;
        }
        if (s_first_early_name == BR_NULL) {
            s_first_early_name = p->name;
        }
        const int rc = p->early_init();
        br_log_info("[PLUGIN] phase=%s name=%s rc=%d", phase_key(BR_PHASE_EARLY), p->name, rc);
        if (rc != 0) {
            plugin_fail(p->name, BR_PHASE_EARLY, rc);
        }
    }

    /* ---- 4 CORE / LATE: category-decided init ---- */
    for (br_u32 want = BR_PHASE_CORE; want <= BR_PHASE_LATE; want++) {
        s_phase_reached = want;
        for (br_u32 k = 0u; k < s_count; k++) {
            const br_u32 i = s_order[k];
            const br_plugin_t *p = s_plugins[i];
            if (init_phase_of(p) != want) {
                continue;
            }
            const int rc = p->init();
            br_log_info("[PLUGIN] phase=%s name=%s rc=%d", phase_key(want), p->name, rc);
            if (rc != 0) {
                plugin_fail(p->name, want, rc);
            }
        }
    }

    /* ---- 5 globally enable IRQs (after LATE, before START) ---- */
    br_irq_cpu_enable();
    br_log_info("[PLUGIN] irq=on (LATE done, all plugin init returned)");

    /* ---- 6 START: non-APP first (topological order), APP after (topological order) ---- */
    s_phase_reached = BR_PHASE_APP;
    s_non_app_start_total = 0u;
    s_non_app_start_done  = 0u;
    for (br_u32 k = 0u; k < s_count; k++) {
        const br_plugin_t *p = s_plugins[s_order[k]];
        if (p->plugin_type != BR_PLUGIN_TYPE_APP && p->start != BR_PLUGIN_NO_HOOK) {
            s_non_app_start_total++;
        }
    }
    for (br_u32 pass = 0u; pass < 2u; pass++) {
        /* pass 0 = non-APP; pass 1 = APP. APP last is an **explicit rule** (§6.2's table:
         * "after globally enabling IRQs start(all plugins) -> app.start()"), not a topological coincidence --
         * in practice app/hello comes first in topological order. */
        if (pass == 1u) {
            s_app_start_last_ok = (s_non_app_start_done == s_non_app_start_total);
        }
        for (br_u32 k = 0u; k < s_count; k++) {
            const br_u32 i = s_order[k];
            const br_plugin_t *p = s_plugins[i];
            const br_bool want_app = (pass == 1u) ? BR_TRUE : BR_FALSE;
            const br_bool is_app = (p->plugin_type == BR_PLUGIN_TYPE_APP) ? BR_TRUE : BR_FALSE;
            if (want_app != is_app) {
                continue;
            }
            if (p->start == BR_PLUGIN_NO_HOOK) {
                continue;
            }
            if (is_app) {
                s_in_app_start = BR_TRUE;
            }
            const int rc = p->start();
            if (is_app) {
                s_in_app_start = BR_FALSE;
            }
            if (!is_app) {
                s_non_app_start_done++;
            }
            br_log_info("[PLUGIN] phase=start name=%s rc=%d", p->name, rc);
            if (rc != 0) {
                plugin_fail(p->name, BR_PHASE_APP, rc);
            }
        }
    }

    /* ---- 7 self-test pass (after START, before the scheduler takes over; ADR-0010) ----
     * Why it must be here: the timer is armed, IRQs are on, devices are registered, mounts are in place --
     * all existing consistency suites have their preconditions met; and once `br_sched_run()` takes over, control flow can never
     * return to the "run tests sequentially on one thread" shape.
     *
     * WORKAROUND(br-wa-boot-002): "before the scheduler takes over" is **not** the same as "single-threaded" -- timer preemption
     * already works before `br_sched_run()` (ADR-0011's IRQ-exit stack switch only checks "is there a ready task"),
     * so **any plugin that created a thread in the start phase** (app/hello does) makes that thread run concurrently with
     * this self-test pass from that moment on. ADR-0022 really hit it: both sides accessed the same eMMC card/volume at once,
     * with symptoms of EROFS read EIO, SDHCI command timeout, and a red FAT write closure. The action then was to move
     * "APP cases that touch devices" back into the start phase (see `app/hello/src/main.c`), **not** to change this --
     * the correct exit condition is to add a "no preemption during the self-test pass" constraint to the phase machine (or move the self-test pass
     * after `br_sched_run()` in a scheduler-driven shape); that is a phase-model change, out of scope here. */
    br_plugin_manager_selftest();

    /* ---- 8 scheduler: if registered, hand over; if not ⇒ the last start() was supposed to hold the CPU ---- */
    if (br_sched_registered()) {
        br_log_info("[PLUGIN] manager: scheduler registered ⇒ br_sched_run()");
        br_sched_run();
    }
    br_log_warn("[PLUGIN] manager: no scheduler and no start() holding the CPU ⇒ WFI halt (transition bridge)");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ==================================================================== self-test pass */

/*
 * Self-test is **observation**, not a cell of boot, so its discipline **differs** from init/start's (ADR-0010 §2.4):
 *   - init/start failure ⇒ `plugin_fail` stops the machine (ruling G6: boot failure does not degrade);
 *   - self-test failure ⇒ **record only, do not stop**: red/green is judged by the gate (`[XXXCONF] FAIL` is in gates.toml's forbid list),
 *     because "some case is red" and "the system cannot boot" are two different things, and mixing them makes debugging hopeless.
 *
 * Order = init's topological order (no separate rule): self-tests have no dependencies; topological order just gives the "output order"
 * **determinism** (same log order across two runs of the same image), which matters for diffing logs.
 */
void br_plugin_manager_selftest(void)
{
    if (s_sorted == BR_FALSE) {
        br_panic("plugin_manager: selftest() must be called after the topological sort");
    }

    br_log_info("[SELFTEST] ---- plugin self-tests (after START, before the scheduler takes over) ----");

    br_u32 ran = 0u;      /* plugins whose self-test actually ran */
    br_u32 skipped = 0u;  /* no selftest hook (switch off / not declared) */
    br_u32 fails = 0u;    /* sum of plugin self-tests' **failed item counts** */
    br_u32 errors = 0u;   /* times a hook itself returned a negative value */

    for (br_u32 k = 0u; k < s_count; k++) {
        const br_plugin_t *p = s_plugins[s_order[k]];
        if (p->selftest == BR_PLUGIN_NO_HOOK) {
            skipped++;
            continue;
        }

        const int rc = p->selftest();
        if (rc < 0) {
            /* Negative = the hook itself failed (not "N items failed"). Count separately, so "a case is red"
             * and "the self-test never finished" are not mixed into one number. */
            errors++;
            br_log_error("[SELFTEST] ERROR %s: selftest hook returned %d (hook error, not a failure count)",
                         p->name, rc);
        } else {
            fails += (br_u32)rc;
            br_log_info("[SELFTEST] %s rc=%d", p->name, rc);
        }
        ran++;
    }

    /* core's own suites (plugin manager/service registry/scheduler/sync; implementations all in `core/selftest/`).
     * They share the switch with plugin self-tests --
     * when off, `core/selftest/` is not compiled at all ⇒ the weak reference here reads BR_NULL and is skipped.
     * This is how "core-side code also stays out of the image" is implemented (ADR-0010 §2.3 / build.rs). */
    if (br_core_selftest != BR_NULL) {
        const int rc = br_core_selftest();
        if (rc < 0) {
            errors++;
            br_log_error("[SELFTEST] ERROR core: rc=%d", rc);
        } else {
            fails += (br_u32)rc;
            br_log_info("[SELFTEST] core rc=%d", rc);
        }
        ran++;
    } else {
        skipped++;
    }

    br_log_info("[SELFTEST] SUMMARY plugins=%u ran=%u skipped=%u fails=%u errors=%u",
                s_count, ran, skipped, fails, errors);
}

/* ==================================================================== observation surface */

const char *br_plugin_phase_self(const br_plugin_t *p)
{
    if (p == BR_NULL || p->phase_self == BR_NULL) {
        return "?";
    }
    return p->phase_self;
}

br_u32 br_plugin_count(void)
{
    return section_count();
}

const br_plugin_t *br_plugin_at(br_u32 index)
{
    const br_u32 n = section_count();
    if (index >= n) {
        return BR_NULL;
    }
    if (s_sorted && index < s_count) {
        return s_plugins[s_order[index]];
    }
    /* Manager has not sorted yet ⇒ fall back to section order (topological order only matters after run()). */
    return section_at(index);
}

const br_plugin_t *br_plugin_find(const char *name)
{
    const br_u32 i = index_of_name(name);
    if (i == (br_u32)-1) {
        return BR_NULL;
    }
    return section_at(i);
}

const char *br_plugin_phase_name(br_u32 phase)
{
    switch (phase) {
    case BR_PHASE_EARLY: return "EARLY";
    case BR_PHASE_CORE:  return "CORE";
    case BR_PHASE_LATE:  return "LATE";
    case BR_PHASE_APP:   return "APP";
    default:             return "?";
    }
}

br_u32 br_plugin_phase_reached(void)
{
    return s_phase_reached;
}

br_u32 br_plugin_init_failures(void)
{
    return s_init_failures;
}

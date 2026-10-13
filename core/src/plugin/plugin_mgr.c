/*
 * Plugin manager: scan the `.br_plugins` section into a table + find the platform plugin
 * (boot chain 1), run the platform's early_init (boot chain 2), then topology + phase drive
 * (boot chain 4): build init edges, Kahn sort, EARLY/CORE/LATE init, enable IRQs, START
 * (non-APP first, APP last by rule), self-test pass, hand off to the scheduler.
 * A cycle is a compose-time hard error, re-proven at runtime (full path + br_panic); the first
 * failure stops the machine (ruling G6). Clock/logging and heap setup are not here (main.c).
 * Log phase names are lowercase on purpose: tests/gates.toml forbids "EARLY"; the public
 * br_plugin_phase_name() keeps the uppercase form.
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include "plugin_mgr_internal.h"

/* ---- section bounds ---- */

/*
 * Linker boundary symbols: GNU ld does not generate `__start_/__stop_` for a section name
 * containing `.`; `KEEP` stops `--gc-sections` from dropping the descriptors.
 */
extern const br_plugin_t __br_plugins_start[];
extern const br_plugin_t __br_plugins_stop[];

/* Weak ref to the core self-test entry (core/selftest/): not compiled when the switch is off,
 * so the symbol may be missing. NULL is the only criterion for "self-test not in this image". */
extern int br_core_selftest(void) __attribute__((weak));

/* ---- static bounds ---- */

/* BR_PLUGIN_MAX (descriptor bound) is in plugin_mgr_internal.h; self-test sizes a temp array. */
#define BR_PLUGIN_MAX_EDGES  128u  /* upper bound on init edges */
#define BR_PLUGIN_CYCLE_BUF  512u  /* cycle-path text buffer */
#define BR_DEP_TERM_GUARD    512u  /* hard bound when walking deps (no overrun even if a bad table does not terminate) */

/* ---- state ---- */
/* `[self-test readable]` entries dropped `static` for plugin_mgr_internal.h's contract and
 * remain core-internal; s_order/s_init_done/start counters stay private. */

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

/* ---- section enumeration ---- */

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
    /* The section is the name table; answer from it even if scan() has not run yet. */
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

/* ---- phases and completion points ---- */

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

/* Which phase `init` lands in, by category: non Service/Interface => CORE, Service/Interface
 * => LATE; no init hook => EARLY. Non-static so TC-PLUG-003 can recheck phase monotonicity. */
br_u32 init_phase_of(const br_plugin_t *p)
{
    if (p->init == BR_PLUGIN_NO_HOOK) {
        return BR_PHASE_EARLY;
    }
    return is_service_like(p) ? BR_PHASE_LATE : BR_PHASE_CORE;
}

/* ---- topological sort ---- */

/* Kahn topological sort (pure: touches no plugin state, so negative cases can run it on a
 * fake graph). edges[e] = {from,to} = "from depends on to" => to is dequeued first.
 * BR_TRUE = sorted (first n of `order` valid); BR_FALSE = cycle in `cycle` (closed path, first
 * and last index equal, `cycle_len` counts the repeated closing entry). TC-PLUG-002b runs it. */
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

        /* Iterative DFS (explicit stack): no recursion. */
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
                        /* back edge => the cycle is from v's position in the stack up to the top */
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

/* ---- failure stop ---- */

static BR_NORETURN void plugin_fail(const char *name, br_u32 phase, int rc)
{
    s_init_failures++;
    br_log_error("[PLUGIN] FAIL %s phase=%s rc=%d",
                 (name != BR_NULL) ? name : "(null)", phase_key(phase), rc);
    br_panic("plugin_manager: first failure stops the machine (ruling G6): %s phase=%s rc=%d",
             (name != BR_NULL) ? name : "(null)", phase_key(phase), rc);
}

/* ---- manager ---- */

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
                /* Provider must exist by compose-time closure; missing is a hard error. */
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

/* Cycle path text (bounded; may truncate long names). */
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

/* ---- boot chain 1 ---- */

void br_plugin_manager_init(void)
{
    /* No output: the early console belongs to boot chain 2 (QEMU drops PL011 DR now). */
    scan();

    /* Find platform via plugin_type, not topology: page tables/console are not ready. */
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

/* ---- boot chain 2 ---- */

void br_plugin_manager_platform_init(void)
{
    if (s_init_done == BR_FALSE || s_plat_index == (br_u32)-1) {
        /* Dropped by QEMU too (console not up), but worse to run on with an unscanned section. */
        br_panic("plugin_manager: br_plugin_manager_init() must be called before platform_init()");
    }

    const br_plugin_t *p = s_plugins[s_plat_index];
    if (p->early_init == BR_PLUGIN_NO_HOOK) {
        br_panic("plugin_manager: platform plugin `%s` has no early_init hook; "
                 "console/PIC/page tables must be built by it first (design 1-01 §9)", p->name);
    }

    /* EARLY's first step is the platform's early_init (hard constraint); TC-PLUG-003 samples it. */
    s_phase_reached    = BR_PHASE_EARLY;
    s_first_early_name = p->name;

    const int rc = p->early_init();
    br_log_info("[PLUGIN] phase=%s name=%s rc=%d", phase_key(BR_PHASE_EARLY), p->name, rc);
    if (rc != 0) {
        plugin_fail(p->name, BR_PHASE_EARLY, rc);
    }
}

/* ---- boot chain 4 ---- */

BR_NORETURN void br_plugin_manager_run(void)
{
    if (s_init_done == BR_FALSE) {
        br_panic("plugin_manager: br_plugin_manager_init() must be called before br_plugin_manager_run()");
    }

    /* Console is up from boot chain 2, so logs are meaningful from here on. */
    br_log_info("[PLUGIN] manager: .br_plugins section has %u descriptors", s_count);

    /* Duplicate names should have been caught at compose time (globally unique). */
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

    /* ---- 3 EARLY: remaining plugins in topological order (platform already ran) ---- */
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
        /* pass 0 = non-APP, pass 1 = APP: APP last is an explicit rule, not topological order. */
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

    /* ---- 7 self-test pass (after START, before the scheduler takes over) ----
     * Timer armed, IRQs on, devices registered: existing suites have their preconditions met.
     *
     * WORKAROUND(br-wa-boot-002): timer preemption already works before br_sched_run(), so a plugin
     * that starts a thread in the start phase (app/hello does) runs concurrently with this pass
     * pass -- keep device-touching APP cases in the start phase. */
    br_plugin_manager_selftest();

    /* ---- 8 scheduler: hand over if registered, else the last start() holds the CPU ---- */
    if (br_sched_registered()) {
        br_log_info("[PLUGIN] manager: scheduler registered ⇒ br_sched_run()");
        br_sched_run();
    }
    br_log_warn("[PLUGIN] manager: no scheduler and no start() holding the CPU ⇒ WFI halt (transition bridge)");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ---- self-test pass ---- */

/* Self-test is observation, not boot: init/start failure stops the machine (ruling G6); a
 * self-test failure is only recorded (the gate judges [XXXCONF] FAIL). */
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
            /* Negative = the hook itself failed, not "N items failed": counted separately. */
            errors++;
            br_log_error("[SELFTEST] ERROR %s: selftest hook returned %d (hook error, not a failure count)",
                         p->name, rc);
        } else {
            fails += (br_u32)rc;
            br_log_info("[SELFTEST] %s rc=%d", p->name, rc);
        }
        ran++;
    }

    /* Core's own suites share the switch with plugin self-tests; when off, core/selftest/ is
     * not compiled and the weak reference reads BR_NULL. */
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

/* ---- observation surface ---- */

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
    /* Not sorted yet => section order (topological order matters only after run()). */
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

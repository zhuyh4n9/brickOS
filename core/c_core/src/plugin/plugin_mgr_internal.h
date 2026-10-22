/*
 * Plugin manager internal contract (core-private, not for plugins). Exposes only what the
 * self-test reads -- TC-PLUG-002/002b/003 exercise the real topo_sort, init_phase_of, edge and
 * position tables; lifecycle state, phase and failure counts stay private. Not part of the
 * br_plugin.h interface surface.
 */
#ifndef BR_PLUGIN_MGR_INTERNAL_H
#define BR_PLUGIN_MGR_INTERNAL_H

#include <br/core/br_plugin.h>
#include <br/core/br_types.h>

/* Upper bound on descriptors per segment (static bound); the self-test uses it for temp arrays. */
#define BR_PLUGIN_MAX 32u

/* One init edge: `from` depends on `to` => `to` completes first. Indices = segment order. */
typedef struct plug_edge {
    br_u16 from;
    br_u16 to;
} plug_edge_t;

/* ---- section bounds (linker-provided) ---- */

/*
 * GNU ld does not generate `__start_/__stop_` for a section name containing `.`, so the platform
 * linker script defines these two labels around `KEEP(*(.br_plugins))`. Declared here, not in the
 * `.c` that uses them: ADR-0038 / R1 makes a header the only place a cross-translation-unit
 * declaration may live.
 */
extern const br_plugin_t __br_plugins_start[];
extern const br_plugin_t __br_plugins_stop[];

/* ---- manager state: one read-only snapshot for the self-test ----
 *
 * The state itself is `static` in plugin_mgr.c (ADR-0038 / R2: a global never leaves the plugin).
 * The conformance suite reads this snapshot instead of the variables, so the internal header holds
 * no mutable declaration and cannot drift into a second source of truth.
 */
typedef struct br_plugin_mgr_view {
    const br_plugin_t *const *plugins;   /* segment order */
    br_u32  count;                       /* entries in the segment */
    const plug_edge_t *edges;            /* only kind == BR_DEP_INIT */
    br_u32  edge_count;
    const br_u16 *pos;                   /* segment index -> topological position */
    br_bool sorted;
    br_u32  rt_index;                    /* segment index of the runtime plugin */
    /* "ordering invariants" traces (TC-PLUG-003) */
    const char *first_early_name;        /* whose early_init really ran first */
    br_bool app_start_last_ok;           /* before the first APP start: all non-APP had run */
    br_bool in_app_start;                /* currently inside an APP's start */
    /* APP ordering traces (TC-PLUG-004, ADR-0037) */
    br_u32  app_init_index;              /* section index of the APP; (br_u32)-1 = no APP in the image */
    br_bool app_early_init_ok;           /* every APP descriptor has early_init == NO_HOOK */
    const char *last_init_name;          /* whose init ran most recently (APP last, when it has one) */
} br_plugin_mgr_view_t;

const br_plugin_mgr_view_t *br_plugin_mgr_view(void);

/* Which phase `init` falls in -- decided by category. Self-test rechecks phase monotonicity. */
br_u32 init_phase_of(const br_plugin_t *p);

/* Kahn topological sort (pure algorithm). BR_TRUE = sorted (first n entries of order valid);
 * BR_FALSE = cycle (cycle holds the closed path). */
br_bool topo_sort(br_u32 n, const plug_edge_t *edges, br_u32 m,
                  br_u16 *order, br_u16 *cycle, br_u32 *cycle_len);

/*
 * Generated-object count (**weak definition** given by `build/gen/<plugin>/plugin_desc.c`).
 * TC-PLUG-001 uses it to cross-check the count in the segment. Meaning of the weak reference:
 * when the generated object is not compiled into the image the file still links standalone,
 * and the case then fails loudly (instead of an obscure link failure).
 */
extern const br_u32 br_plugin_gen_total __attribute__((weak));

#endif /* BR_PLUGIN_MGR_INTERNAL_H */

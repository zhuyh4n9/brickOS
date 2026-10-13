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

/* ---- manager state (self-test read-only; defined in plugin_mgr.c) ---- */
extern const br_plugin_t *s_plugins[BR_PLUGIN_MAX];   /* segment order */
extern br_u32  s_count;                                /* count in the segment */
extern plug_edge_t s_edges[];                          /* only kind == BR_DEP_INIT */
extern br_u32  s_edge_count;
extern br_u16  s_pos[BR_PLUGIN_MAX];                   /* segment index -> topological position */
extern br_bool s_sorted;
extern br_u32  s_plat_index;                           /* segment index of the platform plugin */

/* ---- execution traces for the "order invariants" (criteria of TC-PLUG-003) ---- */
extern const char *s_first_early_name;   /* whose early_init really ran first */
extern br_bool s_app_start_last_ok;      /* before the first APP start: all non-APP had run */
extern br_bool s_in_app_start;           /* currently inside an APP's start */

/* ---- algorithms (run by the self-test, so not static) ---- */

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

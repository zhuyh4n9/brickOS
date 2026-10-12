/*
 * brickOS prototype v0.2.0 — plugin manager **internal** contract (core-private, not for plugins)
 *
 * Why this header exists (same approach as `core/src/sched/sched_internal.h` /
 * `core/src/irq/irq_internal.h`): the plugin manager's self-test suite
 * (`core/selftest/plugin_selftest.c`, ADR-0010) must verify the **real implementation**, not a
 * replica of it —
 *   - `TC-PLUG-002` checks the topological order of every init edge ⇒ it must read the built
 *     edge table and the "segment index → topological position" table;
 *   - `TC-PLUG-002b` is a **negative case**: hand-build a 3-node cycle and pass it to
 *     `topo_sort()`. If the case copied the sorter, the negative case would only prove "the
 *     copy reports a cycle" and nothing about the real algorithm ⇒ it must read the real one;
 *   - `TC-PLUG-003` asserts "EARLY first step = platform", "APP last", and phase monotonicity
 *     of each init edge ⇒ it must read the phase decision function and the execution traces
 *     (who ran early_init first; whether all non-APP finished before APP start).
 *
 * The exposed surface **deliberately stops at the symbols the self-test really reads**:
 * lifecycle state (`s_init_done`), current phase (`s_phase_reached`), failure count
 * (`s_init_failures`) and start count stay private — phase and failure count already have
 * observation APIs (`br_plugin_phase_reached()` / `br_plugin_init_failures()`), so no second
 * surface is opened.
 *
 * This header does **not** enter the golden interface surface of `br/core/br_plugin.h` (same
 * principle as ADR-0005 ruling 9 on self-test hooks): putting the test surface into the
 * interface would turn "change a test case" into an interface change.
 */
#ifndef BR_PLUGIN_MGR_INTERNAL_H
#define BR_PLUGIN_MGR_INTERNAL_H

#include <br/core/br_plugin.h>
#include <br/core/br_types.h>

/* Upper bound on descriptors per segment (static-bound technique, design 3-02 §14.6); the self-test uses it for temp arrays. */
#define BR_PLUGIN_MAX 32u

/* One init edge: `from` **depends on** `to` ⇒ `to` must complete first. Indices = segment order. */
typedef struct plug_edge {
    br_u16 from;
    br_u16 to;
} plug_edge_t;

/* ---- manager state (self-test read-only; defined in plugin_mgr.c) ---- */
extern const br_plugin_t *s_plugins[BR_PLUGIN_MAX];   /* segment order */
extern br_u32  s_count;                                /* count in the segment */
extern plug_edge_t s_edges[];                          /* only kind == BR_DEP_INIT */
extern br_u32  s_edge_count;
extern br_u16  s_pos[BR_PLUGIN_MAX];                   /* segment index → topological position */
extern br_bool s_sorted;
extern br_u32  s_plat_index;                           /* segment index of the platform plugin */

/* ---- execution traces for the "order invariants" (criteria of TC-PLUG-003) ---- */
extern const char *s_first_early_name;   /* whose early_init really ran first */
extern br_bool s_app_start_last_ok;      /* before the first APP start: all non-APP had run */
extern br_bool s_in_app_start;           /* currently inside an APP's start */

/* ---- algorithms (the self-test runs the real code, so they cannot be static) ---- */

/* Which phase `init` falls in — decided by **category** (`1-01` §9). Self-test uses it to recheck phase monotonicity. */
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

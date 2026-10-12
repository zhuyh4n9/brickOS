/*
 * brickOS prototype v0.2.0 — plugin-manager selftest suite (core-private; TC-PLUG-*)
 *
 * Moved verbatim from `core/src/plugin/plugin_mgr.c` (ADR-0010): the cases, case ids
 * (`TC-PLUG-001/002/003/002b`) and logs are unchanged —— the gate `tests/gates.toml`
 * judges red/green from `[PLGCONF] PASS/FAIL`, `[PLGCONF] SUMMARY` and require_tags.
 *
 * Why it moved out of the production file:
 *   - the production file keeps only the mechanism (segment scan/topology/phase
 *     driving) and no longer carries cases;
 *   - with selftest off, the whole `core/selftest/` directory is not compiled ⇒ test
 *     code does not enter the image (not merely does not run);
 *   - the entry no longer enters `br_plugin.h`: a test surface in the golden interface
 *     would make "change one case" an interface change.
 *
 * White-box access path: `../src/plugin/plugin_mgr_internal.h`. ★ It reads real state
 * and the real algorithm: the TC-PLUG-002b negative case runs the very `topo_sort()`
 * in plugin_mgr.c (not a copy), otherwise the negative case would only prove that a
 * copy reports a cycle. See that header for the exposed list and rationale.
 *
 * Entry contract (same as `br_plugin_manager_selftest` for plugin selftests): returns
 * the number of failed items (0 = all green), records without stopping —— red/green is
 * judged by the gate (ADR-0010 §2.4).
 */
#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_types.h>

#include "../src/plugin/plugin_mgr_internal.h"

/* Entry prototype (declarations are centralized in `core_selftest.c`; restated here only
 * to satisfy the global-function prototype discipline). */
int br_plugin_selftest(void);

static br_u32 s_pc_pass;
static br_u32 s_pc_fail;

static void pc_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_pc_pass++;
        br_log_info("[PLGCONF] PASS %s %s", tag, what);
    } else {
        s_pc_fail++;
        br_log_info("[PLGCONF] FAIL %s %s", tag, what);
    }
}

/*
 * Negative-case fixture for TC-PLUG-002b: a 3-node cycle (a→b→c→a) and a 3-node DAG (c→b→a).
 * WORKAROUND(br-wa-test-001): this suite's `TC-PLUG-*` ids are self-assigned (6-01 has no
 * such group); the descriptive text is the actual criterion; per-item alignment with the
 * design table is in WORKAROUNDS.md under br-wa-test-001.
 */
static br_bool cycle_negative_case(void)
{
    static const plug_edge_t cyc[3] = { {0u, 1u}, {1u, 2u}, {2u, 0u} };
    static const plug_edge_t dag[2] = { {2u, 1u}, {1u, 0u} };
    br_u16 order[BR_PLUGIN_MAX];
    br_u16 cycle[BR_PLUGIN_MAX];
    br_u32 clen = 0u;

    /* (a) the cycle must be detected, with the complete path reported: 3 nodes + closing
     * entry = 4 items. */
    if (topo_sort(3u, cyc, 3u, order, cycle, &clen)) {
        return BR_FALSE;
    }
    if (clen != 4u || cycle[0] != cycle[3]) {
        return BR_FALSE;
    }
    for (br_u16 want = 0u; want < 3u; want++) {
        br_bool seen = BR_FALSE;
        for (br_u32 j = 0u; j < 3u; j++) {
            if (cycle[j] == want) {
                seen = BR_TRUE;
                break;
            }
        }
        if (!seen) {
            return BR_FALSE;
        }
    }

    /* (b) negative control: the same algorithm must order the acyclic graph as c→b→a
     * (otherwise "always reports a cycle" would also be a false green). */
    if (!topo_sort(3u, dag, 2u, order, cycle, &clen)) {
        return BR_FALSE;
    }
    return (order[0] == 0u) && (order[1] == 1u) && (order[2] == 2u);
}

int br_plugin_selftest(void)
{
    s_pc_pass = 0u;
    s_pc_fail = 0u;

    br_log_info("[PLGCONF] plugin-manager conformance (.br_plugins / init DAG / phases)");

    /* ---- TC-PLUG-001: segment non-empty and count == generated count ---- */
    {
        const br_u32 count = br_plugin_count();
        br_bool ok = (count > 0u);
        br_u32  total = 0u;
        br_bool have_total = (&br_plugin_gen_total != BR_NULL);
        if (have_total) {
            total = br_plugin_gen_total;
            ok = ok && (count == total);
        } else {
            ok = BR_FALSE;   /* generated count symbol absent = descriptors are not generated ⇒ explicit red */
        }
        br_log_info("[PLGCONF] descriptors=%u gen_total=%u", count, total);
        pc_report(ok, "TC-PLUG-001",
                  "segment non-empty and count == generated count (generated count symbol present)");
    }

    /* ---- TC-PLUG-002: topological order satisfies every init edge (per-edge check, report the edge) ---- */
    {
        br_bool ok = s_sorted;
        const char *bad_from = BR_NULL;
        const char *bad_to = BR_NULL;
        for (br_u32 e = 0u; e < s_edge_count && ok; e++) {
            const br_u16 f = s_edges[e].from;
            const br_u16 t = s_edges[e].to;
            /* from depends on to ⇒ to must come earlier */
            if (!(s_pos[t] < s_pos[f])) {
                ok = BR_FALSE;
                bad_from = s_plugins[f]->name;
                bad_to = s_plugins[t]->name;
            }
        }
        if (!ok && bad_from != BR_NULL) {
            br_log_info("[PLGCONF] violating edge: %s -> %s", bad_from, bad_to);
        }
        br_log_info("[PLGCONF] init_edges=%u", s_edge_count);
        pc_report(ok, "TC-PLUG-002", "topological order satisfies all init edges (per-edge check)");
    }

    /* ---- TC-PLUG-003: phase monotonicity + order invariants (APP last / selftest after all starts) ----
     *
     * ★ The third cell of this case changed criterion after ADR-0010; the story is worth
     *   keeping, or the next reader will think it was weakened:
     *   The old criterion `s_app_start_last_ok && s_in_app_start` asserted "all non-APP
     *   starts finished, AND this case is running inside APP's start" —— at that time
     *   selftest was called by APP's start, so "I am running in APP's start" was the
     *   self-witness of that call site.
     *   Now selftest is driven by `br_plugin_manager_selftest()` after ALL starts have
     *   returned (ADR-0010 §2.2), so `s_in_app_start` is always BR_FALSE ⇒ the old
     *   criterion can structurally never hold.
     *   The new criteria assert the same fact in another direction, and more strongly:
     *     - `s_app_start_last_ok` still asserts the combination rule "APP last" was
     *       honored in the START phase (a historical fact recorded at that time,
     *       independent of when selftest runs);
     *     - `s_in_app_start == BR_FALSE` asserts selftest does NOT run inside any
     *       plugin's start —— exactly the new call site's contract (old asserted
     *       "inside", new asserts "outside"; both are self-witnesses);
     *     - `br_plugin_phase_reached() == BR_PHASE_APP` asserts the phase reached APP ⇒
     *       the START phase really finished, rather than "selftest ran before it started".
     *   Together = "all STARTs completed, and selftest happens after", matching the
     *   ADR-0010 ruling. */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 e = 0u; e < s_edge_count; e++) {
            const br_plugin_t *a = s_plugins[s_edges[e].from];   /* consumer */
            const br_plugin_t *b = s_plugins[s_edges[e].to];     /* provider */
            const br_u32 ra = init_phase_of(a);
            const br_u32 rb = init_phase_of(b);
            if (rb > ra) {
                ok = BR_FALSE;
                br_log_info("[PLGCONF] phase violation: %s(%s) depends on %s(%s)",
                            a->name, br_plugin_phase_name(ra), b->name, br_plugin_phase_name(rb));
            }
        }
        pc_report(ok, "TC-PLUG-003",
                  "phase monotonicity: provider completion rank <= consumer for every init edge");

        pc_report((s_plat_index != (br_u32)-1) && (s_first_early_name != BR_NULL)
                  && (s_first_early_name == s_plugins[s_plat_index]->name),
                  "TC-PLUG-003",
                  "first early-phase step = platform plugin's early_init (explicit plugin_type, not topological order)");

        pc_report(s_app_start_last_ok, "TC-PLUG-003",
                  "APP last: before the first APP start, all non-APP starts had completed");

        pc_report((s_in_app_start == BR_FALSE) &&
                  (br_plugin_phase_reached() == BR_PHASE_APP), "TC-PLUG-003",
                  "selftest timing: runs after ALL starts returned (inside no start), and phase already reached APP");
    }

    /* ---- TC-PLUG-002b: cycle-detection negative case ---- */
    pc_report(cycle_negative_case(), "TC-PLUG-002b",
              "cycle negative case: 3-node cycle detected with full path; same algorithm orders the DAG");

    br_log_info("[PLGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pc_pass, s_pc_fail, s_pc_pass + s_pc_fail);

    return (int)s_pc_fail;
}

/*
 * Plugin-manager selftest suite (core-private; TC-PLUG-001/002/003/002b). The gate judges
 * red/green from [PLGCONF] PASS/FAIL, [PLGCONF] SUMMARY and require_tags.
 * White-box access via ../src/plugin/plugin_mgr_internal.h; TC-PLUG-002b runs the real
 * topo_sort() from plugin_mgr.c, not a copy.
 * Returns the number of failed items (0 = all green); never stops early.
 */
#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_types.h>

#include "../src/plugin/plugin_mgr_internal.h"

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
 * Negative-case fixture for TC-PLUG-002b: a 3-node cycle (a->b->c->a) and a 3-node DAG (c->b->a).
 * WORKAROUND(br-wa-test-001): the TC-PLUG-* ids are self-assigned; the descriptive text is the
 * criterion (alignment with the design table is in WORKAROUNDS.md).
 */
static br_bool cycle_negative_case(void)
{
    static const plug_edge_t cyc[3] = { {0u, 1u}, {1u, 2u}, {2u, 0u} };
    static const plug_edge_t dag[2] = { {2u, 1u}, {1u, 0u} };
    br_u16 order[BR_PLUGIN_MAX];
    br_u16 cycle[BR_PLUGIN_MAX];
    br_u32 clen = 0u;

    /* (a) the cycle must be detected with the complete path: 3 nodes + closing entry = 4 items. */
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

    /* (b) negative control: the same algorithm must order the acyclic graph as c->b->a. */
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
            ok = BR_FALSE;   /* generated count symbol absent = descriptors are not generated => explicit red */
        }
        br_log_info("[PLGCONF] descriptors=%u gen_total=%u", count, total);
        pc_report(ok, "TC-PLUG-001",
                  "segment non-empty and count == generated count (generated count symbol present)");
    }

    /* ---- TC-PLUG-002: topological order satisfies every init edge (per-edge check) ---- */
    {
        br_bool ok = s_sorted;
        const char *bad_from = BR_NULL;
        const char *bad_to = BR_NULL;
        for (br_u32 e = 0u; e < s_edge_count && ok; e++) {
            const br_u16 f = s_edges[e].from;
            const br_u16 t = s_edges[e].to;
            /* from depends on to => to must come earlier */
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

    /* ---- TC-PLUG-003: phase monotonicity + order invariants (APP last / after all starts) ----
     * selftest runs after every start returned, so s_in_app_start is always BR_FALSE; reaching
     * BR_PHASE_APP self-witnesses that the START phase finished. */
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

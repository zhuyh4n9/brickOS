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
    const br_plugin_mgr_view_t *v = br_plugin_mgr_view();

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
        br_bool ok = v->sorted;
        const char *bad_from = BR_NULL;
        const char *bad_to = BR_NULL;
        for (br_u32 e = 0u; e < v->edge_count && ok; e++) {
            const br_u16 f = v->edges[e].from;
            const br_u16 t = v->edges[e].to;
            /* from depends on to => to must come earlier */
            if (!(v->pos[t] < v->pos[f])) {
                ok = BR_FALSE;
                bad_from = v->plugins[f]->name;
                bad_to = v->plugins[t]->name;
            }
        }
        if (!ok && bad_from != BR_NULL) {
            br_log_info("[PLGCONF] violating edge: %s -> %s", bad_from, bad_to);
        }
        br_log_info("[PLGCONF] init_edges=%u", v->edge_count);
        pc_report(ok, "TC-PLUG-002", "topological order satisfies all init edges (per-edge check)");
    }

    /* ---- TC-PLUG-003: phase monotonicity + order invariants (APP last / after all starts) ----
     * selftest runs after every start returned, so `in_app_start` is always BR_FALSE; reaching
     * BR_PHASE_APP self-witnesses that the START phase finished. */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 e = 0u; e < v->edge_count; e++) {
            const br_plugin_t *a = v->plugins[v->edges[e].from];   /* consumer */
            const br_plugin_t *b = v->plugins[v->edges[e].to];     /* provider */
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

        pc_report((v->rt_index != (br_u32)-1) && (v->first_early_name != BR_NULL)
                  && (v->first_early_name == v->plugins[v->rt_index]->name),
                  "TC-PLUG-003",
                  "first early-phase step = platform plugin's early_init (explicit plugin_type, not topological order)");

        pc_report(v->app_start_last_ok, "TC-PLUG-003",
                  "APP last: before the first APP start, all non-APP starts had completed");

        pc_report((v->in_app_start == BR_FALSE) &&
                  (br_plugin_phase_reached() == BR_PHASE_APP), "TC-PLUG-003",
                  "selftest timing: runs after ALL starts returned (inside no start), and phase already reached APP");
    }

    /* ---- TC-PLUG-004: APP ordering (ADR-0037) ----
     * Two rules the generator implements but the runtime must not take on trust:
     * ① the APP has no `early_init` (it registers through the section scan, so the hook has nothing
     *   to do) -- re-proven on the descriptor above, sampled here;
     * ② the APP's `init` runs in the APP phase, i.e. **after every other plugin's init**: the
     *   trace of the last init to run must be the APP, whenever the APP has an init hook at all
     *   (`phase = "early"` means "no init hook", and then rule ② has no subject). */
    {
        const br_plugin_t *app = (v->app_init_index != (br_u32)-1) ? v->plugins[v->app_init_index] : BR_NULL;

        pc_report(v->app_early_init_ok && (v->app_init_index != (br_u32)-1) &&
                  (app != BR_NULL) && (app->early_init == BR_PLUGIN_NO_HOOK),
                  "TC-PLUG-004",
                  "APP has no early_init: its descriptor carries BR_PLUGIN_NO_HOOK (registration is section-driven)");

        if (app != BR_NULL && app->init != BR_PLUGIN_NO_HOOK) {
            pc_report((v->last_init_name != BR_NULL) && (v->last_init_name == app->name),
                      "TC-PLUG-004",
                      "APP init is last: no plugin's init ran after the APP's");
            pc_report(init_phase_of(app) == BR_PHASE_APP, "TC-PLUG-004",
                      "APP init lands in the APP phase (after CORE and LATE, before IRQs are enabled)");
        } else {
            /* No APP or an APP without an init hook: rule ② is vacuous, so state that instead of
             * silently passing a check that never had a subject. */
            pc_report(1, "TC-PLUG-004",
                      "no APP init hook in this image (`phase = \"early\"`) => the 'APP init last' rule has no subject");
        }
    }

    /* ---- TC-PLUG-002b: cycle-detection negative case ---- */
    pc_report(cycle_negative_case(), "TC-PLUG-002b",
              "cycle negative case: 3-node cycle detected with full path; same algorithm orders the DAG");

    br_log_info("[PLGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pc_pass, s_pc_fail, s_pc_pass + s_pc_fail);

    return (int)s_pc_fail;
}

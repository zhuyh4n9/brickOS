/*
 * brickOS prototype v0.2.0 — 插件管理器自检套件(core 私有; TC-PLUG-*)
 *
 * 从 `core/src/plugin/plugin_mgr.c` **原样搬来**(ADR-0010): 用例、case id
 * (`TC-PLUG-001/002/003/002b`)与日志一字未改 —— 门禁 `tests/gates.toml` 按
 * `[PLGCONF] PASS/FAIL`、`[PLGCONF] SUMMARY` 与 require_tags 判红绿。
 *
 * 为什么搬出生产文件:
 *   - 生产文件只留机制(扫段/拓扑/相驱动), 不再背着用例;
 *   - 关掉自检时整个 `core/selftest/` 目录**不参与编译** ⇒ 测试代码不进镜像(不只是不跑);
 *   - 入口不再进 `br_plugin.h`: 测试面进 golden 接口会让"改一个用例"变成接口变更。
 *
 * 白盒访问路径: `../src/plugin/plugin_mgr_internal.h`。★ 它读的是**真**状态与**真**算法:
 * `TC-PLUG-002b` 的负例跑的就是 plugin_mgr.c 里那个 `topo_sort()`(不是副本), 否则负例
 * 只证明副本会报环。暴露清单与理由见该头注。
 *
 * 入口口径(与 `br_plugin_manager_selftest` 对插件自检一致): 返回**失败项数**
 * (0 = 全绿), 只记不停 —— 红绿由门禁判(ADR-0010 §2.4)。
 */
#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_types.h>

#include "../src/plugin/plugin_mgr_internal.h"

/* 入口原型(声明面集中在 `core_selftest.c`; 这里重述只为满足全局函数的原型纪律)。 */
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
 * TC-PLUG-002b 的负例夹具: 3 节点环(a→b→c→a)与 3 节点 DAG(c→b→a)。
 * WORKAROUND(br-wa-test-001): 本套件的 `TC-PLUG-*` id 是**自编号**(6-01 表里没有这一组),
 * 自述文字才是判据内容; 与设计表逐条对齐见 WORKAROUNDS.md 的 br-wa-test-001。
 */
static br_bool cycle_negative_case(void)
{
    static const plug_edge_t cyc[3] = { {0u, 1u}, {1u, 2u}, {2u, 0u} };
    static const plug_edge_t dag[2] = { {2u, 1u}, {1u, 0u} };
    br_u16 order[BR_PLUGIN_MAX];
    br_u16 cycle[BR_PLUGIN_MAX];
    br_u32 clen = 0u;

    /* (a) 环必须被检出, 且报出**完整**路径: 3 个节点 + 收尾闭合 = 4 项。 */
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

    /* (b) 负控制: 同样的算法对无环图必须排得出 c→b→a(否则"总能报环"也是假绿)。 */
    if (!topo_sort(3u, dag, 2u, order, cycle, &clen)) {
        return BR_FALSE;
    }
    return (order[0] == 0u) && (order[1] == 1u) && (order[2] == 2u);
}

int br_plugin_selftest(void)
{
    s_pc_pass = 0u;
    s_pc_fail = 0u;

    br_log_info("[PLGCONF] plugin-manager conformance (.br_plugins / init-DAG / 相位)");

    /* ---- TC-PLUG-001: 段非空且条数 == 生成物数量 ---- */
    {
        const br_u32 count = br_plugin_count();
        br_bool ok = (count > 0u);
        br_u32  total = 0u;
        br_bool have_total = (&br_plugin_gen_total != BR_NULL);
        if (have_total) {
            total = br_plugin_gen_total;
            ok = ok && (count == total);
        } else {
            ok = BR_FALSE;   /* 生成物计数符号缺席 = 描述符不是生成物 ⇒ 明确报红 */
        }
        br_log_info("[PLGCONF] descriptors=%u gen_total=%u", count, total);
        pc_report(ok, "TC-PLUG-001",
                  "段非空 且 条数 == 生成物数量(生成物计数符号在场)");
    }

    /* ---- TC-PLUG-002: 拓扑序满足每条 init 边(逐边校验, 违例报出边) ---- */
    {
        br_bool ok = s_sorted;
        const char *bad_from = BR_NULL;
        const char *bad_to = BR_NULL;
        for (br_u32 e = 0u; e < s_edge_count && ok; e++) {
            const br_u16 f = s_edges[e].from;
            const br_u16 t = s_edges[e].to;
            /* from 依赖 to ⇒ to 的位次必须更靠前 */
            if (!(s_pos[t] < s_pos[f])) {
                ok = BR_FALSE;
                bad_from = s_plugins[f]->name;
                bad_to = s_plugins[t]->name;
            }
        }
        if (!ok && bad_from != BR_NULL) {
            br_log_info("[PLGCONF] 违例边: %s -> %s", bad_from, bad_to);
        }
        br_log_info("[PLGCONF] init_edges=%u", s_edge_count);
        pc_report(ok, "TC-PLUG-002", "拓扑序满足全部 init 边(逐边校验)");
    }

    /* ---- TC-PLUG-003: 相位单调 + 顺序不变量(APP 最后 / 自检在全部 start 之后) ----
     *
     * ★ 本用例的**第三格**在 ADR-0010 后换过一次判据, 原委值得留下, 否则下一个人
     *   会以为它是被削弱了:
     *   旧判据 `s_app_start_last_ok && s_in_app_start` 断言"非 APP 的 start 全跑完,
     *   **且本用例正跑在 APP 的 start 里**" —— 那时候自检由 APP 的 start 调用, 所以
     *   "我正跑在 APP 的 start 里"是**那个调用点的自证**。
     *   现在自检由 `br_plugin_manager_selftest()` 在**全部 start 返回之后**驱动
     *   (ADR-0010 §2.2), 于是 `s_in_app_start` 恒为 BR_FALSE ⇒ 旧判据**结构上不可能成立**。
     *   新判据把同一件事换个方向断言, 而且**更强**:
     *     - `s_app_start_last_ok` 仍断言"APP 最后"这条组合规则在 START 相被遵守
     *       (它是 START 相**当时记下的历史事实**, 与自检何时跑无关);
     *     - `s_in_app_start == BR_FALSE` 断言"自检**不在**任何插件的 start 里跑" ——
     *       这正是新调用点的契约(旧判据断言"在里面", 新判据断言"在外面", 都是自证);
     *     - `br_plugin_phase_reached() == BR_PHASE_APP` 断言"相位已走到 APP" ⇒
     *       START 相确实**跑完了**, 而不是"还没开始就来自检"。
     *   三者合起来 = "START 全部完成, 且自检发生在它之后", 与 ADR-0010 的裁决一字对应。 */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 e = 0u; e < s_edge_count; e++) {
            const br_plugin_t *a = s_plugins[s_edges[e].from];   /* 依赖方 */
            const br_plugin_t *b = s_plugins[s_edges[e].to];     /* 提供方 */
            const br_u32 ra = init_phase_of(a);
            const br_u32 rb = init_phase_of(b);
            if (rb > ra) {
                ok = BR_FALSE;
                br_log_info("[PLGCONF] 相位违例: %s(%s) 依赖 %s(%s)",
                            a->name, br_plugin_phase_name(ra), b->name, br_plugin_phase_name(rb));
            }
        }
        pc_report(ok, "TC-PLUG-003",
                  "相位单调: 每条 init 边的提供方完成点 rank <= 消费方");

        pc_report((s_plat_index != (br_u32)-1) && (s_first_early_name != BR_NULL)
                  && (s_first_early_name == s_plugins[s_plat_index]->name),
                  "TC-PLUG-003",
                  "early 相第一步 = platform 插件的 early_init(显式取 plugin_type, 不靠拓扑序)");

        pc_report(s_app_start_last_ok, "TC-PLUG-003",
                  "APP 最后: 进第一个 APP start 之前, 非 APP 的 start 已全部跑完");

        pc_report((s_in_app_start == BR_FALSE) &&
                  (br_plugin_phase_reached() == BR_PHASE_APP), "TC-PLUG-003",
                  "自检时机: 跑在**全部 start 返回之后**(不在任何 start 里), 且相位已到 APP");
    }

    /* ---- TC-PLUG-002b: 环检测负例 ---- */
    pc_report(cycle_negative_case(), "TC-PLUG-002b",
              "环检测负例: 3 节点环被检出且报完整路径; 同算法对 DAG 排得出序");

    br_log_info("[PLGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pc_pass, s_pc_fail, s_pc_pass + s_pc_fail);

    return (int)s_pc_fail;
}

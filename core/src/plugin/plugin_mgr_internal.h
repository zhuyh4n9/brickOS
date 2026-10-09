/*
 * brickOS prototype v0.2.0 — 插件管理器**内部**契约(core 私有, 不给插件)
 *
 * 为什么需要这个头(与 `core/src/sched/sched_internal.h` / `core/src/irq/irq_internal.h`
 * 同一手法): 插件管理器的自检套件(`core/selftest/plugin_selftest.c`, ADR-0010)要验证
 * 的是**真的实现**, 不是它的复刻 ——
 *   - `TC-PLUG-002` 逐条 init 边校验拓扑序 ⇒ 要读建好的边表与"段下标 → 拓扑位次"表;
 *   - `TC-PLUG-002b` 是**负例**: 手工造一个 3 节点环丢给 `topo_sort()`。这个排序器若在
 *     用例里再抄一份, 负例只证明"副本会报环", 对真算法一无所证 ⇒ 必须读真身;
 *   - `TC-PLUG-003` 断言"EARLY 第一步 = platform""APP 最后"以及每条 init 边的相位单调
 *     ⇒ 要读相位判定函数与执行痕迹(谁第一个跑 early_init、APP start 之前非 APP 是否跑完)。
 *
 * 暴露面**刻意只到自检真正读到的符号为止**: 生命周期状态(`s_init_done`)、当前相位
 * (`s_phase_reached`)、失败计数(`s_init_failures`)以及 start 计数继续私有 —— 相位与
 * 失败计数已有观测 API(`br_plugin_phase_reached()` / `br_plugin_init_failures()`),
 * 无需再开一面。
 *
 * 本头**不进** `br/core/br_plugin.h` 的 golden 接口面(与 ADR-0005 裁定 9 对自检钩子的
 * 处置同源): 测试面进接口会让"改一个用例"变成接口变更。
 */
#ifndef BR_PLUGIN_MGR_INTERNAL_H
#define BR_PLUGIN_MGR_INTERNAL_H

#include <br/core/br_plugin.h>
#include <br/core/br_types.h>

/* 段里描述符条数上界(设计 3-02 §14.6 的静态上界手法); 自检用它开临时数组。 */
#define BR_PLUGIN_MAX 32u

/* 一条 init 边: `from` **依赖** `to` ⇒ `to` 必须先完成。下标 = 段序。 */
typedef struct plug_edge {
    br_u16 from;
    br_u16 to;
} plug_edge_t;

/* ---- 管理器状态(自检只读; 定义在 plugin_mgr.c) ---- */
extern const br_plugin_t *s_plugins[BR_PLUGIN_MAX];   /* 段序 */
extern br_u32  s_count;                                /* 段里的条数 */
extern plug_edge_t s_edges[];                          /* 只含 kind == BR_DEP_INIT */
extern br_u32  s_edge_count;
extern br_u16  s_pos[BR_PLUGIN_MAX];                   /* 段下标 → 拓扑位次 */
extern br_bool s_sorted;
extern br_u32  s_plat_index;                           /* platform 插件的段下标 */

/* ---- "顺序不变量"的执行痕迹(TC-PLUG-003 的判据) ---- */
extern const char *s_first_early_name;   /* 第一个真的跑起来的 early_init 属于谁 */
extern br_bool s_app_start_last_ok;      /* 调用第一个 APP start 之前: 非 APP 已全跑完 */
extern br_bool s_in_app_start;           /* 当前正处在 APP 的 start 里 */

/* ---- 算法(自检要跑真身, 所以不能是 static) ---- */

/* `init` 落在哪一相 —— 由**类别**决定(`1-01` §9)。自检用它复核相位单调。 */
br_u32 init_phase_of(const br_plugin_t *p);

/* Kahn 拓扑排序(纯算法)。BR_TRUE = 排好了(order 前 n 项有效);
 * BR_FALSE = 有环(cycle 里是闭合路径)。 */
br_bool topo_sort(br_u32 n, const plug_edge_t *edges, br_u32 m,
                  br_u16 *order, br_u16 *cycle, br_u32 *cycle_len);

/*
 * 生成物数量(`build/gen/<plugin>/plugin_desc.c` 给的**弱定义**)。TC-PLUG-001 用它对照
 * 段里的条数。弱引用的意义: 没把生成物编进镜像时仍可单独链接, 此时用例明确报红
 * (而不是链接失败得不明不白)。
 */
extern const br_u32 br_plugin_gen_total __attribute__((weak));

#endif /* BR_PLUGIN_MGR_INTERNAL_H */

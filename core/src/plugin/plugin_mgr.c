/*
 * brickOS prototype v0.2.0 — 插件管理器(plugin_manager)
 *
 * 设计依据(设计仓库):
 *   - `1-01` §9 启动序列、§6.2 生命周期四相与"每插件两个完成点"、§6.5 init-DAG(环 = 硬错误)
 *   - `3-05` §2(plugin_manager: 扫段 → 拓扑 → 相驱动)、§2.2(`__br_plugins_start/stop`)
 *   - `3-01` §13.3(段收集: 插件近零导出面, 靠 `.br_plugins` 段 + 链接器边界符号枚举)
 *
 * 本文件做的**全部**事情(裁定见 `docs/decisions/0005-plugin-manager.md`):
 *   ① core.init 的落点(v0.2 由本函数代做): 时钟 → 日志
 *   ② 扫 `.br_plugins` → 段条数自证(重名 = 组合期本应拦住的事, 运行期也自证)
 *   ③ 建 init 边(只取 `kind == BR_DEP_INIT`)→ **Kahn 拓扑排序**
 *      * 有环 ⇒ 打印**完整环路径**并 `br_panic`(环是组合期硬错误; 运行期也要能自证)
 *   ④ EARLY: **第一步显式取 platform 插件的 early_init**(靠 `plugin_type`, 不靠拓扑序的
 *      巧合 —— 此刻 console/PIC/页表还没就绪), 之后其余插件按拓扑序
 *   ⑤ CORE: 类别决定的 `init`(非 Service/Interface; `1-01` §9/§6.2)
 *   ⑥ LATE: Service / Interface 的 `init`
 *   ⑦ **全局开中断**(core 的 `br_irq_cpu_enable()`)
 *   ⑧ START: 拓扑序, 但 **APP 最后是显式规则**(拓扑序里 app/hello 可能排第一)
 *   ⑨ `br_sched_registered()` ⇒ `br_sched_run()`; 没注册就让最后一个 start() 自己占住
 *      CPU(过渡桥: 调度器落地前, APP 的 start 不返回 —— 见 ADR-0005 §2.5)
 *
 * 失败粒度(裁定 G6): **首败即停机** —— 打 `[PLUGIN] FAIL <name> phase=<相> rc=<n>` 后
 * `br_panic`。init 失败 = 启动失败, 不降级(设计 `3-02` §14.3 的同一义务)。
 *
 * 为什么日志里的相名是**小写**(early/core/late/start): `tests/gates.toml` 的 smoke 门禁
 * 用 `forbid = ["EARLY"]` 抓"延时自检打出了 EARLY"(延时短于请求); 插件管理器的相名若用
 * 大写会**误触**那条禁止模式。相名的对外呈现(`br_plugin_phase_name`)仍是设计文档的
 * 大写形态, 只有日志这一处取小写。
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* ==================================================================== 段边界 */

/*
 * 链接脚本给的边界符号(`platform/…/src/link.ld`, 与 `.br_extable` 同款: 段名含 `.`,
 * GNU ld 不会自动生成 `__start_/__stop_` ⇒ 必须显式赋值)。`KEEP` 保证 `--gc-sections`
 * 不裁掉描述符。
 */
extern const br_plugin_t __br_plugins_start[];
extern const br_plugin_t __br_plugins_stop[];

/*
 * 生成物数量: 每个 `build/gen/<plugin>/plugin_desc.c` 给一条**同值的弱定义**
 * (见该模板与 ADR-0005 §2.4)。弱引用的意义: 没把生成物编进镜像时本文件仍可单独链接,
 * 此时 TC-PLUG-001 会明确报红(而不是链接失败得不明不白)。
 */
extern const br_u32 br_plugin_gen_total __attribute__((weak));

/* ==================================================================== 静态上界 */

#define BR_PLUGIN_MAX        32u   /* 段里描述符条数上界(设计 3-02 §14.6 的静态上界手法) */
#define BR_PLUGIN_MAX_EDGES  128u  /* init 边上界 */
#define BR_PLUGIN_CYCLE_BUF  512u  /* 环路径文本缓冲 */
#define BR_DEP_TERM_GUARD    512u  /* 遍历 deps 的硬上界(坏表不终止时也不越界) */

/* 一条 init 边: `from` **依赖** `to` ⇒ `to` 必须先完成。下标 = 段序。 */
typedef struct plug_edge {
    br_u16 from;
    br_u16 to;
} plug_edge_t;

/* ==================================================================== 状态 */

static const br_plugin_t *s_plugins[BR_PLUGIN_MAX];   /* 段序 */
static br_u32  s_count;                                /* 段里的条数 */
static plug_edge_t s_edges[BR_PLUGIN_MAX_EDGES];       /* 只含 kind == BR_DEP_INIT */
static br_u32  s_edge_count;
static br_u16  s_order[BR_PLUGIN_MAX];                 /* 拓扑序(段下标) */
static br_u16  s_pos[BR_PLUGIN_MAX];                   /* 段下标 → 拓扑位次 */
static br_bool s_sorted;

static br_u32  s_phase_reached = BR_PHASE_EARLY;
static br_u32  s_init_failures;
static br_u32  s_plat_index = (br_u32)-1;   /* platform 插件的段下标(EARLY 第一步) */

/* ---- 自检用: "顺序不变量"的执行痕迹 ---- */
static const char *s_first_early_name;   /* 第一个真的跑起来的 early_init 属于谁 */
static br_u32  s_non_app_start_done;     /* 已执行的非 APP start 数 */
static br_u32  s_non_app_start_total;    /* 非 APP 且有 start 钩子的插件数 */
static br_bool s_app_start_last_ok;      /* 调用第一个 APP start 之前: 非 APP 已全跑完 */
static br_bool s_in_app_start;           /* 当前正处在 APP 的 start 里 */

/* ==================================================================== 段枚举 */

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
        br_panic("plugin_manager: .br_plugins 有 %u 条描述符, 超过静态上界 %u",
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
    /* 段本身就是"名字表"(启动期由链接器建好); 不依赖 scan() 是否跑过 —— 观测入口
     * 在管理器进入之前也应当能回答"段里有没有这个名字"。 */
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

/* ==================================================================== 相与完成点 */

/* 日志用的相名(小写, 见文件头注)。 */
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
 * `init` 落在哪一相 —— 由**类别**决定(`1-01` §9: 非 Service/Interface ⇒ CORE,
 * Service/Interface ⇒ LATE)。没有 init 钩子 ⇒ 完成点 = EARLY(② 与 ① 重合)。
 */
static br_u32 init_phase_of(const br_plugin_t *p)
{
    if (p->init == BR_PLUGIN_NO_HOOK) {
        return BR_PHASE_EARLY;
    }
    return is_service_like(p) ? BR_PHASE_LATE : BR_PHASE_CORE;
}

/* ==================================================================== 拓扑排序 */

/*
 * Kahn 拓扑排序(纯算法: 不碰任何插件声明面以外的东西, 所以负例用例可以拿假图跑它)。
 *
 * 语义: `edges[e] = {from, to}` = "from 依赖 to" ⇒ to 必须先出队。
 * 返回 BR_TRUE = 排好了(`order` 前 n 项有效); BR_FALSE = 有环(`cycle` 里是**闭合**路径,
 * 首尾同一下标, `cycle_len` 含重复的收尾项)。
 */
static br_bool topo_sort(br_u32 n, const plug_edge_t *edges, br_u32 m,
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
                continue;   /* 已入队/已出队: 不重复减 */
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

    /* ---- 有环: 在"未出队"的节点里 DFS 取出**完整环路径** ---- */
    {
        br_u8  state[BR_PLUGIN_MAX];
        br_u16 stack[BR_PLUGIN_MAX];

        for (br_u32 i = 0u; i < n; i++) {
            state[i] = 0u;
        }
        *cycle_len = 0u;

        /* 迭代式 DFS(显式栈), 免得为 n=32 的图写递归又怕栈深。 */
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
                        /* 回边 ⇒ 从 v 在栈里的位置到栈顶就是环 */
                        br_u32 at = 0u;
                        while (at < depth && stack[at] != v) {
                            at++;
                        }
                        br_u32 len = 0u;
                        for (br_u32 j = at; j < depth; j++) {
                            cycle[len++] = stack[j];
                        }
                        cycle[len++] = v;   /* 闭合 */
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

/* ==================================================================== 失败停机 */

static BR_NORETURN void plugin_fail(const char *name, br_u32 phase, int rc)
{
    s_init_failures++;
    br_log_error("[PLUGIN] FAIL %s phase=%s rc=%d",
                 (name != BR_NULL) ? name : "(null)", phase_key(phase), rc);
    br_panic("plugin_manager: 首个失败即停机(裁定 G6): %s phase=%s rc=%d",
             (name != BR_NULL) ? name : "(null)", phase_key(phase), rc);
}

/* ==================================================================== 管理器 */

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
                break;                      /* 终止符: `.name == BR_NULL` */
            }
            if (d->kind != BR_DEP_INIT) {
                continue;                   /* 只有 init 边参与拓扑 */
            }
            const br_u32 to = index_of_name(d->name);
            if (to == (br_u32)-1) {
                /* 组合期闭包本应保证提供方在场; 缺了就当硬错误(报出来比静默错序好)。 */
                br_panic("plugin_manager: `%s` 声明了 init 依赖 `%s`, 但它不在 .br_plugins 段里",
                         s_plugins[i]->name, d->name);
            }
            if (s_edge_count >= BR_PLUGIN_MAX_EDGES) {
                br_panic("plugin_manager: init 边超过上界 %u", (br_u32)BR_PLUGIN_MAX_EDGES);
            }
            s_edges[s_edge_count].from = (br_u16)i;
            s_edges[s_edge_count].to   = (br_u16)to;
            s_edge_count++;
        }
    }
}

/* 环路径文本(带边界; 名字过长时截断, 仍然指出"有环 + 环上有谁")。 */
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

BR_NORETURN void br_plugin_manager_run(void)
{
    /* ---- ① core.init 的落点(v0.2 由本函数代做): 时钟 → 日志 ----
     * 设计 1-01 §9 的 core.init 还没有独立入口, 而 EARLY 相起就要有可观测性(设计
     * 3-05 §2 的"扫段/排序"必须能打日志)。见 ADR-0005 §2.6。
     * 这两步只读架构计数器/置软件状态, **不产生任何输出** —— 输出要等 console 起来。 */
    br_clock_init();
    br_log_init();
    br_log_set_level(BR_LOG_DEBUG);

    /* ---- ② 扫段(纯计算, 无输出: 此刻 console 还没 init) ---- */
    scan();

    /* ---- ④ EARLY 第一步: platform 插件的 early_init ----
     * ★ 顺序上的**硬约束**: 管理器在 EARLY 相之前不能打任何日志 —— 早期 console 是
     *   platform 自己的 early_init 里才配好的(`br_console_init`), 在那之前写 PL011
     *   的 DR 会被 QEMU 丢掉(CR.UARTEN = 0)。所以"扫段 + 找 platform + 跑它的
     *   early_init"这三步都在**静默**里完成, 日志从这之后才有意义。
     * ★ 取 platform 靠 `plugin_type` 字段(显式), 不靠拓扑序的巧合: 此刻 PIC/页表/
     *   console 都还没就绪, 拓扑序里谁排第一与"能不能观测"无关。 */
    {
        br_u32 plat = (br_u32)-1;
        for (br_u32 i = 0u; i < s_count; i++) {
            if (s_plugins[i]->plugin_type == BR_PLUGIN_TYPE_PLATFORM) {
                plat = i;
                break;
            }
        }
        if (plat == (br_u32)-1) {
            br_panic("plugin_manager: 段里没有 plugin_type = platform 的插件; "
                     "EARLY 相的第一步无从谈起(设计 1-01 §9)");
        }
        s_phase_reached = BR_PHASE_EARLY;
        {
            const br_plugin_t *p = s_plugins[plat];
            if (p->early_init == BR_PLUGIN_NO_HOOK) {
                br_panic("plugin_manager: platform 插件 `%s` 没有 early_init 钩子; "
                         "console/PIC/页表必须由它先建(设计 1-01 §9)", p->name);
            }
            s_first_early_name = p->name;
            const int rc = p->early_init();
            br_log_info("[PLUGIN] phase=%s name=%s rc=%d", phase_key(BR_PHASE_EARLY), p->name, rc);
            if (rc != 0) {
                plugin_fail(p->name, BR_PHASE_EARLY, rc);
            }
        }
        s_plat_index = plat;   /* 后面 EARLY 的其余插件里要跳过它 */

        /* 现在 console 已经可用, 从这里开始才有日志 */
        br_log_info("[PLUGIN] manager: .br_plugins 段 %u 条描述符", s_count);

        /* 重名自证: 重名 = 组合期(全局唯一)本应拦住的事。 */
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
                    br_panic("plugin_manager: .br_plugins 里重名 `%s`(全局唯一是组合期硬约束)", a);
                }
            }
        }

        /* ---- ③ init 边 + 拓扑排序 ---- */
        build_edges();
        br_log_info("[PLUGIN] manager: %u 条 init 边(只取 kind=init)", s_edge_count);
    }

    {
        br_u16 cycle[BR_PLUGIN_MAX];
        br_u32 cycle_len = 0u;
        char   text[BR_PLUGIN_CYCLE_BUF];

        if (!topo_sort(s_count, s_edges, s_edge_count, s_order, cycle, &cycle_len)) {
            cycle_text(cycle, cycle_len, text, (br_u32)sizeof(text));
            br_log_error("[PLUGIN] cycle: %s", text);
            br_panic("plugin_manager: init-DAG 有环(组合期硬错误, 运行期自证): %s", text);
        }
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        s_pos[s_order[i]] = (br_u16)i;
    }
    s_sorted = BR_TRUE;

    /* ---- ④(续) EARLY: 其余插件按拓扑序 ---- */
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

    /* ---- ⑤⑥ CORE / LATE: 类别决定的 init ---- */
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

    /* ---- ⑦ 全局开中断(LATE 之后, START 之前) ---- */
    br_irq_cpu_enable();
    br_log_info("[PLUGIN] irq=on (LATE 完成, 全部插件 init 已返回)");

    /* ---- ⑧ START: 非 APP 先(拓扑序), APP 后(拓扑序) ---- */
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
        /* pass 0 = 非 APP; pass 1 = APP。APP 最后是**显式规则**(§6.2 的表:
         * "全局开中断后 start(全部插件) → app.start()"), 不靠拓扑序的巧合 ——
         * 实测拓扑序里 app/hello 排在第一个。 */
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

    /* ---- ⑨ 调度器: 注册了就交给它; 没注册 ⇒ 最后一个 start() 本该占住 CPU ---- */
    if (br_sched_registered()) {
        br_log_info("[PLUGIN] manager: 调度器已注册 ⇒ br_sched_run()");
        br_sched_run();
    }
    br_log_warn("[PLUGIN] manager: 没有调度器, 也没有 start() 占住 CPU ⇒ WFI 停机(过渡桥)");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ==================================================================== 观测面 */

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
    /* 管理器还没排过序 ⇒ 退回段序(拓扑序在 run() 之后才有意义)。 */
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

/* ==================================================================== 一致性用例 */

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

/* TC-PLUG-002b 的负例夹具: 3 节点环(a→b→c→a)与 3 节点 DAG(c→b→a)。  * WORKAROUND(br-wa-test-001): 本套件的 `TC-PLUG-*` id 是**自编号**(6-01 表里没有这一组),
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

void br_plugin_conformance(void)
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

    /* ---- TC-PLUG-003: 相位单调 + 顺序不变量(APP 最后 / 本用例由 APP start 调用) ---- */
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

        pc_report(s_app_start_last_ok && s_in_app_start, "TC-PLUG-003",
                  "APP 最后: 非 APP 的 start 全部先跑完, 且本用例正跑在 APP 的 start 里");
    }

    /* ---- TC-PLUG-002b: 环检测负例 ---- */
    pc_report(cycle_negative_case(), "TC-PLUG-002b",
              "环检测负例: 3 节点环被检出且报完整路径; 同算法对 DAG 排得出序");

    br_log_info("[PLGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pc_pass, s_pc_fail, s_pc_pass + s_pc_fail);
}

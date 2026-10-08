/*
 * brickOS prototype v0.2.0 — 插件元契约与插件管理器(core 侧)
 *
 * 设计依据(在 brickOS-Design 分支上):
 *   - `1-01` §6.1(元契约 br_plugin_t / BR_PLUGIN 宏)、§13.3(段收集)、§9(启动序列)
 *   - `3-05` §2(plugin_manager: 扫段 → 拓扑 → 相驱动)、§2.2(__br_plugins_start/stop)
 *   - `1-01` §6.1 的"近零导出面"(CA-10): 插件**不导出符号**, 靠 `.br_plugins` 段收集
 *
 * 谁写这个头: **core 手写**(裁定 G8)。它不是生成物 —— 生成物是各插件的
 * `build/gen/<plugin>/plugin_desc.c`, 它 `#include <br/core/br_plugin.h>` 并实例化一个
 * `br_plugin_t`。于是"元契约"只有一处真值: 本文件。
 *
 * 与设计的**偏离**(逐条登记在 `docs/decisions/0005-plugin-manager.md`):
 *   - `aligned(4)` → `aligned(8)`: 结构体里有指针(`1-01` §13.3 写的 4 是给纯标量表用的,
 *     对含指针的结构体不足 —— 链接器把 `.br_plugins` 对齐到 8 才安全)。
 *   - `br_res_t` 的预算字段在运行期**只作诊断**(预算真值在 manifest, 由 `brickie check`
 *     执法, `plugin_type = "platform"` 的 `[[res]]` 是容量)—— 见 ADR-0005 §2。
 */
#ifndef BR_CORE_BR_PLUGIN_H
#define BR_CORE_BR_PLUGIN_H

#include <br/core/br_types.h>

/* ==================================================================== 相位 */

/* 四相(1-01 §9): EARLY(不用堆/无线程/关中断) → CORE → LATE → APP。
 * `rank` 单调(1-01 §9 的完成点断言就建在它上面)。 */
#define BR_PHASE_EARLY  0u
#define BR_PHASE_CORE   1u
#define BR_PHASE_LATE   2u
#define BR_PHASE_APP    3u
/* 依赖声明里的"不约束完成点"(`[[dep]].phase` 缺省)。 */
#define BR_PHASE_NONE   0xFFu

/* ==================================================================== 依赖 */

/* 结构依赖的三种边(4-04 / contract §7.1 的 [[dep]].kind)。 */
#define BR_DEP_INIT     0u   /* 初始化顺序: 禁环, 参与拓扑排序 */
#define BR_DEP_RUNTIME  1u   /* 运行期调用: 不参与拓扑 */
#define BR_DEP_TYPE     2u   /* 类型/头文件: 不参与拓扑 */

/* 一条依赖边。`deps` 以 `.name == BR_NULL` **结尾**(不是靠长度)。 */
typedef struct br_dep {
    const char *name;      /* 被依赖插件的全名(`platform/qemu-aarch64`) */
    br_u8       kind;      /* BR_DEP_* */
    br_u8       phase;     /* 断言: 提供方的完成点(仅 kind=INIT 有意义) */
    br_u16      compat_gen;
} br_dep_t;

/* ==================================================================== 调度类别 */

/* `[plugin].sched_class`(1-01 §9 的组合合法性矩阵; 运行期**只记录不执法**,
 * 裁定 G10 —— 执法在组合期 `brickie check`)。 */
#define BR_SCHED_CLASS_SAFE_PREEMPT  0u
#define BR_SCHED_CLASS_COOP_ONLY     1u
#define BR_SCHED_CLASS_TT_SAFE       2u

/* ==================================================================== 插件类别 */

/*
 * `plugin_type` / `subkind` **进描述符**: 插件管理器不靠命名约定猜类别
 * (`1-01` §6.1 的表里没有这两个字段, 是本原型的必要补充 —— 见 ADR-0005 §2:
 *  "EARLY 相的第一步必须是 platform 的 early_init, 否则 console 还没起来";
 *  要知道谁是 platform 就得有字段, 而且它本来就写在 plugin.toml 里)。
 */
#define BR_PLUGIN_TYPE_APP       0u
#define BR_PLUGIN_TYPE_INTERFACE 1u
#define BR_PLUGIN_TYPE_ABILITY   2u
#define BR_PLUGIN_TYPE_PLATFORM  3u

#define BR_SUBKIND_NONE      0u
#define BR_SUBKIND_SCHEDULER 1u
#define BR_SUBKIND_FRAMEWORK 2u
#define BR_SUBKIND_IO        3u
#define BR_SUBKIND_FS        4u
#define BR_SUBKIND_SERVICE   5u

/* `[plugin].phase` 的自述(只作呈现/诊断 —— init 落在哪一相由**类别**决定,
 * 见 `1-01` §9: 非 Service/Interface ⇒ CORE, Service/Interface ⇒ LATE)。
 * 取它的入口在 `br_plugin_t` 定义之后声明(见"观测"节)。 */

/* ==================================================================== 资源 */

/* `[[res]]` 的运行期镜像(诊断用; 预算真值在 manifest, 见本文件头注)。 */
typedef struct br_res {
    br_u32 ram_kib;
    br_u32 stack_kib;
} br_res_t;

/* ==================================================================== 描述符 */

/*
 * 插件描述符。**生成物实例化它**(`build/gen/<plugin>/plugin_desc.c`), 手写插件
 * 既不需要也不应该自己写 —— 唯一真值是 `plugin.toml` + 各插件头文件。
 */
typedef struct br_plugin {
    const char *name;                 /* plugin.toml 的 [plugin].name */
    br_u32      plugin_type;          /* BR_PLUGIN_TYPE_* */
    br_u32      subkind;              /* BR_SUBKIND_* */
    const char *phase_self;           /* [plugin].phase 的自述("early"/"core"/"late"/"app") */
    br_u16      ver[4];               /* COMPAT_GEN.MAJOR.MINOR.REVISE */
    br_u16      api_rev;
    br_u32      sched_class;          /* BR_SCHED_CLASS_* */
    const br_dep_t *deps;             /* 以 .name == BR_NULL 结尾; BR_NULL = 无边 */
    const char *abi_id;
    const char *const *api_syms;      /* 仅 Interface 插件占有符号族(1-01 §7.3) */
    br_res_t    res;
    int (*early_init)(void);          /* 可空(用 BR_PLUGIN_NO_HOOK) */
    int (*init)(void);
    int (*start)(void);
} br_plugin_t;

/* 空钩子的写法: 生成物里"该插件没有这一相"时用它, 让 plugin_manager 不用判 0/非 0 两种空。 */
#define BR_PLUGIN_NO_HOOK  ((int (*)(void))0)

/* 段名与**链接器边界符号**(由 `platform/…/src/link.ld` 定义, 带 KEEP)。
 * 段名含 `.`, GNU ld 不会自动生成 `__start_/__stop_` ⇒ 边界符号必须显式给。 */
#define BR_PLUGIN_SECTION_NAME ".br_plugins"

/*
 * 声明一个描述符(生成物用的展开宏; `deps` 传 `const br_dep_t[]` 或 BR_NULL)。
 * `aligned(8)`: 结构体含指针 —— 见文件头注的偏离登记。
 */
#define BR_PLUGIN_SECTION  __attribute__((used, section(BR_PLUGIN_SECTION_NAME), aligned(8)))

#define BR_PLUGIN_DEFINE(sym, nm, v0, v1, v2, v3, rev, cls, dp, abi, syms, res_, ei, in, st) \
    static const br_plugin_t BR_PLUGIN_SECTION sym = {                                       \
        .name = (nm),                                                                        \
        .ver = {(v0), (v1), (v2), (v3)},                                                     \
        .api_rev = (rev),                                                                    \
        .sched_class = (cls),                                                                \
        .deps = (dp),                                                                        \
        .abi_id = (abi),                                                                     \
        .api_syms = (syms),                                                                  \
        .res = (res_),                                                                       \
        .early_init = (ei),                                                                  \
        .init = (in),                                                                        \
        .start = (st),                                                                       \
    }

/* ==================================================================== 管理器 */

/* 插件管理器的入口(由 `platform/…/src/start.S` 在 platform.early_init 之后调用)。
 *
 * 它做的**全部**事情(1-01 §9 / 3-05 §2):
 *   扫 `.br_plugins` → 建 init 边 → Kahn 拓扑排序(有环 ⇒ panic, 报完整环路径)
 *   → EARLY(逐插件 early_init) → CORE(类别决定的 init) → LATE → 全局开中断
 *   → START(拓扑序, APP 最后) → br_sched_run()(core 的 idle/首次调度)。
 *
 * 不返回。 */
BR_NORETURN void br_plugin_manager_run(void);

/* ==================================================================== 观测 */

/* `[plugin].phase` 的自述串(`p` 为空 ⇒ "?")。 */
const char *br_plugin_phase_self(const br_plugin_t *p);

/* 描述符总数(段里的条数)。 */
br_u32 br_plugin_count(void);

/* 按**拓扑序**取第 i 个描述符(i >= count ⇒ BR_NULL)。 */
const br_plugin_t *br_plugin_at(br_u32 index);

/* 按名字查(线性; 名字表是启动期建好的静态数组)。 */
const br_plugin_t *br_plugin_find(const char *name);

/* 相位名("EARLY"/"CORE"/"LATE"/"APP")与当前进度(走到了第几相; 观测用)。 */
const char *br_plugin_phase_name(br_u32 phase);
br_u32 br_plugin_phase_reached(void);

/* init 失败计数(0 = 全部成功; 原型口径: 首败即停机, 所以它只会是 0 或 1)。 */
br_u32 br_plugin_init_failures(void);

/*
 * 插件管理器的一致性用例(TC-PLUG-*, 6-01 §3.9):
 *   001 段非空且条数 == 生成物数量
 *   002 拓扑序满足所有 init 边(逐边校验)
 *   003 相位单调(每条 init 边的提供方完成点 <= 消费方)
 *   002b 环检测(**负例**): 用手工构造的假边集跑一遍 Kahn, 必须报环且报出完整路径
 * 打印 `[PLGCONF] PASS/FAIL <id> …` 并以 `[PLGCONF] SUMMARY pass=N fail=M` 收尾。
 * 由 APP 的 start() 调用(与 br_dump_conformance 同一位置、同一形态)。
 */
void br_plugin_conformance(void);

#endif /* BR_CORE_BR_PLUGIN_H */

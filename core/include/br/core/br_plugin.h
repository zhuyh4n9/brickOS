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
    /*
     * 自检钩子(接口测试的**唯一**入口; 可空)。
     *
     * 语义: 返回**失败项数**(0 = 全绿), 并把每项结论打进日志(`[XXXCONF] PASS/FAIL <id> ...`);
     * 负数 = 钩子自身出错(与失败项数分开记, 见 plugin_mgr 的 rc 口径)。
     * 它**不是**四相里的第五相: 相位是"能力完成点"(1-01 §9 的四相), 而自检是**验证**,
     * 对所有插件都在同一时刻发生(全部 start 之后、调度器接管之前 —— 见 ADR-0010)。
     *
     * ★ 为什么不是 `[[export]]` 里的 API: 与 early_init/init/start 同源(ADR-0005 裁定 9)——
     *   自检是**组合期契约**(生成物引用它), 不是插件对外提供的能力。消费者调用自检会让
     *   "测试面"进入 golden 接口, 于是改一个用例就变成接口变更。
     *
     * ★ 生成期开关(ADR-0010 §2.3): `product.toml [selftest]` 关掉自检时, 生成物在这里发
     *   `BR_PLUGIN_NO_HOOK` ⇒ 测试代码无人引用 ⇒ `--gc-sections` 把它整段裁出镜像。
     *   所以"关了自检的镜像"里**没有测试代码**, 而不只是"不跑测试"。
     */
    int (*selftest)(void);
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
/*
 * `BR_PLUGIN_DEFINE` **不含 `selftest`**(形参表冻结, 与 ADR-0005 裁定 1 同源: 生成物已改为
 * 直接实例化结构体)。宏用户省略 `.selftest` ⇒ 该字段按 C 规则零初始化 = `BR_PLUGIN_NO_HOOK`
 * ⇒ "无自检"。要挂自检的手写描述符请直接写具名初始化器并在末尾加 `.selftest = 函数名`。
 */

/* ==================================================================== 管理器 */

/*
 * 管理器的三个入口 = 启动链的三格(见 `br/core/br_main.h` 与 ADR-0008):
 *
 *   ① br_plugin_manager_init()            core 平台无关初始化(bootstrap 阶段 ①)
 *   ② br_plugin_manager_platform_init()   platform 插件初始化(bootstrap 阶段 ②)
 *   ④ br_plugin_manager_run()             拓扑 + 相位驱动(bootstrap 阶段 ④)
 *
 * 为什么拆成三格而不是一个大函数: "扫段/找 platform"必须在任何 C 代码之前(且此刻
 * console 还没起来), "platform 的 early_init"是启动链的第二格, 而"相位驱动"要等到
 * core 的堆与地址映射都就绪(阶段 ③)才能跑 —— 三者的前提各不相同(ADR-0008 §2)。
 */

/*
 * ① 平台无关的静态准备: 扫 `.br_plugins` 段建插件表 + 显式找出 platform 插件。
 *
 * **全程静默**(此刻早期 console 还没 init —— 它属 platform 的 `early_init`; 在那之前
 * 写 PL011 的 DR 会被 QEMU 丢掉)。段空/超静态上界/段里没有 `plugin_type = platform`
 * ⇒ 直接 panic(镜像本就不可能成立)。
 *
 * 由 core 的入口 `br_core_main()` 调用; 建边/拓扑/相位驱动在 ④。
 */
void br_plugin_manager_init(void);

/*
 * ② platform 插件初始化: 显式取 `plugin_type == BR_PLUGIN_TYPE_PLATFORM` 的那一条,
 * 跑它的 `early_init`(早期 console / PIC 注册与绑定表 / region 表 / 页表 ops)。
 *
 * ★ 取 platform 靠**字段**而不是拓扑序的巧合: 此刻 PIC/页表/console 都还没就绪,
 *   "谁排第一"与"能不能观测"无关。这条也是 TC-PLUG-003 的判据
 *   ("第一个真的跑起来的 early_init 属于 platform")。
 *
 * 前置: `br_plugin_manager_init()` 已跑过。首败即停机(与 ④ 的 init/start 同一纪律)。
 */
void br_plugin_manager_platform_init(void);

/*
 * ④ 建 init 边 → Kahn 拓扑排序(有环 ⇒ panic, 报完整环路径)
 *   → EARLY(其余插件的 early_init) → CORE(类别决定的 init) → LATE → 全局开中断
 *   → START(拓扑序, 但 APP 最后是显式规则)
 *   → **SELFTEST pass**(逐插件 `selftest`; 见下)
 *   → `br_sched_run()`(core 的 idle/首次调度)。
 *
 * 前置: ①② 已跑过(启动链顺序, 见 `br/core/br_main.h`)。不返回。
 */
BR_NORETURN void br_plugin_manager_run(void);

/*
 * 自检 pass(ADR-0010)。**不是第五相**: 相位是"能力完成点", 自检是**验证** ——
 * 它对所有插件都在同一时刻发生, 顺序只是"复用了 init 的拓扑序"以求确定性。
 *
 * 为什么在这个时刻(START 之后、`br_sched_run()` 之前): 此刻 timer 已 armed、中断已开、
 * 设备已注册、挂载已就位 —— 现有全部一致性套件的前提都成立; 而调度器一旦接管, 控制流
 * 就再也不会回到"单线程跑测试"的形态。
 *
 * 口径:
 *   - `selftest == BR_PLUGIN_NO_HOOK` 的插件跳过(**大多数镜像里都是这样**: 生成期开关
 *     关掉自检时, 生成物发的就是 NO_HOOK);
 *   - 返回 **失败项数**(0 = 全绿); 负数 = 钩子自身出错。两者都**不**停机 ——
 *     自检是**观测**, 红绿由门禁判(`[XXXCONF] FAIL` 在 gates.toml 的 forbid 里);
 *     这与 init/start 的"首败即停机"(裁定 G6)是**两条不同的纪律**, 见 ADR-0010 §2.4。
 *   - pass 末尾打一行 `[SELFTEST] SUMMARY plugins=N ran=M fails=K`(启动证据)。
 */
void br_plugin_manager_selftest(void);

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
 * 插件管理器自检套件已移到 `core/selftest/plugin_selftest.c`(ADR-0010), 故不在此声明。
 * 为什么不留在对外头: 自检是**测试面**, 不是插件能力 —— 声明进 `br_*.h` 会让
 * "改一个用例"变成接口变更(golden 接口 hash 覆盖的正是对外声明面); 实现与声明面
 * 都在 core/selftest/。
 */

#endif /* BR_CORE_BR_PLUGIN_H */

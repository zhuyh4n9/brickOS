/*
 * brickOS prototype v0.2.0 — core 自检套件的**唯一入口**(core/selftest/)
 *
 * 设计依据与逐条裁定: `docs/decisions/0010-plugin-selftest.md`。
 *
 * ## 这个目录是什么
 *   **接口测试**, 与核心代码分离。设计侧把"目标侧一致性用例"当作交付物的一部分
 *   (`6-01-test.md` 的 `TC-*` 目录), 而原型此前把它们的实现**混在生产 .c 里**
 *   (svc.c / sync.c / sched_core.c / plugin_mgr.c 各带一段 `*_conformance`)。
 *   本目录把测试收拢到一处, 于是:
 *     - 生产文件只留机制, 不再背着几百行用例;
 *     - "关掉自检 = 测试代码不进镜像"成为**机械事实**(整目录不参与编译);
 *     - 用例的读者不必在生产实现里翻找判定逻辑。
 *
 * ## 为什么是"同开关"而不是独立开关
 *   `product.toml [selftest]` 同时管插件自检与 core 自检 —— 一个产品要么带测试、
 *   要么不带。两个开关会引入"core 测了、插件没测"这种半吊子状态, 而它没有任何
 *   合法用途(ADR-0010 §2.3)。
 *
 * ## 聚合器的口径
 *   与 `br_plugin_manager_selftest()` 对插件所做的一样: **只汇总不下判决** ——
 *   返回**失败项数之和**, 日志由各套件自己打(`[PLGCONF]`/`[SVCCONF]`/...);
 *   红绿由门禁判(`tests/gates.toml` 的 forbid)。**任何失败都不停机**(ADR-0010 §2.4)。
 *
 * ## 与启动链的关系
 *   本函数被 `br_plugin_manager_selftest()` 经**弱引用**调用(见 plugin_mgr.c):
 *   自检关掉时本目录不参与编译 ⇒ 符号不存在 ⇒ 空指针 ⇒ 管理器跳过。于是"编了没有"
 *   与"要不要跑"是同一个事实, 不存在第二处开关会漂移。
 */
#include <br/core/br_types.h>

/*
 * 各子系统的自检入口。它们**曾经**是生产文件里的 `*_conformance()`, 现在定义在
 * 本目录, 声明集中在这里(而不是散在四个对外头里)——
 * 理由: 自检入口**不是插件的对外能力**, 不该进 `br_*.h` 的 golden 面(与 ADR-0005
 * 裁定 9 对钩子的处置同源: 测试面进接口会让"改一个用例"变成接口变更)。
 */
int br_plugin_selftest(void);   /* player: 插件管理器(段/拓扑/相位/APP 最后) */
int br_service_selftest(void);  /* 服务注册表(发布/查找/错误码) */
int br_sched_selftest(void);    /* 调度框架 + 所选调度器(真线程 create/yield/join/sleep) */
int br_sync_selftest(void);     /* 同步原语 + 时间(sync.c/sched_core.c 的等待面) */
int br_work_selftest(void);     /* 下半部/工作队列(ADR-0011: 有界/FIFO/非重入/bh 禁令) */

/* 对外只此一个入口(被 plugin_mgr 弱引用); 原型给 -Wmissing-prototypes。 */
int br_core_selftest(void);

int br_core_selftest(void)
{
    /* 顺序: 先"静态组合是否正确"(插件管理器), 再 core 的运行期设施(服务表 → 调度 →
     * 同步 → 下半部)。同步套件排在调度之后是因为它**真的创建线程并阻塞**, 会改变调度
     * 状态; 工作队列最后 —— 它不改调度状态, 但 case_wq_005 会刻意触发一次 bh 禁令的
     * 留痕, 放在最后免得那条 trace 干扰别的用例的"有没有新事件"判断。 */
    int fails = 0;

    fails += br_plugin_selftest();
    fails += br_service_selftest();
    fails += br_sched_selftest();
    fails += br_sync_selftest();
    fails += br_work_selftest();

    return fails;
}

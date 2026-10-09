/*
 * brickOS prototype v0.2.0 — 服务注册表**内部**契约(core 私有, 不给插件)
 *
 * 为什么需要这个头(与 `core/src/sched/sched_internal.h` / `core/src/irq/irq_internal.h`
 * 同一手法): 注册表的自检套件(`core/selftest/svc_selftest.c`, ADR-0010)要拿
 * `selftest.` 前缀的临时条目做 publish → lookup 的往返, 并在结束时把它们**移出表**。
 * 这两件事公开面都给不了:
 *
 *   - `svc_remove`(白盒出表): 对外 API(`br_service_publish` / `br_service_lookup` /
 *     `br_service_count` / `br_service_name_at` / `br_service_lookup_hits`)**没有移除** ——
 *     重名只报 -EEXIST, 发布是单向的(设计 `3-01` §9 就没有 unpublish)。所以
 *     "清掉自己发的条目"用公开面**做不到**:
 *       * 不清理 ⇒ 用例条目留在表里, 污染后续 dump 的注册表清单;
 *       * 每次用唯一名字只能绕开重复跑时的 -EEXIST, 解决不了上面那一条;
 *       * "重建注册表"没有 API, 生产语义里也不该有 —— 注册表只增。
 *     结论: 必须白盒暴露 `svc_remove`(而不是在用例里另立一张平行表)。
 *   - `name_equal`: 用例要复核 `br_service_name_at()` 报的名字确实是自己发的那个。
 *     它是**生产**的比较函数(publish/lookup/hits 都在用), 不是测试专用 ——
 *     让它只留一处真值, 好过在用例里再写一份"看着一样"的字符串比较。
 *
 *   放在 `core/src/svc/` 下而不是 `br/core/br_svc.h`: 这是 core 私有符号, **不进**
 *   golden 接口面 —— 于是"改一个用例"不会改接口 hash(与 ADR-0005 裁定 9 对自检钩子的
 *   处置同源: 测试面不是插件能力)。
 */
#ifndef BR_SVC_INTERNAL_H
#define BR_SVC_INTERNAL_H

#include <br/core/br_types.h>

/* 名字相等(publish/lookup/lookup_hits 的生产比较函数; 自检用它复核 name_at 的返回)。 */
br_bool name_equal(const char *a, const char *b);

/* 从注册表移除一条(name 不在表里 ⇒ 静默返回)。生产面没有 unpublish;
 * 只有自检用它清掉自己的临时条目。 */
void svc_remove(const char *name);

#endif /* BR_SVC_INTERNAL_H */

/*
 * brickOS prototype v0.2.0 — 服务注册表(core 侧)
 *
 * 设计依据: `3-01` §9(`br_service_publish` / `br_service_lookup`),
 *           `3-06`(服务管理)、`11-01`(服务模型)、`1-01` §9(发布/查找发生在 init 相)。
 *
 * 语义(逐字照设计, 不做增删):
 *   - **core 只管 名字 → 指针**, 不解释 ops 的类型(类型安全是插件之间的事);
 *   - 发布 = 服务方在自己的 `init` 里调; 查找 = 依赖方在自己的 `init` 里调
 *     ⇒ 相序 + init 拓扑序**共同保证**"取得到"(1-01 §9 的两个完成点);
 *   - 重名 ⇒ `-EEXIST`(不覆盖 —— 覆盖会让"谁是真身"取决于启动顺序);
 *   - 查不到 ⇒ 返回 `BR_NULL`(不是错误码: 设计的签名就是返回指针, `3-01` §9)。
 *
 * 与设计的偏离: 本刀新增两个**观测**入口(count/name_at), 供 5-01 风格的 dump 与
 * 一致性用例读注册表; 它们不改变发布/查找语义(ADR-0005 §2)。
 */
#ifndef BR_CORE_BR_SVC_H
#define BR_CORE_BR_SVC_H

#include <br/core/br_types.h>

/* 注册表容量: 静态上界(与 trace 环、IRQ 域池同款 —— 设计允许 manifest 裁剪,
 * v0.2 取默认值)。 */
#ifndef BR_SERVICE_MAX
#define BR_SERVICE_MAX 32u
#endif

/* 发布一个服务。`ops` 由服务方拥有(通常指向一个静态 const 结构);
 * 重名 ⇒ -EEXIST; 表满 ⇒ -ENOSPC; name/ops 为空 ⇒ -EINVAL。 */
int br_service_publish(const char *name, const void *ops);

/* 查找一个服务。不存在 ⇒ BR_NULL。 */
const void *br_service_lookup(const char *name);

/* ---- 观测 ---- */
br_u32 br_service_count(void);
const char *br_service_name_at(br_u32 index);   /* index 越界 ⇒ BR_NULL */
/* 该服务是否已被查找过(记账: 5-01 §4 的"谁在用谁")。 */
br_u32 br_service_lookup_hits(const char *name);

/*
 * 注册表一致性用例(TC-SVC-001..003, 6-01 §3.8):
 *   001 publish 后 lookup 拿到同一指针(且 name_at/count 一致)
 *   002 重名 publish ⇒ -EEXIST(且原指针未被覆盖)
 *   003 未发布的 name ⇒ BR_NULL; 非法入参(空 name/空 ops) ⇒ -EINVAL
 * 打印 `[SVCCONF] PASS/FAIL <id> …` + `[SVCCONF] SUMMARY pass=N fail=M`。
 * 它自己用的名字全部带 `selftest.` 前缀, 且**用例结束后从表里移除**(白盒),
 * 免得污染后续 dump 的注册表清单。
 */
void br_service_conformance(void);

#endif /* BR_CORE_BR_SVC_H */

/*
 * brickOS prototype v0.2.0 — 服务注册表(core 公地)
 *
 * 设计依据: `3-01` §9(`br_service_publish` / `br_service_lookup`)、`3-06`(服务管理)、
 *           `1-01` §9(发布 = 服务方 init; 查找 = 依赖方 init ⇒ 相序 + init 拓扑序共同保证取得到)。
 *
 * 语义(逐字照设计, 不做增删):
 *   - core 只管 **名字 → 指针**, 不解释 ops 的类型;
 *   - 重名 ⇒ `-EEXIST`(不覆盖 —— 覆盖会让"谁是真身"取决于启动顺序);
 *   - 表满 ⇒ `-ENOSPC`; 空 `name`/空 `ops` ⇒ `-EINVAL`;
 *   - 查不到 ⇒ `BR_NULL`(设计的签名就是返回指针)。
 *
 * 单线程前提(设计 `3-06` §2 第 1 项): **发布/查找都发生在 init 相**(EARLY/CORE/LATE),
 * 那时全局关中断、无线程 ⇒ 注册表不需要锁。START 相之后再发布是调用方的错, 本文件
 * 不做检测(也不假装线程安全)。
 *
 * 与设计的偏离(登记在 `docs/decisions/0005-plugin-manager.md` §2.7):
 *   - 新增三个**观测**入口(`br_service_count` / `br_service_name_at` /
 *     `br_service_lookup_hits`, 见 `br_svc.h`), 供 dump 风格呈现与一致性用例读表;
 *   - 白盒移除入口 `svc_remove`(声明在 `core/src/svc/svc_internal.h`): 设计里没有
 *     unpublish, 而 core 自检(ADR-0010)要用 `selftest.` 前缀的临时条目并**自己清干净**
 *     —— 用例本体已搬到 `core/selftest/svc_selftest.c`。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_svc.h>
#include <br/core/br_types.h>

#include "svc_internal.h"

/* ==================================================================== 表 */

typedef struct br_svc_entry {
    const char *name;
    const void *ops;
    br_u32      hits;   /* lookup 命中次数(记账: 5-01 §4 的"谁在用谁") */
} br_svc_entry_t;

static br_svc_entry_t s_svc[BR_SERVICE_MAX];
static br_u32 s_count;

/* 名字相等(publish/lookup/hits 的比较; 自检也用它复核 name_at 的返回 —— 见 svc_internal.h)。 */
br_bool name_equal(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

/* ==================================================================== 发布/查找 */

int br_service_publish(const char *name, const void *ops)
{
    if (name == BR_NULL || ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (name_equal(s_svc[i].name, name)) {
            return BR_ERR(BR_EEXIST);   /* 不覆盖 */
        }
    }
    if (s_count >= BR_SERVICE_MAX) {
        return BR_ERR(BR_ENOSPC);
    }
    s_svc[s_count].name = name;
    s_svc[s_count].ops  = ops;
    s_svc[s_count].hits = 0u;
    s_count++;
    return 0;
}

const void *br_service_lookup(const char *name)
{
    if (name == BR_NULL) {
        return BR_NULL;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (name_equal(s_svc[i].name, name)) {
            s_svc[i].hits++;
            return s_svc[i].ops;
        }
    }
    return BR_NULL;
}

/* ==================================================================== 观测 */

br_u32 br_service_count(void)
{
    return s_count;
}

const char *br_service_name_at(br_u32 index)
{
    if (index >= s_count) {
        return BR_NULL;
    }
    return s_svc[index].name;
}

br_u32 br_service_lookup_hits(const char *name)
{
    if (name == BR_NULL) {
        return 0u;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (name_equal(s_svc[i].name, name)) {
            return s_svc[i].hits;
        }
    }
    return 0u;
}

/* 白盒移除(生产面没有 unpublish; 只有自检用它清掉自己的临时条目 —— 见 svc_internal.h)。 */
void svc_remove(const char *name)
{
    if (name == BR_NULL) {
        return;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (!name_equal(s_svc[i].name, name)) {
            continue;
        }
        for (br_u32 j = i + 1u; j < s_count; j++) {
            s_svc[j - 1u] = s_svc[j];
        }
        s_count--;
        s_svc[s_count].name = BR_NULL;
        s_svc[s_count].ops  = BR_NULL;
        s_svc[s_count].hits = 0u;
        return;
    }
}


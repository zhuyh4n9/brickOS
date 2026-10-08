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
 *   - 一致性用例自带 `selftest.` 前缀并在结束时**白盒移除**自己的条目(不污染后续 dump)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_svc.h>
#include <br/core/br_types.h>

/* ==================================================================== 表 */

typedef struct br_svc_entry {
    const char *name;
    const void *ops;
    br_u32      hits;   /* lookup 命中次数(记账: 5-01 §4 的"谁在用谁") */
} br_svc_entry_t;

static br_svc_entry_t s_svc[BR_SERVICE_MAX];
static br_u32 s_count;

static br_bool name_equal(const char *a, const char *b)
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

/* 白盒移除(只给本文件的一致性用例用: 用例条目不能留在表里污染后续 dump)。 */
static void svc_remove(const char *name)
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

/* ==================================================================== 一致性用例 */

/* 用例用的名字全部带 `selftest.` 前缀(结束后从表里移除)。 */
#define SVC_T_A   "selftest.svc.a"
#define SVC_T_B   "selftest.svc.missing"

static br_u32 s_sc_pass;
static br_u32 s_sc_fail;

static void sc_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_sc_pass++;
        br_log_info("[SVCCONF] PASS %s %s", tag, what);
    } else {
        s_sc_fail++;
        br_log_info("[SVCCONF] FAIL %s %s", tag, what);
    }
}

void br_service_conformance(void)
{
    static const br_u32 ops_a = 0xA1B2C3D4u;   /* "ops" 只需是一个稳定指针 */
    static const br_u32 ops_b = 0x5A5A1234u;

    s_sc_pass = 0u;
    s_sc_fail = 0u;

    br_log_info("[SVCCONF] service registry conformance (publish/lookup/错误码)");

    /* 防御性清理: 上一次次跑留下的同名条目(重复调用用例时应幂等)。 */
    svc_remove(SVC_T_A);

    /* ---- TC-SVC-001: publish 后 lookup 拿到同一指针(且 name_at/count 一致) ---- */
    {
        const br_u32 before = br_service_count();
        const int rp = br_service_publish(SVC_T_A, &ops_a);
        const void *got = br_service_lookup(SVC_T_A);
        const br_u32 after = br_service_count();

        br_bool ok = (rp == 0) && (got == (const void *)&ops_a) && (after == before + 1u);
        /* name_at/count 与表一致 */
        ok = ok && (after > 0u);
        if (ok) {
            br_bool found = BR_FALSE;
            for (br_u32 i = 0u; i < after; i++) {
                if (name_equal(br_service_name_at(i), SVC_T_A)) {
                    found = BR_TRUE;
                    break;
                }
            }
            ok = found;
        }
        ok = ok && (br_service_lookup_hits(SVC_T_A) >= 1u);
        sc_report(ok, "TC-SVC-001", "publish → lookup 往返: 指针一致, count/name_at 同步, 命中记账");
    }

    /* ---- TC-SVC-002: 重名 publish ⇒ -EEXIST, 且原指针未被覆盖 ---- */
    {
        const int rp2 = br_service_publish(SVC_T_A, &ops_b);
        const void *still = br_service_lookup(SVC_T_A);
        sc_report((rp2 == BR_ERR(BR_EEXIST)) && (still == (const void *)&ops_a),
                  "TC-SVC-002", "重复 publish ⇒ -EEXIST 且原 ops 未被覆盖");
    }

    /* ---- TC-SVC-003: 未发布的名字 ⇒ BR_NULL; 非法入参 ⇒ -EINVAL ---- */
    {
        const void *miss = br_service_lookup(SVC_T_B);
        const int   e_name = br_service_publish(BR_NULL, &ops_b);
        const int   e_ops  = br_service_publish(SVC_T_B, BR_NULL);
        sc_report((miss == BR_NULL) && (e_name == BR_ERR(BR_EINVAL))
                  && (e_ops == BR_ERR(BR_EINVAL)),
                  "TC-SVC-003", "lookup 缺失 ⇒ NULL; 空 name/空 ops ⇒ -EINVAL");
    }

    /* ---- 收尾: 用例条目出表(不污染后续 dump 的注册表清单) ---- */
    svc_remove(SVC_T_A);
    svc_remove(SVC_T_B);

    br_log_info("[SVCCONF] SUMMARY pass=%u fail=%u total=%u",
                s_sc_pass, s_sc_fail, s_sc_pass + s_sc_fail);
}

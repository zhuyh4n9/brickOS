/*
 * brickOS prototype v0.2.0 — 服务注册表自检套件(core 公地; TC-SVC-001..003)
 *
 * 从 `core/src/svc/svc.c` **原样搬来**(ADR-0010): 用例与其日志一字未改 ——
 * 门禁 `tests/gates.toml` 按 `[SVCCONF] PASS/FAIL` 与 `[SVCCONF] SUMMARY` 判红绿。
 *
 * 为什么搬出生产文件:
 *   - 生产文件只留机制(表/发布/查找/观测), 不再背着用例;
 *   - 关掉自检时整个 `core/selftest/` 目录**不参与编译** ⇒ 测试代码不进镜像(不只是不跑);
 *   - 入口不再进 `br_svc.h`: 测试面进 golden 接口会让"改一个用例"变成接口变更。
 *
 * 白盒访问路径: `../src/svc/svc_internal.h`(为什么必须白盒, 见该头注)。
 *
 * 入口口径(与 `br_plugin_manager_selftest` 对插件自检一致): 返回**失败项数**
 * (0 = 全绿), 只记不停 —— 红绿由门禁判(ADR-0010 §2.4)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_svc.h>
#include <br/core/br_types.h>

#include "../src/svc/svc_internal.h"

/* 入口原型(声明面集中在 `core_selftest.c`; 这里重述只为满足全局函数的原型纪律)。 */
int br_service_selftest(void);

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

int br_service_selftest(void)
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

    return (int)s_sc_fail;
}

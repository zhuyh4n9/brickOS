/*
 * brickOS prototype v0.2.0 — dev-core 的**自检套件**(framework/dev-core/src/dev_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)。
 *
 * ## 为什么自检不在 `dev_core.c` 里(本文件存在的理由)
 *   测试入口**不是插件的对外面**: 它若留在 `include/` 里, "加一条用例"就会变成 golden
 *   接口面的变更, 而消费者(别的插件)也绝不该调它。挪进独立的 `src/dev_selftest.c` 后
 *   `dev_core.c` 只剩生产路径; 用例仍在同一插件的编译单元集里(`[build].sources` 覆盖
 *   `src/` 下的全部 `.c`)。
 *
 * ## 它测什么, 与生产代码的边界在哪
 *   设备域注册表的**总套件**(自编号 `TC-DEV-*`): 只经 `br/dev/br_dev.h` 的**公开 API**
 *   驱动(命名规则 / 注册表 / ioctl 编码)。本件没有"被测对象就是内部机制本身"的用例,
 *   所以**不需要**内部头 —— 对照 vfs-core 的 `src/vfs_internal.h`: 那种访问器只为
 *   "用公开 API 表达不出来"的判据存在, 这里一条都没有。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[DEVCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[DEVCONF] FAIL`)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int dev_core_selftest(void);

/* ==================================================================== 一致性用例 */

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[DEVCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[DEVCONF] FAIL %s %s", tag, what);
    }
}

int dev_core_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[DEVCONF] dev-core conformance (registry / naming / ioctl 编码)");

    /* ---- TC-DEV-001: 命名规则(纯函数: 不碰注册表, 所以可以尽情枚举反例) ---- */
    {
        const br_bool ok =
            (br_dev_name_valid("uart0")   == BR_TRUE)  &&
            (br_dev_name_valid("a")       == BR_TRUE)  &&
            (br_dev_name_valid("nor0x")   == BR_TRUE)  &&
            (br_dev_name_valid(BR_NULL)   == BR_FALSE) &&
            (br_dev_name_valid("")        == BR_FALSE) &&
            (br_dev_name_valid("Uart0")   == BR_FALSE) &&   /* 大写 */
            (br_dev_name_valid("0uart")   == BR_FALSE) &&   /* 数字开头 */
            (br_dev_name_valid("u/art")   == BR_FALSE) &&   /* 斜杠: 名 = 路径分量 */
            (br_dev_name_valid("u_art")   == BR_FALSE) &&   /* 下划线 */
            (br_dev_name_valid("uart0uart0uart0uart0uart0uart0uart0") == BR_FALSE); /* 超长 */
        pc(ok, "TC-DEV-001", "命名规则 [a-z][a-z0-9]* / 无斜杠 / 长度上界");
    }

    /* ---- TC-DEV-002: 非法登记被拒且**不污染**注册表 ----
     * 为什么只用"非法输入"而不造一个测试设备: dev-core 没有注销 API(v1 无热插拔 ——
     * 设计 SD-4/O-S3), 一旦登记就**永远**出现在 /dev 里; 用例不该往被观测的命名空间里
     * 扔垃圾。正面的登记路径由真实驱动覆盖(io/uart-pl011 在它的 CORE init 里登记 uart0),
     * 本用例只确认"坏输入进不来 + 条数不变"。 */
    {
        const br_u32 before = br_dev_count();
        br_dev_class_entry_t e = { 0u, BR_NULL, BR_NULL, BR_NULL };

        const int r1 = br_dev_add("Bad", &e);          /* 名字非法(大写) */
        e.class_id = BR_CLASS_NONE;
        const int r2 = br_dev_add("testdev", &e);      /* class_id = NONE: 没有形状解释者 */
        const int r3 = br_dev_add("testdev", BR_NULL); /* entry 为空 */
        const int r4 = br_dev_add(BR_NULL, BR_NULL);   /* 两个都空 */

        const br_bool ok = (r1 == BR_ERR(BR_EINVAL)) && (r2 == BR_ERR(BR_EINVAL)) &&
                           (r3 == BR_ERR(BR_EINVAL)) && (r4 == BR_ERR(BR_EINVAL)) &&
                           (br_dev_count() == before) &&
                           (br_dev_lookup("testdev") == BR_NULL);
        pc(ok, "TC-DEV-002", "非法登记被拒(-EINVAL)且注册表条数不变");
    }

    /* ---- TC-DEV-003: 查找与缺失(缺失必须返回 BR_NULL, 不是错误码) ---- */
    {
        const br_bool ok = (br_dev_lookup("nosuchdev") == BR_NULL) &&
                           (br_dev_lookup(BR_NULL) == BR_NULL) &&
                           (br_dev_name_at(br_dev_count()) == BR_NULL) &&
                           (br_dev_entry_at(br_dev_count()) == BR_NULL);
        pc(ok, "TC-DEV-003", "未注册/越界 ⇒ BR_NULL");
    }

    /* ---- TC-DEV-004: 枚举与查找同源(逐条 name_at ↔ lookup 往返) ---- */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const char *n = br_dev_name_at(i);
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            if (n == BR_NULL || e == BR_NULL) {
                ok = BR_FALSE;
                break;
            }
            if (br_dev_name_valid(n) == BR_FALSE) {
                ok = BR_FALSE;
                break;
            }
            if (br_dev_lookup(n) != e) {
                ok = BR_FALSE;
                break;
            }
        }
        pc(ok, "TC-DEV-004", "枚举条目: 名字合法 且 lookup(name) == entry_at(i)");
    }

    /* ---- TC-DEV-005: 重名 ⇒ -EEXIST(用表里**已有的**名字, 不新增条目) ---- */
    {
        br_bool ok = BR_TRUE;
        const br_u32 n = br_dev_count();
        if (n > 0u) {
            const char *first = br_dev_name_at(0u);
            const br_dev_class_entry_t *e = br_dev_entry_at(0u);
            ok = (first != BR_NULL) && (e != BR_NULL) &&
                 (br_dev_add(first, e) == BR_ERR(BR_EEXIST));
        } else {
            /* 表空: 没有"已有名字"可用 ⇒ 本用例**不适用**。如实报 PASS 但在描述里写明
             * 前提, 免得读日志的人以为它测过重名。 */
            br_log_info("[DEVCONF] note: 注册表为空 ⇒ TC-DEV-005 只验证了'不适用'分支");
        }
        pc(ok, "TC-DEV-005", "重名 ⇒ -EEXIST(唯一性执法)");
    }

    /* ---- TC-DEV-006: ioctl 编码(与 Linux _IOC 位布局一致; 纯编译期) ---- */
    {
        typedef struct { br_u32 a; br_u32 b; } ioc_payload_t;
        const br_u32 cmd = BR_IOWR('u', 7u, ioc_payload_t);
        const br_bool ok = (BR_IOC_DIR(cmd)  == (BR_IOC_READ | BR_IOC_WRITE)) &&
                           (BR_IOC_TYPE(cmd) == (br_u32)'u') &&
                           (BR_IOC_NR(cmd)   == 7u) &&
                           (BR_IOC_SIZE(cmd) == (br_u32)sizeof(ioc_payload_t));
        pc(ok, "TC-DEV-006", "ioctl 编码 dir[31:30]/size[29:16]/type[15:8]/nr[7:0]");
    }

    br_log_info("[DEVCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}

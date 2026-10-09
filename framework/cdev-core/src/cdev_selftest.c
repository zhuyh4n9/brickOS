/*
 * brickOS prototype v0.2.0 — cdev-core 的**自检套件**(framework/cdev-core/src/cdev_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)。
 *
 * ## 为什么自检不在 `cdev_core.c` 里(本文件存在的理由)
 *   测试入口不是插件的对外面(见 `include/br/dev/br_cdev.h` 的说明)。挪进 `src/` 后
 *   生产文件只剩生产路径, 而"用例读内部"这件事被收进 `src/cdev_internal.h` 的两个
 *   访问器里 —— 那正是本件与 dev-core 的差别。
 *
 * ## 它测什么, 与生产代码的边界在哪
 *   字符设备子分类与适配层(自编号 `TC-CDEV-*`): 绝大多数判据只经 `br_cdev.h`/`br_dev.h`
 *   的公开 API(注册前置校验 / 取回 / flash 子型)。**例外是两条与"内部机制本身"绑定的
 *   判据**: TC-CDEV-004/005 要按**指针同一性**认出"哪些条目是本件填的 open_file 钩子",
 *   TC-CDEV-006 要逐槽位查看**适配层那张表**(lseek/fsync/poll_attach 必须为空)。
 *   这两样用公开 API 表达不出来 ⇒ 经 `src/cdev_internal.h` 的两个访问器读; 它们一律
 *   **转调生产实现**(不复制逻辑), 理由见该头文件。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[CDEVCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[CDEVCONF] FAIL`)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>

#include <br/dev/br_cdev.h>
#include <br/dev/br_dev.h>

#include "cdev_internal.h"

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int cdev_core_selftest(void);

/* ==================================================================== 一致性用例 */

/*
 * 用例专用的一对最小 ops(只为把"名字非法"/"勾选必填槽位"这两类**纯前置校验**测出来)。
 * 它们**不被注册**(用例断言注册表条数不变), 所以不会往 /dev 里扔东西。
 */
static int conf_open(void *dev_priv, br_u32 flags, void **sess)
{
    (void)dev_priv; (void)flags;
    if (sess != BR_NULL) {
        *sess = BR_NULL;
    }
    return BR_OK;
}

static int conf_close(void *sess)
{
    (void)sess;
    return BR_OK;
}

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[CDEVCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[CDEVCONF] FAIL %s %s", tag, what);
    }
}

int cdev_core_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[CDEVCONF] cdev-core conformance (注册前置校验 / 会话适配 / flash 子型)");

    br_cdev_ops_t good = { 0 };
    good.open  = conf_open;
    good.close = conf_close;

    /* ---- TC-CDEV-001: 名字非法 ⇒ -EINVAL, 且**不污染**注册表 ----
     * 为什么只用"非法输入": dev-core 没有注销 API(v1 无热插拔)⇒ 一旦登记就永远出现在
     * /dev 里。正面路径由真实驱动覆盖(io/uart-pl011 在 CORE init 里登记 uart0)。 */
    {
        const br_u32 before = br_dev_count();
        const int r_upper = br_cdev_register("Cdev0", &good, BR_NULL);   /* 大写开头 */
        const int r_slash = br_cdev_register("c/dev", &good, BR_NULL);   /* 含斜杠 */
        const int r_empty = br_cdev_register("", &good, BR_NULL);        /* 空名 */
        const br_bool ok = (r_upper == BR_ERR(BR_EINVAL)) &&
                           (r_slash == BR_ERR(BR_EINVAL)) &&
                           (r_empty == BR_ERR(BR_EINVAL)) &&
                           (br_dev_count() == before);
        pc(ok, "TC-CDEV-001", "名字非法 ⇒ -EINVAL 且注册表条数不变");
    }

    /* ---- TC-CDEV-002: 必填槽位(open/close)缺失 ⇒ -EINVAL ---- */
    {
        br_cdev_ops_t only_open = { 0 };
        only_open.open = conf_open;                    /* 缺 close */
        br_cdev_ops_t only_close = { 0 };
        only_close.close = conf_close;                 /* 缺 open */

        const int r1 = br_cdev_register("cdevx", &only_open,  BR_NULL);
        const int r2 = br_cdev_register("cdevx", &only_close, BR_NULL);
        const int r3 = br_cdev_register("cdevx", BR_NULL,     BR_NULL);
        const br_bool ok = (r1 == BR_ERR(BR_EINVAL)) &&
                           (r2 == BR_ERR(BR_EINVAL)) &&
                           (r3 == BR_ERR(BR_EINVAL));
        pc(ok, "TC-CDEV-002", "ops 缺 open/close 或 ops 为空 ⇒ -EINVAL");
    }

    /* ---- TC-CDEV-003: 未注册的名字 ⇒ BR_NULL, 且不改写出参 ---- */
    {
        void *priv = (void *)(br_uintptr_t)1u;
        const br_bool ok = (br_cdev_get("nosuchcdev", &priv) == BR_NULL) &&
                           (br_cdev_get(BR_NULL, BR_NULL) == BR_NULL) &&
                           (priv == (void *)(br_uintptr_t)1u);
        pc(ok, "TC-CDEV-003", "未注册/空名字 ⇒ BR_NULL 且不动出参");
    }

    /* ---- TC-CDEV-004: 已注册 cdev 的往返(get 拿到的 ops == 条目登记的 ops) ---- */
    {
        /* 本件填在 cdev 条目上的 open_file 钩子 —— 按**指针同一性**认出"自家条目" */
        const cdev_open_file_hook_t own_hook = cdev_internal_open_file_hook();
        br_bool ok = BR_TRUE;
        br_u32  checked = 0u;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            const char *n = br_dev_name_at(i);
            if (e == BR_NULL || n == BR_NULL) {
                continue;
            }
            if (e->class_id != (br_u16)BR_CLASS_CDEV || e->open_file != own_hook) {
                continue;   /* flash 子型/别的子分类不属本用例 */
            }
            void *priv = BR_NULL;
            const br_cdev_ops_t *got = br_cdev_get(n, &priv);
            ok = ok && (got != BR_NULL) && (got == (const br_cdev_ops_t *)e->class_priv);
            checked++;
        }
        if (checked == 0u) {
            br_log_info("[CDEVCONF] note: 组合里没有 cdev 设备 ⇒ TC-CDEV-004 只走了'不适用'分支");
        }
        pc(ok, "TC-CDEV-004", "br_cdev_get(name) == 条目登记的 ops(逐个 cdev 往返)");
    }

    /* ---- TC-CDEV-005: 重名 ⇒ -EEXIST(用表里已有的真实设备名) ---- */
    {
        const cdev_open_file_hook_t own_hook = cdev_internal_open_file_hook();
        const char *first = BR_NULL;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_CDEV &&
                e->open_file == own_hook) {
                first = br_dev_name_at(i);
                break;
            }
        }
        br_bool ok = BR_TRUE;
        if (first != BR_NULL) {
            void *priv = BR_NULL;
            const br_cdev_ops_t *ops = br_cdev_get(first, &priv);
            ok = (ops != BR_NULL) && (br_cdev_register(first, ops, priv) == BR_ERR(BR_EEXIST));
        } else {
            br_log_info("[CDEVCONF] note: 没有 cdev 设备 ⇒ TC-CDEV-005 只走了'不适用'分支");
        }
        pc(ok, "TC-CDEV-005", "重名 ⇒ -EEXIST(唯一性由 dev-core 执法, 本件透传)");
    }

    /* ---- TC-CDEV-006: 适配层的槽位形状(lseek/fsync/poll_attach 恒空 ⇒ -ENOTSUP 的机制) ---- */
    {
        const br_file_ops_t *fops = cdev_internal_file_ops();
        const br_bool ok = (fops->open != BR_NULL) &&
                           (fops->read != BR_NULL) &&
                           (fops->write != BR_NULL) &&
                           (fops->ioctl != BR_NULL) &&
                           (fops->poll != BR_NULL) &&
                           (fops->close != BR_NULL) &&
                           (fops->lseek == BR_NULL) &&
                           (fops->fsync == BR_NULL) &&
                           (fops->poll_attach == BR_NULL);
        pc(ok, "TC-CDEV-006", "适配层转发槽位齐备; lseek/fsync/poll_attach 留空(= -ENOTSUP)");
    }

    /* ---- TC-CDEV-007: flash 子型的前置校验与取回 ---- */
    {
        br_flash_ops_t fops = { 0 };
        br_flash_geom_t geom = { 0 };

        const int r_null     = br_flash_register("nor0", BR_NULL, &geom, BR_NULL);
        const int r_zerogeom = br_flash_register("nor0", &fops, &geom, BR_NULL);   /* 几何全 0 */
        geom.read_size = 1u; geom.prog_size = 1u;
        geom.block_size = 4096u; geom.block_count = 64u;
        const int r_noacts = br_flash_register("nor0", &fops, &geom, BR_NULL);     /* 缺 read/program/erase */

        br_flash_geom_t got;
        const int r_geom_missing = br_flash_geom_get("nor0", &got);
        const int r_geom_null    = br_flash_geom_get("nor0", BR_NULL);

        const br_bool ok = (r_null == BR_ERR(BR_EINVAL)) &&
                           (r_zerogeom == BR_ERR(BR_EINVAL)) &&
                           (r_noacts == BR_ERR(BR_EINVAL)) &&
                           (r_geom_missing == BR_ERR(BR_ENODEV)) &&
                           (r_geom_null == BR_ERR(BR_EINVAL)) &&
                           (br_flash_get("nor0", BR_NULL) == BR_NULL);
        pc(ok, "TC-CDEV-007", "flash: 几何自洽/必填动作在注册期校验; 未注册取回 ⇒ BR_NULL/-ENODEV");
    }

    /* ---- TC-CDEV-008: flash 条目不提供文件面(open_file == BR_NULL ⇒ /dev 节点打不开) ----
     * 这条是"v1 的 raw flash 无文件面"的**机械判据**(ADR-0009 §3 裁定 6): 若哪天有人
     * 顺手给 flash 接上 open_file, 它会红 —— 那时该改的是这张判定与文档, 不是偷偷放行。 */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            const char *n = br_dev_name_at(i);
            if (e != BR_NULL && br_flash_get(n, BR_NULL) != BR_NULL) {
                ok = ok && (e->open_file == BR_NULL);
            }
        }
        pc(ok, "TC-CDEV-008", "flash 条目 open_file == BR_NULL(v1 raw flash 无文件面)");
    }

    br_log_info("[CDEVCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}

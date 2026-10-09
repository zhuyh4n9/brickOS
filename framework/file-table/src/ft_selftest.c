/*
 * brickOS prototype v0.2.0 — 文件表的一致性套件(framework/file-table/src/ft_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)、
 *                 `docs/decisions/0012-file-table-and-errno.md`(本件与 errno 对齐)。
 *
 * ## 为什么本套件一行都不需要 QEMU
 *   文件表是**纯算法**: 它从头到尾**不解引用** `br_file_t`(契约头明写"只持指针与比较")。
 *   于是用例可以拿"几个互不相同的假指针"当句柄, 把表满 / dup2 顶替 / 最后一个引用
 *   这些只有真实负载才碰得到的边界**全部**跑到 —— 而且跑在宿主上只要几毫秒
 *   (`tests/host/ft_test.c` 是同一条腿的宿主版, 判据是退出码)。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[FTCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 plugin-test: require 摘要全绿,
 *   forbid `[FTCONF] FAIL`, require_tags 逐条点名 TC-FT-*)。
 *
 * WORKAROUND(br-wa-test-001): `TC-FT-*` 是**自编号**(设计 `6-01` 没有文件表这一组);
 * 自述文字才是判据内容, 与设计用例表的逐条对齐见 WORKAROUNDS.md。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int file_table_selftest(void);

/* ==================================================================== 假句柄 */

/* 表不解引用句柄 ⇒ 用例只需要 N 个**互不相同**的指针值。
 * 用静态字节数组取址, 而不是"整数字面量转指针"(后者是实现定义行为, 且会被 -Werror 拦)。
 * 16 个: 0..7 当"文件句柄", 9..15 当"**未被改写**"的哨兵(见 TC-FT-008 的 disp3)。 */
#define FAKE_N 16u
static br_u8 s_fake[FAKE_N][16];

static br_file_t *fake(br_u32 i)
{
    return (br_file_t *)(void *)&s_fake[i][0];
}

/* ==================================================================== 报告 */

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[FTCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[FTCONF] FAIL %s %s", tag, what);
    }
}

/* 把表填满(句柄循环复用 FAKE_N 个假指针 —— 表只按指针相等比较, 重复不影响"满")。 */
static void fill_all(void)
{
    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        (void)br_ft_alloc(fake(i % FAKE_N), 0u);
    }
}

/* ==================================================================== 用例 */

int file_table_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[FTCONF] file-table conformance (fd 表 / dup / 引用判定 / errno)");

    /* ---- TC-FT-001: 空表与最小号分配 ---- */
    {
        (void)br_ft_reset();
        const br_u32 n0 = br_ft_count();
        const int    f0 = br_ft_alloc(fake(0), 0x0001u);
        const br_u32 n1 = br_ft_count();

        pc((n0 == 0u) && (f0 == 0) && (n1 == 1u) &&
           (br_ft_get(0) == fake(0)) && (br_ft_fd_at(0u) == 0) && (br_ft_fd_at(1u) == BR_FT_NONE),
           "TC-FT-001", "空表 ⇒ 首个 fd = 0; count/fd_at 与在场集合一致");
    }

    /* ---- TC-FT-002: **最小可用** fd(POSIX 的核心约定), 不是"递增计数器" ---- */
    {
        (void)br_ft_reset();
        const int a = br_ft_alloc(fake(0), 0u);
        const int b = br_ft_alloc(fake(1), 0u);
        const int c = br_ft_alloc(fake(2), 0u);
        (void)br_ft_release(b, BR_NULL);
        const int d = br_ft_alloc(fake(3), 0u);

        pc((a == 0) && (b == 1) && (c == 2) && (d == 1) && (br_ft_get(1) == fake(3)),
           "TC-FT-002", "释放 1 后再分配回到**最小空位** 1(而非 3)");
    }

    /* ---- TC-FT-003: 表满 ⇒ -EMFILE; 空句柄 ⇒ -EINVAL; 都**不留副作用** ---- */
    {
        (void)br_ft_reset();
        fill_all();
        const int over = br_ft_alloc(fake(0), 0u);
        const int nul  = br_ft_alloc(BR_NULL, 0u);
        const br_bool intact = (br_ft_count() == BR_FT_MAX) && (br_ft_get(0) == fake(0));

        pc((over == BR_ERR(BR_EMFILE)) && (nul == BR_ERR(BR_EINVAL)) && (intact == BR_TRUE),
           "TC-FT-003", "表满 ⇒ -EMFILE; 空句柄 ⇒ -EINVAL; 已有 fd 不受影响");
    }

    /* ---- TC-FT-004: 非法 fd 一律 -EBADF(越界与空槽**同一个码**) ---- */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0u);
        br_u32 fl = 0u;
        const br_bool ok =
            (br_ft_alloc(fake(1), 0u) == 1) &&
            (br_ft_release(1, BR_NULL) == 0) &&                    /* 1 变成空槽 */
            (br_ft_get(-1) == BR_NULL) &&
            (br_ft_get((int)BR_FT_MAX) == BR_NULL) &&
            (br_ft_get(1) == BR_NULL) &&
            (br_ft_get_flags(-1, &fl) == BR_ERR(BR_EBADF)) &&
            (br_ft_get_flags(1, &fl) == BR_ERR(BR_EBADF)) &&       /* 空槽 */
            (br_ft_set_flags(1, 0u) == BR_ERR(BR_EBADF)) &&
            (br_ft_release(1, BR_NULL) == BR_ERR(BR_EBADF));
        pc(ok, "TC-FT-004", "越界 / 空槽 / 负数 ⇒ 一律 -EBADF, get ⇒ BR_NULL");
    }

    /* ---- TC-FT-005: flags 是**每 fd 的快照**(存与还, 表不解释) ---- */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0x0021u);
        br_u32 got = 0u;
        const int r_get  = br_ft_get_flags(0, &got);
        const int r_neg  = br_ft_get_flags(0, BR_NULL);
        const int r_set  = br_ft_set_flags(0, 0x0042u);
        br_u32 got2 = 0u;
        (void)br_ft_get_flags(0, &got2);

        pc((r_get == 0) && (got == 0x0021u) && (r_set == 0) && (got2 == 0x0042u) &&
           (r_neg == BR_ERR(BR_EINVAL)),
           "TC-FT-005", "get/set flags 往返; 空 out ⇒ -EINVAL");
    }

    /* ---- TC-FT-006: release 的"最后一个引用"判定 ---- */
    {
        /* (a) 独占 ⇒ 回报句柄; 空槽 ⇒ -EBADF 且**必把 out 清零** */
        (void)br_ft_reset();
        br_file_t *last = fake(7);                  /* 先塞非空值: 验证"必被改写" */
        (void)br_ft_alloc(fake(0), 0u);
        const int r1 = br_ft_release(0, &last);

        br_file_t *last2 = fake(7);
        const int bad = br_ft_release(0, &last2);   /* 同一个槽, 已经空了 */

        /* (b) 共享 ⇒ 不回报; 最后一个引用走了 ⇒ 回报 */
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(1), 0u);             /* fd 0 */
        const int d = br_ft_dup(0);                 /* fd 1 与 fd 0 共享 */
        br_file_t *last3 = fake(7);
        const int r2 = br_ft_release(0, &last3);    /* 仍有 fd 1 ⇒ NULL */
        br_file_t *last4 = BR_NULL;
        const int r3 = br_ft_release(d, &last4);    /* 最后一个 ⇒ 回报 */

        pc((r1 == 0) && (last == fake(0)) &&
           (bad == BR_ERR(BR_EBADF)) && (last2 == BR_NULL) &&
           (d == 1) && (r2 == 0) && (last3 == BR_NULL) &&
           (r3 == 0) && (last4 == fake(1)),
           "TC-FT-006", "独占 ⇒ 回报句柄; 共享 ⇒ 回报 NULL; 非法 ⇒ -EBADF + 清零");
    }

    /* ---- TC-FT-007: dup 共享同一 open file description ---- */
    {
        (void)br_ft_reset();
        br_u32 fl0 = 0u;
        br_u32 fl1 = 0u;
        (void)br_ft_alloc(fake(2), 0x0003u);
        const int d = br_ft_dup(0);
        (void)br_ft_get_flags(0, &fl0);
        (void)br_ft_get_flags(d, &fl1);
        const int dup_bad = br_ft_dup(7);
        const br_bool ok =
            (d == 1) && (br_ft_get(d) == fake(2)) && (fl0 == fl1) &&
            (br_ft_count() == 2u) && (dup_bad == BR_ERR(BR_EBADF));
        pc(ok, "TC-FT-007", "dup: 同句柄 + flags 快照一致; 非法 oldfd ⇒ -EBADF");
    }

    /* ---- TC-FT-008: dup2 的目标顶替与 displaced 回报 ----
     * 状态用注释里的 `fd0=A fd1=B …` 逐步跟踪, 免得断言与表状态对不上。 */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0u);      /* fd0=A */
        (void)br_ft_alloc(fake(1), 0u);      /* fd1=B */
        (void)br_ft_alloc(fake(2), 0u);      /* fd2=C */

        /* (a) 目标独占(C) ⇒ 回报被顶掉的句柄; 结果 fd0=A fd1=B fd2=A */
        br_file_t *disp1 = fake(9);
        const int r1 = br_ft_dup2(0, 2, &disp1);

        /* (b) 目标句柄(A)仍被 fd0 引用 ⇒ **不**回报; 结果 fd0=A fd1=B fd2=B */
        br_file_t *disp2 = fake(9);
        const int r2 = br_ft_dup2(1, 2, &disp2);

        /* (c) dup2(f,f) 原地不动: 返回 f, 表不变; `displaced` 按契约被写成 BR_NULL
         *     (它在**任何**返回路径上都会被写 —— 见 br_ft.h) */
        br_file_t *disp3 = fake(9);
        const int r3 = br_ft_dup2(1, 1, &disp3);

        /* (d) 非法 oldfd(空槽)/ 越界 newfd ⇒ -EBADF, 表与状态都不变 */
        br_file_t *disp4 = fake(9);
        const int r4 = br_ft_dup2(3, 0, &disp4);
        const int r5 = br_ft_dup2(0, (int)BR_FT_MAX, &disp4);
        const int r6 = br_ft_dup2(0, -1, &disp4);

        const br_bool ok =
            (r1 == 2) && (disp1 == fake(2)) &&
            (r2 == 2) && (disp2 == BR_NULL) &&
            (r3 == 1) && (disp3 == BR_NULL) &&
            (r4 == BR_ERR(BR_EBADF)) && (r5 == BR_ERR(BR_EBADF)) &&
            (r6 == BR_ERR(BR_EBADF)) && (disp4 == BR_NULL) &&
            (br_ft_get(0) == fake(0)) && (br_ft_get(1) == fake(1)) &&
            (br_ft_get(2) == fake(1)) && (br_ft_count() == 3u);

        pc(ok, "TC-FT-008",
           "dup2: 目标顶替回报 displaced; 共享目标 ⇒ NULL; dup2(f,f) 原地; 非法 ⇒ -EBADF");
    }

    /* ---- TC-FT-009: release_all 按指针去重 + 装不下则表不变 ---- */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0u);      /* fd 0 */
        (void)br_ft_alloc(fake(1), 0u);      /* fd 1 */
        (void)br_ft_dup(0);                  /* fd 2 与 fd 0 共享 */
        (void)br_ft_dup(1);                  /* fd 3 与 fd 1 共享 */

        br_file_t *small[1];
        const int too_small = br_ft_release_all(small, 1u);       /* 需要 2 条 ⇒ -ENOSPC */
        const br_bool intact = (br_ft_count() == 4u);

        br_file_t *got[4] = { BR_NULL, BR_NULL, BR_NULL, BR_NULL };
        const int n = br_ft_release_all(got, 4u);
        const br_bool dedup =
            (n == 2) &&
            (((got[0] == fake(0)) && (got[1] == fake(1))) ||
             ((got[0] == fake(1)) && (got[1] == fake(0))));

        pc((too_small == BR_ERR(BR_ENOSPC)) && (intact == BR_TRUE) &&
           (dedup == BR_TRUE) && (br_ft_count() == 0u) && (br_ft_get(0) == BR_NULL),
           "TC-FT-009", "release_all: 同句柄只报一条; cap 不足 ⇒ -ENOSPC 且表不变");
    }

    /* ---- TC-FT-010: errno 取值(与 Linux 内核对齐; 宿主 ft-test 拿 <errno.h> 逐码对拍) ---- */
    {
        const br_bool ok =
            (BR_EINVAL == 22) && (BR_EBADF == 9) && (BR_EMFILE == 24) && (BR_ENFILE == 23) &&
            (BR_ENOENT == 2) && (BR_ENOTEMPTY == 39) && (BR_ENOTSUP == 95) &&
            (BR_ETIMEDOUT == 110) && (BR_EACCES == 13) && (BR_ESPIPE == 29) &&
            (BR_EOPNOTSUPP == BR_ENOTSUP) && (BR_EWOULDBLOCK == BR_EAGAIN);
        pc(ok, "TC-FT-010", "errno 编号 = Linux(内核 errno-base/errno.h); 别名同值");
    }

    /* ---- TC-FT-011: 边界与观测面 ---- */
    {
        (void)br_ft_reset();
        int last = -2;
        for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
            last = br_ft_alloc(fake(i % FAKE_N), 0u);
        }
        /* 升序观测: fd_at(k) == k(因为全满) */
        br_bool order = BR_TRUE;
        for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
            if (br_ft_fd_at(i) != (int)i) {
                order = BR_FALSE;
                break;
            }
        }
        const br_bool ok =
            (last == (int)(BR_FT_MAX - 1u)) &&
            (br_ft_fd_at(BR_FT_MAX) == BR_FT_NONE) &&
            (order == BR_TRUE) &&
            (br_ft_reset() == BR_FT_MAX);            /* 满表被清 ⇒ 丢弃数 = 槽数 */
        pc(ok, "TC-FT-011", "最后一个合法 fd = BR_FT_MAX-1; fd_at 升序; reset 回报丢弃数");
    }

    br_log_info("[FTCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}

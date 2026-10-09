/*
 * tests/host/ft_test.c — 文件表(framework/file-table)的**宿主**门禁
 *
 * 为什么文件表值得一条宿主用例:
 *   它是**纯算法**(契约头明写"表从头到尾不解引用 br_file_t"), 于是"最小可用 fd /
 *   表满 / dup2 顶替 / 最后一个引用"这些只有真实负载才碰得到的边界可以在这里全跑到,
 *   而不必开 QEMU —— 与 mem-test / sync-test / sched-test 的分工同型(算法性质 → 宿主,
 *   端到端 → 目标)。
 *
 * 本用例干三件事:
 *   ① **errno 与内核逐码对拍** —— `core/include/br/core/br_error.h` 的每一个 `BR_E*`
 *      都拿宿主 `<errno.h>` 的同名宏比一次。这是"errno 值与 Linux kernel 保持一致"这条
 *      要求的**可执行判据**: 抄错一个数字, 这里立刻红, 而不是等到用户态 errno 对不上。
 *      (宿主是 x86-64, 目标是 aarch64; 两者共用内核的 `asm-generic` 编号 ⇒ 这条对拍
 *      对目标同样成立。)
 *   ② **重跑 in-image 自检套件** `file_table_selftest()`(TC-FT-001..011, 同一份源码、
 *      同一批 tag)⇒ 目标与宿主两条腿的判据**同源**, 少一份会漂的副本。
 *   ③ 宿主独有的边界(表满后的越界访问 / 空参健壮性)。
 *
 * 判据 = 退出码(0 = 全绿), 与其它 [[hosttest]] 一致。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

/* in-image 自检套件(定义在 `framework/file-table/src/ft_selftest.c`; 同一个编译单元集
 * 里一起编, 所以宿主跑的是**同一份**用例, 不是改写过的副本)。 */
int file_table_selftest(void);

/* ==================================================================== 宿主替身 */

/* 为什么替身只这几行: `file_table.c` 对外的依赖只有"日志"与"关中断"两条。
 * 宿主上没有中断 ⇒ 关中断退化成恒等; 日志落到 vprintf(用例允许 libc)。 */
static br_log_level_t s_level = BR_LOG_INFO;

void br_log_write(br_log_level_t level, const char *fmt, ...)
{
    if (fmt == BR_NULL) {
        return;
    }
    if ((unsigned)level < (unsigned)s_level) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
    (void)putchar('\n');
}

br_irq_state_t br_irq_lock(void)
{
    return 0u;                      /* 宿主单线程: 临界区是恒等映射 */
}

void br_irq_unlock(br_irq_state_t st)
{
    (void)st;
}

/* ==================================================================== 报告 */

static int s_pass;
static int s_fail;

static void rep(int cond, const char *tag, const char *desc)
{
    if (cond) {
        s_pass++;
        printf("[FTTEST] PASS %s %s\n", tag, desc);
    } else {
        s_fail++;
        printf("[FTTEST] FAIL %s %s\n", tag, desc);
    }
}

/* ==================================================================== 假句柄 */

#define FAKE_N 16u
static br_u8 s_fake[FAKE_N][16];

static br_file_t *fake(br_u32 i)
{
    return (br_file_t *)(void *)&s_fake[i][0];
}

static void fill_all(void)
{
    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        (void)br_ft_alloc(fake(i % FAKE_N), 0u);
    }
}

/* ==================================================================== ① errno 对拍 */

/* 一张"本仓名 ↔ 内核名"的表; 每个 `BR_E*` 都必须在这里出现一次(漏了就是漏了对拍)。 */
#define BR_ERRNO_PAIRS(X)      \
    X(BR_EPERM, EPERM);         \
    X(BR_ENOENT, ENOENT);       \
    X(BR_ESRCH, ESRCH);         \
    X(BR_EINTR, EINTR);         \
    X(BR_EIO, EIO);             \
    X(BR_ENXIO, ENXIO);         \
    X(BR_E2BIG, E2BIG);         \
    X(BR_ENOEXEC, ENOEXEC);     \
    X(BR_EBADF, EBADF);         \
    X(BR_ECHILD, ECHILD);       \
    X(BR_EAGAIN, EAGAIN);       \
    X(BR_ENOMEM, ENOMEM);       \
    X(BR_EACCES, EACCES);       \
    X(BR_EFAULT, EFAULT);       \
    X(BR_ENOTBLK, ENOTBLK);     \
    X(BR_EBUSY, EBUSY);         \
    X(BR_EEXIST, EEXIST);       \
    X(BR_EXDEV, EXDEV);         \
    X(BR_ENODEV, ENODEV);       \
    X(BR_ENOTDIR, ENOTDIR);     \
    X(BR_EISDIR, EISDIR);       \
    X(BR_EINVAL, EINVAL);       \
    X(BR_ENFILE, ENFILE);       \
    X(BR_EMFILE, EMFILE);       \
    X(BR_ENOTTY, ENOTTY);       \
    X(BR_ETXTBSY, ETXTBSY);     \
    X(BR_EFBIG, EFBIG);         \
    X(BR_ENOSPC, ENOSPC);       \
    X(BR_ESPIPE, ESPIPE);       \
    X(BR_EROFS, EROFS);         \
    X(BR_EMLINK, EMLINK);       \
    X(BR_EPIPE, EPIPE);         \
    X(BR_EDOM, EDOM);           \
    X(BR_ERANGE, ERANGE);       \
    X(BR_EDEADLK, EDEADLK);     \
    X(BR_ENAMETOOLONG, ENAMETOOLONG); \
    X(BR_ENOLCK, ENOLCK);       \
    X(BR_ENOSYS, ENOSYS);       \
    X(BR_ENOTEMPTY, ENOTEMPTY); \
    X(BR_ELOOP, ELOOP);         \
    X(BR_EOVERFLOW, EOVERFLOW); \
    X(BR_ENOTSUP, ENOTSUP);     \
    X(BR_ETIMEDOUT, ETIMEDOUT); \
    X(BR_EDQUOT, EDQUOT);       \
    X(BR_ECANCELED, ECANCELED); \
    X(BR_EOWNERDEAD, EOWNERDEAD); \
    X(BR_ENOTRECOVERABLE, ENOTRECOVERABLE); \
    X(BR_EOPNOTSUPP, EOPNOTSUPP); \
    X(BR_EWOULDBLOCK, EWOULDBLOCK)

static int s_errno_bad;
static int s_errno_checked;

#define BR_ERRNO_CMP(br, sys)                                        \
    do {                                                             \
        s_errno_checked++;                                           \
        if ((int)(br) != (int)(sys)) {                               \
            s_errno_bad++;                                           \
            printf("[FTTEST] MISMATCH %s = %d, 内核 %s = %d\n",       \
                   #br, (int)(br), #sys, (int)(sys));                \
        }                                                            \
    } while (0)

/* ==================================================================== ③ 宿主独有边界 */

static void host_only_cases(void)
{
    /* ---- TC-FT-H01: 表满之后**越界与空槽仍然只是 -EBADF**, 不越界读内存 ---- */
    {
        (void)br_ft_reset();
        fill_all();
        const int over = br_ft_alloc(fake(0), 0u);
        br_u32 fl = 0u;
        const int neg  = br_ft_get_flags(-1, &fl);
        const int high = br_ft_get_flags((int)BR_FT_MAX, &fl);
        const int far  = br_ft_get_flags(0x7fffffff, &fl);
        rep((over == BR_ERR(BR_EMFILE)) && (neg == BR_ERR(BR_EBADF)) &&
            (high == BR_ERR(BR_EBADF)) && (far == BR_ERR(BR_EBADF)) &&
            (br_ft_get((int)BR_FT_MAX) == BR_NULL),
            "TC-FT-H01", "表满 ⇒ -EMFILE; 越界/极端 fd(含 INT_MAX)一律 -EBADF");
    }

    /* ---- TC-FT-H02: dup 表满时**不动 oldfd**(POSIX: 失败不留副作用) ---- */
    {
        (void)br_ft_reset();
        fill_all();
        const int d = br_ft_dup(0);
        rep((d == BR_ERR(BR_EMFILE)) && (br_ft_count() == BR_FT_MAX) &&
            (br_ft_get(0) == fake(0)),
            "TC-FT-H02", "dup 表满 ⇒ -EMFILE 且已有 fd 不被改动");
    }

    /* ---- TC-FT-H03: 空参健壮性(NULL out / cap=0) ---- */
    {
        (void)br_ft_reset();
        const int r_null_cap = br_ft_release_all(BR_NULL, 1u);     /* out 空而 cap 非 0 */
        const int r_empty    = br_ft_release_all(BR_NULL, 0u);     /* 空表 + cap 0: 什么都不报 */
        (void)br_ft_alloc(fake(0), 0u);
        br_file_t *one = BR_NULL;
        const int r_cap0 = br_ft_release_all(&one, 0u);            /* 有 1 条要报却 cap=0 */
        rep((r_null_cap == BR_ERR(BR_EINVAL)) && (r_empty == 0) &&
            (r_cap0 == BR_ERR(BR_ENOSPC)) && (br_ft_count() == 1u),
            "TC-FT-H03", "release_all: out=NULL+cap!=0 ⇒ -EINVAL; cap 不足 ⇒ -ENOSPC 且表不变");
    }

    /* ---- TC-FT-H04: 计数不变量(在场槽位个数 = count), 反复操作后仍成立 ---- */
    {
        (void)br_ft_reset();
        br_u32 expect = 0u;
        for (br_u32 round = 0u; round < 4u; round++) {
            (void)br_ft_alloc(fake(round % FAKE_N), 0u);
            expect++;
            (void)br_ft_dup(0);
            expect++;
            if (round == 0u) {
                (void)br_ft_release(1, BR_NULL);
                expect--;
            }
        }
        /* 真实在场数 = 槽位非空数(fd_at 遍历一遍自己数) */
        br_u32 counted = 0u;
        while (br_ft_fd_at(counted) != BR_FT_NONE) {
            counted++;
        }
        rep((br_ft_count() == expect) && (counted == expect),
            "TC-FT-H04", "反复 alloc/dup/release 后 count == 非空槽位数 == fd_at 遍历数");
    }
}

/* ==================================================================== main */

int main(void)
{
    printf("=== ft-test: framework/file-table(宿主) ===\n");

    /* ① errno 与内核逐码对拍 */
    BR_ERRNO_PAIRS(BR_ERRNO_CMP);
    rep(s_errno_bad == 0, "TC-FT-H05", "errno 编号与 Linux <errno.h> 逐码一致");
    printf("[FTTEST] errno 对拍: %d 码, 不一致 %d 个\n", s_errno_checked, s_errno_bad);

    /* ② 重跑 in-image 套件(同一份源码) */
    const int suite_fails = file_table_selftest();
    rep(suite_fails == 0, "TC-FT-H06", "in-image 套件 TC-FT-001..011 在宿主上全绿");
    if (suite_fails != 0) {
        printf("[FTTEST] in-image 套件失败项: %d\n", suite_fails);
    }

    /* ③ 宿主独有边界 */
    host_only_cases();

    printf("[FTTEST] SUMMARY pass=%d fail=%d total=%d\n", s_pass, s_fail, s_pass + s_fail);
    return s_fail;
}

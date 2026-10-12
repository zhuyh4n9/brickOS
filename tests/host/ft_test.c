/*
 * tests/host/ft_test.c — **host** gate for the file table (framework/file-table)
 *
 * Why the file table deserves a host case:
 *   it is a **pure algorithm** (the contract header states "the table never dereferences
 *   br_file_t"), so the boundaries only real workload would hit — smallest usable fd / table
 *   full / dup2 replacement / last reference — can all be exercised here without booting QEMU;
 *   same split as mem-test / sync-test / sched-test (algorithmic properties → host,
 *   end-to-end → target).
 *
 * This case does three things:
 *   ① **errno compared code by code with the kernel** — every `BR_E*` in
 *      `core/include/br/core/br_error.h` is compared against the host `<errno.h>` macro of the
 *      same name. This is the **executable criterion** for "errno values stay consistent with
 *      the Linux kernel": copy one number wrong and this goes red at once, instead of waiting
 *      until userspace errno mismatches. (The host is x86-64 and the target aarch64; both
 *      share the kernel's `asm-generic` numbering ⇒ the comparison holds on the target too.)
 *   ② **re-run the in-image self-test suite** `file_table_selftest()` (TC-FT-001..011, same
 *      source, same tag set) ⇒ the target and host legs share one criterion source, one less
 *      copy that can drift.
 *   ③ host-only boundaries (out-of-range access after table full / null-argument robustness).
 *
 * Criterion = exit code (0 = all green), same as other [[hosttest]].
 */
#include <errno.h>          /* ★ must come **before our own** errno header: see the capture below */

/*
 * ★ **Ordering requirement** for the errno comparison (ADR-0027): this repo's errno names are
 *   now the POSIX originals (`EPERM`/`EIO`/…, no `BR_` prefix) ⇒ if our header were included
 *   first, `X(EPERM, EPERM)` would compare "itself with itself" (always true, silently losing
 *   coverage). So the host <errno.h> values are captured into the array **first**, and only
 *   then does our header take part in the comparison — comparing "our names" vs "host values".
 */
#define BR_ERRNO_NAMES(X)     \
    X(EPERM) \
    X(ENOENT) \
    X(ESRCH) \
    X(EINTR) \
    X(EIO) \
    X(ENXIO) \
    X(E2BIG) \
    X(ENOEXEC) \
    X(EBADF) \
    X(ECHILD) \
    X(EAGAIN) \
    X(ENOMEM) \
    X(EACCES) \
    X(EFAULT) \
    X(ENOTBLK) \
    X(EBUSY) \
    X(EEXIST) \
    X(EXDEV) \
    X(ENODEV) \
    X(ENOTDIR) \
    X(EISDIR) \
    X(EINVAL) \
    X(ENFILE) \
    X(EMFILE) \
    X(ENOTTY) \
    X(ETXTBSY) \
    X(EFBIG) \
    X(ENOSPC) \
    X(ESPIPE) \
    X(EROFS) \
    X(EMLINK) \
    X(EPIPE) \
    X(EDOM) \
    X(ERANGE) \
    X(EDEADLK) \
    X(ENAMETOOLONG) \
    X(ENOLCK) \
    X(ENOSYS) \
    X(ENOTEMPTY) \
    X(ELOOP) \
    X(EOVERFLOW) \
    X(ENOTSUP) \
    X(ETIMEDOUT) \
    X(EUCLEAN) \
    X(EDQUOT) \
    X(ECANCELED) \
    X(EOWNERDEAD) \
    X(ENOTRECOVERABLE) \
    X(EOPNOTSUPP) \
    X(EWOULDBLOCK)

static const int s_host_errno[] = {
#define BR_HOST_CAP(n) (int)(n),
    BR_ERRNO_NAMES(BR_HOST_CAP)
#undef BR_HOST_CAP
};

/* Undefine the host macros of the same names: the values were captured above, and now our own
 * definitions must take effect. */
#undef EPERM
#undef ENOENT
#undef ESRCH
#undef EINTR
#undef EIO
#undef ENXIO
#undef E2BIG
#undef ENOEXEC
#undef EBADF
#undef ECHILD
#undef EAGAIN
#undef ENOMEM
#undef EACCES
#undef EFAULT
#undef ENOTBLK
#undef EBUSY
#undef EEXIST
#undef EXDEV
#undef ENODEV
#undef ENOTDIR
#undef EISDIR
#undef EINVAL
#undef ENFILE
#undef EMFILE
#undef ENOTTY
#undef ETXTBSY
#undef EFBIG
#undef ENOSPC
#undef ESPIPE
#undef EROFS
#undef EMLINK
#undef EPIPE
#undef EDOM
#undef ERANGE
#undef EDEADLK
#undef ENAMETOOLONG
#undef ENOLCK
#undef ENOSYS
#undef ENOTEMPTY
#undef ELOOP
#undef EOVERFLOW
#undef ENOTSUP
#undef ETIMEDOUT
#undef EUCLEAN
#undef EDQUOT
#undef ECANCELED
#undef EOWNERDEAD
#undef ENOTRECOVERABLE
#undef EOPNOTSUPP
#undef EWOULDBLOCK


#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>

#include <stdarg.h>
#include <stdio.h>

/* in-image self-test suite (defined in `framework/file-table/src/ft_selftest.c`; compiled
 * together in the same translation-unit set, so the host runs the **same** cases, not a
 * rewritten copy). */
int file_table_selftest(void);

/* ==================================================================== host stand-ins */

/* Why the stand-ins are only these lines: `file_table.c` depends externally on just two
 * things, "log" and "disable interrupts". The host has no interrupts ⇒ disabling degrades to
 * identity; logging goes to vprintf (cases may use libc). */
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
    return 0u;                      /* host is single-threaded: the critical section is the identity */
}

void br_irq_unlock(br_irq_state_t st)
{
    (void)st;
}

/* ==================================================================== report */

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

/* ==================================================================== fake handles */

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

/* ==================================================================== ① errno comparison */

/* A "repo name ↔ kernel name" table; every `BR_E*` must appear here once (a miss is lost
 * coverage). */
static int s_errno_bad;
static int s_errno_checked;

static int s_errno_i;
#define BR_ERRNO_CMP(n)                                              \
    do {                                                             \
        s_errno_checked++;                                           \
        if ((int)(n) != s_host_errno[s_errno_i]) {                   \
            s_errno_bad++;                                           \
            printf("[FTTEST] MISMATCH %s = %d, host = %d\n",          \
                   #n, (int)(n), s_host_errno[s_errno_i]);           \
        }                                                            \
        s_errno_i++;                                                 \
    } while (0);

/* ==================================================================== ③ host-only boundaries */

static void host_only_cases(void)
{
    /* ---- TC-FT-H01: after table full, **out-of-range and empty slots are still just
     * -EBADF**, with no out-of-bounds memory read ---- */
    {
        (void)br_ft_reset();
        fill_all();
        const int over = br_ft_alloc(fake(0), 0u);
        br_u32 fl = 0u;
        const int neg  = br_ft_get_flags(-1, &fl);
        const int high = br_ft_get_flags((int)BR_FT_MAX, &fl);
        const int far  = br_ft_get_flags(0x7fffffff, &fl);
        rep((over == BR_ERR(EMFILE)) && (neg == BR_ERR(EBADF)) &&
            (high == BR_ERR(EBADF)) && (far == BR_ERR(EBADF)) &&
            (br_ft_get((int)BR_FT_MAX) == BR_NULL),
            "TC-FT-H01", "table full ⇒ -EMFILE; out-of-range/extreme fds (incl. INT_MAX) always -EBADF");
    }

    /* ---- TC-FT-H02: on dup table full, **oldfd is untouched** (POSIX: failure leaves no
     * side effect) ---- */
    {
        (void)br_ft_reset();
        fill_all();
        const int d = br_ft_dup(0);
        rep((d == BR_ERR(EMFILE)) && (br_ft_count() == BR_FT_MAX) &&
            (br_ft_get(0) == fake(0)),
            "TC-FT-H02", "dup table full ⇒ -EMFILE and existing fds unchanged");
    }

    /* ---- TC-FT-H03: null-argument robustness (NULL out / cap=0) ---- */
    {
        (void)br_ft_reset();
        const int r_null_cap = br_ft_release_all(BR_NULL, 1u);     /* out null, cap non-zero */
        const int r_empty    = br_ft_release_all(BR_NULL, 0u);     /* empty table + cap 0: report nothing */
        (void)br_ft_alloc(fake(0), 0u);
        br_file_t *one = BR_NULL;
        const int r_cap0 = br_ft_release_all(&one, 0u);            /* 1 entry to report but cap=0 */
        rep((r_null_cap == BR_ERR(EINVAL)) && (r_empty == 0) &&
            (r_cap0 == BR_ERR(ENOSPC)) && (br_ft_count() == 1u),
            "TC-FT-H03", "release_all: out=NULL+cap!=0 ⇒ -EINVAL; cap too small ⇒ -ENOSPC and table unchanged");
    }

    /* ---- TC-FT-H04: count invariant (number of live slots = count) holds after repeated
     * operations ---- */
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
        /* real live count = non-empty slots (count them by walking fd_at) */
        br_u32 counted = 0u;
        while (br_ft_fd_at(counted) != BR_FT_NONE) {
            counted++;
        }
        rep((br_ft_count() == expect) && (counted == expect),
            "TC-FT-H04", "after repeated alloc/dup/release, count == non-empty slots == fd_at walk count");
    }
}

/* ==================================================================== main */

int main(void)
{
    printf("=== ft-test: framework/file-table (host) ===\n");

    /* ① errno compared code by code with the kernel */
    BR_ERRNO_NAMES(BR_ERRNO_CMP);
    rep(s_errno_bad == 0, "TC-FT-H05", "errno numbers match Linux <errno.h> code by code");
    printf("[FTTEST] errno comparison: %d codes, %d mismatches\n", s_errno_checked, s_errno_bad);

    /* ② re-run the in-image suite (same source) */
    const int suite_fails = file_table_selftest();
    rep(suite_fails == 0, "TC-FT-H06", "in-image suite TC-FT-001..011 all green on the host");
    if (suite_fails != 0) {
        printf("[FTTEST] in-image suite failures: %d\n", suite_fails);
    }

    /* ③ host-only boundaries */
    host_only_cases();

    printf("[FTTEST] SUMMARY pass=%d fail=%d total=%d\n", s_pass, s_fail, s_pass + s_fail);
    return s_fail;
}

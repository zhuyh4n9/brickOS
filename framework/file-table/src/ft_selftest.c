/*
 * File table conformance suite (framework/file-table). The table never dereferences br_file_t, so
 * cases use distinct fake pointers as handles and need no QEMU. WORKAROUND(br-wa-test-001): TC-FT-*
 * are self-assigned (there is no file-table group in the design); the self-describing text is the
 * criterion. Returns the number of failures; logs [FTCONF] PASS/FAIL.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>

/* Referenced by the generated plugin_desc.c .selftest hook. */
int file_table_selftest(void);

/* Distinct pointer values only: handles are never dereferenced. 16 in total -- 0..7 as "file
 * handles", 9..15 as "unoverwritten" sentinels (see TC-FT-008 disp3). */
#define FAKE_N 16u
static br_u8 s_fake[FAKE_N][16];

static br_file_t *fake(br_u32 i)
{
    return (br_file_t *)(void *)&s_fake[i][0];
}

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

/* Fill the table; handles cycle through the fake pointers (only pointer equality matters). */
static void fill_all(void)
{
    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        (void)br_ft_alloc(fake(i % FAKE_N), 0u);
    }
}

int file_table_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[FTCONF] file-table conformance (fd table / dup / ref counting / errno)");

    /* TC-FT-001: empty table and lowest-number allocation. */
    {
        (void)br_ft_reset();
        const br_u32 n0 = br_ft_count();
        const int    f0 = br_ft_alloc(fake(0), 0x0001u);
        const br_u32 n1 = br_ft_count();

        pc((n0 == 0u) && (f0 == 0) && (n1 == 1u) &&
           (br_ft_get(0) == fake(0)) && (br_ft_fd_at(0u) == 0) && (br_ft_fd_at(1u) == BR_FT_NONE),
           "TC-FT-001", "empty table ⇒ first fd = 0; count/fd_at match the present set");
    }

    /* TC-FT-002: lowest available fd, not an incrementing counter. */
    {
        (void)br_ft_reset();
        const int a = br_ft_alloc(fake(0), 0u);
        const int b = br_ft_alloc(fake(1), 0u);
        const int c = br_ft_alloc(fake(2), 0u);
        (void)br_ft_release(b, BR_NULL);
        const int d = br_ft_alloc(fake(3), 0u);

        pc((a == 0) && (b == 1) && (c == 2) && (d == 1) && (br_ft_get(1) == fake(3)),
           "TC-FT-002", "after releasing 1, allocation returns the **lowest free slot** 1 (not 3)");
    }

    /* TC-FT-003: table full => -EMFILE; null handle => -EINVAL; neither leaves side effects. */
    {
        (void)br_ft_reset();
        fill_all();
        const int over = br_ft_alloc(fake(0), 0u);
        const int nul  = br_ft_alloc(BR_NULL, 0u);
        const br_bool intact = (br_ft_count() == BR_FT_MAX) && (br_ft_get(0) == fake(0));

        pc((over == BR_ERR(EMFILE)) && (nul == BR_ERR(EINVAL)) && (intact == BR_TRUE),
           "TC-FT-003", "table full ⇒ -EMFILE; null handle ⇒ -EINVAL; existing fds unaffected");
    }

    /* TC-FT-004: any invalid fd is -EBADF (out of range and empty slot share one code). */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0u);
        br_u32 fl = 0u;
        const br_bool ok =
            (br_ft_alloc(fake(1), 0u) == 1) &&
            (br_ft_release(1, BR_NULL) == 0) &&                    /* 1 becomes an empty slot */
            (br_ft_get(-1) == BR_NULL) &&
            (br_ft_get((int)BR_FT_MAX) == BR_NULL) &&
            (br_ft_get(1) == BR_NULL) &&
            (br_ft_get_flags(-1, &fl) == BR_ERR(EBADF)) &&
            (br_ft_get_flags(1, &fl) == BR_ERR(EBADF)) &&       /* empty slot */
            (br_ft_set_flags(1, 0u) == BR_ERR(EBADF)) &&
            (br_ft_release(1, BR_NULL) == BR_ERR(EBADF));
        pc(ok, "TC-FT-004", "out of range / empty slot / negative ⇒ always -EBADF, get ⇒ BR_NULL");
    }

    /* TC-FT-005: flags are a per-fd snapshot (stored and returned, not interpreted). */
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
           (r_neg == BR_ERR(EINVAL)),
           "TC-FT-005", "get/set flags round-trip; null out ⇒ -EINVAL");
    }

    /* TC-FT-006: release's "last reference" decision. */
    {
        /* (a) exclusive => report the handle; empty slot => -EBADF and the out-param is zeroed. */
        (void)br_ft_reset();
        br_file_t *last = fake(7);                  /* seed a non-null value: verify "always overwritten" */
        (void)br_ft_alloc(fake(0), 0u);
        const int r1 = br_ft_release(0, &last);

        br_file_t *last2 = fake(7);
        const int bad = br_ft_release(0, &last2);   /* same slot, now empty */

        /* (b) shared => no report; last reference gone => report. */
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(1), 0u);             /* fd 0 */
        const int d = br_ft_dup(0);                 /* fd 1 shares with fd 0 */
        br_file_t *last3 = fake(7);
        const int r2 = br_ft_release(0, &last3);    /* fd 1 remains => NULL */
        br_file_t *last4 = BR_NULL;
        const int r3 = br_ft_release(d, &last4);    /* last one => report */

        pc((r1 == 0) && (last == fake(0)) &&
           (bad == BR_ERR(EBADF)) && (last2 == BR_NULL) &&
           (d == 1) && (r2 == 0) && (last3 == BR_NULL) &&
           (r3 == 0) && (last4 == fake(1)),
           "TC-FT-006", "exclusive ⇒ report handle; shared ⇒ report NULL; invalid ⇒ -EBADF + zeroed");
    }

    /* TC-FT-007: dup shares the same open file description. */
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
            (br_ft_count() == 2u) && (dup_bad == BR_ERR(EBADF));
        pc(ok, "TC-FT-007", "dup: same handle + matching flag snapshot; invalid oldfd ⇒ -EBADF");
    }

    /* TC-FT-008: dup2 target displacement and displaced reporting (fd0=A fd1=B fd2=C). */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0u);      /* fd0=A */
        (void)br_ft_alloc(fake(1), 0u);      /* fd1=B */
        (void)br_ft_alloc(fake(2), 0u);      /* fd2=C */

        /* (a) target is exclusive (C) => report the displaced handle; result fd0=A fd1=B fd2=A */
        br_file_t *disp1 = fake(9);
        const int r1 = br_ft_dup2(0, 2, &disp1);

        /* (b) target (A) still referenced by fd0 => not reported; result fd0=A fd1=B fd2=B */
        br_file_t *disp2 = fake(9);
        const int r2 = br_ft_dup2(1, 2, &disp2);

        /* (c) dup2(f,f) is a no-op (returns f, unchanged); displaced written BR_NULL always. */
        br_file_t *disp3 = fake(9);
        const int r3 = br_ft_dup2(1, 1, &disp3);

        /* (d) invalid oldfd (empty slot) / bad newfd => -EBADF, table/state unchanged */
        br_file_t *disp4 = fake(9);
        const int r4 = br_ft_dup2(3, 0, &disp4);
        const int r5 = br_ft_dup2(0, (int)BR_FT_MAX, &disp4);
        const int r6 = br_ft_dup2(0, -1, &disp4);

        const br_bool ok =
            (r1 == 2) && (disp1 == fake(2)) &&
            (r2 == 2) && (disp2 == BR_NULL) &&
            (r3 == 1) && (disp3 == BR_NULL) &&
            (r4 == BR_ERR(EBADF)) && (r5 == BR_ERR(EBADF)) &&
            (r6 == BR_ERR(EBADF)) && (disp4 == BR_NULL) &&
            (br_ft_get(0) == fake(0)) && (br_ft_get(1) == fake(1)) &&
            (br_ft_get(2) == fake(1)) && (br_ft_count() == 3u);

        pc(ok, "TC-FT-008",
           "dup2: displacing a target reports displaced; shared target ⇒ NULL; dup2(f,f) is a no-op; invalid ⇒ -EBADF");
    }

    /* TC-FT-009: release_all dedupes by pointer; table unchanged if it does not fit. */
    {
        (void)br_ft_reset();
        (void)br_ft_alloc(fake(0), 0u);      /* fd 0 */
        (void)br_ft_alloc(fake(1), 0u);      /* fd 1 */
        (void)br_ft_dup(0);                  /* fd 2 shares with fd 0 */
        (void)br_ft_dup(1);                  /* fd 3 shares with fd 1 */

        br_file_t *small[1];
        const int too_small = br_ft_release_all(small, 1u);       /* needs 2 entries => -ENOSPC */
        const br_bool intact = (br_ft_count() == 4u);

        br_file_t *got[4] = { BR_NULL, BR_NULL, BR_NULL, BR_NULL };
        const int n = br_ft_release_all(got, 4u);
        const br_bool dedup =
            (n == 2) &&
            (((got[0] == fake(0)) && (got[1] == fake(1))) ||
             ((got[0] == fake(1)) && (got[1] == fake(0))));

        pc((too_small == BR_ERR(ENOSPC)) && (intact == BR_TRUE) &&
           (dedup == BR_TRUE) && (br_ft_count() == 0u) && (br_ft_get(0) == BR_NULL),
           "TC-FT-009", "release_all: one report per handle; insufficient cap ⇒ -ENOSPC and table unchanged");
    }

    /* TC-FT-010: errno values (Linux kernel numbering). */
    {
        const br_bool ok =
            (EINVAL == 22) && (EBADF == 9) && (EMFILE == 24) && (ENFILE == 23) &&
            (ENOENT == 2) && (ENOTEMPTY == 39) && (ENOTSUP == 95) &&
            (ETIMEDOUT == 110) && (EACCES == 13) && (ESPIPE == 29) &&
            (EOPNOTSUPP == ENOTSUP) && (EWOULDBLOCK == EAGAIN);
        pc(ok, "TC-FT-010", "errno values = Linux (kernel errno-base/errno.h); aliases share values");
    }

    /* TC-FT-011: bounds and observation surface. */
    {
        (void)br_ft_reset();
        int last = -2;
        for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
            last = br_ft_alloc(fake(i % FAKE_N), 0u);
        }
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
            (br_ft_reset() == BR_FT_MAX);            /* full table cleared => dropped count = slot count */
        pc(ok, "TC-FT-011", "last valid fd = BR_FT_MAX-1; fd_at ascending; reset reports dropped count");
    }

    br_log_info("[FTCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}

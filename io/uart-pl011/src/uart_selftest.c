/*
 * brickOS prototype v0.2.0 — **self-test suite** of io/uart-pl011 (io/uart-pl011/src/uart_selftest.c)
 *
 * Design basis and ruling: `docs/decisions/0010-plugin-selftest.md` (self-test separated from core code).
 *
 * ## Why the self-test is not in `uart_pl011.c` (the reason this file exists)
 *   The test entry point is not part of the device's public surface (see the notes in
 *   `include/br/io/br_uart_pl011.h`): if it entered the golden interface hash, "add one case" would
 *   become an interface change. Moved into `src/`, the production file keeps only the driver itself;
 *   cases still live in the same plugin compile unit set (`[build].sources` covers every `.c` under `src/`).
 *
 * ## What it tests, and where the boundary with production code lies
 *   Device round-trip (locally numbered `TC-IO-*`): driven only through the **public API**
 *   (br_stat / br_open_err / br_file_* / ioctl), touching no internal symbol — otherwise "the device
 *   really opens through devfs + the cdev adapter" would have no criterion covering it.
 *   **The only exception is "platform facts"**: the base / refclk / baud constants (same source as the
 *   platform console) come from `src/pl011_internal.h` — those are not "internal mechanism" but the
 *   **platform data truth** the suite must assert; writing them separately in the production file and
 *   in the suite would reduce the criterion to "a copy equals a copy". Register access is **not**
 *   borrowed from there: the suite does not touch MMIO (it asks the device via ioctl), so that header
 *   has no register accessors either.
 *
 * ## Contract with br_plugin_manager_selftest()
 *   Returns the **number of failures** (0 = all green); logs its own output
 *   (`[IOCONF] PASS/FAIL ...` + `SUMMARY`).
 *   **A failure does not halt** — red/green is judged by the gate (`[IOCONF] FAIL` is in the forbid
 *   list of `tests/gates.toml`).
 */
#include <br/io/br_uart_pl011.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

#include "pl011_internal.h"

/* Self-test entry prototype (defined at the end of this file; referenced by the generated
 * `plugin_desc.c` `.selftest` hook). */
int uart_pl011_selftest(void);

/* =====================================================================
 * Conformance cases (locally numbered `TC-IO-*`; registered under br-wa-test-001)
 *
 *    Driven only through the **public API** (br_stat / br_open_err / br_file_*), touching no
 *    internal symbol — otherwise "the device really opens through devfs + the cdev adapter"
 *    would have no criterion covering it.
 *    ★ Each case opens and closes on its own. Even when it goes red mid-way it must clean up,
 *      otherwise the exclusive claim is not released and later cases go red in a chain (turning
 *      "one real bug" into "twelve fake bugs") while the root cause drowns.
 *    ★ Prints `[IOCONF] PASS/FAIL TC-IO-00N <desc>`, with a SUMMARY at the end; returns the failure count.
 * ===================================================================== */

#define IO_CONF_PATH   "/dev/" BR_UART_PL011_DEV_NAME

/* The two commands recurring in cases, assembled per the header comments (same nr convention as
 * this file's ioctl) */
#define IO_CONF_CMD_GET_INFO  BR_IOR(BR_UART_IOC_MAGIC, BR_UART_IOC_GET_INFO, br_uart_pl011_info_t)
#define IO_CONF_CMD_SET_BAUD  BR_IOW(BR_UART_IOC_MAGIC, BR_UART_IOC_SET_BAUD, br_u32)

static br_u32 s_io_pass;
static br_u32 s_io_fail;

static void io_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_io_pass++;
        br_log_info("[IOCONF] PASS %s %s", tag, what);
    } else {
        s_io_fail++;
        br_log_info("[IOCONF] FAIL %s %s", tag, what);
    }
}

int uart_pl011_selftest(void)
{
    s_io_pass = 0u;
    s_io_fail = 0u;

    br_log_info("[IOCONF] uart-pl011 conformance (%s: stat/open/ioctl/write/poll/close)",
                IO_CONF_PATH);

    /* ---- TC-IO-001: device node visible and of FILE type ---- */
    {
        br_stat_t  st = { 0u, 0u, 0u, 0u };   /* ADR-0013: the nlink field was appended */
        const int  rc = br_stat(IO_CONF_PATH, &st);
        io_conf((rc == 0) && (st.type == BR_INODE_FILE), "TC-IO-001",
                "br_stat(" IO_CONF_PATH ") succeeds and type = FILE");
    }

    /* ---- TC-IO-002: exclusivity (7-01 §1: policy belongs to the driver) ---- */
    {
        int        e1 = 0;
        int        e2 = 0;
        br_file_t *f1 = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &e1);
        br_file_t *f2 = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &e2);

        const br_bool ok = (f1 != BR_NULL) && (e1 == 0) &&
                           (f2 == BR_NULL) && (e2 == BR_ERR(EBUSY));
        io_conf(ok, "TC-IO-002",
                "first open succeeds; a second open while already open ⇒ -EBUSY");

        /* Cleanup must happen outside the criterion: if f2 unexpectedly succeeded it must still be
         * closed, otherwise every later case is blocked by the exclusive claim */
        if (f2 != BR_NULL) {
            (void)br_file_close(f2);
        }
        if (f1 != BR_NULL) {
            (void)br_file_close(f1);
        }
    }

    /* ---- TC-IO-003: GET_INFO reports the platform facts ---- */
    {
        int                  err  = 0;
        br_file_t           *f    = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_uart_pl011_info_t info = { 0u, 0u, 0u, 0u, 0u };
        int                  rc   = BR_ERR(EINVAL);

        if (f != BR_NULL) {
            rc = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (rc == 0) &&
                           (info.base == (br_u32)BR_PL011_BASE) &&
                           (info.refclk_hz == BR_PL011_REFCLK_HZ) &&
                           (info.baud == BR_PL011_BAUD);
        io_conf(ok, "TC-IO-003",
                "GET_INFO: base=0x09000000 refclk=24MHz baud=115200");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-004: write goes through (bytes appearing on the QEMU console are evidence, not a
     * criterion) ----
     * Device write is **byte transparent**: no '\n' → CRLF conversion (that is the console's policy,
     * see console_pl011.c) ⇒ the test string carries CRLF itself, to avoid stair-stepped line breaks. */
    {
        static const char msg[] = "uart-pl011: /dev/" BR_UART_PL011_DEV_NAME
                                  " write path OK\r\n";
        int        err = 0;
        br_file_t *f   = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_s64     n   = BR_ERR(EINVAL);

        if (f != BR_NULL) {
            n = br_file_write(f, msg, (br_size_t)(sizeof msg - 1u));
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (n == (br_s64)(sizeof msg - 1u));
        io_conf(ok, "TC-IO-004", "write of a short literal returns the byte count");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-005: empty RX FIFO + non-blocking ⇒ -EAGAIN ----
     * The premise is "the FIFO is empty" (nobody typed into the QEMU serial port since boot). If the
     * host pressed a key first, this case honestly reports red — that is a broken premise, not a
     * driver bug; we do not flush first just to "always be green" (that would turn this criterion
     * into a criterion for something else). */
    {
        int        err = 0;
        br_file_t *f   = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &err);
        char       one = '\0';
        br_s64     n   = 0;

        if (f != BR_NULL) {
            n = br_file_read(f, &one, 1u);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (n == BR_ERR(EAGAIN));
        io_conf(ok, "TC-IO-005", "empty RX FIFO + BR_O_NONBLOCK: read ⇒ -EAGAIN");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-006: poll sets POLLOUT and **clears** the bits not set ---- */
    {
        int        err = 0;
        br_file_t *f   = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_u32     ev  = 0xffffffffu;   /* preload all ones: verify "unset bits must be cleared" */
        int        rc  = BR_ERR(EINVAL);

        if (f != BR_NULL) {
            rc = br_file_poll(f, &ev);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (rc == 0) &&
                           ((ev & BR_POLLOUT) != 0u) &&
                           ((ev & ~(BR_POLLIN | BR_POLLOUT)) == 0u);
        io_conf(ok, "TC-IO-006", "poll: TX not full ⇒ BR_POLLOUT, other event bits are 0");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-007: wrong magic / unknown nr ⇒ -ENOTSUP (not -EINVAL) ---- */
    {
        int        err  = 0;
        br_file_t *f    = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        int        r_magic = BR_ERR(EINVAL);
        int        r_nr    = BR_ERR(EINVAL);

        if (f != BR_NULL) {
            r_magic = br_file_ioctl(f, BR_IO('z', 0u), BR_NULL);
            r_nr    = br_file_ioctl(f, BR_IO(BR_UART_IOC_MAGIC, 9u), BR_NULL);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_magic == BR_ERR(ENOTSUP)) &&
                           (r_nr == BR_ERR(ENOTSUP));
        io_conf(ok, "TC-IO-007",
                "wrong magic BR_IO('z',0) and unknown nr both ⇒ -ENOTSUP");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-008: the cdev file surface has no lseek/fsync (empty slots ⇒ -ENOTSUP) ---- */
    {
        int        err    = 0;
        br_file_t *f      = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_s64     r_seek = 0;
        int        r_sync = 0;

        if (f != BR_NULL) {
            r_seek = br_file_lseek(f, 0, BR_SEEK_SET);
            r_sync = br_file_fsync(f);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_seek == (br_s64)BR_ERR(ENOTSUP)) &&
                           (r_sync == BR_ERR(ENOTSUP));
        io_conf(ok, "TC-IO-008",
                "lseek/fsync on a cdev handle ⇒ -ENOTSUP (v1 shape: adapter slots are empty)");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-009: close returns 0 and the exclusive claim is released with it ---- */
    {
        int         e1 = 0;
        int         e2 = 0;
        br_file_t  *f  = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &e1);
        const int   r_close  = (f != BR_NULL) ? br_file_close(f) : BR_ERR(EINVAL);
        br_file_t  *g  = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &e2);
        const int   r_close2 = (g != BR_NULL) ? br_file_close(g) : BR_ERR(EINVAL);

        const br_bool ok = (f != BR_NULL) && (e1 == 0) && (r_close == 0) &&
                           (g != BR_NULL) && (e2 == 0) && (r_close2 == 0);
        io_conf(ok, "TC-IO-009", "close returns 0 and a later open succeeds again (claim released)");
    }

    /* ---- TC-IO-010: a call needing an argument given NULL ⇒ -EINVAL (a different thing from -ENOTSUP) ---- */
    {
        int        err    = 0;
        br_file_t *f      = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        int        r_poll = 0;
        int        r_info = 0;
        int        r_baud = 0;

        if (f != BR_NULL) {
            r_poll = br_file_poll(f, BR_NULL);
            r_info = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, BR_NULL);
            r_baud = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, BR_NULL);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_poll == BR_ERR(EINVAL)) &&
                           (r_info == BR_ERR(EINVAL)) &&
                           (r_baud == BR_ERR(EINVAL));
        io_conf(ok, "TC-IO-010",
                "NULL arg to poll(NULL events) / GET_INFO / SET_BAUD ⇒ -EINVAL");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-011: SET_BAUD representability validation and "current baud rate" update ----
     * QEMU's stdio serial backend does not throttle by baud rate, so changing the divisor here does
     * **not** garble later logs; on real hardware it would also change the console rate (heading ⑦)
     * ⇒ the case therefore always writes 115200 back at the end. */
    {
        int                  err    = 0;
        br_file_t           *f      = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_u32               too_hi = 4000000u;   /* > refclk/16 ⇒ IBRD computes to 0 */
        br_u32               zero   = 0u;         /* baud=0 ⇒ divisor 0, must be explicitly rejected */
        br_u32               good   = 9600u;      /* representable: IBRD=156 FBRD=16 */
        br_u32               back   = BR_PL011_BAUD;
        int                  r_hi = 0;
        int                  r_zero = 0;
        int                  r_good = 0;
        int                  r_info1 = 0;
        int                  r_back = 0;
        int                  r_info2 = 0;
        br_uart_pl011_info_t info1 = { 0u, 0u, 0u, 0u, 0u };
        br_uart_pl011_info_t info2 = { 0u, 0u, 0u, 0u, 0u };

        if (f != BR_NULL) {
            r_hi    = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &too_hi);
            r_zero  = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &zero);
            r_good  = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &good);
            r_info1 = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info1);
            r_back  = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &back);
            r_info2 = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info2);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_hi == BR_ERR(EINVAL)) &&
                           (r_zero == BR_ERR(EINVAL)) &&
                           (r_good == 0) && (r_info1 == 0) && (info1.baud == good) &&
                           (r_back == 0) && (r_info2 == 0) && (info2.baud == BR_PL011_BAUD);
        io_conf(ok, "TC-IO-011",
                "SET_BAUD: 4MHz/0 ⇒ -EINVAL; 9600 takes effect and reads back; restore 115200");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-012: FLUSH_RX drains the FIFO ---- */
    {
        int                  err   = 0;
        br_file_t           *f     = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_uart_pl011_info_t info  = { 0u, 0u, 0u, 0u, 0u };
        int                  r_flush = 0;
        int                  r_info  = 0;

        if (f != BR_NULL) {
            r_flush = br_file_ioctl(f, BR_IO(BR_UART_IOC_MAGIC, BR_UART_IOC_FLUSH_RX), BR_NULL);
            r_info  = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (r_flush == 0) &&
                           (r_info == 0) && (info.rx_available == 0u);
        io_conf(ok, "TC-IO-012", "after FLUSH_RX rx_available == 0");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    br_log_info("[IOCONF] SUMMARY pass=%u fail=%u total=%u",
                s_io_pass, s_io_fail, s_io_pass + s_io_fail);
    return (int)s_io_fail;
}

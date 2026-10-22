/*
 * PL011 UART cdev driver (M2 form A): write surface is narrow -- read DR/FR/RSR and IBRD/FBRD/CR
 * readback, write DR/IBRD/FBRD/CR (only clear CR -> write divisors -> restore CR) and ICR, never
 * LCR_H or CR during probe, because platform console_pl011.c owns the kernel log and panic
 * channel. v1 is a polling cdev, not an interrupt tty: WORKAROUND(br-wa-io-001); TC-IO-* cases
 * live in src/uart_selftest.c. Every function here is thread-only (never called from an ISR).
 */
#include <br/driver/br_uart_pl011.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>     /* br_malloc / br_free (session allocation) */
#include <br/hal/br_sched.h>   /* br_task_sleep (polling tick of blocking read) */
#include <br/core/br_time.h>    /* BR_US_PER_MS */
#include <br/core/br_types.h>
#include <br/dev/br_cdev.h>     /* br_cdev_ops_t / br_cdev_register (implemented/called here) */
#include <br/dev/br_dev.h>      /* BR_IOC_* encoding macros */
#include <br/vfs/br_vfs.h>      /* BR_O_NONBLOCK / BR_POLL* (read/poll semantic bits) */

/* Platform data (base/refclk/baud), shared with the selftest suite. */
#include "pl011_internal.h"

/* Lifecycle hook prototypes (also declared in the header). */
int uart_pl011_early_init(void);
int uart_pl011_init(void);
int uart_pl011_start(void);

/* Register offsets and bit definitions used by this driver alone; platform data (base / refclk /
 * baud) lives in src/pl011_internal.h, shared with the selftest suite. The console side holds its
 * own copy and the two are _Static_assert-checked. */

/* PL011 register offsets (PrimeCell UART, ARM DDI 0183) */
#define BR_PL011_DR          0x000u   /* Data Register                    */
#define BR_PL011_RSR         0x004u   /* Receive Status (read) / Error Clear (write) */
#define BR_PL011_FR          0x018u   /* Flag Register                    */
#define BR_PL011_IBRD        0x024u   /* Integer Baud Rate Divisor        */
#define BR_PL011_FBRD        0x028u   /* Fractional Baud Rate Divisor     */
#define BR_PL011_LCR_H       0x02cu   /* Line Control (this file **never** writes it) */
#define BR_PL011_CR          0x030u   /* Control                          */
#define BR_PL011_ICR         0x044u   /* Interrupt Clear                  */

/* FR status bits */
#define BR_PL011_FR_RXFE     (1u << 4)   /* RX FIFO empty */
#define BR_PL011_FR_TXFF     (1u << 5)   /* TX FIFO full  */
#define BR_PL011_FR_RXFF     (1u << 6)   /* RX FIFO full (16 bytes) */

/* Receive status bit: overrun (FR[3] is BUSY, not OE) */
#define BR_PL011_RSR_OE      (1u << 3)

/* Overrun error interrupt clear (ICR[3] is reserved on PL011) */
#define BR_PL011_ICR_OEIC    (1u << 10)

/* CR control bits: listed for readability and used as a mask in SET_BAUD's sanity check; CR
 * restore always writes the read-back value, never a hand-assembled one. */
#define BR_PL011_CR_UARTEN   (1u << 0)
#define BR_PL011_CR_TXE      (1u << 8)
#define BR_PL011_CR_RXE      (1u << 9)

/* RX FIFO depth (PL011 fixed at 16; upper bound of GET_INFO's rx_available) */
#define BR_PL011_FIFO_DEPTH  16u

/* Refclk/baud/divisor formulas live in src/pl011_internal.h (shared with the suite). */

/* MMIO accessors (volatile; no bitfields/DMA) */

static br_u32 pl011_reg_read(br_u32 off)
{
    return *(volatile br_u32 *)(BR_PL011_BASE + off);
}

static void pl011_reg_write(br_u32 off, br_u32 value)
{
    *(volatile br_u32 *)(BR_PL011_BASE + off) = value;
}

/* Single PL011 instance in v1 (multiple instances need pluggable platform data). */

/* Session (opaque to cdev-core): flags = BR_O_* at open (v1 reads only BR_O_NONBLOCK);
 * rx_dropped = overruns observed by this session. */
typedef struct br_uart_pl011_sess {
    br_u32 flags;
    br_u32 rx_dropped;
} pl011_sess_t;

/* Device private, handed to cdev-core at registration and passed back verbatim as open's
 * dev_priv: baud = current rate (tracked in software; probe must not read hardware); opened =
 * exclusivity flag (driver-owned policy). */
typedef struct br_uart_pl011_dev {
    br_u32  baud;
    br_bool opened;
} pl011_dev_t;

static pl011_dev_t s_dev = {
    .baud   = BR_PL011_BAUD,
    .opened = BR_FALSE,
};

/* Hardware helpers with no session semantics */

/*
 * Sample and clear overrun. Reading RSR is the sample (in FIFO mode it pops the status entry
 * paired with a DR read), so call sites are only "there really is data to read" or GET_INFO --
 * never the empty-FIFO blocking loop, which would desynchronize status entries from DR reads.
 */
static void pl011_sample_overrun(pl011_sess_t *s)
{
    const br_u32 rsr = pl011_reg_read(BR_PL011_RSR);

    if ((rsr & BR_PL011_RSR_OE) == 0u) {
        return;
    }

    s->rx_dropped++;

    /* Device-side clear (ICR.OEIC). v1 enables no UART interrupts, so this is idempotent. */
    pl011_reg_write(BR_PL011_ICR, BR_PL011_ICR_OEIC);
}

/* Lower bound on bytes currently readable from the RX FIFO (FR reports no level) */
static br_u32 pl011_rx_available(void)
{
    const br_u32 fr = pl011_reg_read(BR_PL011_FR);

    if ((fr & BR_PL011_FR_RXFE) != 0u) {
        return 0u;
    }
    if ((fr & BR_PL011_FR_RXFF) != 0u) {
        return BR_PL011_FIFO_DEPTH;
    }
    return 1u;   /* at least 1; the exact count is only known by reading */
}

/* Drain the RX FIFO, counting discarded bytes into rx_dropped (FLUSH_RX). */
static void pl011_drain_rx(pl011_sess_t *s)
{
    while ((pl011_reg_read(BR_PL011_FR) & BR_PL011_FR_RXFE) == 0u) {
        (void)pl011_reg_read(BR_PL011_DR);   /* reading it is discarding it */
        s->rx_dropped++;
    }
}

/* Baud rate: SET_BAUD mechanics (the divisors belong to the console-shared line). */

/*
 * Compute IBRD/FBRD; not representable => -EINVAL. baud == 0 is filtered first to avoid a
 * divide-by-zero; IBRD == 0 means baud > refclk/16 (1.5 Mbaud at 24 MHz); IBRD > 0xFFFF exceeds
 * the 16-bit divisor register; an FBRD that rounds to 64 carries into IBRD+1 (otherwise the 6-bit
 * field truncates to 0, a 1/64 rate error).
 */
static int pl011_divisors_for(br_u32 baud, br_u32 *ibrd_out, br_u32 *fbrd_out)
{
    if (baud == 0u) {
        return BR_ERR(EINVAL);
    }

    br_u32 ibrd = BR_PL011_DIV_IBRD(BR_PL011_REFCLK_HZ, baud);
    br_u32 fbrd = BR_PL011_DIV_FBRD(BR_PL011_REFCLK_HZ, baud);

    if (fbrd >= 64u) {
        ibrd++;
        fbrd -= 64u;
    }
    if ((ibrd == 0u) || (ibrd > 0xffffu)) {
        return BR_ERR(EINVAL);
    }

    *ibrd_out = ibrd;
    *fbrd_out = fbrd;
    return 0;
}

/*
 * Program the divisors. ARM DDI 0183: clear CR.UARTEN before changing IBRD/FBRD and restore CR
 * after -- changing divisors while running puts a character of undefined width on the line.
 * Restore writes the read-back CR, not a hand-assembled UARTEN|TXE|RXE, so the console's line
 * setting (even "UARTEN off") is preserved. FR.TXFE is not awaited: v1's transmitter has nothing
 * in flight, and an unbounded wait would add a livelock surface to the ioctl.
 */
static void pl011_program_divisors(br_u32 ibrd, br_u32 fbrd)
{
    const br_u32 cr   = pl011_reg_read(BR_PL011_CR);
    const br_u32 want = BR_PL011_CR_UARTEN | BR_PL011_CR_TXE | BR_PL011_CR_RXE;

    pl011_reg_write(BR_PL011_CR, 0u);
    pl011_reg_write(BR_PL011_IBRD, ibrd);
    pl011_reg_write(BR_PL011_FBRD, fbrd);
    pl011_reg_write(BR_PL011_CR, cr);

    /* Debug-only lead: missing UARTEN|TXE|RXE means the console has not brought the line up. */
    if ((cr & want) != want) {
        br_log_debug("uart-pl011: CR=0x%08x at SET_BAUD lacks UARTEN|TXE|RXE; "
                     "divisors changed, line enable kept as the console had it", cr);
    }
}

/* cdev session ops */

/*
 * open(dev_priv, flags, &sess): enforce exclusivity before allocating (a rejected call must
 * allocate nothing); the session is br_malloc'd and its lifetime is tied to open, with at most
 * one live session so the allocation count stays bounded.
 * Errors: -EINVAL (bad args/private) / -EBUSY (already open) / -ENOMEM (heap exhausted).
 */
static int pl011_open(void *dev_priv, br_u32 flags, void **sess)
{
    /* v1 has one instance: verify the private instead of silently ignoring a mismatch. */
    if ((dev_priv != (void *)&s_dev) || (sess == BR_NULL)) {
        return BR_ERR(EINVAL);
    }

    if (s_dev.opened != BR_FALSE) {
        return BR_ERR(EBUSY);   /* exclusive: driver-owned policy */
    }

    pl011_sess_t *s = (pl011_sess_t *)br_malloc(sizeof(pl011_sess_t));
    if (s == BR_NULL) {
        return BR_ERR(ENOMEM);
    }

    s->flags      = flags;   /* stored verbatim: v1 interprets only BR_O_NONBLOCK, the rest later */
    s->rx_dropped = 0u;

    s_dev.opened = BR_TRUE;
    *sess        = (void *)s;
    return 0;
}

/*
 * read(sess, buf, n): pull at most n bytes from the RX FIFO. Non-blocking with nothing available
 * => -EAGAIN; blocking => sleep 1ms and retry, with no timeout. Once some bytes are read, an empty
 * FIFO ends the call as a short read ("drain what is readable", never pad up to n).
 */
static br_s64 pl011_read(void *sess, void *buf, br_size_t n)
{
    if ((sess == BR_NULL) || ((buf == BR_NULL) && (n > 0u))) {
        return BR_ERR(EINVAL);
    }

    pl011_sess_t *s   = (pl011_sess_t *)sess;
    br_u8        *out = (br_u8 *)buf;
    br_size_t     got = 0u;

    while (got < n) {
        const br_u32 fr = pl011_reg_read(BR_PL011_FR);

        if ((fr & BR_PL011_FR_RXFE) == 0u) {
            pl011_sample_overrun(s);                                 /* paired with this DR read */
            out[got] = (br_u8)(pl011_reg_read(BR_PL011_DR) & 0xffu);        /* DR[7:0] = data */
            got++;
            continue;
        }

        /* FIFO empty */
        if (got > 0u) {
            break;                                          /* short read */
        }
        if ((s->flags & BR_O_NONBLOCK) != 0u) {
            return BR_ERR(EAGAIN);                       /* non-blocking: ask once */
        }

        /* Sleep rather than spin: under coop, spinning stalls the whole machine. No timeout. */
        (void)br_task_sleep((br_time_t)BR_US_PER_MS);
    }

    return (br_s64)got;
}

/*
 * write(sess, buf, n): busy-wait for TX FIFO space byte by byte, then write; returns n. The wait is
 * bounded by FIFO depth x bit time (~1.4 ms at 115200), so sleeping would only cut throughput.
 * Bytes are written verbatim: '\n' -> CRLF is console/terminal policy, the device stays
 * byte-transparent.
 */
static br_s64 pl011_write(void *sess, const void *buf, br_size_t n)
{
    if ((sess == BR_NULL) || ((buf == BR_NULL) && (n > 0u))) {
        return BR_ERR(EINVAL);
    }

    const br_u8 *p = (const br_u8 *)buf;

    for (br_size_t i = 0u; i < n; i++) {
        while ((pl011_reg_read(BR_PL011_FR) & BR_PL011_FR_TXFF) != 0u) {
            /* Busy wait for TX FIFO space; v1 has no TX interrupt. */
        }
        pl011_reg_write(BR_PL011_DR, (br_u32)p[i]);
    }

    return (br_s64)n;
}

/*
 * ioctl(sess, cmd, arg). Dispatch on type (family magic) + nr; dir/size are not strictly validated
 * because v1's callers are in-house and rejecting a semantically unambiguous command on size bits
 * would turn the encoding into a runtime landmine. Unknown type/nr = -ENOTSUP.
 * Errors: -ENOTSUP (not this device's command / unimplemented) / -EINVAL (missing argument or
 * unrepresentable baud rate).
 */
static int pl011_ioctl(void *sess, br_u32 cmd, void *arg)
{
    if (sess == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    pl011_sess_t *s = (pl011_sess_t *)sess;

    /* Wrong family magic: -ENOTSUP, not -EINVAL -- the command may be legal for another device
     * family, and that must stay separate from "the argument given to me is wrong". */
    if (BR_IOC_TYPE(cmd) != (br_u32)BR_UART_IOC_MAGIC) {
        return BR_ERR(ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_UART_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }

        /* Sample overrun once so the query reflects bytes dropped since the last read. */
        pl011_sample_overrun(s);

        br_uart_pl011_info_t *info = (br_uart_pl011_info_t *)arg;
        info->base         = (br_u32)BR_PL011_BASE;
        info->refclk_hz    = BR_PL011_REFCLK_HZ;
        info->baud         = s_dev.baud;
        info->rx_available = pl011_rx_available();
        info->rx_dropped   = s->rx_dropped;
        return 0;
    }

    case BR_UART_IOC_FLUSH_RX:
        /* No-argument command: arg may be null and is not checked. */
        pl011_drain_rx(s);
        return 0;

    case BR_UART_IOC_SET_BAUD: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }

        /* arg = br_u32 baud rate (by value, not a struct). Aligning the caller's pointer is the
         * caller's responsibility. */
        const br_u32 baud = *(const br_u32 *)arg;
        br_u32       ibrd = 0u;
        br_u32       fbrd = 0u;

        const int rc = pl011_divisors_for(baud, &ibrd, &fbrd);
        if (rc != 0) {
            return rc;   /* -EINVAL: not representable (incl. baud=0), hardware untouched */
        }

        pl011_program_divisors(ibrd, fbrd);
        s_dev.baud = baud;   /* update current baud only after successful programming */
        return 0;
    }

    default:
        /* Unknown nr: -ENOTSUP, never silently succeed. */
        return BR_ERR(ENOTSUP);
    }
}

/*
 * poll(sess, events): v1 queries the ready bits only (no waiting, no wait-queue registration).
 * RX has data => BR_POLLIN; TX not full => BR_POLLOUT.
 * Unset bits must be cleared, so assign rather than OR: the caller's buffer may be reused, and
 * keeping old values would report "no data" as "has data".
 */
static int pl011_poll(void *sess, br_u32 *events)
{
    if ((sess == BR_NULL) || (events == BR_NULL)) {
        return BR_ERR(EINVAL);
    }

    const br_u32 fr = pl011_reg_read(BR_PL011_FR);
    br_u32       ev = 0u;

    if ((fr & BR_PL011_FR_RXFE) == 0u) {
        ev |= BR_POLLIN;
    }
    if ((fr & BR_PL011_FR_TXFF) == 0u) {
        ev |= BR_POLLOUT;
    }

    *events = ev;
    return 0;
}

/*
 * close(sess): free the session = release exclusivity. Destroy before releasing the flag, so no
 * window exists where the session is freed but still reachable from device state.
 */
static int pl011_close(void *sess)
{
    if (sess == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    br_free(sess);
    s_dev.opened = BR_FALSE;
    return 0;
}

/*
 * suspend/resume: always -ENOTSUP (reserved slots keep the ops layout binary-compatible; v1 has no
 * PM caller and saves no register state, so returning 0 would misreport capability).
 * -ENOTSUP rather than -EINVAL: the command and the situation are legal, this version simply lacks
 * the capability.
 */
static int pl011_suspend(void *dev_priv)
{
    (void)dev_priv;
    return BR_ERR(ENOTSUP);
}

static int pl011_resume(void *dev_priv)
{
    (void)dev_priv;
    return BR_ERR(ENOTSUP);
}

/* Ops table: layout and content are contract; every slot is given explicitly, not by zero-init. */
static const br_cdev_ops_t s_ops = {
    .open    = pl011_open,
    .read    = pl011_read,
    .write   = pl011_write,
    .ioctl   = pl011_ioctl,
    .poll    = pl011_poll,
    .close   = pl011_close,
    .suspend = pl011_suspend,
    .resume  = pl011_resume,
};

/* Lifecycle hooks (name = symbol_prefix + phase; referenced by generated plugin_desc.c) */

/*
 * EARLY: no action -- no threads/heap yet, and the PL011 line settings still belong to the
 * platform early console, so initializing the UART here would alter the panic channel.
 */
int uart_pl011_early_init(void)
{
    return 0;
}

/*
 * CORE: register the cdev, which makes it appear in /dev. rc is propagated verbatim: -EINVAL
 * (bad name/ops) / -EEXIST (duplicate name) / -ENOSPC (registry full) must all surface at boot.
 */
int uart_pl011_init(void)
{
    const int rc = br_cdev_register(BR_UART_PL011_DEV_NAME, &s_ops, &s_dev);

    if (rc != 0) {
        br_log_error("uart-pl011: cdev registration failed rc=%d (name=%s)",
                     rc, BR_UART_PL011_DEV_NAME);
        return rc;
    }

    br_log_info("uart-pl011: /dev/%s registered (cdev; PL011 @ 0x%08x, %u baud, %u MHz refclk)",
                BR_UART_PL011_DEV_NAME, (br_u32)BR_PL011_BASE,
                BR_PL011_BAUD, BR_PL011_REFCLK_HZ / 1000000u);
    return 0;
}

/*
 * START: conformance cases do not run here -- core's br_plugin_manager_selftest() drives them via
 * the descriptor's .selftest hook (src/uart_selftest.c). Only one boot summary line remains.
 * Returns 0, not a failure count: a non-zero return would make the plugin manager treat it as a
 * boot failure.
 */
int uart_pl011_start(void)
{
    br_log_info("uart-pl011: /dev/%s ready (PL011 @ 0x%08x, %u baud)",
                BR_UART_PL011_DEV_NAME, (br_u32)BR_PL011_BASE, BR_PL011_BAUD);
    return 0;
}

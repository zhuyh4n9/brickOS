/*
 * brickOS prototype v0.2.0 - PL011 UART character device (cdev) driver (implementation)
 *
 * Design basis (authoritative, item by item):
 *   - `1-03-roadmap.md` §1 plugin list: `io/uart-pl011` = "PL011: early polling console (M0:
 *     platform early console, **does not register a device**) -> **interrupt tty (M2: registers
 *     cdev)**". This file implements M2 **form A** (`8-01-device.md` §1.2): register the same
 *     PL011 as a cdev => `/dev/uart0` can be opened by `br_open`. The "interrupt tty" (RX
 *     interrupt + wait-queue) is the next small step of the same M2, see (2) below.
 *   - `8-01-device.md` §3 (cdev session ops: `open/read/write/ioctl/poll/close` + D22 reserved
 *     `suspend/resume`), §4 (SD-6 blocking semantics / SD-10 negative errno / ISR ban), §5
 *     (ioctl uses Linux-compatible encoding, this device family magic `'u'`)
 *   - `7-01-vfs.md` §1 (exclusive-device policy **belongs to the driver**: uart class returns
 *     `-EBUSY` in `ops->open`), §3 (SD-7 poll staged: v1 = query ready bits; the waiting side is
 *     "poll + sleep")
 *   - Per-item rulings and deviations are logged in `WORKAROUND(br-wa-io-001)`; conformance cases
 *     (TC-IO-*) are in `src/uart_selftest.c`, driven by core's selftest pass (ADR-0010); the id
 *     self-numbering is logged in `br-wa-test-001`.
 *
 * ============================================================================
 * (1) Relationship to the platform early console (same hardware, two forms -- by design)
 * ============================================================================
 *   The platform's `console_pl011.c` is the M0 early console (polling putc, standalone, no
 *   device registration); it keeps owning the **kernel log** and the **panic channel**. This
 *   driver is the **device-semantics** side of `/dev/uart0`.
 *   Both share one PL011 register window, so this file deliberately keeps its register **write
 *   surface** narrow:
 *
 *     read:  DR (data) / FR (status) / RSR (receive status: overrun) / readback of IBRD,FBRD,CR
 *     write: DR (data) / IBRD,FBRD,CR (**only** SET_BAUD's "clear CR -> write divisors ->
 *            restore CR") / ICR (clear source)
 *     never touch: LCR_H (8N1 + FIFO enable, the console's line settings) / CR during probe
 *            (early_init/probe)
 *
 *   Why "registering a device" is not "reinitializing the hardware": in the image the console
 *   brings up the panic channel first, while the cdev CORE phase comes later. Rewriting CR/LCR_H
 *   here would be a **second owner** changing line settings -- once the two disagree, panic
 *   output turns into garbage exactly when it is needed most. So this driver does not bring-up,
 *   it reuses.
 *   (The reverse -- let io/uart-pl011 own initialization and have the console call it -- would
 *   make the panic channel depend on a plugin that may not be composed in; design `1-01` §8
 *   requires the early console to be self-contained, so that option is rejected.)
 *
 * ============================================================================
 * (2) Why v1 is a "polling cdev" and not an "interrupt tty" (honest deviation)
 * ============================================================================
 *   The design goal is an interrupt tty, but SD-7 splits event waiting into two stages:
 *   **v1 = poll + br_task_sleep**, and only v2 adds wait-queue exact wakeup (which must land
 *   together with preempt, else polling latency jitters on a preemptible product, R-S1).
 *   So this file:
 *     - does **not** enable UART interrupts (no IMSC writes / no ISR registration) => no
 *       concurrency, hence no lock needed;
 *     - blocking read polls with `br_task_sleep(1ms)` (see (5));
 *     - poll answers only "current ready bits" (SD-7 v1 wording) and never waits.
 *   When the second half of M2 (interrupt tty) lands, the ops shape here **need not change** --
 *   only read's waiting mechanism (sleep polling -> wait-queue) and the addition of IMSC/ISR.
 *
 * ============================================================================
 * (3) Session and exclusivity (7-01 §1: policy belongs to the driver)
 * ============================================================================
 *   Session = `{flags, rx_dropped}` (private to this file, handed opaquely to the cdev adapter).
 *   Exclusivity does **not** rely on cdev-core: the right to reject a second opener is
 *   **policy**, and uart is a classic exclusive device (two consumers interleaving reads on one
 *   FIFO both end up incomplete). So `open` returns `-EBUSY` when a session already exists, and
 *   `close` releases it.
 *   The "allow multiple readers per open flags" option is rejected: v1 has a single APP, and
 *   multiple readers only add "who ate this byte" ambiguity, which costs far more than -EBUSY.
 *
 * ============================================================================
 * (4) Honest source of overrun (rx_dropped) (differs from the task text, explicitly ruled)
 * ============================================================================
 *   The task text suggests "FR bit3 = OE, clear with ICR bit3". Per ARM DDI 0183 (PrimeCell
 *   UART):
 *     - `UARTFR[3]` is **BUSY** (transmitting), **not** OE -- using it as a dropped-byte count
 *       would record "shifting out" as "RX overflow"; UARTFR has no OE bit at all.
 *     - The honest source of overrun is `UARTRSR[3]` (receive status register; in FIFO mode it
 *       is the read port of the receive status FIFO, popped in lockstep with DR reads), the same
 *       origin as DR `[11]`=OE.
 *     - The interrupt-side clear bit is `UARTICR[10]`=OEIC; ICR bit3 is reserved on PL011.
 *   So this file samples RSR[3], counts into the session's rx_dropped, and writes ICR.OEIC to
 *   fulfill the "clear the source on the device side" duty (v1 enables no UART interrupts, so
 *   the write is idempotent; writing it now means M2 need not come back for it).
 *   The "FR[3] + ICR[3]" form is **rejected**: it would fabricate counts on real hardware and
 *   write a reserved bit -- copying the instruction against the hardware.
 *
 * ============================================================================
 * (5) Blocking semantics and "may wait forever" (SD-6 / SD-7)
 * ============================================================================
 *   read: when the FIFO is empty, non-blocking => `-EAGAIN`; blocking => `br_task_sleep(1ms)`
 *         then retry. **v1 has no timeout** -- what is awaited is "whether the outside world
 *         supplies bytes", and no deadline simply means no deadline (we do not pretend to know
 *         when the peer types). The cost is that the calling thread may never return; a thread
 *         blocking solely on uart should be covered by the upper layer (runtime/posix poll /
 *         timeout), not by the driver.
 *   write: waits byte by byte for `FR.TXFF` to clear. This is a **busy wait**, not sleep: what
 *         is awaited is "the data just written moving out one slot", bounded by FIFO depth x bit
 *         time (16 bytes at 115200 ~= 1.4 ms); sleeping 1 ms for it would halve throughput
 *         again. Different direction, different mechanism -- not an inconsistency but two kinds
 *         of waiting.
 *
 * ============================================================================
 * (6) Honest upper bound of rx_available
 * ============================================================================
 *   PL011's FR has **no** RX FIFO level field (no 16550-style level register), only RXFE
 *   (empty) / RXFF (full). So GET_INFO's rx_available can only give a **lower bound**:
 *     RXFE => 0; RXFF => 16 (FIFO depth 16); otherwise => 1 (at least 1; the exact count is not
 *     visible from FR).
 *   Reporting 1 is conservatively explainable ("at least 1"); reporting a guessed middle value
 *   would be a lie.
 *   (PL011's FR has only the RXFE/RXFF status bits and **no** FIFO depth field; the header
 *    already documents 0/1/16 as the honest lower bound -- an exact value requires FIFO reads,
 *    and query semantics should have no side effects.)
 *
 * ============================================================================
 * (7) Shared-line risk of SET_BAUD (why it must be stated)
 * ============================================================================
 *   IBRD/FBRD are parameters of the **whole PL011 line**, and the console and /dev/uart0 are the
 *   same line: changing the baud rate also changes the rate of the kernel log / panic channel.
 *   v1 still provides this ioctl (device semantics should be complete, and it is the best carrier
 *   for "representability check + registers sharing constants with the console"), but states the
 *   risk plainly: a real product should coordinate through the termios layer (the console owns
 *   the line) instead of letting two consumers write independently.
 *   When restoring CR, write the **read-back value back unchanged** rather than assembling
 *   `UARTEN|TXE|RXE`: the truth of the line settings lives on the console side, and this driver
 *   only borrows the divisor slot.
 *
 * ============================================================================
 * (8) ISR discipline
 * ============================================================================
 *   Every function in this file is **thread-only** (`8-01` §4: device/storage domain APIs must
 *   not be called from an ISR, whitelist empty): they call `br_malloc`/`br_free`/
 *   `br_task_sleep` and also spin waiting for TX. v1 enables no interrupts, so no ISR reaches
 *   here.
 */
#include <br/io/br_uart_pl011.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>     /* br_malloc / br_free (session allocation) */
#include <br/core/br_sched.h>   /* br_task_sleep (polling tick of blocking read) */
#include <br/core/br_time.h>    /* BR_US_PER_MS */
#include <br/core/br_types.h>
#include <br/dev/br_cdev.h>     /* br_cdev_ops_t / br_cdev_register (implemented/called here) */
#include <br/dev/br_dev.h>      /* BR_IOC_* encoding macros */
#include <br/vfs/br_vfs.h>      /* BR_O_NONBLOCK / BR_POLL* (read/poll semantic bits) */

#include "pl011_internal.h"    /* platform data (base/refclk/baud); shared with the selftest suite */

/* Lifecycle hook prototypes (name = symbol_prefix + phase, referenced by generated code; also in
 * the header. Repeated here so a -Wmissing-prototypes reader sees at a glance that these three
 * are this file's whole external surface.) */
int uart_pl011_early_init(void);
int uart_pl011_init(void);
int uart_pl011_start(void);

/* =====================================================================
 * 1. Hardware registers (platform data base/refclk/baud live in src/pl011_internal.h --
 *    production and the selftest suite share **one source of truth**; only the register offsets
 *    and bit definitions needed by this driver alone remain here)
 *
 *    Same hardware as platform/qemu-aarch64/src/console_pl011.c in two forms, with values
 *    written independently: they belong to **different plugins**, and core provides no
 *    "PL011 register table" header of unclear ownership. If the values disagree, each side's
 *    _Static_assert (in pl011_internal.h) / test cases report it at compile time and in boot
 *    logs.
 * ===================================================================== */

/* PL011 register offsets (PrimeCell UART, ARM DDI 0183) */
#define BR_PL011_DR          0x000u   /* Data Register                    */
#define BR_PL011_RSR         0x004u   /* Receive Status (read) / Error Clear (write) */
#define BR_PL011_FR          0x018u   /* Flag Register                    */
#define BR_PL011_IBRD        0x024u   /* Integer Baud Rate Divisor        */
#define BR_PL011_FBRD        0x028u   /* Fractional Baud Rate Divisor     */
#define BR_PL011_LCR_H       0x02cu   /* Line Control (this file **never** writes it) */
#define BR_PL011_CR          0x030u   /* Control                          */
#define BR_PL011_ICR         0x044u   /* Interrupt Clear                  */

/* FR status bits (only these two + full are needed) */
#define BR_PL011_FR_RXFE     (1u << 4)   /* RX FIFO empty */
#define BR_PL011_FR_TXFF     (1u << 5)   /* TX FIFO full  */
#define BR_PL011_FR_RXFF     (1u << 6)   /* RX FIFO full (16 bytes) */

/* Receive status bit: overrun (not FR[3] -- FR[3] is BUSY, see header (4)) */
#define BR_PL011_RSR_OE      (1u << 3)

/* ICR clear bit: overrun error interrupt clear (not ICR[3], that is reserved, see header (4)) */
#define BR_PL011_ICR_OEIC    (1u << 10)

/* CR control bits. **Registered only, never assembled by hand**: CR restore uses the read-back
 * value (header (7)). Listed so "which bits make up the line" is a readable fact, and used as a
 * mask in SET_BAUD's sanity check. */
#define BR_PL011_CR_UARTEN   (1u << 0)
#define BR_PL011_CR_TXE      (1u << 8)
#define BR_PL011_CR_RXE      (1u << 9)

/* RX FIFO depth (PL011 fixed at 16; upper bound of GET_INFO's rx_available) */
#define BR_PL011_FIFO_DEPTH  16u

/* Reference clock / baud / divisor constants and the divisor formulas live in
 * src/pl011_internal.h (shared by production and the suite). */

/* =====================================================================
 * 2. Register access (volatile; no bitfields / no DMA -- this is MMIO, not memory)
 * ===================================================================== */

static br_u32 pl011_reg_read(br_u32 off)
{
    return *(volatile br_u32 *)(BR_PL011_BASE + off);
}

static void pl011_reg_write(br_u32 off, br_u32 value)
{
    *(volatile br_u32 *)(BR_PL011_BASE + off) = value;
}

/* =====================================================================
 * 3. Driver state (single instance -- v1's static composition has one PL011; multiple instances
 *    must wait for platform data to become pluggable)
 * ===================================================================== */

/* Session (design `8-01` §3's "opaque sess"): holds only the two things that truly survive
 * across calls.
 *   flags      = BR_O_* at open (v1 interprets only BR_O_NONBLOCK)
 *   rx_dropped = overruns observed by this session (header (4)) */
typedef struct br_uart_pl011_sess {
    br_u32 flags;
    br_u32 rx_dropped;
} pl011_sess_t;

/* Device private (handed to cdev-core at registration, then passed back verbatim as open's
 * dev_priv):
 *   baud   = current baud rate (GET_INFO answer; probe must not read hardware -- divisor
 *            registers are readable, but such a "guess what the console wrote" readback would
 *            turn "we never changed it" into "we claim it" -- so track our own)
 *   opened = exclusivity flag (7-01 §1: policy belongs to the driver) */
typedef struct br_uart_pl011_dev {
    br_u32  baud;
    br_bool opened;
} pl011_dev_t;

static pl011_dev_t s_dev = {
    .baud   = BR_PL011_BAUD,
    .opened = BR_FALSE,
};

/* =====================================================================
 * 4. Hardware plumbing (small helpers with no session semantics)
 * ===================================================================== */

/*
 * Sample and clear overrun. **Reading RSR is the sample** (in FIFO mode it pops the status
 * entry paired with a DR read), so call sites are only "there really is data to read" or
 * GET_INFO -- never spinning in the empty-FIFO blocking loop, which would desynchronize status
 * FIFO entries from DR reads (see header (4)).
 */
static void pl011_sample_overrun(pl011_sess_t *s)
{
    const br_u32 rsr = pl011_reg_read(BR_PL011_RSR);

    if ((rsr & BR_PL011_RSR_OE) == 0u) {
        return;
    }

    s->rx_dropped++;

    /* Device-side clear: ICR.OEIC. v1 enables no UART interrupts => idempotent; but the duty
     * stays here so M2 need not come back for it (a level interrupt without clearing = livelock,
     * the same discipline as design 3-02 §8.2). */
    pl011_reg_write(BR_PL011_ICR, BR_PL011_ICR_OEIC);
}

/* **Lower bound** on bytes currently readable from the RX FIFO (header (6): FR reports no
 * level) */
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

/* Drain the RX FIFO, discarding and counting into rx_dropped (FLUSH_RX implementation) */
static void pl011_drain_rx(pl011_sess_t *s)
{
    while ((pl011_reg_read(BR_PL011_FR) & BR_PL011_FR_RXFE) == 0u) {
        (void)pl011_reg_read(BR_PL011_DR);   /* reading it is discarding it: FLUSH means "no longer wanted" */
        s->rx_dropped++;
    }
}

/* =====================================================================
 * 5. Baud rate (all of SET_BAUD's mechanics + one note on the console-shared line risk)
 * ===================================================================== */

/*
 * Representability check: compute IBRD/FBRD; not representable => -EINVAL.
 *   baud == 0        => divisor 0 (not filtered first, this is a divide-by-zero: not a "bad
 *                       argument" but "the program would crash")
 *   IBRD == 0        => divisor < 1 => baud > refclk/16 (1.5 Mbaud at 24 MHz), PL011 cannot
 *                       represent it
 *   IBRD > 0xFFFF    => divisor register is only 16 bits (extremely low baud rates)
 *   FBRD rounds to 64 => carry into IBRD+1, FBRD=0 (otherwise the 6-bit field truncates to 0, a
 *                       1/64 rate error)
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
 * Program the divisors. ARM requirement (DDI 0183): clear CR.UARTEN before changing IBRD/FBRD,
 * restore CR after -- changing divisors while running puts a character of undefined width on the
 * line.
 *
 * * The restored value is the **read-back CR**, not a hand-assembled `UARTEN|TXE|RXE`: CR is the
 *   line setting shared by the console and /dev/uart0 (header (7)). Assembling constants would
 *   silently rewrite the console's configuration into "what I think it is"; read-modify-write
 *   faithfully preserves even states like "UARTEN was off".
 * * Do not wait for FR.TXFE before disabling: v1's transmitter is a single-threaded log, so there
 *   is no data in flight when SET_BAUD is called; adding an unbounded wait would instead hang
 *   the ioctl when flow control / the peer does not read -- manufacturing a livelock surface
 *   inside the driver (unlike read's infinite wait: read waits for external input, which is
 *   semantics).
 */
static void pl011_program_divisors(br_u32 ibrd, br_u32 fbrd)
{
    const br_u32 cr   = pl011_reg_read(BR_PL011_CR);
    const br_u32 want = BR_PL011_CR_UARTEN | BR_PL011_CR_TXE | BR_PL011_CR_RXE;

    pl011_reg_write(BR_PL011_CR, 0u);
    pl011_reg_write(BR_PL011_IBRD, ibrd);
    pl011_reg_write(BR_PL011_FBRD, fbrd);
    pl011_reg_write(BR_PL011_CR, cr);

    /* Sanity check (does not change behavior): missing line-enable bits mean the console has not
     * brought up yet or someone disabled the UART. Debug level -- not an error, just a lead for
     * -v debugging; it should not spam a normal boot. */
    if ((cr & want) != want) {
        br_log_debug("uart-pl011: CR=0x%08x at SET_BAUD lacks UARTEN|TXE|RXE; "
                     "divisors changed, line enable kept as the console had it", cr);
    }
}

/* =====================================================================
 * 6. cdev ops implementation (design `8-01` §3's session-shaped form)
 * ===================================================================== */

/*
 * open(dev_priv, flags, &sess)
 *   (1) enforce exclusivity (check before allocating: a rejected call should allocate nothing);
 *   (2) session allocated with br_malloc -- not stuffed into any static pool: the session
 *       lifetime is tied to "open", and exclusivity guarantees at most one at a time => the
 *       allocation count is naturally bounded (no fragmentation risk).
 *   Errors: -EINVAL (bad args/private) / -EBUSY (already open) / -ENOMEM (heap exhausted).
 */
static int pl011_open(void *dev_priv, br_u32 flags, void **sess)
{
    /* v1 has one instance: the private handed in at CORE-phase registration is necessarily
     * &s_dev. Verify rather than silently ignore -- a different private means registration and
     * lookup were mismatched; failing early beats "opened a shadow device". */
    if ((dev_priv != (void *)&s_dev) || (sess == BR_NULL)) {
        return BR_ERR(EINVAL);
    }

    if (s_dev.opened != BR_FALSE) {
        return BR_ERR(EBUSY);   /* exclusive: policy in the driver, see header (3) */
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
 * read(sess, buf, n): pull at most n bytes from the RX FIFO.
 *   Non-blocking with zero bytes => -EAGAIN; blocking => wake every 1ms and retry (**no
 *   timeout**, header (5)).
 *   After some bytes are obtained, an empty FIFO => immediate **short read**: "drain what is
 *   readable" fits character-device semantics better than "fill up to n" (when data arrives is
 *   not the caller's decision), and it keeps a non-blocking read from turning into a blocking one
 *   just to reach n.
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
            return BR_ERR(EAGAIN);                       /* SD-7 v1: non-blocking = ask once */
        }

        /* Blocking wait: sleep rather than spin -- what is awaited is the outside world,
         * possibly for a long time, and must not burn CPU (under coop scheduling spinning means
         * "the whole machine stalls"). No timeout (header (5)). */
        (void)br_task_sleep((br_time_t)BR_US_PER_MS);
    }

    return (br_s64)got;
}

/*
 * write(sess, buf, n): wait for TX FIFO space byte by byte, then write. Returns bytes written
 * (= n).
 *   Busy-waits on TXFF (header (5) explains why this is not a sleep). Writing DR means "send one
 *   byte" with no conversion -- '\n' -> CRLF is console/terminal policy; the device side must be
 *   byte-transparent, otherwise the same data would differ across the two paths out.
 */
static br_s64 pl011_write(void *sess, const void *buf, br_size_t n)
{
    if ((sess == BR_NULL) || ((buf == BR_NULL) && (n > 0u))) {
        return BR_ERR(EINVAL);
    }

    const br_u8 *p = (const br_u8 *)buf;

    for (br_size_t i = 0u; i < n; i++) {
        while ((pl011_reg_read(BR_PL011_FR) & BR_PL011_FR_TXFF) != 0u) {
            /* Busy wait: TX FIFO has space. With no TX interrupt in v1, waiting is all we can do. */
        }
        pl011_reg_write(BR_PL011_DR, (br_u32)p[i]);
    }

    return (br_s64)n;
}

/*
 * ioctl(sess, cmd, arg).
 *   Dispatch dimension = **type (family magic) + nr**; dir/size are not strictly validated:
 *   design `8-01` §5 says the encoding's purpose is "static table lookup / manifest generation",
 *   and v1's callers are in-house -- rejecting a semantically unambiguous command just because
 *   the size bits disagree would turn "the encoding in the docs" into "a runtime landmine".
 *   Unknown type/nr = -ENOTSUP.
 *   Errors: -ENOTSUP (not this device's command / unimplemented) / -EINVAL (missing argument or
 *   unrepresentable baud rate).
 */
static int pl011_ioctl(void *sess, br_u32 cmd, void *arg)
{
    if (sess == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    pl011_sess_t *s = (pl011_sess_t *)sess;

    /* Wrong family magic => this command is not ours. Use -ENOTSUP, not -EINVAL: the command
     * itself may be legal (another device family), it is just unimplemented -- this must stay
     * separate from "the argument given to me is wrong" (SD-10). */
    if (BR_IOC_TYPE(cmd) != (br_u32)BR_UART_IOC_MAGIC) {
        return BR_ERR(ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_UART_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }

        /* Also sample overrun once (header (4)): the query honestly reflects bytes dropped since
         * the last read. The cost is possibly popping one extra status entry -- v1 exposes no
         * per-character error status, so no information is lost. */
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
        /* No-argument command: arg may be null and is not checked (if direction/size validation
         * is added later, arg still should not be required here) */
        pl011_drain_rx(s);
        return 0;

    case BR_UART_IOC_SET_BAUD: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }

        /* arg = br_u32 baud rate (by value, not a struct). Alignment is the caller's
         * responsibility (vfs passes a br_u32 object; -mstrict-align only constrains
         * compiler-generated accesses, it does not align the caller's pointer). */
        const br_u32 baud = *(const br_u32 *)arg;
        br_u32       ibrd = 0u;
        br_u32       fbrd = 0u;

        const int rc = pl011_divisors_for(baud, &ibrd, &fbrd);
        if (rc != 0) {
            return rc;   /* -EINVAL: not representable (incl. baud=0), hardware **untouched** */
        }

        pl011_program_divisors(ibrd, fbrd);
        s_dev.baud = baud;   /* update "current baud" only after successful programming */
        return 0;
    }

    default:
        /* Unknown nr (wrong magic already rejected above): explicitly "not supported", never
         * silently succeed */
        return BR_ERR(ENOTSUP);
    }
}

/*
 * poll(sess, events): v1 = **query** the ready bits (SD-7; no waiting, no wait-queue
 * registration).
 *   Set rules: RX has data => BR_POLLIN; TX not full => BR_POLLOUT.
 *   Unset bits **must be cleared** => assign rather than OR (the caller's events may be a reused
 *   buffer; "I answer only the state I see" -- keeping old values would report "no data" as "has
 *   data").
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
 * close(sess): free the session = release exclusivity.
 * The order is "destroy first, then release": reversed, under concurrency a second open could
 * catch a window where the previous session is freed but not yet removed from device state. v1 is
 * single-threaded; this order merely writes the invariant where it is visible.
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
 * suspend/resume: **always -ENOTSUP** (D22 reserved slot, design `8-01` §4 last part / O-S6).
 *   Why not "return 0 pretending success": returning 0 claims "after power-down / clock gating I
 *   save and restore the line settings and FIFO state" -- v1 has no PM caller and saves/restores
 *   no register state, so that would misreport capability.
 *   The slot exists (= non-NULL) because the ops layout is in the golden (D14): adding fields
 *   later = binary incompatibility, reserving now avoids breaking it.
 *   The error code is -ENOTSUP rather than -EINVAL: the command is legal and the situation is
 *   legal, this version simply lacks the capability (SD-10).
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

/* ops table (static const: layout and content are contract; every slot given explicitly, not
 * relying on C zero-init) */
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

/* =====================================================================
 * 7. Lifecycle hooks (name = symbol_prefix + phase; referenced by generated plugin_desc.c)
 * ===================================================================== */

/*
 * EARLY phase: **no action**. At this point there are no threads (cannot br_task_sleep) and no
 * heap (cannot br_malloc), and device registration waits for the CORE phase (the subclass
 * framework comes up first, then the heap). More importantly, PL011 line settings currently
 * belong to the platform early console -- "initializing the UART" here would alter the panic
 * channel's configuration (header (1)).
 */
int uart_pl011_early_init(void)
{
    return 0;
}

/*
 * CORE phase: register the cdev. Successful registration makes it appear in /dev (devfs
 * enumerates the dev-core registry).
 * rc is **propagated verbatim**: -EINVAL (bad name/ops) / -EEXIST (duplicate name) / -ENOSPC
 * (registry full) are all errors that must surface at boot; swallowing one means "the device
 * looks present but was never registered", the hardest class of silent failure to debug.
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
 * START phase: this device's conformance cases **do not run here** -- after ADR-0010 they are
 * driven uniformly by core's `br_plugin_manager_selftest()` through the descriptor's `.selftest`
 * hook (see src/uart_selftest.c).
 * Only one device summary line remains here as boot evidence (heap/threads/devfs are all up now,
 * and the device was registered in the CORE phase).
 * Returns 0 and **not** a failure count: pass/fail is decided by the gate reading the log output
 * of the selftest suite (see src/uart_selftest.c) -- returning non-zero from start would make the
 * plugin manager treat it as a "boot failure" and could lose the later phases and other plugins'
 * evidence along with it.
 */
int uart_pl011_start(void)
{
    br_log_info("uart-pl011: /dev/%s ready (PL011 @ 0x%08x, %u baud)",
                BR_UART_PL011_DEV_NAME, (br_u32)BR_PL011_BASE, BR_PL011_BAUD);
    return 0;
}

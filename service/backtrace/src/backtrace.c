/*
 * Backtrace capture implementation. The target captures the fp chain; the host symbolizes offline.
 * Build precondition: the whole image is compiled with -fno-omit-frame-pointer, else -O2 drops x29
 * as an ordinary register and the chain breaks at frame 0.
 * aarch64 (AAPCS64) frame chain: prologue `stp x29, x30, [sp, #-16]!; mov x29, sp` => [x29] is the
 * caller's fp and [x29 + 8] the return address; the stack grows down, so fp strictly increases.
 * br_bt_* are thread-only; the fault-site entry takes fp/pc from the exception frame and never
 * raises an exception itself.
 */
#include <br/debug/br_bt.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include "backtrace_internal.h"

/* Bounds-unknown fallback: lower bound = walker's SP, upper bound = SP + 1 MiB. The boot stack is
 * only tens of KiB, so 1 MiB covers every caller frame yet stops an fp smashed to a wild value. */
#define BT_FALLBACK_STACK_BYTES  (1024u * 1024u)

static br_uintptr_t s_stack_bottom;
static br_uintptr_t s_stack_top;     /* s_stack_top == 0 => bounds unset */

static br_bt_frame_t s_last[BR_BT_MAX_FRAMES];   /* last capture snapshot */
static br_u32        s_last_count;

/* Stack range for this walk: platform-set bounds if present, else the fallback window. The fallback
 * lower bound is the walker's own SP, below every caller frame, so it never kills a legit frame. */
static void bt_bounds(br_uintptr_t *lo, br_uintptr_t *hi)
{
    if (s_stack_top != 0u) {
        *lo = s_stack_bottom;
        *hi = s_stack_top;
        return;
    }

    br_uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    *lo = sp;
    *hi = sp + (br_uintptr_t)BT_FALLBACK_STACK_BYTES;
}

/*
 * Walk the fp chain from (fp, pc); frame 0 is the passed pair. All guards are checked before any
 * dereference: fp != 0, 16-byte aligned, inside the stack range, strictly increasing,
 * depth < BR_BT_MAX_DEPTH, frames < max. Returns the number of frames written to out.
 */
static br_u32 bt_walk(br_uintptr_t fp, br_uintptr_t pc,
                      br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }

    br_uintptr_t lo;
    br_uintptr_t hi;
    bt_bounds(&lo, &hi);

    br_u32 depth = 0u;

    while ((depth < max) && (depth < BR_BT_MAX_DEPTH)) {
        if (fp == 0u) {
            break;
        }
        if ((fp % 16u) != 0u) {
            break;
        }
        if ((fp < lo) || (fp >= hi)) {
            break;
        }
        if ((depth > 0u) && (fp <= out[depth - 1u].fp)) {
            break;   /* must increase strictly */
        }

        out[depth].pc    = pc;
        out[depth].fp    = fp;
        /* fp + 16 is the canonical frame slot (past the save area), not the real SP. */
        out[depth].sp    = fp + 16u;
        out[depth].depth = depth;
        depth++;

        if ((depth >= max) || (depth >= BR_BT_MAX_DEPTH)) {
            break;
        }

        /* volatile: the stack may change under us */
        const volatile br_uintptr_t *frame = (const volatile br_uintptr_t *)fp;
        const br_uintptr_t next_fp = frame[0];
        const br_uintptr_t ret_pc  = frame[1];

        if (next_fp <= fp) {
            break;   /* loop or smashed chain */
        }
        fp = next_fp;
        pc = ret_pc;
    }

    return depth;
}

/* Walk and store the static snapshot reused by br_bt_last/br_bt_print (avoids re-walking). */
static br_u32 bt_snapshot(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max)
{
    const br_u32 n = bt_walk(fp, pc, out, max);

    if (out != s_last) {
        const br_u32 k = (n < BR_BT_MAX_FRAMES) ? n : BR_BT_MAX_FRAMES;
        for (br_u32 i = 0u; i < k; i++) {
            s_last[i] = out[i];
        }
    }
    s_last_count = (n < BR_BT_MAX_FRAMES) ? n : BR_BT_MAX_FRAMES;
    return n;
}

/* noinline is a semantic requirement: frame 0's fp must be this function's own frame, else the
 * chain loses a level (TC-DBG-010). The header carries no attribute, so the ABI is unchanged. */
__attribute__((noinline))
br_u32 br_bt_capture(br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }

    br_uintptr_t fp;
    br_uintptr_t lr;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    __asm__ volatile("mov %0, x30" : "=r"(lr));

    /* Frame 0's pc is lr, the same return-address convention as the chain frames: no correction. */
    return bt_snapshot(fp, lr, out, max);
}

__attribute__((noinline))
br_u32 br_bt_capture_from(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }
    return bt_snapshot(fp, pc, out, max);
}

int br_bt_set_stack_bounds(br_uintptr_t bottom, br_uintptr_t top)
{
    if ((bottom == 0u) || (top <= bottom)) {
        return BR_ERR(EINVAL);
    }
    s_stack_bottom = bottom;
    s_stack_top    = top;
    return 0;
}

static void bt_emit(const br_bt_frame_t *fr, br_u32 n)
{
    for (br_u32 i = 0u; i < n; i++) {
        br_log_info("[BT] %u: pc=0x%lx fp=0x%lx sp=0x%lx",
                    fr[i].depth, fr[i].pc, fr[i].fp, fr[i].sp);
    }
    br_log_info("[BT] frames=%u", n);
}

br_u32 br_bt_print(void)
{
    /* Capture straight into the snapshot, saving a 512 B intermediate buffer */
    const br_u32 n = br_bt_capture(s_last, BR_BT_MAX_FRAMES);
    bt_emit(s_last, n);
    return n;
}

br_u32 br_bt_print_from(br_uintptr_t fp, br_uintptr_t pc)
{
    const br_u32 n = br_bt_capture_from(fp, pc, s_last, BR_BT_MAX_FRAMES);
    bt_emit(s_last, n);
    return n;
}

br_u32 br_bt_last_count(void)
{
    return s_last_count;
}

const br_bt_frame_t *br_bt_last(void)
{
    return s_last;
}

int br_bt_init(void)
{
    /* Clear the last-capture snapshot; the stack bounds are set elsewhere and must survive this
     * LATE-phase init. */
    for (br_u32 i = 0u; i < BR_BT_MAX_FRAMES; i++) {
        s_last[i].pc    = 0u;
        s_last[i].fp    = 0u;
        s_last[i].sp    = 0u;
        s_last[i].depth = 0u;
    }
    s_last_count = 0u;

    return 0;
}

/* Accessors for backtrace_selftest.c only: they touch the production state itself (no copy), so a
 * case can restore bounds to "unset" (0/0), which br_bt_set_stack_bounds rejects. */

void br_bt_internal_read_stack_bounds(br_uintptr_t *bottom, br_uintptr_t *top)
{
    if (bottom != BR_NULL) {
        *bottom = s_stack_bottom;
    }
    if (top != BR_NULL) {
        *top = s_stack_top;
    }
}

void br_bt_internal_restore_stack_bounds(br_uintptr_t bottom, br_uintptr_t top)
{
    s_stack_bottom = bottom;
    s_stack_top    = top;
}


/* Plugin lifecycle hooks referenced by the generated plugin_desc.c. Self-declared prototypes
 * satisfy -Wmissing-prototypes (the hook names come from the generator, not the public header). */
int backtrace_early_init(void);
int backtrace_init(void);
int backtrace_start(void);

/* EARLY phase: no action; the platform sets the stack bounds in its own early_init. */
int backtrace_early_init(void)
{
    return 0;
}

/* LATE phase: delegates to br_bt_init(). */
int backtrace_init(void)
{
    return br_bt_init();
}

int backtrace_start(void)
{
    return 0;
}

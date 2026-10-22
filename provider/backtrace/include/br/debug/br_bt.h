/*
 * Stack backtrace capture service (public surface): the target captures the fp chain, the host
 * symbolizes offline; no symbol table is embedded. Build precondition: the whole image is compiled
 * with -fno-omit-frame-pointer, else the x29 chain breaks. All br_bt_* are thread-only; the
 * fault-site entry takes fp/pc from the exception frame and never re-triggers an exception.
 */
#ifndef BR_DEBUG_BR_BT_H
#define BR_DEBUG_BR_BT_H

#include <br/core/br_types.h>

/* Max frames per capture; static buffer, no allocation. */
#define BR_BT_MAX_FRAMES   16u
/* Absolute fp-chain depth guard (cycle prevention). */
#define BR_BT_MAX_DEPTH    32u

/* One frame; pc is the return address of the call site (next instruction after return). */
typedef struct {
    br_uintptr_t pc;
    br_uintptr_t fp;
    br_uintptr_t sp;      /* derived from fp (canonical frame slot); 0 if not derivable */
    br_u32       depth;
} br_bt_frame_t;

/* LATE-phase init: clears the capture snapshot and returns 0; stack bounds are left untouched
 * (they are set separately, after init). */
int br_bt_init(void);

/*
 * Declare the stack range (optional): lets the walk stop on "fp out of bounds" instead of walking
 * off. Unset => only the BR_BT_MAX_DEPTH, 16-byte alignment and strictly-increasing guards apply.
 * Must be called after br_bt_init, which clears state. No caller in this prototype: stack bounds
 * are a platform fact and platform must not depend on this plugin, so the walk uses the fallback.
 */
int br_bt_set_stack_bounds(br_uintptr_t bottom, br_uintptr_t top);

/* Capture from the current context. Returns frames written; out==NULL or max==0 => 0. */
br_u32 br_bt_capture(br_bt_frame_t *out, br_u32 max);

/* Capture from a given site (exception frame / known fp); pc becomes frame 0's pc.
 * Stops on fp == 0, misaligned, out of range, non-monotonic or too deep. Returns count. */
br_u32 br_bt_capture_from(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max);

/* Capture and print "[BT] <i>: pc=0x<hex> fp=0x<hex> sp=0x<hex>" per frame, then "[BT] frames=<n>".
 * Returns the frame count. */
br_u32 br_bt_print(void);
br_u32 br_bt_print_from(br_uintptr_t fp, br_uintptr_t pc);

/* Snapshot of the last capture, so dump can reuse it without re-walking. */
br_u32 br_bt_last_count(void);
const br_bt_frame_t *br_bt_last(void);

#endif /* BR_DEBUG_BR_BT_H */

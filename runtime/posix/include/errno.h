/*
 * brickOS prototype v0.2.0 — POSIX error-code surface (provided by the runtime/posix plugin)
 *
 * ★★ This header **no longer defines any name or value** — both live in core:
 *      - values (single source of truth): `br/core/br_error.h` (verbatim from the Linux kernel,
 *        host `ft-test` cross-checks every code);
 *      - POSIX names:     `br/core/br_errno.h` (each written as a `BR_E*` alias).
 *    This header owns only the two things **unique** to the POSIX runtime:
 *      ① the `errno` **macro** (per-thread slot);
 *      ② this service's own rendering bound (`BR_POSIX_STRERROR_MAX`).
 *
 * ★ Why "names moved up to core" is right (ADR-0023): before that version the POSIX name table
 *   hung off this plugin ⇒ ① a product not selecting `runtime/posix` had no POSIX names at all
 *   (yet the discipline "internal error codes reuse POSIX numbering" should not depend on an
 *   optional plugin being present); ② third-party ports (uboot-fat / uboot-erofs) each had to
 *   copy a `#define EIO EIO` — both a second source of truth and a pointless edit to upstream.
 *
 * ★ `errno` is a **macro** pointing at a **per-thread** slot (since v0.2): the slot lives in
 *   this service's own thread record (`svc_thread_t` in `runtime/posix/src/posix.c`) and its
 *   address comes from `br_posix_errno()`. So the classic global-errno bug "thread A's failure
 *   read by thread B" no longer exists. core still has no generic TLS slot (`11-02` P-4), but
 *   the pthread layer already has a per-thread record table ⇒ core need not change
 *   (ADR-0019). `pthread_key_*` uses slots in the same table.
 *   ⚠ So **never** write `extern int errno;` again — POSIX itself defines `errno` as a macro.
 *   ★ This piece **must** stay in this plugin: it needs a "per-thread slot", which is this
 *      service's state (core has no TLS). Codes in core, slot in the POSIX runtime — that split
 *      is "internal error codes decoupled from the POSIX runtime".
 *
 * ★ Usage follows POSIX: a failing function returns -1 and sets `errno`; success does **not**
 *   guarantee errno is cleared.
 */
#ifndef BR_POSIX_ERRNO_H
#define BR_POSIX_ERRNO_H

/* ★ Names and values both come from core (a re-export ⇒ independent of this plugin's presence). */
#include <br/core/br_errno.h>

/* Address of the per-thread errno slot (defined in src/posix.c; unregistered threads lazily get a record). */
int *br_posix_errno(void);

/* POSIX defines `errno` as a modifiable lvalue ⇒ the macro expands to a dereference. */
#define errno   (*br_posix_errno())

/* The success code is not an E*: this prototype uses 0 (as does POSIX). Keep the old name (it is an existing symbol this plugin exports). */
#define BR_POSIX_OK   0

/* Maximum `strerror` buffer (including trailing NUL) — a caller buffer at least this large is never truncated. */
#define BR_POSIX_STRERROR_MAX  40

#endif /* BR_POSIX_ERRNO_H */

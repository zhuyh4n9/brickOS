/*
 * brickOS prototype v0.2.0 — file table contract (public face of the
 * framework/file-table plugin)
 *
 * Design basis (design repo):
 *   - `11-01-service.md` §1 (D18): POSIX **fd table = sole owner of shared state**;
 *   - `1-01-architecture.md` §7.2 rule 3 (one owner for shared state) + D19 (the capability
 *     framework exists as a **plugin** — "framework piece = plugin identity, core discipline");
 *   - `3-06-service-mgmt.md` §2 (three shared-state owners: fd table / socket table / trace ring);
 *   - `7-01-vfs.md` §1/§4.1 (fd table entries uniformly hold `br_file_t*`; `br_open` is the
 *     low-level primitive of the fd table).
 *
 * ============================== what this file is, and is not ==============================
 * **Is**: a **static fixed-size table** of "small integer → `br_file_t*`" plus all its semantics
 *       (lowest free fd / table full / invalid fd / dup sharing / "last reference" on release).
 * **Is not**: POSIX `open/read/write` — those are the face of **runtime/posix** (runtime/posix
 *       maps `open()` to `br_open_err()` + this table's `br_ft_alloc()`, and `close()` to
 *       `br_ft_release()` + `br_file_close()`). This file **only does the table**, not file I/O.
 *
 * ## Why the fd table is its own piece (not left inside runtime/posix)
 *   The design says "fd table = sole owner of svc-posix" (design-side plugin name; this tree
 *   calls it `runtime/posix`, see ADR-0015). That rule protects **single ownership** (rule 3),
 *   not "the owner must be the POSIX service". After extracting the table into a framework piece:
 *     ① **ownership is clearer** — the state lives in `framework/file-table`, the only place in
 *        the tree that writes `s_slots`; runtime/posix becomes a **consumer** and no longer
 *        keeps a table of its own;
 *     ② the v2 **socket table** can reuse the same table semantics (design `3-06` §2 puts their
 *        ownership side by side) instead of rewriting "lowest free fd / dup / last reference";
 *     ③ the table is **pure algorithm** (no device, no FS, no POSIX), so boundaries such as
 *        "table full / dup2 displacement / reference emptiness" run on the host without QEMU.
 *
 * ## Zero dependencies (deliberate)
 *   `br_file_t` is only **forward-declared** — this file never dereferences it, only holds and
 *   compares pointers. So it has **no `[[dep]]`** (same shape as `framework/vfs-core`: "empty
 *   deps is the design result, not an omission") and does not pull vfs-core into any composition.
 *
 * ## Reference semantics: refcount = **number of live slots** (no counter)
 *   POSIX's "same open file description" is here simply the **same `br_file_t*`**: two fds
 *   pointing at one pointer ⇒ shared offset and state (exactly `dup`/`dup2` semantics). Thus
 *   "has the last reference gone" **is answered by the table itself** (scan the live slots),
 *   with no `refcount` field to keep in sync with slots — one field less, one class of "count
 *   drift" defect less. The cost is the O(n) scan in `release`/`release_all` (n = `BR_FT_MAX` =
 *   32, negligible).
 *
 * ## Lifetime ownership (why `release` **hands back** the handle)
 *   This file **does not call** `br_file_close()`: that would make vfs-core a real dependency
 *   and let the "table" reach past its own boundary to manage file lifetime. So
 *   `br_ft_release(fd, &last_file)` returns the handle to the caller when the **last reference
 *   disappears**, and runtime/posix calls `br_file_close()`:
 *
 *       int close(int fd) {
 *           br_file_t *f = BR_NULL;
 *           int rc = br_ft_release(fd, &f);      // -EBADF ⇒ userspace EBADF
 *           if (rc != 0) { return rc; }
 *           if (f != BR_NULL) { return br_file_close(f); }   // last reference: really close
 *           return 0;                                        // only dropped one fd
 *       }
 *
 *   This is a **verbatim translation** of POSIX `close(2)` semantics, not "the table closes the
 *   file along the way".
 */
#ifndef BR_FT_BR_FT_H
#define BR_FT_BR_FT_H

#include <br/core/br_types.h>

/* Forward declaration: `br_file_t` is an opaque handle (vfs-core D14 contract). Only pointers here. */
typedef struct br_file br_file_t;

/* ==================================================================== upper bound */

/* Upper bound on fd slots (compile time; aligned with brick.toml `[[res]] ram` — 32 × 16 B = 512 B).
 * POSIX 0/1/2 (stdin/stdout/stderr) are not special slots: runtime/posix installs three standard
 * streams in init, so they **naturally** take the lowest three numbers — no "reserved range" needed. */
#define BR_FT_MAX   32u

/* Return value for "no such fd" (`br_ft_fd_at`; keeps it distinct from "fd 0"). */
#define BR_FT_NONE  (-1)

/* ==================================================================== lifecycle */

/* Clear the whole table, return the **number of discarded slots** (0 = already empty).
 * ★ It does **not** hand back handles — deliberately honest: "zero the table" and "close the
 *   files" are two different things, and this file owns only the former. Use
 *   `br_ft_release_all()` on the normal exit path; this function is only for plugin init
 *   (the static table is BSS, already empty) and test isolation. Non-zero means **someone forgot
 *   to close a file**. */
br_u32 br_ft_reset(void);

/* Number of live fds (observation). */
br_u32 br_ft_count(void);

/* The `index`-th **live** fd (ascending fd order; `index` from 0, out of range ⇒ `BR_FT_NONE`).
 * Use = observation/iteration (dump "who has what open"); same shape as `br_mount_path_at`. */
int br_ft_fd_at(br_u32 index);

/* ==================================================================== table ops */

/*
 * Allocate a slot holding `{file, flags}`, return the **lowest free fd**.
 *   `file == BR_NULL` ⇒ `-EINVAL` (an empty slot is not a "legal empty file"; see the "live test" below).
 *   Table full ⇒ `-EMFILE` (process-level fd exhaustion; `ENFILE` is system-level, this is a
 *   single-process table).
 *   `flags` is **opaque** here: the table does not interpret it, only stores and returns it
 *   (runtime/posix owns interpretation).
 */
int br_ft_alloc(br_file_t *file, br_u32 flags);

/* Get the handle (changes no state). Invalid fd / empty slot ⇒ `BR_NULL`.
 * ★ **Live test = `file != BR_NULL`**: there is no separate used bit, so "how many fds are in the
 *   table" and "how many non-null handles are in the table" always give the same answer (they
 *   cannot disagree). */
br_file_t *br_ft_get(int fd);

/* Read/write the flags in the slot. Invalid fd ⇒ `-EBADF`; `out == BR_NULL` ⇒ `-EINVAL`.
 * `br_ft_set_flags` is a **whole replacement** (not "change status bits only"): OR back any
 *   access mode you want to keep — the access mode is fixed at "open time", and a setter able to
 *   change it would be the dangerous thing. */
int br_ft_get_flags(int fd, br_u32 *out);
int br_ft_set_flags(int fd, br_u32 flags);

/*
 * Release one fd (= first half of POSIX `close`).
 *   Invalid fd ⇒ `-EBADF`, table unchanged.
 *   `last_file` may be null; when non-null: the **last reference to this handle disappeared** ⇒
 *   write the handle (caller owns `br_file_close`); another fd still points at it ⇒ write
 *   `BR_NULL` (caller does nothing).
 *   Return 0 = success (whether or not a handle was handed back).
 */
int br_ft_release(int fd, br_file_t **last_file);

/*
 * Release **all** fds (exit/teardown path; semantics = repeated `br_ft_release`).
 *   `out[0..cap)` collects handles whose "last reference disappeared", **exactly one entry per
 *   handle** (deduplicated by pointer, so the live fd count may exceed `cap` and still fit).
 *   Returns the number written; if it does not fit ⇒ `-ENOSPC` and the **table is unchanged**
 *   (dry run first — clearing halfway is worse: the caller gets neither all handles nor the
 *   original state).
 *   `out == BR_NULL && cap != 0` ⇒ `-EINVAL`.
 */
int br_ft_release_all(br_file_t **out, br_u32 cap);

/* ==================================================================== dup family */

/*
 * `dup(2)`: new fd points at the same handle (shared offset and state); flags are copied as a
 *   per-slot snapshot.
 *   Returns the lowest free fd, or `-EBADF` (bad oldfd) / `-EMFILE` (table full — oldfd is
 *   **untouched** in that case).
 *
 * ★ The two identities of flags (callers must know): the access mode is **per fd** (a dup'ed fd
 *   must not become writable just by sharing), while `O_APPEND`/`O_NONBLOCK` belong to the
 *   **open file description** per POSIX (shared). This file can store only one snapshot; "the
 *   shared bits" are kept on the `br_file_t` side (`br_file_flags`) and by the driver —
 *   runtime/posix `F_SETFL` must land shared bits on the file side too, not only in this table.
 *   This is a **known semantic boundary**, recorded in `docs/decisions/0012-…`.
 */
int br_ft_dup(int oldfd);

/*
 * `dup2(2)`: copy `oldfd` to a **specified** `newfd`.
 *   Bad `oldfd` ⇒ `-EBADF`; `newfd` out of range ⇒ `-EBADF`; `oldfd == newfd` ⇒ return newfd
 *   (no change).
 *   `newfd` occupied ⇒ equivalent to `close(newfd)` first: if the displaced handle has no other
 *   reference it is handed back via `displaced` (nullable) for the caller to `br_file_close`,
 *   otherwise nothing is handed back.
 *   Returns `newfd` ≥ 0, or a negative errno.
 * ★ `displaced` is written on **every** return path (failure / nothing to hand back ⇒
 *   `BR_NULL`) — so the caller never reads an uninitialized value; distinguishing "nobody was
 *   displaced" from "the displaced handle has other references" is unnecessary: both cases mean
 *   "do nothing".
 */
int br_ft_dup2(int oldfd, int newfd, br_file_t **displaced);

#endif /* BR_FT_BR_FT_H */

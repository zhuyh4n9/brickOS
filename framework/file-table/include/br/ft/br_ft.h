/*
 * File table contract (public surface of framework/file-table): static fixed-size "small int ->
 * br_file_t*" map with POSIX fd semantics (lowest free fd, dup/dup2, last reference on release).
 * A handle's refcount is simply the number of live slots pointing at it (no counter field); release
 * hands the handle back when the last slot goes, so this piece never calls br_file_close().
 */
#ifndef BR_FT_BR_FT_H
#define BR_FT_BR_FT_H

#include <br/core/br_types.h>

/* br_file_t is opaque here: only pointers are held and compared, never dereferenced. */
typedef struct br_file br_file_t;

/* Upper bound on fd slots (compile time; 32 x 16 B = 512 B, matching brick.toml [[res]] ram). The
 * numbers 0/1/2 are not reserved slots: standard streams simply take the lowest three. */
#define BR_FT_MAX   32u

/* Return value for "no such fd" (br_ft_fd_at; distinct from fd 0). */
#define BR_FT_NONE  (-1)

/* Clear the whole table; returns the number of discarded slots (0 = already empty). Does not hand
 * back handles -- use br_ft_release_all() on the exit path; a non-zero return means some file was
 * left unclosed. */
br_u32 br_ft_reset(void);

/* Number of live fds. */
br_u32 br_ft_count(void);

/* The index-th live fd in ascending fd order (index from 0; out of range => BR_FT_NONE). */
int br_ft_fd_at(br_u32 index);

/*
 * Allocate a slot holding {file, flags}, returning the lowest free fd. file == BR_NULL => -EINVAL.
 * Table full => -EMFILE (process-level fd exhaustion; ENFILE is system-level, this is a
 * single-process table). flags is opaque: stored and returned, never interpreted here.
 */
int br_ft_alloc(br_file_t *file, br_u32 flags);

/* Handle in the slot, or BR_NULL (changes no state). Presence test is file != BR_NULL: there is no
 * separate used bit, so fd count and non-null handle count cannot disagree. */
br_file_t *br_ft_get(int fd);

/* Read/write the slot flags. Invalid fd => -EBADF; out == BR_NULL => -EINVAL. set_flags is a whole
 * replacement of the per-fd snapshot, not a bit update. */
int br_ft_get_flags(int fd, br_u32 *out);
int br_ft_set_flags(int fd, br_u32 flags);

/*
 * Release one fd (first half of POSIX close). Invalid fd => -EBADF, table unchanged. When last_file
 * is non-null and the last reference to the handle disappeared it receives the handle (caller then
 * owns br_file_close); if another fd still points at it, BR_NULL. Returns 0 either way.
 */
int br_ft_release(int fd, br_file_t **last_file);

/*
 * Release all fds (exit/teardown; semantics = repeated br_ft_release). out[0..cap) collects the
 * handles whose last reference disappeared, exactly one entry per handle (deduplicated by pointer).
 * Returns the number written; if it does not fit => -ENOSPC and the table is unchanged (dry run
 * first). out == BR_NULL && cap != 0 => -EINVAL.
 */
int br_ft_release_all(br_file_t **out, br_u32 cap);

/*
 * dup(2): new fd points at the same handle (shared offset and state); flags are copied as a
 * per-slot snapshot. Returns the lowest free fd, -EBADF (bad oldfd) or -EMFILE (table full;
 * oldfd untouched).
 * The access mode is per fd, while O_APPEND/O_NONBLOCK belong to the open file description: those
 * shared bits live on the br_file_t side (br_file_flags) and runtime/posix F_SETFL must update them
 * there too.
 */
int br_ft_dup(int oldfd);

/*
 * dup2(2): copy oldfd to newfd. Bad oldfd or out-of-range newfd => -EBADF; oldfd == newfd => newfd.
 * An occupied newfd is closed first: if the displaced handle has no other reference it is handed
 * back via displaced (nullable) for br_file_close. displaced is written on every return path
 * (BR_NULL when nothing is handed back), so callers never read an uninitialized value. Returns
 * newfd >= 0 or a negative errno.
 */
int br_ft_dup2(int oldfd, int newfd, br_file_t **displaced);

#endif /* BR_FT_BR_FT_H */

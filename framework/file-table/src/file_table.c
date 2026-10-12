/*
 * brickOS prototype v0.2.0 — file table implementation (framework/file-table plugin)
 *
 * Contract and all semantics are in `include/br/ft/br_ft.h` (this file holds mechanism only); per-item
 * rulings are in `docs/decisions/0012-file-table-and-errno.md`.
 *
 * ## Six things, matching the contract header one for one
 *   ① static fixed-size table + "presence test = handle non-NULL" (no separate used bit)
 *   ② lowest free fd allocation / table full -EMFILE / NULL handle -EINVAL
 *   ③ invalid fd always -EBADF (out-of-range and empty slot share one code — POSIX does not distinguish them, and distinguishing would leak information)
 *   ④ refcount = number of present slots (determined by scanning the table, no counter); release reports the "last reference"
 *   ⑤ dup / dup2 (POSIX semantics, including dup2 target displacement and the displaced report)
 *   ⑥ release_all (exit path; dry run first, table unchanged if it does not fit)
 *
 * ## Concurrency (why IRQ masking, not a spinlock)
 *   The table is shared state called concurrently by multiple threads (and a future socket layer). Protection
 *   granularity is one table operation, the critical section is a few instructions (32-slot scan), and it never sleeps inside
 *   the critical section — so IRQ masking (core's `br_irq_lock`, design 3-02 §6.4, nesting-safe) is the correct and cheapest
 *   form: no object, no init, no "lock not initialized" class of defects.
 *   The basis for `sched_class = SAFE_PREEMPT` is the same: no sleeping + full critical-section coverage => preemption-safe.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>

/* Lifecycle hook prototypes (the generated code derives and references them from `symbol_prefix` = `file_table_`;
 * `-Wmissing-prototypes` is satisfied by the prototypes here, same approach as vfs-core). */
int file_table_early_init(void);
int file_table_init(void);
int file_table_start(void);

/* ==================================================================== Static table */

typedef struct ft_slot {
    br_file_t *file;    /* BR_NULL = empty slot (this is the presence test) */
    br_u32     flags;   /* opaque: the table only stores and returns it; the consumer (runtime/posix) interprets it */
} ft_slot_t;

static ft_slot_t s_slots[BR_FT_MAX];
static br_u32    s_count;   /* always equals the number of non-empty slots (invariant maintained solely here) */

_Static_assert(BR_FT_MAX > 0u, "BR_FT_MAX must be positive");
_Static_assert((sizeof(s_slots) + sizeof(s_count)) <= 1024u,
               "file table exceeds the 1 KiB budget declared in brick.toml [[res]]");

/* ==================================================================== Internal helpers */

static br_bool ft_in_range(int fd)
{
    return ((fd >= 0) && ((br_u32)fd < BR_FT_MAX)) ? BR_TRUE : BR_FALSE;
}

static br_bool ft_in_use(br_u32 i)
{
    return (s_slots[i].file != BR_NULL) ? BR_TRUE : BR_FALSE;
}

/* Does any fd other than `skip` point to the same handle (= does the handle have another reference)? */
static br_bool ft_shared_with_others(br_file_t *file, int skip)
{
    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        if ((int)i == skip) {
            continue;
        }
        if ((ft_in_use(i) == BR_TRUE) && (s_slots[i].file == file)) {
            return BR_TRUE;
        }
    }
    return BR_FALSE;
}

/* Detach one slot (caller holds the lock and guarantees it is non-empty). */
static void ft_clear_slot(br_u32 i)
{
    s_slots[i].file  = BR_NULL;
    s_slots[i].flags = 0u;
    s_count--;
}

/* ==================================================================== Lifecycle */

br_u32 br_ft_reset(void)
{
    const br_irq_state_t st = br_irq_lock();
    br_u32 dropped = 0u;

    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        if (ft_in_use(i) == BR_TRUE) {
            dropped++;
        }
        s_slots[i].file  = BR_NULL;
        s_slots[i].flags = 0u;
    }
    s_count = 0u;

    br_irq_unlock(st);
    return dropped;
}

br_u32 br_ft_count(void)
{
    const br_irq_state_t st = br_irq_lock();
    const br_u32 n = s_count;
    br_irq_unlock(st);
    return n;
}

int br_ft_fd_at(br_u32 index)
{
    const br_irq_state_t st = br_irq_lock();
    br_u32 seen = 0u;
    int    found = BR_FT_NONE;

    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        if (ft_in_use(i) == BR_FALSE) {
            continue;
        }
        if (seen == index) {
            found = (int)i;
            break;
        }
        seen++;
    }

    br_irq_unlock(st);
    return found;
}

/* ==================================================================== Table operations */

int br_ft_alloc(br_file_t *file, br_u32 flags)
{
    if (file == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    int fd = BR_FT_NONE;

    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        if (ft_in_use(i) == BR_FALSE) {
            s_slots[i].file  = file;
            s_slots[i].flags = flags;
            s_count++;
            fd = (int)i;
            break;
        }
    }

    br_irq_unlock(st);
    return (fd >= 0) ? fd : BR_ERR(EMFILE);
}

br_file_t *br_ft_get(int fd)
{
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_NULL;
    }

    const br_irq_state_t st = br_irq_lock();
    br_file_t *f = s_slots[(br_u32)fd].file;
    br_irq_unlock(st);
    return f;
}

int br_ft_get_flags(int fd, br_u32 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_ERR(EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    const br_bool ok = ft_in_use((br_u32)fd);

    if (ok == BR_TRUE) {
        *out = s_slots[(br_u32)fd].flags;
    }

    br_irq_unlock(st);
    return (ok == BR_TRUE) ? 0 : BR_ERR(EBADF);
}

int br_ft_set_flags(int fd, br_u32 flags)
{
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_ERR(EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    const br_bool ok = ft_in_use((br_u32)fd);

    if (ok == BR_TRUE) {
        s_slots[(br_u32)fd].flags = flags;
    }

    br_irq_unlock(st);
    return (ok == BR_TRUE) ? 0 : BR_ERR(EBADF);
}

int br_ft_release(int fd, br_file_t **last_file)
{
    if (last_file != BR_NULL) {
        *last_file = BR_NULL;
    }
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_ERR(EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    int rc = 0;

    if (ft_in_use((br_u32)fd) == BR_FALSE) {
        rc = BR_ERR(EBADF);
    } else {
        br_file_t  *f      = s_slots[(br_u32)fd].file;
        const br_bool shared = ft_shared_with_others(f, fd);

        ft_clear_slot((br_u32)fd);

        if ((last_file != BR_NULL) && (shared == BR_FALSE)) {
            *last_file = f;                 /* last reference: hand to the caller for br_file_close */
        }
    }

    br_irq_unlock(st);
    return rc;
}

int br_ft_release_all(br_file_t **out, br_u32 cap)
{
    if ((out == BR_NULL) && (cap != 0u)) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    br_bool first_of[BR_FT_MAX];
    br_u32 distinct = 0u;
    int    rc;

    /* First pass (before touching anything): mark the first slot of each handle and count the
     * pointer-deduplicated entries. Dedup must finish before clearing slots — clearing as we go would erase earlier duplicates and report one handle twice. */
    for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
        first_of[i] = BR_FALSE;
        if (ft_in_use(i) == BR_FALSE) {
            continue;
        }
        br_bool first = BR_TRUE;
        for (br_u32 j = 0u; j < i; j++) {
            if ((ft_in_use(j) == BR_TRUE) && (s_slots[j].file == s_slots[i].file)) {
                first = BR_FALSE;
                break;
            }
        }
        first_of[i] = first;
        if (first == BR_TRUE) {
            distinct++;
        }
    }

    if (distinct > cap) {
        rc = BR_ERR(ENOSPC);             /* does not fit => table unchanged (partial clearing is worse) */
    } else {
        br_u32 n = 0u;
        for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
            if (ft_in_use(i) == BR_FALSE) {
                continue;
            }
            if (first_of[i] == BR_TRUE) {
                out[n] = s_slots[i].file;
                n++;
            }
            ft_clear_slot(i);
        }
        rc = (int)n;
    }

    br_irq_unlock(st);
    return rc;
}

/* ==================================================================== dup family */

int br_ft_dup(int oldfd)
{
    if (ft_in_range(oldfd) == BR_FALSE) {
        return BR_ERR(EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    int rc;

    if (ft_in_use((br_u32)oldfd) == BR_FALSE) {
        rc = BR_ERR(EBADF);
    } else {
        br_file_t  *f  = s_slots[(br_u32)oldfd].file;
        const br_u32 fl = s_slots[(br_u32)oldfd].flags;

        rc = BR_ERR(EMFILE);             /* on a full table oldfd is untouched (POSIX: failure leaves no side effects) */
        for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
            if (ft_in_use(i) == BR_FALSE) {
                s_slots[i].file  = f;
                s_slots[i].flags = fl;      /* snapshot copy (shared bit: see "flags has two identities" in the contract header) */
                s_count++;
                rc = (int)i;
                break;
            }
        }
    }

    br_irq_unlock(st);
    return rc;
}

int br_ft_dup2(int oldfd, int newfd, br_file_t **displaced)
{
    if (displaced != BR_NULL) {
        *displaced = BR_NULL;
    }
    if ((ft_in_range(oldfd) == BR_FALSE) || (ft_in_range(newfd) == BR_FALSE)) {
        return BR_ERR(EBADF);
    }

    if (oldfd == newfd) {
        /* POSIX: dup2(fd, fd) returns fd itself when fd is valid (without closing it). */
        return (br_ft_get(oldfd) != BR_NULL) ? newfd : BR_ERR(EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    int rc;

    if (ft_in_use((br_u32)oldfd) == BR_FALSE) {
        rc = BR_ERR(EBADF);
    } else {
        /* ① target occupied => first do the equivalent of close(newfd). */
        if (ft_in_use((br_u32)newfd) == BR_TRUE) {
            br_file_t  *victim = s_slots[(br_u32)newfd].file;
            const br_bool shared = ft_shared_with_others(victim, newfd);

            ft_clear_slot((br_u32)newfd);

            if ((displaced != BR_NULL) && (shared == BR_FALSE)) {
                *displaced = victim;
            }
        }
        /* ② put oldfd's handle on newfd (both fds now share one open file description). */
        s_slots[(br_u32)newfd].file  = s_slots[(br_u32)oldfd].file;
        s_slots[(br_u32)newfd].flags = s_slots[(br_u32)oldfd].flags;
        s_count++;
        rc = newfd;
    }

    br_irq_unlock(st);
    return rc;
}

/* ==================================================================== Lifecycle hooks */

int file_table_early_init(void)
{
    /* EARLY: the static table is BSS => already empty. A nonzero return means some plugin touched the table before EARLY. */
    const br_u32 dropped = br_ft_reset();

    if (dropped != 0u) {
        br_log_warn("file-table: early_init dropped %u unclosed fds", dropped);
    }
    return 0;
}

int file_table_init(void)
{
    /* Nothing to initialize here: the table is BSS and there are no external dependencies (zero [[dep]]).
     * Deliberately no empty API — same as vfs-core's init (R-S4: every line in a framework piece must have a consumer). */
    return 0;
}

int file_table_start(void)
{
    /* START phase leaves one line of startup evidence (table capacity + fds already present — the latter is normally 0:
     * this piece does not install standard streams, and `runtime/posix` deliberately does not either (v1 has no process model, ADR-0014 §2.9). */
    br_log_info("file-table: cap=%u fd slots, %u open", (br_u32)BR_FT_MAX, br_ft_count());
    return 0;
}

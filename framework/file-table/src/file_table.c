/*
 * brickOS prototype v0.2.0 — 文件表实现(framework/file-table 插件)
 *
 * 契约与全部语义见 `include/br/ft/br_ft.h`(本文件只放机制); 逐条裁定见
 * `docs/decisions/0012-file-table-and-errno.md`。
 *
 * ## 六件事, 与契约头一一对应
 *   ① 静态定长表 + "在场判据 = 句柄非空"(没有独立的 used 位)
 *   ② 最小可用 fd 分配 / 表满 -EMFILE / 空句柄 -EINVAL
 *   ③ 非法 fd 一律 -EBADF(越界与空槽**同一个码** —— POSIX 不区分, 区分了反而泄漏信息)
 *   ④ 引用数 = 在场槽位个数(扫表判定, 不设计数器); release 回报"最后一个引用"
 *   ⑤ dup / dup2(POSIX 语义, 含 dup2 的目标顶替与 displaced 回报)
 *   ⑥ release_all(退出路径; 先干跑, 装不下则表不变)
 *
 * ## 并发(为什么是关中断而不是 spinlock)
 *   表是**共享状态**, 会被多线程(以及将来的 socket 层)并发调用。保护的粒度是"一次
 *   表操作", 临界区只有几条指令(32 槽扫描), 且**从不在临界区里睡眠** —— 于是关中断
 *   (core 的 `br_irq_lock`, 设计 3-02 §6.4, 嵌套安全)就是正确且最省的形态:
 *   不需要对象、不需要 init、不会有"锁没初始化"这类缺陷。
 *   `sched_class = SAFE_PREEMPT` 的依据也在这里: 无睡眠 + 临界区全覆盖 ⇒ 抢占下安全。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>

/* 生命周期钩子原型(生成物按 `symbol_prefix` = `file_table_` 推导并引用它们;
 * `-Wmissing-prototypes` 由这里的原型满足, 与 vfs-core 同法)。 */
int file_table_early_init(void);
int file_table_init(void);
int file_table_start(void);

/* ==================================================================== 静态表 */

typedef struct ft_slot {
    br_file_t *file;    /* BR_NULL = 空槽(**这就是**在场判据) */
    br_u32     flags;   /* 不透明: 表只存与还, 解释权归消费者(runtime/posix) */
} ft_slot_t;

static ft_slot_t s_slots[BR_FT_MAX];
static br_u32    s_count;   /* 恒等于"非空槽位个数"(不变量, 由本文件独占维护) */

_Static_assert(BR_FT_MAX > 0u, "BR_FT_MAX 必须为正");
_Static_assert((sizeof(s_slots) + sizeof(s_count)) <= 1024u,
               "文件表超出 plugin.toml [[res]] 声明的 1 KiB 预算");

/* ==================================================================== 内部小件 */

static br_bool ft_in_range(int fd)
{
    return ((fd >= 0) && ((br_u32)fd < BR_FT_MAX)) ? BR_TRUE : BR_FALSE;
}

static br_bool ft_in_use(br_u32 i)
{
    return (s_slots[i].file != BR_NULL) ? BR_TRUE : BR_FALSE;
}

/* 除 `skip` 之外还有别的 fd 指向同一句柄吗(= 该句柄还有别的引用吗)? */
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

/* 摘掉一个槽(调用方持锁并保证它非空)。 */
static void ft_clear_slot(br_u32 i)
{
    s_slots[i].file  = BR_NULL;
    s_slots[i].flags = 0u;
    s_count--;
}

/* ==================================================================== 生命周期 */

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

/* ==================================================================== 表操作 */

int br_ft_alloc(br_file_t *file, br_u32 flags)
{
    if (file == BR_NULL) {
        return BR_ERR(BR_EINVAL);
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
    return (fd >= 0) ? fd : BR_ERR(BR_EMFILE);
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
        return BR_ERR(BR_EINVAL);
    }
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_ERR(BR_EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    const br_bool ok = ft_in_use((br_u32)fd);

    if (ok == BR_TRUE) {
        *out = s_slots[(br_u32)fd].flags;
    }

    br_irq_unlock(st);
    return (ok == BR_TRUE) ? 0 : BR_ERR(BR_EBADF);
}

int br_ft_set_flags(int fd, br_u32 flags)
{
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_ERR(BR_EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    const br_bool ok = ft_in_use((br_u32)fd);

    if (ok == BR_TRUE) {
        s_slots[(br_u32)fd].flags = flags;
    }

    br_irq_unlock(st);
    return (ok == BR_TRUE) ? 0 : BR_ERR(BR_EBADF);
}

int br_ft_release(int fd, br_file_t **last_file)
{
    if (last_file != BR_NULL) {
        *last_file = BR_NULL;
    }
    if (ft_in_range(fd) == BR_FALSE) {
        return BR_ERR(BR_EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    int rc = 0;

    if (ft_in_use((br_u32)fd) == BR_FALSE) {
        rc = BR_ERR(BR_EBADF);
    } else {
        br_file_t  *f      = s_slots[(br_u32)fd].file;
        const br_bool shared = ft_shared_with_others(f, fd);

        ft_clear_slot((br_u32)fd);

        if ((last_file != BR_NULL) && (shared == BR_FALSE)) {
            *last_file = f;                 /* 最后一个引用: 交调用方去 br_file_close */
        }
    }

    br_irq_unlock(st);
    return rc;
}

int br_ft_release_all(br_file_t **out, br_u32 cap)
{
    if ((out == BR_NULL) && (cap != 0u)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    br_bool first_of[BR_FT_MAX];
    br_u32 distinct = 0u;
    int    rc;

    /* 第一趟(**动手之前**): 标出"每个句柄的头一个槽", 并数出按指针去重的条数。
     * 去重表必须在清槽之前算完 —— 边清边判会把先前的副本抹掉, 同一个句柄就被回报两次。 */
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
        rc = BR_ERR(BR_ENOSPC);             /* 装不下 ⇒ 表**不变**(半途清空更糟) */
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

/* ==================================================================== dup 族 */

int br_ft_dup(int oldfd)
{
    if (ft_in_range(oldfd) == BR_FALSE) {
        return BR_ERR(BR_EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    int rc;

    if (ft_in_use((br_u32)oldfd) == BR_FALSE) {
        rc = BR_ERR(BR_EBADF);
    } else {
        br_file_t  *f  = s_slots[(br_u32)oldfd].file;
        const br_u32 fl = s_slots[(br_u32)oldfd].flags;

        rc = BR_ERR(BR_EMFILE);             /* 表满时 oldfd **不动**(POSIX: 失败不留副作用) */
        for (br_u32 i = 0u; i < BR_FT_MAX; i++) {
            if (ft_in_use(i) == BR_FALSE) {
                s_slots[i].file  = f;
                s_slots[i].flags = fl;      /* 快照复制(共享位见契约头的"flags 两个身份") */
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
        return BR_ERR(BR_EBADF);
    }

    if (oldfd == newfd) {
        /* POSIX: dup2(fd, fd) 在 fd 有效时返回 fd 本身(**不关闭**)。 */
        return (br_ft_get(oldfd) != BR_NULL) ? newfd : BR_ERR(BR_EBADF);
    }

    const br_irq_state_t st = br_irq_lock();
    int rc;

    if (ft_in_use((br_u32)oldfd) == BR_FALSE) {
        rc = BR_ERR(BR_EBADF);
    } else {
        /* ① 目标已占用 ⇒ 先等价 close(newfd)。 */
        if (ft_in_use((br_u32)newfd) == BR_TRUE) {
            br_file_t  *victim = s_slots[(br_u32)newfd].file;
            const br_bool shared = ft_shared_with_others(victim, newfd);

            ft_clear_slot((br_u32)newfd);

            if ((displaced != BR_NULL) && (shared == BR_FALSE)) {
                *displaced = victim;
            }
        }
        /* ② 把 oldfd 的句柄放到 newfd 上(两个 fd 现在共享同一 open file description)。 */
        s_slots[(br_u32)newfd].file  = s_slots[(br_u32)oldfd].file;
        s_slots[(br_u32)newfd].flags = s_slots[(br_u32)oldfd].flags;
        s_count++;
        rc = newfd;
    }

    br_irq_unlock(st);
    return rc;
}

/* ==================================================================== 生命周期钩子 */

int file_table_early_init(void)
{
    /* EARLY: 静态表是 BSS ⇒ 本来就是空的。返回非 0 说明有插件在 EARLY 之前就动了表。 */
    const br_u32 dropped = br_ft_reset();

    if (dropped != 0u) {
        br_log_warn("file-table: early_init 丢弃了 %u 个未关闭的 fd", dropped);
    }
    return 0;
}

int file_table_init(void)
{
    /* 本件没有要初始化的状态: 表是 BSS, 也没有外部依赖(零 [[dep]])。
     * 刻意不造空 API —— 与 vfs-core 的 init 同法(R-S4: 框架件往里的每一行都要有消费者)。 */
    return 0;
}

int file_table_start(void)
{
    /* START 相只留一行启动证据(表容量 + 已在场的 fd 数 —— 后者正常为 0: 本件不装
     * 标准流, 而 `runtime/posix` **也刻意不装**(v1 没有进程模型, ADR-0014 §2.9)。 */
    br_log_info("file-table: cap=%u fd slots, %u open", (br_u32)BR_FT_MAX, br_ft_count());
    return 0;
}

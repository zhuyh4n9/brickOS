/*
 * brickOS prototype v0.2.0 — vfs-core **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/vfs_selftest.c`。放在 `src/`(而不是 `include/`)是刻意的 ——
 * `include/` 下的一切都是**插件对外面**(会被 `brickie check` 的接口域治理, 也进
 * `descriptor_include` 的选用范围); 而这里的东西**只有自检**需要。
 *
 * ## 为什么自检要读内部
 *   有几条判据的**被测对象就是内部机制本身**, 用公开 API 表达不出来:
 *     - TC-VFS-002 要断言"每个挂载点的路径经最长前缀匹配都命中它自己" —— 那正是
 *       `mount_match` 的语义。用 `br_open` 间接测只能覆盖"某条路径落到了哪个 FS",
 *       无法逐条遍历挂载表;
 *     - 同一个用例还要直接查看 `s_mounts[i]`(挂载表是内部布局)。
 *   为了这些而把它们做成**公开 API** 会更糟: 挂载表的内部视图进了 golden 接口面,
 *   以后改布局就是接口变更(而它明明是实现细节)。
 *
 * ## 边界纪律(与 core 的 `*_internal.h` 同源)
 *   这里**只**暴露自检真的用到的东西, 逐条在下面写明用途。生产代码(`br_vfs.c`)的
 *   其余部分仍然是 `static` —— 不存在"为了测试把整个文件摊开"这回事。
 */
#ifndef BR_VFS_CORE_INTERNAL_H
#define BR_VFS_CORE_INTERNAL_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* 规范化路径缓冲的上界(生产实现里的 `BR_VFS_PATH_MAX`)。自检要造路径缓冲 ⇒ 需要它;
 * 暴露的是**容量常量**而不是缓冲本身, 所以没有共享可变状态的隐患。
 * ★ ADR-0013 把它从 64 抬到 128(符号链接拼接要放得下)—— **必须与 `br_vfs.c` 的同名常量
 *   逐字一致**: 下面那张内部视图是**靠布局等价**强转出来的, 两处一旦不等, 自检读到的就是
 *   错位的字节(实测踩过: 只改了生产侧, TC-VFS-002 立刻红且现象是"所有挂载都指向 /")。
 *   现在这条一致性由生产侧的 `_Static_assert(sizeof(vfs_mount_t) == sizeof(...))` 执法。 */
#define BR_VFS_PATH_MAX_INTERNAL  128u

/* 内存比较(生产实现是 static vfs_mem_eq)。自检用它做"读回的字节与写入的一致"的判据 ——
 * 无 libc 环境里没有 memcmp 可借(插件不跨件借 core/string.c 的符号)。 */
br_bool br_vfs_internal_mem_eq(const void *a, const void *b, br_size_t n);

/* 一条挂载的内部视图。★ 与 `br_vfs.c` 里的定义**必须逐字段一致** ——
 * 它是同一个类型的两处声明(不放到这套头里给生产代码用, 是因为生产代码不需要它)。
 * 用途: TC-VFS-002 逐条遍历挂载表, 核对"路径 → 匹配结果 = 自己"。 */
typedef struct vfs_mount_internal {
    br_bool            used;
    char               path[BR_VFS_PATH_MAX_INTERNAL];
    br_u32             len;
    const br_fs_ops_t *ops;
    void              *priv;
    br_inode_t        *root;
} vfs_mount_internal_t;

/* 挂载表快照(自检用)。`*n` = 已挂载条数。 */
const vfs_mount_internal_t *br_vfs_internal_mounts(br_u32 *n);

/* 最长前缀匹配(自检用): 返回命中的挂载下标, 未命中 ⇒ (br_u32)-1。
 * ★ 直接转调生产实现 —— 负例/正例都必须跑**真算法**, 否则用例验证的是副本。 */
br_u32 br_vfs_internal_mount_match(const char *norm);

/* 路径规范化(自检用): 对 `norm` 的期望做逐条断言(折叠 '//' / 去尾 '/' / 拒相对)。 */
int br_vfs_internal_path_norm(const char *in, char *out, br_size_t cap);

/* 字符串比较(自检用; 生产实现是 static vfs_str_eq)。 */
br_bool br_vfs_internal_str_eq(const char *a, const char *b);

#endif /* BR_VFS_CORE_INTERNAL_H */

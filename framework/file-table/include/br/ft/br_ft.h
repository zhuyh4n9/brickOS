/*
 * brickOS prototype v0.2.0 — 文件表契约(framework/file-table 插件的对外面)
 *
 * 设计依据(设计仓库):
 *   - `11-01-service.md` §1(D18): POSIX 的 **fd 表 = 共享状态单一主人**判例;
 *   - `1-01-architecture.md` §7.2 规则 3(共享状态唯一主人)+ D19(能力框架以**插件**形态
 *     存在 —— "框架件 = 插件的身份, core 的纪律");
 *   - `3-06-service-mgmt.md` §2(三类共享状态主人: fd 表 / socket 表 / trace 环);
 *   - `7-01-vfs.md` §1/§4.1(fd 表项统一持有 `br_file_t*`; `br_open` 是 fd 表的底层原语)。
 *
 * ============================== 本件是什么, 不是什么 ==============================
 * **是**: 一张"小整数 → `br_file_t*`"的**静态定长表** + 它的全部语义
 *       (最小可用 fd / 表满 / 非法 fd / dup 共享 / 释放时判"最后一个引用")。
 * **不是**: POSIX 的 `open/read/write` —— 那些是 **runtime/posix** 的面(runtime/posix 把
 *       `open()` 映射成 `br_open_err()` + 本表的 `br_ft_alloc()`, 把 `close()` 映射成
 *       `br_ft_release()` + `br_file_close()`)。本件**只做表**, 不做文件的读写。
 *
 * ## 为什么 fd 表要独立成件(而不是留在 runtime/posix 里)
 *   设计说"fd 表 = svc-posix 的唯一主人"(设计侧的插件名, 本树里叫 `runtime/posix`,
 *   见 ADR-0015), 那条规则要保的是**单一主人**(规则 3),
 *   不是"主人必须是 POSIX 服务"。把表抽成框架件之后:
 *     ① **主人更清楚了** —— 状态住在 `framework/file-table`, 它是全树唯一写 `s_slots`
 *        的地方; runtime/posix 变成**消费者**, 不再自己揣一张表;
 *     ② v2 的 **socket 表** 可以复用同一张表的语义(设计 `3-06` §2 把二者的主人问题
 *        并列), 而不必再写一遍"最小可用 fd / dup / 最后一个引用";
 *     ③ 表是**纯算法**(无设备、无 FS、无 POSIX), 于是能在宿主上把
 *        "表满/dup2 顶替/引用判空"这些边界跑到, 不需要 QEMU。
 *
 * ## 零依赖(刻意的)
 *   `br_file_t` 只作**前向声明** —— 本件从头到尾**不解引用**它, 只持有与比较指针。
 *   于是本件**没有 `[[dep]]`**(与 `framework/vfs-core` 同型: "deps 为空是设计结果,
 *   不是漏写"), 也不把 vfs-core 拉进任何组合。
 *
 * ## 引用语义: 引用数 = **在场槽位的个数**(不设计数器)
 *   POSIX 的"同一 open file description"在本件里就是**同一个 `br_file_t*`**:
 *   两个 fd 指向同一个指针 ⇒ 共享偏移与状态(这正是 `dup`/`dup2` 的语义)。
 *   于是"最后一个引用走了没有"**由表自己回答**(扫一遍在场槽位), 不需要一个要与槽位
 *   保持同步的 `refcount` 字段 —— 少一个字段, 就少一类"计数漂移"缺陷。
 *   代价是 `release`/`release_all` 里的 O(n) 扫描(n = `BR_FT_MAX` = 32, 可忽略)。
 *
 * ## 生命周期归属(为什么 `release` 要**回报**句柄)
 *   本件**不调用** `br_file_close()`: 那会把 vfs-core 变成一条真依赖, 也会让"表"越过
 *   自己的边界去管文件的生死。于是 `br_ft_release(fd, &last_file)` 在**最后一个引用
 *   消失时**把句柄交回调用方, 由 runtime/posix 调 `br_file_close()`:
 *
 *       int close(int fd) {
 *           br_file_t *f = BR_NULL;
 *           int rc = br_ft_release(fd, &f);      // -EBADF ⇒ 用户态 EBADF
 *           if (rc != 0) { return rc; }
 *           if (f != BR_NULL) { return br_file_close(f); }   // 最后一个引用: 真关
 *           return 0;                                        // 只是摘了一个 fd
 *       }
 *
 *   这是 POSIX `close(2)` 语义的**逐字翻译**, 而不是"表顺手把文件关了"。
 */
#ifndef BR_FT_BR_FT_H
#define BR_FT_BR_FT_H

#include <br/core/br_types.h>

/* 前向声明:`br_file_t` 是不透明句柄(vfs-core 的 D14 契约)。本件只持指针。 */
typedef struct br_file br_file_t;

/* ==================================================================== 上界 */

/* fd 槽数上界(编译期; 与 plugin.toml 的 `[[res]] ram` 对齐 —— 32 × 16 B = 512 B)。
 * POSIX 的 0/1/2(stdin/stdout/stderr)不是特殊槽: runtime/posix 在 init 里先装三个
 * 标准流, 于是它们**自然**占住最小三个号 —— 不需要"保留段"这种额外机制。 */
#define BR_FT_MAX   32u

/* "没有这个 fd"的返回值(`br_ft_fd_at` 用; 与"fd 0"区分开)。 */
#define BR_FT_NONE  (-1)

/* ==================================================================== 生命周期 */

/* 清空全表, 返回**被丢弃的槽位数**(0 = 本来就空)。
 * ★ 它**不回报**句柄 —— 这是刻意的诚实: "把表清零"与"把文件关掉"是两件事, 而本件
 *   不负责后者。正常退出路径请用 `br_ft_release_all()`; 本函数只用于插件 init
 *   (静态表是 BSS, 本来就是空的)与用例的隔离前置。返回非 0 说明**有人忘了关文件**。 */
br_u32 br_ft_reset(void);

/* 在场 fd 个数(观测)。 */
br_u32 br_ft_count(void);

/* 第 `index` 个**在场** fd(按 fd 升序; `index` 从 0 起, 越界 ⇒ `BR_FT_NONE`)。
 * 用途 = 观测/遍历(dump 出"谁开着什么"), 与 `br_mount_path_at` 同型。 */
int br_ft_fd_at(br_u32 index);

/* ==================================================================== 表操作 */

/*
 * 分配一个槽写下 `{file, flags}`, 返回**最小可用 fd**。
 *   `file == BR_NULL` ⇒ `-EINVAL`(空槽位不是"合法的空文件", 见下面的"在场判据")。
 *   表满 ⇒ `-EMFILE`(进程级 fd 用尽; `ENFILE` 是系统级的, 本件是单进程表)。
 *   `flags` 对本件是**不透明**的: 表不解释它, 只存与还(解释权归 runtime/posix)。
 */
int br_ft_alloc(br_file_t *file, br_u32 flags);

/* 取句柄(不改变任何状态)。fd 非法/空槽 ⇒ `BR_NULL`。
 * ★ **在场判据 = `file != BR_NULL`**: 没有独立的 used 位, 于是"表里有几个 fd"与
 *   "表里有几个非空句柄"永远是同一个答案(不可能对不上)。 */
br_file_t *br_ft_get(int fd);

/* 读/写槽位里的 flags。非法 fd ⇒ `-EBADF`; `out == BR_NULL` ⇒ `-EINVAL`。
 * `br_ft_set_flags` 是**整体替换**(不是"只改状态位"): 要保留访问模式就自己按位并回来
 *   —— 因为访问模式由"打开时"决定, 让一个能改访问模式的 setter 存在才是危险的。 */
int br_ft_get_flags(int fd, br_u32 *out);
int br_ft_set_flags(int fd, br_u32 flags);

/*
 * 释放一个 fd(= POSIX `close` 的前半步)。
 *   fd 非法 ⇒ `-EBADF`, 表不变。
 *   `last_file` 可空; 非空时: **该句柄的最后一个引用消失** ⇒ 写入句柄(调用方负责
 *   `br_file_close`); 还有别的 fd 指向它 ⇒ 写入 `BR_NULL`(调用方什么都不用做)。
 *   返回 0 = 成功(无论有没有回报句柄)。
 */
int br_ft_release(int fd, br_file_t **last_file);

/*
 * 释放**全部** fd(退出/收尾路径; 语义 = 逐个 `br_ft_release`)。
 *   `out[0..cap)` 收"最后一个引用消失"的句柄, **每个句柄恰好一条**(按指针去重,
 *   所以在场 fd 数可以大于 `cap` 而仍然装得下)。
 *   返回写出的条数; 装不下 ⇒ `-ENOSPC` 且**表不变**(先干跑一遍再动手 —— 半途清空
 *   会更糟: 调用方既拿不到全部句柄, 也回不到原状态)。
 *   `out == BR_NULL && cap != 0` ⇒ `-EINVAL`。
 */
int br_ft_release_all(br_file_t **out, br_u32 cap);

/* ==================================================================== dup 族 */

/*
 * `dup(2)`: 新 fd 指向同一句柄(共享偏移与状态), flags 随槽位复制一份快照。
 *   返回最小可用 fd, 或 `-EBADF`(oldfd 非法)/ `-EMFILE`(表满 —— 此时**不动** oldfd)。
 *
 * ★ flags 的两个身份(用的人必须知道): 访问模式是**每 fd**的(dup 出来的新 fd 不能因为
 *   共享就变成可写), 而 `O_APPEND`/`O_NONBLOCK` 按 POSIX 属**open file description**
 *   (共享)。本件只能存一份快照; "共享的那些位"由 `br_file_t` 侧(`br_file_flags`)与
 *   驱动自己保存 —— runtime/posix 的 `F_SETFL` 应把共享位同时落到文件侧, 不要只改本表。
 *   这是本件**已知的语义边界**, 登记在 `docs/decisions/0012-…`。
 */
int br_ft_dup(int oldfd);

/*
 * `dup2(2)`: 把 `oldfd` 复制到**指定**的 `newfd`。
 *   `oldfd` 非法 ⇒ `-EBADF`; `newfd` 越界 ⇒ `-EBADF`; `oldfd == newfd` ⇒ 返回 newfd(不动)。
 *   `newfd` 已占用 ⇒ 先等价 `close(newfd)`: 被顶掉的句柄若已无其它引用, 经 `displaced`
 *   回报(可空)交调用方 `br_file_close`, 否则不回报。
 *   返回 `newfd` ≥ 0, 或负 errno。
 * ★ `displaced` 在**任何**返回路径上都会被写(失败/无需回报 ⇒ `BR_NULL`)——
 *   于是调用方永远读不到未初始化的值; 想区分"没人被顶掉"与"被顶掉的还有人引用",
 *   不需要: 两种情况的动作都是"什么都不做"。
 */
int br_ft_dup2(int oldfd, int newfd, br_file_t **displaced);

#endif /* BR_FT_BR_FT_H */

# 0013 — VFS 符号链接与硬链接(解析在 vfs-core / 数据体共享在 tmpfs)

> 状态: **已落地**(QEMU virt aarch64; `[VFSCONF] 20/0`(13 → 20, 链接族 7 条新用例);
> 门禁 `fs-test` 的 tag 名单同步扩到 20)。
> 影响面: `framework/vfs-core/**`(`include/br/vfs/br_vfs.h`: `BR_INODE_SYMLINK`、
> `BR_O_NOFOLLOW`、`BR_STAT_NLINK`、`BR_SYMLINK_MAX`、`br_inode_ops` 三个新槽位、`br_stat_t.nlink`、
> `br_fstat`/`br_lstat`/`br_symlink`/`br_readlink`/`br_link`; `src/br_vfs.c`: 走查链改成
> **可展开符号链接的重启式解析** + 布局守卫; `src/vfs_internal.h`: 路径上界 64 → 128);
> `fs/tmpfs/src/tmpfs.c`(**数据体引用计数** = 硬链接; `symlink`/`readlink`/`link` 三个槽位;
> `unlink` 认符号链接); `io/uart-pl011/src/uart_selftest.c`(`br_stat_t` 布局变更的具名初始化);
> `framework/vfs-core/plugin.toml`(导出面 + 结构布局); `tests/gates.toml`(`fs-test` 的 tag 名单)。
> 设计依据: `7-01-vfs.md` §1(SD-1 统一可打开模型)/§2(SD-15/D23 四层 ops; 原文把
> `symlink/readlink/mknod` 写作"v2 预留: 槽位未占; 到 v2 再 append")、`7-01` §4.1、
> `8-01-device.md` §2(D21 的设备接入与"看得见打不开")、`3-01` §11(错误码子集)。
> 相关: **ADR-0009**(vfs-core + tmpfs 的建立; 本 ADR 改的是它定下的节点模型与走查链)、
> **ADR-0014**(POSIX 运行时; 它是 `link`/`symlink`/`readlink` 的第一个消费者)、
> ADR-0012(负 errno 与内核逐值对齐 —— 本 ADR 首次启用 `-EXDEV`)。
> 触发: 用户要求"文件系统需要支持 link/symbol link, 以便支持 link/symbol link 操作"。

## 1. 背景: 为什么这不该只做成"POSIX 面的一层模拟"

`link`/`symlink`/`readlink` 是**文件系统的语义**, 不是 API 皮肤能凭空造出来的:

- 硬链接要求"两个名字 → **同一份数据**"(改一个名字的内容, 另一个必须看见)。若只在
  `runtime/posix` 里"复制一份数据", 那不叫硬链接 —— 而且任何一个 FS 消费者(不经 POSIX 的)
  都拿不到它;
- 符号链接要求"目标串可以被解析" —— 而目标可能是**绝对路径**(要重新匹配挂载表)或
  **相对路径**(要相对"链接所在目录")。这两件事**只有掌握挂载表与完整路径的 vfs 做得到**。

所以本 ADR 落在 vfs-core + tmpfs 两层, POSIX 面(ADR-0014)只是它的**第一个消费者**。

## 2. 决策

### 2.1 链接的**解析**在 vfs-core, FS 只回答"目标串是什么"

`br_inode_ops` 加三个槽位, 职责切得很窄:

| 槽位 | FS 回答什么 | vfs 做什么 |
|---|---|---|
| `symlink(dir, name, target)` | 建一条链接(存目标串) | 只做"父目录 + 名字"的解析与前缀拼接 |
| `readlink(ino, buf, cap)` | 目标串(不跟随) | 拿它去**重启走查** |
| `link(dir, name, target_ino)` | 给一个 inode 再加一个名字 | 跨挂载检查(`-EXDEV`)、目录检查(`-EPERM`) |

**理由**: 绝对目标要重启挂载表匹配、相对目标要相对链接所在目录、中间分量的链接也要展开 ——
三件都需要 vfs 的全局视野。FS 若自己做, 每个 FS 都要复制一遍挂载表逻辑(vfs 的 SD-15 把
"路径走查在 vfs-core"写死了, 本 ADR 是它的直接推论)。

### 2.2 展开用**重启**而不是递归(4 KiB 栈的硬约束)

走查链会被 APP 线程以 **4 KiB 栈**调用(`3-01` §2.2 的栈尺寸), 递归 8 层会把栈压穿。
所以 `resolve_norm()` 的实现是: 逐级下推 → 撞到链接就**把"前缀 + 目标 + 剩余分量"拼成
一条新路径** → 从挂载根**重来**。代价是"走查可能重复几遍", 在 v1 的单 APP 文件规模下可忽略
(SD-3 已把"无 cache"的性能上限登记为 R-S3)。

### 2.3 `BR_VFS_SYMLINK_DEPTH = 8`; 环与自指由同一个计数兜住

展开深度超上界 ⇒ `-ELOOP`。2-环(`a→b→a`)与自指(`a→a`)不必特判 —— 计数到了就是 `-ELOOP`,
且**保证不卡死**(`TC-VFS-018` 专门钉"不卡死"这一条, 而不只是钉错误码)。

### 2.4 **相对目标的 `..` 不跨挂载**(有意的边界)

走查始终在**一个挂载内**下推(`mount_match` 定挂载, FS 的 `lookup` 解 `..`), 于是相对目标里的
`..` 到挂载根就停住。**这不是 Linux 语义**(Linux 会跨挂载点), 是 v1 的有意边界 ——
登记在 §4.1。做成 Linux 语义需要"挂载点感知的 `..`", 那是运行时挂载/卸载(O-S3)的同一块地基。

### 2.5 `O_CREAT` 撞上**悬空**符号链接时, 建的是**目标**(不是覆盖链接)

`open(link, O_CREAT)` 里 `link` 是一条指向不存在文件的符号链接。POSIX 的答复是"建**目标**"
(`link` 自己已经存在, 建不了)。实现上这条要求 vfs 把"这次实际走到了哪条路径"记下来并交给
`O_CREAT` 分支 —— 于是 `resolve_leaf()` 多一个 `final_out` 出参, 展开后的路径被用于后续的
`create`。这是"展开在 vfs"的**必然推论**(FS 看不到链接, 自然也建不了目标), 单列一条免得
被当成细节忽略。

### 2.6 硬链接: 数据从"节点的字段"变成"**被名字共享的体**"(tmpfs)

硬链接的语义是"两个名字 → 同一个 inode → 同一份数据/同一个长度/同一个偏移基准"。
ADR-0009 那一版把 `data/size/cap` 直接放在节点上 —— 那样 `link` 只能复制数据, 而"复制"
不是硬链接。本 ADR 把三者移进一个带**引用计数**的 `tmpfs_body_t`:

```
node ──body──> { data, size, cap, refs }
```

- `refs > 1` ⇒ 多名字共享(= 硬链接); `refs == 0` ⇒ 数据缓冲与配额额度一起归还;
- **不变量: 文件与符号链接节点恒有体**(建立时分配), 目录恒没有 ⇒ 所有 `n->body->…`
  都不必判空(少一整类"忘了判空"的缺陷);
- **符号链接的目标串也存在体里**(它本来就是"一小段数据")⇒ 不必给每个节点加一个
  `char target[BR_SYMLINK_MAX]` 固定数组, 节点尺寸反而比 ADR-0009 那版**更小**(少三个字段);
- `st_nlink` = `body->refs`。

**配额口径不变**(仍是 Σ `body->cap`, 占堆的是容量): `link` 不新增容量 ⇒ 不占新额度;
`unlink` 一个名字时**只在 refs 归零**才交还额度。这条保住了 ADR-0009 的记账不变量
("加多少减多少"), 只是账挂在体上而不是节点上。

### 2.7 三个槽位放在 `br_inode_ops` **表尾**(append-only); `mknod` 仍不占位

设计 `7-01` §2 自己写的约定就是"到 v2 再 append"。本 ADR **按那个约定**追加在表尾 ⇒
既有 FS 的具名初始化**不受影响**(未填 = `BR_NULL` ⇒ 该 FS 不支持, vfs 得 `-ENOTSUP`)。
`mknod` 不占位: 设备节点经 devfs 接入(D21), 不需要 `mknod`。

`BR_INODE_SYMLINK = 3u` 是**第三个类型值**(append-only, 旧值 0/1/2 不变)。
tmpfs 给符号链接节点的 `fops` 是 `BR_NULL`(**"看得见打不开"**)—— vfs 在展开那一层或
`O_NOFOLLOW` 那一层就拦住它(⇒ `-ELOOP`), 走不到文件面。这正是 D21 的"看得见打不开"
形态的又一个实例。

### 2.8 `br_stat_t` 追加 `nlink`; 路径级面加四个入口

| 面 | 变化 | 为什么 |
|---|---|---|
| `br_stat_t` | 追加 `nlink` + `BR_STAT_NLINK` valid 位 | 硬链接的**可观测面**: 消费者要能看见"这个名字之外还有没有别的名字指向同一份数据" |
| `br_lstat` | 新增 | POSIX 的"不跟随末级"必须有一个入口(否则 `stat` 一个链接只能看到目标) |
| `br_fstat` | 新增 | ★ 见下 |
| `br_symlink`/`br_readlink`/`br_link` | 新增 | 路径级便捷面(与 `br_mkdir` 等同层) |

**`br_fstat` 为什么必须有**(而不是让消费者自己调 `iops->getattr`): 消费者拿不到"这个 fd 对应
的路径"(它可能已经被 rename/unlink), 而 `br_inode_t` 虽在头里可见, 让消费者去调 ops 表属于
**越过契约** —— ops 是给 **FS 实现者**的扩展点, 不是给消费者的入口。vfs 提供这一层之后,
POSIX 面的 `fstat` 才真的是"1:1 映射"而不是"自己伸手进 inode"。

### 2.9 错误码口径(本 ADR 首次启用 `-EXDEV`)

| 情形 | 码 | 依据 |
|---|---|---|
| 跨挂载硬链接 | `-EXDEV` | v1 没有"跨 FS inode"概念。★ 它一直在错误码表里(ADR-0012), **到本 ADR 才有语义** |
| 目录硬链接 | `-EPERM` | POSIX 如此(而且是"不允许", 不是"不支持"); vfs 挡一次, tmpfs 再挡一次(自检也走公开 API, 所以要两层都成立) |
| 展开深度超上界 / 2-环 / 自指 | `-ELOOP` | §2.3 |
| `readlink` 用在非符号链接 | `-EINVAL` | POSIX 如此 |
| 目标串超 `BR_SYMLINK_MAX`(96) | `-ENAMETOOLONG` | §2.10 |
| 拼接后超路径缓冲(128) | `-ENAMETOOLONG` | 语义上的"名字过长"。★ 与"分量超 `BR_NAME_MAX`"的 `-ENOSPC` 是**两回事**(后者归 ADR-0009 的口径) |
| 该 FS 不填这三个槽位 | `-ENOTSUP` | §2.7 |
| `open(link, O_NOFOLLOW)` 而末级是链接 | `-ELOOP` | POSIX 的答复(链接没有文件面, 不该被 open 打开) |

### 2.10 两个静态上界与一次**路径缓冲扩容**

- `BR_SYMLINK_MAX = 96`: 单条链接的**目标串**上界。比 `BR_NAME_MAX`(32)宽得多(一条链接常
  指向路径而非单个名字), 又小于路径缓冲 ⇒ "目标是相对路径时必然能被拼出来"这条性质由
  **数值关系**保证, 而不是靠运行期检查。
- `BR_VFS_PATH_MAX`: **64 → 128**。旧的 64 装不下"前缀 + 目标 + 剩余分量"的拼接。
  ★ 这次扩容踩了一个坑, 值得记下来(见 §2.10)。

### 2.11 布局守卫: 内部视图靠**强转**, 所以两处声明必须逐字段一致

`br_vfs_internal_mounts()` 是把 `s_mounts` **强转**成 `vfs_mount_internal_t*` 给自检看的
(见 `src/vfs_internal.h` 的设计说明: 自检要遍历真实的挂载表, 而"把内部视图做成公开 API"
会让实现细节进 golden)。那次强转要求两处声明**逐字段一致** —— 而 `BR_VFS_PATH_MAX` 一旦只改了
生产侧、没改内部视图的 `path[64]`, 后果是**所有挂载都指向 `/`**(字节错位), 症状离原因很远。

处置: 把注释里的约定升级成**编译期断言**:

```c
_Static_assert(sizeof(vfs_mount_t) == sizeof(vfs_mount_internal_t), "…布局必须一致…");
_Static_assert(BR_VFS_PATH_MAX == BR_VFS_PATH_MAX_INTERNAL, "…必须一致…");
```

这是本 ADR 唯一一条"为防将来再踩"而加的机制 —— 它换来的是一条**编译期**错误,
而不是又一次"离原因很远"的运行期现象。

## 3. 落地清单与判据

| 面 | 落点 |
|---|---|
| 契约 | `framework/vfs-core/include/br/vfs/br_vfs.h`(3 个槽位 + 4 个路径级入口 + `br_fstat`; 4 个新常量/字段) |
| 走查 | `src/br_vfs.c`: `resolve_norm()` 重启式展开(深度上界 8)+ `mkdir_p` 改成按全路径前缀逐级建(中间分量的链接也要展开) |
| tmpfs | `src/tmpfs.c`: `tmpfs_body_t` 引用计数 + 三个槽位 + `unlink` 认符号链接 + `getattr` 报 `nlink` |
| 用例 | `src/vfs_selftest.c` 的 `TC-VFS-014..020`(7 条): 建链与 lstat/stat 跟随 / readlink 边界 / 经链接读写 / 绝对与含 `..` 的相对目标 / `O_NOFOLLOW` 与环 / 硬链接与跨挂载 `-EXDEV` / 链接参与名字空间变更 |
| 门禁 | `tests/gates.toml` 的 `fs-test` tag 名单 13 → 20 |
| 声明面 | `framework/vfs-core/plugin.toml`(导出面 + `br_inode_ops_t`/`br_stat_t` 的结构布局行) |

**验证(实测)**

```
$ brickie test fs-test            # 门禁全绿
  [VFSCONF]  SUMMARY pass=20 fail=0 total=20      ← 13 条旧例 + 7 条链接族
  [DEVCONF]  SUMMARY pass=6  fail=0 total=6
  [CDEVCONF] SUMMARY pass=8  fail=0 total=8
  [IOCONF]   SUMMARY pass=12 fail=0 total=12
$ brickie iface status framework/vfs-core#vfs     # 一致(hash sha256:744bb360…)
  变更集: ADDED BR_INODE_SYMLINK / BR_O_NOFOLLOW / BR_STAT_NLINK / BR_SYMLINK_MAX /
          br_fstat / br_link / br_lstat / br_readlink / br_symlink;
          CHANGED br_inode_ops_t / br_stat_t
```

## 4. 未做 / 留给下一刀

1. **相对符号链接目标里的 `..` 不跨挂载**(§2.4)。要做成 Linux 语义, 需要"挂载点感知的 `..`",
   与运行时挂载(O-S3)同一块地基。
2. **`readlink` 不报告"目标串被截断"**: 照 POSIX 返回**全长**(调用方自己比较长度与缓冲)。
   本实现额外保证 `buf[cap-1] = '\0'`(POSIX 不要求), 于是调用方的 C 串用法更安全 ——
   这一点写在头注释里, 不是隐含行为。
3. **目录的 `st_nlink` 恒 1**: 没有"子目录数"计数(Linux 会算进去)。等 vfs 补 inode 元数据面时
   一起做。
4. **`mknod` 不占位**(§2.6): 设备节点经 devfs 接入。
5. **没有"链接的链接"优化**: 每级展开都重启走查(§2.2)。SD-3 的"无 cache"是同一个取舍的两面。
6. **设计侧回灌**(属设计仓库, 本 ADR 只登记):
   - `7-01` §2 的 `br_inode_ops` 应补 `symlink`/`readlink`/`link` 三个槽位与
     `BR_INODE_SYMLINK`(本 ADR 按 append-only 提前占用, **设计侧需追认**);
   - `7-01` §2 的 `br_stat_t` 应补 `nlink`; 同一节应补 `br_lstat`/`br_fstat` 与
     `br_symlink`/`br_readlink`/`br_link` 三个路径级入口;
   - `7-01` §7 的开放问题里可以补一条"相对目标的 `..` 是否跨挂载"(本节 §2.4 的边界);
   - `11-02` §2.2 把 `link/symlink/readlink` 判为 **TR-D**; 本 ADR 已实现 ⇒ 原型侧已改档为
     TR-A(见 ADR-0014 §4.8 的设计侧回灌项)。

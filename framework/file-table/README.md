# framework/file-table

文件表(**fd 表**):`小整数 → br_file_t*` 的**静态定长表**, 外加它的全部语义 ——
最小可用 fd、表满、非法 fd、`dup`/`dup2` 共享、以及"最后一个引用走了没有"的判定。

> 设计出处: `11-01` §1(D18: **fd 表 = 共享状态单一主人**判例)、`1-01` §7.2 规则 3
> (共享状态唯一主人)+ D19(框架件 = 插件的身份, core 的纪律)、`3-06` §2(三类共享状态
> 主人: fd 表 / socket 表 / trace 环)、`7-01` §1/§4.1(fd 表项统一持有 `br_file_t*`,
> `br_open` 是它的底层原语)。
> 逐条裁定与偏离: `docs/decisions/0012-file-table-and-errno.md`。

## 本件是什么, 不是什么

| | |
|---|---|
| **是** | 表本身 + 它的语义(分配/查询/flags/释放/dup/dup2/退出清空) |
| **不是** | POSIX 的 `open/read/write/…` —— 那是 **runtime/posix** 的面 |

runtime/posix 把二者接起来(**零转换**):

```c
int open(const char *path, int flags, ...) {
    int err = 0;
    br_file_t *f = br_open_err(path, to_br_flags(flags), &err);
    if (f == BR_NULL) { errno = -err; return -1; }
    int fd = br_ft_alloc(f, (br_u32)flags);
    if (fd < 0) { (void)br_file_close(f); errno = -fd; return -1; }   /* 分配失败要回滚 */
    return fd;
}

ssize_t read(int fd, void *buf, size_t n) {
    br_file_t *f = br_ft_get(fd);
    if (f == BR_NULL) { errno = EBADF; return -1; }
    br_s64 r = br_file_read(f, buf, n);
    if (r < 0) { errno = (int)-r; return -1; }                        /* errno = -ret */
    return (ssize_t)r;
}

int close(int fd) {
    br_file_t *f = BR_NULL;
    int rc = br_ft_release(fd, &f);
    if (rc != 0) { errno = -rc; return -1; }        /* -EBADF */
    if (f != BR_NULL) { return close_file(f); }      /* 最后一个引用: 才真关文件 */
    return 0;                                        /* 只是摘掉了一个 fd */
}
```

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` / `api_type` / `subkind` | `ability` / `native` / `framework` | 能力框架件(D19), 消费者是 native 插件 |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ ② 完成点 = CORE |
| 依赖(init/type/runtime) | **无 `[[dep]]`** | 只持指针不解引用(前向声明)⇒ 与 vfs-core 同型的零依赖 |
| 导出面 | 单元 `filetable`(`unfrozen`/`experimental`): **2 宏 + 11 函数** | `br-filetable.txt` 的候选面 |
| `[selftest]` | `cases = 11`; 钩子 `file_table_selftest()` 由 core 在全部 `start()` 之后驱动 | `plugin.toml` `[selftest]`; ADR-0010 |
| 资源 | `ram` 1 KiB: 32 槽 × 16 B = 512 B(纯静态, **无堆分配**) | `[[res]]` |
| `sched_class` | `SAFE_PREEMPT` | 无睡眠 + 临界区全覆盖(见下) |

## 承重设计(四条)

1. **在场判据 = 句柄非空** —— 没有独立的 `used` 位。于是"表里有几个 fd"与"表里有几个
   非空句柄"**永远是同一个答案**, 不可能对不上。
2. **引用数 = 在场槽位个数**(不设计数器)。POSIX 的"同一 open file description"就是
   **同一个 `br_file_t*`**; "最后一个引用走了没有"由扫表回答 ⇒ 少一个要与槽位保持同步的
   `refcount` 字段, 就少一类"计数漂移"缺陷。代价是 O(n) 扫描(n = 32, 可忽略)。
3. **表不关文件**: `br_ft_release(fd, &last)` 只在**最后一个引用**消失时把句柄**交回**
   调用方。表管 fd 的生死, vfs-core 管文件的生死 —— 于是本件不必依赖 vfs-core。
4. **并发 = 关中断临界区**(core 的 `br_irq_lock`, 嵌套安全): 每次表操作只有几条指令,
   **从不在临界区里睡眠**。不需要锁对象、不需要 init、不会有"锁没初始化"这类缺陷;
   `SAFE_PREEMPT` 的依据也在这里。

## 已知的语义边界(诚实清单)

- **`flags` 是每 fd 的快照**: 访问模式按 POSIX 是每 fd 的, 而 `O_APPEND`/`O_NONBLOCK` 属
  **open file description**(共享)。本件只存一份快照; 共享位的真值在 `br_file_t` 侧
  (`br_file_flags`)与驱动里 ⇒ runtime/posix 的 `F_SETFL` 要把共享位**同时**落到文件侧,
  不能只改本表。登记在 ADR-0012。
- **`br_ft_set_flags` 是整体替换**(不是"只改状态位"): 一个能改访问模式的 setter 存在
  才是危险的 —— 要保留访问模式就自己按位并回来。
- **没有 `F_DUPFD`(指定下限)**: 需要时用 `br_ft_dup2`, 或按 ADR 追加一个函数(面预算)。
- **0/1/2 不是特殊槽**: 标准流由 runtime/posix 在 init 里先装, 于是**自然**占住最小三个号 ——
  不需要"保留段"这种额外机制。

## 一致性用例

`file_table_selftest()` 在 `src/ft_selftest.c`, 逐例打印 `[FTCONF] PASS/FAIL <tag> <desc>`,
末尾 `[FTCONF] SUMMARY pass=%u fail=%u total=%u`。11 例 `TC-FT-001..011`:
空表与最小号 / 最小可用 fd / 表满与空句柄 / 非法 fd 一律 `-EBADF` / flags 往返 /
最后一个引用判定 / dup 共享 / dup2 顶替与 displaced / release_all 去重与 `-ENOSPC` /
errno 取值 / 边界与观测面。

★ 本套件**一行都不需要 QEMU**: 表从头到尾不解引用 `br_file_t`, 于是用例拿"互不相同的
假指针"当句柄就能把全部边界跑到 —— `tests/host/ft_test.c` 是同一条腿的宿主版(判据 =
退出码), 它**额外拿宿主 `<errno.h>` 逐码对拍** `BR_E*`(见 `tests/gates.toml` 的 `ft-test`)。

## 目录

```
plugin.toml                    人写   ← 插件级唯一真值(本目录不改它)
include/br/ft/br_ft.h          人写   ← 文件表契约(本件的对外面)
src/file_table.c               人写   ← 表 + 语义(只留机制)
src/ft_selftest.c              人写   ← 一致性用例 [FTCONF](由 core 驱动)
README.md                      ← 本文件
tests/smoke.toml               ← 声明面用例骨架(与 in-image TC-FT-* 同 id)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-test-001`: `TC-FT-001..011` 是**自编号** —— 设计 `6-01` 的用例表里没有文件表
  这一组。还债动作: 先补出 `6-01` 的用例组, 再把自编号改回正式编号(见根目录
  `WORKAROUNDS.md`)。
- **消费者已到位**(ADR-0014): `runtime/posix` 经 `[[dep]] kind = "init"` 依赖本件, 于是
  它在 `product.toml` 里本可以只靠闭包进入。仍显式列着是刻意的 —— 见 `product.toml`
  那一行的注释(让"文件表在 POSIX 运行时之外也有独立证据"这件事可读)。

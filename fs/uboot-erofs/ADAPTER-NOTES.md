# `ADAPTER-NOTES` — uboot-erofs 的适配层: 设计史与边界判据

> **一句话**: `third_party/` 是"上游的原件"(只读、逐字节), `compat/` 是"上游依赖的
> **环境**", `glue/` 是"上游需要的**对接**"。**适配 = 给上游它依赖的环境, 不是改上游
> 让它适应我们** —— 这条判据的机械落点是哈希锁 + `tools/check-third-party-pristine.sh`。
> 逐件的来源与归属见 [NOTICE](NOTICE); 结构与用例见 [README.md](README.md)。

## 1. 设计史: 为什么是现在这个三角

第一版(ADR-0024)的结构是 `src/`(上游派生副本)+ `adapter/`(本项目适配层), 靠**依赖方向**
与 `tools/check-third-party-isolation.sh` 守边界。复查时暴露了更根本的问题: **`src/` 里装的
是我们改写过的上游副本, 不是上游原件** —— 以 erofs 为例, 旧 `src/` 相对上游做过全文件重排、
把 `IS_ERR/ERR_PTR` 改成出参、把栈上 4 KiB 缓冲提到文件域静态、删多设备表、接地 errno…
每一处都是"改上游代码"。后果: 上游升级只能手工重做全部改写; "哪些是上游的"只能靠人读;
而且为了过本项目的告警集合还得反过来改上游。

ADR-0026 因此取消 `src/` 这一层, 把"上游"与"我们的"彻底分开:

```
prototype/
├── third_party/uboot-imx/   ← 只读 vendor: 逐字节上游原件(路径与上游一致), 参与编译但不编辑
└── adapter/
    ├── third-party-upstream.lock   ← vendor 的 sha256 只读锁
    └── uboot-erofs/
        ├── brick.toml   ← sources **直接指向** third_party 的上游 .c; cflags = vendor 告警口径
        ├── compat/       ← u-boot 运行环境(我们写的适配层)
        └── glue/         ← 块层 + VFS 对接 + 自检(本项目代码)
```

⇒ 旧 `src/` ↔ `adapter/` 的边界判据(`tools/check-third-party-isolation.sh`)与那套
"改写副本"的偏离清单**已被 ADR-0026 取代**; 相应的结构、文件清单与"逐文件对照(源 → 本目录)"
的说法在本插件里都已不成立。

**仍然有效的是这些偏离的"结论"**(它们没有消失, 只是换了落点 —— 从"改上游"变成
"给环境/在本项目侧对接"): 无 libc、`IS_ENABLED` 的编译期开关、栈纪律、块层换 bdev、
出口是 VFS 四层 ops、`DBG_BUGON` 不停机、errno 名字归 core、只读执法、on-disk 布局自检、
`sbi` 的存储。逐条落点见 [README.md](README.md) §4。

## 2. 三角的依赖方向(单向)

```
   third_party/uboot-imx/fs/erofs/*.c + lib/lz4.c     ← 上游原件(GPL-2.0+), 只读
                  │  brick.toml 的 sources 直接点名(无副本)
                  │  上游 #include "内部头"/<u-boot/lz4.h>/<linux/…>/<asm/…>
                  ▼
   fs/uboot-erofs/compat/    ← 兑现上游假设的**环境**: erofs_env.h(类型/宏/日志/
                  │                 字符串/分配器/errno) + 每个 u-boot 头名一个薄壳
                  ▼
   fs/uboot-erofs/glue/      ← 兑现上游需要的**行为**: 块层 shim、VFS 四层 ops、
                                     `sbi` 的存储、on-disk 静态判据、自检
```

- `third_party/` 是**叶子**: 它不知道我们, 我们只 include/编译它;
- 上游的 `.c` 只经 `compat/` 与自己的头取环境, **不**知道 `glue/`(上游 `internal.h` 只
  `extern` 它需要的函数与 `sbi`, 谁提供实现由链接决定);
- `glue/` **可以** include 上游 `internal.h` / `erofs_fs.h`(对接层当然要认识被对接的代码)。

## 3. 判据(机械的, 靠哈希而不靠人读)

`tools/check-third-party-pristine.sh`(登记在 `tests/gates.toml`, 见 ADR-0026 D-5):

1. 锁元数据齐(repo / branch / commit / snapshot_sha256);
2. **vendor 逐字节等于锁**(任一字节被改 ⇒ 红);
3. 锁里每条都在磁盘上(静默丢失也红);
4. vendor 里没有锁外的文件(塞东西也红);
5. `third_party/` 下没有 `brick.toml`(三方件不是 brickOS 插件);
6. `third_party/` 之外没有 `#include <third_party/...>` 这种**按 vendor 路径写死**的包含
   (vendor 的头只经 `-I` 的目录名 `<vendor>/fs/erofs`, 或 `compat/` 里的相对转发壳找到)。

**vendor 代码按它的告警口径编译**(ADR-0026 D-4): 上游按 u-boot 自己的 `-W...` 集合写,
那份**不含**本项目的 `-Wshadow`/`-Wpointer-arith`/`-Wmissing-prototypes`/`-Wsign-compare`/
`-Wunused-parameter`。事实: 上游 `super.c` 的局部变量就叫 `sbi`(故意遮蔽全局)、`data.c`
对 `void *` 做算术、`namei.c` 的 `erofs_namei` 没有前置原型。⇒ `brick.toml` 的
`[build].cflags` 关掉这几条; **不能**为了过我们的告警去改上游 —— 那正是本刀要消灭的
"就地改三方件"。

## 4. 环境事实: "给什么"与"为什么不能随便给一个同宽的"

这些是适配过程中查出来的硬事实, 每条都对应 `compat/erofs_env.h` 里的一段:

| 上游依赖 | 我们提供 | 为什么不能随便给 |
|---|---|---|
| `u8/u16/u32/u64/s64` | `unsigned char`…/`unsigned long long`/`signed long long` | u-boot 的 `u64` **不是** `uint64_t`: 在 LP64 上后者是 `unsigned long` —— 同宽**不同型**, 上游满地的 `%llu` 会与实参不符 |
| `PAGE_SIZE` | 4096(平台事实) | 上游 `internal.h` 有 `#if PAGE_SIZE != 4096 #error` |
| `struct list_head` | **类型本身**(不给遍历/插入) | 上游 `erofs_inode`/`erofs_dentry` **有该字段**(结构体布局要成立), 但本件的 inode 是瞬态路径包装, 从不用那些链 |
| `IS_ERR`/`ERR_PTR`/`PTR_ERR` | 内核那套"错误码放指针高地址侧"的约定 | 上游 `namei.c` 靠它把"找不到目录项"与"真出错"合在一个返回值里 |
| `malloc/calloc/realloc/free` | 接到 core 堆(`br_malloc` 家族) | 裸机 unikernel 没有 libc; core 的 `br_malloc` 返回 NULL 的语义与 libc 一致, 所以接得上 |
| `pr_err/pr_info/pr_debug`、`erofs_err/dbg` | 接到 `br_log_write`(前缀 `uboot-erofs: `) | 上游 `internal.h` **自己**把 `erofs_*` 映射到 `pr_*` ⇒ 名字由上游给, 实现由我们给 |
| `BUILD_BUG_ON` | u-boot 的机制(函数体内 `if` + `__compiletime_error`) | **不能**用 `_Static_assert`: 上游 `erofs_fs.h` 最后一条自检的条件不是整型常量表达式 |
| `get_unaligned/put_unaligned`、`__rcode`、`RELOC_LOADER`、`IS_ENABLED` | 宏/常量 | 上游原文直接用到; `IS_ENABLED` 这里给 `CONFIG_LZ4 1`/`CONFIG_ZLIB 0`, 于是上游的 `#if` 原样成立 |
| `strlen/strnlen/strcmp/strncmp/strchr` | 本文件自研的 `static inline`(加 `uboot_erofs_` 前缀再用宏接回上游裸名) | 任务书硬约束: 不依赖 runtime/posix; 且整编译单元 `-fno-builtin`, 不会自撞 |
| `EFSCORRUPTED` / `E*` 裸名字 | `<br/core/br_errno.h>`(ADR-0023) | errno 名字是 core 的单一真值; `EFSCORRUPTED` 照抄上游 `internal.h` 的 `#ifdef EUCLEAN` 构造, 一个字不改 |
| `struct erofs_sb_info sbi` 的**存储** | 由 `glue/erofs_vfs.c` 定义 | 上游把它放在 `fs/erofs/fs.c`(u-boot 的 blk/DM 胶水层), 本插件不编译那份 ⇒ 缺的那块环境由我们补, 上游代码一个字不改 |

一条反直觉的发现: 上游 `erofs_fs.h` 末尾的 `erofs_check_ondisk_layout_definitions()` 是
`static inline` 而**上游从不调用它** ⇒ 那些 `BUILD_BUG_ON` 永不诊断, **上游这份布局自检
实际是死的**。所以本件的 on-disk 布局判据(文件域 `_Static_assert`, 在 `glue/erofs_vfs.c`)
不能撤 —— 它一定求值, 比上游那份更强。

## 5. 一处历史理由(保留, 但路径已改对)

`compat/u-boot/lz4.h` 不复制 vendor 的 `include/u-boot/lz4.h`, 而是用**相对路径转发**到
原件(`../../../../third_party/uboot-imx/include/u-boot/lz4.h`)。为什么不用 `-I` 指到
`third_party/uboot-imx/include`: brickie 对每个编译单元的 `-I` 是**按字典序**排的, 而
`fs/uboot-erofs/../../third_party/...` 里的 `..` 排在 `compat` 前面 ⇒
`third_party/include` 会**盖住** `compat/` 里的同名壳(`<fs.h>`/`<part.h>` 会被解析到
u-boot 的真头, 而那份还要 `rtc_def.h` 之类 → 编不过)。显式相对路径没有这个歧义。

其余 `compat/` 头都只做一件事: 把上游写的 u-boot 头名接到 `compat/erofs_env.h` 上。
它们是"名字适配", 不含任何上游实现 —— 所以本插件里**没有**"每个 `src/` 文件都带 SPDX"
这类旧说法成立的空间(没有 `src/`, 也没有上游副本)。

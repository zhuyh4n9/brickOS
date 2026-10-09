# 11-02 — runtime/posix 可支持接口清单(现状对账)

> 章节: **11-service**。状态: **成文(实况对账, 非设计提案)**。
> 定位: 本篇回答一个问题 —— **以原型现在的 native 面, `runtime/posix` 今天能实现哪些 POSIX 接口、
> 哪些要先补一件前置、哪些结构上就不做**。它是 `11-01` §2 大纲第 1 项("POSIX 子集清单")的
> **现状版**: `11-01` 写"M2 应当交付什么", 本篇写"以今天的地基, 哪一档能立刻交付"。
> 依赖: `11-01`(D18/服务清单)、`1-01` §7.6(移植双模式 + sqlite 视角的 POSIX 需求)、
> `10-01`(iface-posix 皮肤)、`7-01`(vfs-core 的 1:1 映射口)、`3-01`(native 面)、
> `3-06` §2(fd 表 = 共享状态单一主人)。
> 原型坐标: 分支 `brickOS-prototype-v0.x.0`, 在 `724fa56` 之上叠加了 ADR-0011(rr/bh/workqueue)、
> **ADR-0012(文件表 + errno 对齐)**、**ADR-0014(runtime/posix 本体 + 链接支持)**。
> **每条判定都带原型侧的 `文件:行` 或 ADR 编号**。
>
> ## ★ 实现现状(2026-xx, ADR-0014 落地后回头更新)
> 原型侧已按本清单实现 **TR-A + TR-B(§2 的 37 条)**, 并把 §2.2 里原判 TR-D 的
> `link/symlink/readlink` **提前实现**(用户要求"文件系统支持 link/symlink")。
> 证据: `[POSIXCONF] 20/0`(只经 POSIX 头文件驱动的端到端套件)+ `[VFSCONF] 20/0`
> (链接族的 7 条新用例)+ 门禁 `posix-test`。TR-C 的 6 件前置(P-1…P-9)与 TR-D 的其余
> 条目**仍未做** —— 本清单的档位因此仍然有效, 只是 TR-D 少了 3 条。
>
> ★★ **名字**: 本篇初稿写于插件改名之前, 当时它叫 `svc-posix`。**现两棵树已统一为
> `runtime/posix`**(目录 `runtime/posix/`; 新增命名空间 `runtime` ⇒ `ability`/`service`)——
> 原型的改动见 ADR-0015, 设计侧的跟改见 `brickie-v0.1` §8.3 ①、`4-02` §2 与 §6 的 Q-10(已关闭)。
> D18 那一行保留"`svc-posix`"并加了**修订注**(决策记录不改写原文)。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **cwd** | current working directory | 当前工作目录(POSIX 概念, 归 runtime/posix) |
| **fd** | File Descriptor | 文件描述符(`framework/file-table` 是唯一主人) |
| **FILE** | — | C stdio 的流对象(`fopen` 的返回值) |
| **libc** | C standard library | C 标准库(picolibc/newlib/自研 —— **选型尚未拍**) |
| **OFD** | open file description | 打开文件描述(`dup` 共享的实体 = 同一个 `br_file_t*`) |
| **POSIX** | Portable Operating System Interface | 可移植操作系统接口 |
| **TLS** | Thread-Local Storage | 线程局部存储(`pthread_key_*` 的底座) |
| **TR** | — | 本篇的判定档(A/B/C/D; 见 §2) |
| **VFS** | Virtual File System | `framework/vfs-core`: 挂载表 + 四层 ops + 路径级便捷面 |

> **编号约定**: `D*` = 全局决策; `SD-*`/`CA-*`/`INV-*` = 各域决策/契约/不变量(见各篇);
> `TR-A/B/C/D` = **本篇**的支持档(§2)。

---

## 0. 判定档与判据

| 档 | 含义 | 判据 |
|---|---|---|
| **TR-A** | **今天就能实现** | 需要的 native 原语**已落地且语义够用**; runtime/posix 只做"形状转换 + errno 取负" |
| **TR-B** | 加一层薄适配即可 | 原语在, 但缺一小件(如 `void** retval`、`mode` 参数、`isatty` 的来源) —— 工作量在 runtime/posix 内部 |
| **TR-C** | **要先补一件前置**(列明) | 缺 native 原语或设计口径(wall clock / TLS / libc / 并发锁 / 预算) |
| **TR-D** | **结构上不做** | 与"单地址空间 / 单 APP / 无 fork"的设计前提冲突(`1-01` §2.1), 或设计明确排在 v2+ |

**一条纪律**: 档位只看**底层原语是否在位**, 不看"接口是否常见"。例如 `write()` 是 TR-A,
而 `printf()` 是 TR-C —— 不是因为它难, 而是因为**libc 选型未拍**(`11-01` §3 的开放问题)。

---

## 1. 地基现状(TR-A 全部押在这张表上)

| 能力 | 原语 | 落点 | 语义完整度 |
|---|---|---|---|
| **fd 表** | `br_ft_alloc/get/get_flags/set_flags/release/release_all/dup/dup2` | 原型 `framework/file-table/include/br/ft/br_ft.h`(ADR-0012) | ✅ 最小可用 fd / OFD 共享 / 最后一个引用判定 / `-EBADF`/`-EMFILE` 齐 |
| **打开与文件面** | `br_open_err` + `br_file_read/write/lseek/ioctl/fsync/poll/close` | 原型 `framework/vfs-core/include/br/vfs/br_vfs.h:243-253` | ✅ 设备与文件**同一个** `br_open`(SD-1); `err` 出参是 errno 映射的唯一入口 |
| **路径级便捷面** | `br_stat/mkdir/rmdir/unlink/rename/truncate` | 同上 `:255-261` | ✅ 明写"runtime/posix 的 1:1 映射口"(ADR-0009 §3 裁定 3) |
| **目录面** | `br_opendir/br_readdir/br_closedir` | 同上 `:271-273` | ◐ 每次一条定长 `br_dirent_t`(type/name); 无 `d_off/d_ino` |
| **线程** | `br_task_create/join/exit/yield/sleep/sleep_until/self/name/state` | 原型 `core/include/br/core/br_sched.h:63-83` | ◐ 栈由**调用方**提供; `join` 只有 `int*`; TCB 池 `BR_TASK_MAX = 8`(`core/src/sched/sched_internal.h:82`) |
| **同步** | `br_mutex_*` / `br_sem_*` / `br_cond_*` / `br_spinlock_*` | 原型 `core/include/br/core/br_sync.h:60-131` | ◐ **无 destroy**(`pthread_*_destroy` 无底座) |
| **时间** | `br_clock_now`(单调 µs)/ `br_deadline_from_now`(饱和)/ `br_task_sleep(_until)` | 原型 `core/include/br/core/br_time.h` + `br_sched.h:78-79` | ◐ **无墙钟/RTC**; 分辨率 ≈ **100 ms**(`platform/qemu-aarch64/src/board_irq.c:29` 的 `BR_BOARD_TIMER_PERIOD_US = 100000`) |
| **内存** | `br_malloc/calloc/realloc/free` + contig + 页 + DMA | 原型 `core/include/br/core/br_mem.h:51-57` | ✅ TLSF 堆 1 MiB + 红区/毒化; ◐ 池比例**写死**(`br-wa-mem-001`) |
| **字符串例程** | `memcpy/memmove/memset/memcmp` | 原型 `core/src/string.c` | ◐ **刻意只有这四个**(编译器 lowering 的硬性义务); 无 `strlen/strcpy/strtol/qsort` |
| **设备** | `/dev/uart0`: read/write/ioctl/poll | 原型 `io/uart-pl011/**` | ◐ 轮询 cdev(阻塞 read 靠 `br_task_sleep` 轮询, `br-wa-io-001`) |
| **服务发布** | `br_service_publish/lookup` | 原型 `core/include/br/core/br_svc.h` | ✅ |
| **errno 值域** | `BR_E*` = 内核 `errno-base.h` **整表** + `errno.h` generic 按需(49 码) | 原型 `core/include/br/core/br_error.h`(ADR-0012) | ✅ 编号逐值等于 Linux; 宿主门禁逐码对拍 |

**缺口清单(TR-C 的前置就是这几件)**

| # | 缺什么 | 影响的接口族 |
|---|---|---|
| P-1 | **墙钟/RTC 源**(现在只有自启动以来的单调 µs) | `gettimeofday`/`time`/`clock_gettime(REALTIME)`/`utime` |
| P-2 | **tickless**(现在是 100 ms 周期 tick, "不早醒但有上界无下界"→ 实际晚到 ≈1 tick) | `usleep/nanosleep/poll/select` 的精度语义 |
| P-3 | **libc 选型与落地**(含 `sbrk` 挂接点 —— 设计 `1-01` §4.5 仍是 `[?]`) | 整个 stdio 族 + `str*`/`qsort`/`atoi`; **sqlite 模式 A 的硬前置** |
| P-4 | **per-thread 通用槽位/TLS** | `pthread_key_*`、per-thread `errno`、`pthread_self` 的 POSIX 语义 |
| P-5 | **`br_mutex_destroy`/`br_cond_destroy`** | `pthread_mutex_destroy`/`pthread_cond_destroy` |
| P-6 | **并发保护**(vfs-core 挂载表/句柄表与 tmpfs 树**无锁**) | 多线程下的 `open/read/write/close`(ADR-0009 §5 遗留项 7) |
| P-7 | **`br_mm_map` 恒 `-ENOTSUP`** | `mmap/mprotect/munmap`(设计判"可关", 但要成文) |
| P-8 | **权限模型**(vfs-core 明写"不做访问模式执法", 且**没有** uid/gid/mode) | `chmod/chown/access/umask`; `open` 的 `-EACCES` 来源 |
| P-9 | **`br_fault_handler_register` 未实现**(`3-02` §10.5 归 v2) | `sigaction/signal` 的载体 |

---

## 2. 逐族清单

### 2.1 fd 与文件 I/O —— **本族整体 TR-A**

| POSIX | 映射 | 档 | 备注 |
|---|---|---|---|
| `open`, `openat` | `br_open_err` + `br_ft_alloc` | **TR-A** | `flags`: `BR_O_RDONLY/WRONLY/RDWR/CREAT/EXCL/TRUNC/APPEND/NONBLOCK` 与 POSIX `O_*` 一一对应(数值不同, 需一层翻译); 分配失败要**回滚** `br_file_close` |
| `close` | `br_ft_release` + `br_file_close` | **TR-A** | 语义 = "最后一个引用才真关文件"(ADR-0012 §2.2) |
| `read`/`write` | `br_ft_get` + `br_file_read/write` | **TR-A** | `errno = -ret` 零转换; 短读/短写语义由 FS/驱动决定(PL011 已做短读) |
| `pread`/`pwrite` | 同上 + 保存/恢复偏移 | **TR-B** | vfs 与 runtime/posix **都无 pread 槽位**; 用 `lseek`+`read` 模拟, 代价 = **并发下不原子**(设计 `7-01` 未列槽位) |
| `lseek`, `ftell`, `fseek` | `br_file_lseek` | **TR-A** | `BR_SEEK_SET/CUR/END` |
| `dup`, `dup2` | `br_ft_dup/dup2` | **TR-A** | 含目标顶替与 `displaced` 回报(ADR-0012) |
| `fcntl(F_GETFL/F_SETFL)` | `br_ft_get_flags/set_flags` | **TR-B** | 表存的是**快照**: `O_APPEND/O_NONBLOCK` 属 OFD, 共享位要同时落到 `br_file_t` 侧(ADR-0012 §2.4) |
| `fcntl(F_DUPFD)` | — | **TR-B** | 表无"指定下限"分配; 用 `dup2` 组合 |
| `fcntl(F_SETLK/F_GETLK)` | `br_mutex_*`(进程内) | **TR-B** | 设计已裁定: 单 APP 下**退化为进程内互斥**(`1-01` §7.6), 语义需文档化 |
| `fsync` | `br_file_fsync` | **TR-A** | 卷级 `sync()` 只有 `br_fs_ops.sync` 槽位, **无路径级入口**(要新增或走 fs_priv) |
| `ftruncate`/`truncate` | `br_file` 的 `setattr(BR_STAT_SIZE)` / `br_truncate` | **TR-A** | 路径级与句柄级都有 |
| `isatty` | `br_file_ioctl` 探测 | **TR-B** | 无 tty 层、无 `file_ops` 里的"是终端"标志(PL011 也不提供) |
| `poll`/`select` | `br_file_poll` + `br_task_sleep` 轮询 | **TR-B** | 设计 SD-7 的 v1 口径**就是**轮询+sleep; 与 P-2 叠加 ⇒ 实际唤醒粒度 ≈100 ms |
| `readv`/`writev` | 循环调 read/write | **TR-B** | 可以模拟, 但"部分完成"语义要写清 |
| `mmap`/`munmap`/`mprotect` | `br_mm_map/unmap` 恒 `-ENOTSUP` | **TR-D(暂)** | 设计判 sqlite 的 `mmap` **可关**; 要真实现属 v2 重定位(P-7) |

### 2.2 文件元数据与目录

| POSIX | 映射 | 档 | 备注 |
|---|---|---|---|
| `stat`/`fstat` | `br_stat` / `br_file_inode`+`getattr` | **TR-B** | `br_stat_t` 只有 `{valid, type, size}` ⇒ `st_mode/st_ino/st_nlink/st_mtime` 只能**合成**(`S_IFREG`/`S_IFDIR` 由 `type` 推) |
| `mkdir` | `br_mkdir` | **TR-B** | **无 `mode` 参数** ⇒ 只能忽略(与 P-8 同源) |
| `rmdir` | `br_rmdir` | **TR-A** | 非空 ⇒ `-ENOTEMPTY`; 挂载点 ⇒ `-EBUSY` |
| `unlink`, `remove` | `br_unlink`(+`rmdir`) | **TR-A** | |
| `rename` | `br_rename` | **TR-A** | 跨挂载点 ⇒ `-EXDEV`(由 FS 决定) |
| `opendir`/`readdir`/`closedir` | `br_opendir/readdir/closedir` | **TR-A** | 泛型实现(目录 = 普通 `file_ops`), 无需目录专用 ops |
| `rewinddir` | 重新 `br_opendir` | **TR-B** | 无 `seekdir/telldir` 底座(目录项无 `d_off`) |
| `seekdir`/`telldir` | — | **TR-C** | 需 `br_dirent_t` 加 `d_off`(vfs 侧面变更) |
| `scandir`/`glob` | `readdir` + `malloc` + `qsort` | **TR-C** | 依赖通配/排序例程(P-3) |
| `chdir`/`getcwd` | runtime/posix **自己的 cwd** + 路径拼装 | **TR-B** | vfs 只认绝对路径(`7-01` §2) ⇒ 相对路径由 runtime/posix 解析; 这是"cwd 归 runtime/posix"的落地 |
| `link`/`symlink`/`readlink` | `br_link`/`br_symlink`/`br_readlink` | **TR-A(已实现)** | ★ **档位已改**(ADR-0014): 设计 `7-01` §2 原写"v2 预留槽位", 原型按 append-only **提前占用**了 `br_inode_ops` 表尾的三个槽位, 解析在 vfs-core(目标可为绝对/相对路径 ⇒ 只有掌握挂载表的人做得到), tmpfs 侧用**带引用计数的数据体**实现硬链接。判据 = `TC-VFS-014..020` |
| `mknod` | — | **TR-D** | 设备节点经 devfs 接入(D21), 不需要 `mknod` |
| `chmod`/`chown`/`access`/`umask` | — | **TR-D** | 无权限模型(P-8); 设计把访问模式策略留给 fd 层, 但**没有** uid/gid 概念 |
| `utime`/`utimensat` | — | **TR-C/D** | 依赖墙钟(P-1)且 inode 无时间戳字段 |
| `statfs`/`statvfs` | — | **TR-C** | 无 `fs_ops` 查询槽位(tmpfs 有配额常量但无对外面) |

### 2.3 进程、线程与同步

| POSIX | 映射 | 档 | 备注 |
|---|---|---|---|
| `pthread_create` | `br_task_create` | **TR-B** | 栈**必须**由调用方给 ⇒ 协议 = `pthread_attr_setstack`(或 runtime/posix 从堆上切) |
| `pthread_join` | `br_task_join` | **TR-B** | 只有 `int *exit_code`, 而 POSIX 要 `void **retval` ⇒ 需要一张 side table(或包装线程) |
| `pthread_exit` | `br_task_exit` | **TR-A** | 不返回 |
| `pthread_self` | `br_task_self` | **TR-A** | 返回 `br_thread_t*`, 需转成 `pthread_t` |
| `pthread_detach` | — | **TR-C** | core 只有 join 回收 ZOMBIE 一条路; detach 要 core 支持(J-1) |
| `pthread_yield`/`sched_yield` | `br_task_yield` | **TR-A** | |
| `pthread_mutex_*` | `br_mutex_*` | **TR-C** | `init/lock/trylock/unlock` 齐; **`destroy` 缺**(P-5) |
| `pthread_cond_*` | `br_cond_*` | **TR-C** | `wait/broadcast/signal` 齐; `destroy` 缺(P-5) |
| `pthread_key_*` | — | **TR-C** | 无 TLS(P-4) |
| `pthread_rwlock_*`/`barrier_*`/`spin_*` | `br_spinlock_*`(仅 spin) | **TR-C** | 读写锁/屏障要新原语; `br_spinlock_*` 是原型新增面(设计 0 命中, 见 `1-04` O-17) |
| `sem_*`(POSIX 信号量) | `br_sem_*` | **TR-A** | `sem_wait/trywait/post/init`, 超时走 `br_sem_take` |
| `fork`/`exec*`/`wait*` | — | **TR-D** | 设计前提 = **单地址空间、无 fork/exec**(`1-01` §2.1) |
| `getpid`/`getppid`/`getuid` | 常量 | **TR-B** | 单 APP 下返回固定值即可(需成文) |
| `exit`/`_exit`/`atexit` | — | **TR-C/D** | `br_task_exit` 是**线程**退出; 单地址空间没有"进程退出"通路 ⇒ 要么定义"退到 idle", 要么设计明确不做 |
| `signal`/`sigaction`/`kill`/`sigprocmask` | — | **TR-D** | 无信号模型; fault 注册面归 v2(P-9) |
| `sysconf`/`uname`/`getenv`/`environ` | 常量 / 无 | **TR-B/D** | `uname`/`sysconf` 可返回静态值; **无环境变量模型** |

### 2.4 时间

| POSIX | 映射 | 档 | 备注 |
|---|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` | `br_clock_now` | **TR-A** | µs 精度, 单调 |
| `clock_gettime(CLOCK_REALTIME)`/`time`/`gettimeofday` | — | **TR-C** | 无墙钟(P-1); **sqlite 模式 A 需要 `gettimeofday`**(`1-01` §7.6 明文) ⇒ 要么补纪元源, 要么成文"纪元 = 启动" |
| `sleep`(秒) | `br_task_sleep` | **TR-A** | |
| `usleep`/`nanosleep` | `br_task_sleep_until` | **TR-B** | 语义可给, 但**实际粒度 ≈100 ms**(P-2) ⇒ "子 tick 睡眠"的承诺必须诚实写明 |
| `clock_getres` | 常量 | **TR-B** | 可如实报 100 ms(v1) |
| `times`/`getrusage` | — | **TR-D** | 无 CPU 记账面 |

### 2.5 stdio 与 libc

| POSIX | 映射 | 档 | 备注 |
|---|---|---|---|
| `malloc/calloc/realloc/free` | `br_malloc/…` | **TR-B** | 别名即可(`br_mem.h:53` 就写着"runtime/posix 的 libc stub 需要"); 但"谁是 libc 的 malloc"要定 |
| `memcpy/memset/memmove/memcmp` | `core/src/string.c` | **TR-B** | 该文件明写"runtime/posix 到位后删掉本文件以归一符号" |
| `strlen/strcpy/strcmp/strchr/strtol/qsort/…` | — | **TR-C** | 要么引 libc, 要么自写(P-3) |
| `fopen/fclose/fread/fwrite/fseek/ftell/fflush` | — | **TR-C** | 需 `FILE*` 层(缓冲/模式/EOF 标志); 底座(fd + read/write)已齐 ⇒ **纯增量** |
| `printf/fprintf/snprintf/vsnprintf/puts` | core 有**私有**格式化器 | **TR-C** | `core/src/log.c`、`panic.c`、`service/hexdump` 各自写了一份子集(**都不导出**); 规范做法是提炼一个 `vsnprintf` 再做 stdio |
| `scanf` 族 | — | **TR-C/D** | 无输入解析器; pd 裁剪可判"不做" |
| `setvbuf/setbuf` | — | **TR-C** | 随 `FILE*` 一起做 |
| `perror/strerror` | `br_error.h` 有码表 | **TR-B** | 码表齐了(ADR-0012), 缺的只是"码 → 字符串"表 |

### 2.6 网络(socket 族)—— **整族 v2**

| POSIX | 档 | 备注 |
|---|---|---|
| `socket/bind/listen/accept/connect/send/recv/setsockopt/…` | **TR-D(本代)** | 设计 `11-01` §2 第 2 项: socket 路由到 `service/lwip`, 排 **v2**; 且"谁拥有 socket 表"仍是开放问题(`11-01` §3) |
| `getaddrinfo`/DNS | **TR-D** | 同上 |

> **红利**: 设计已允许按服务边界分解 —— **core 半**(不需 net)与 **sockets 半**
> (依赖 net)可**分开组合**(`1-01` §7.5 附带红利)。所以 socket 后置**不阻塞** v1 的 runtime/posix。

---

## 3. 汇总(计数口径见 §0)

| 族 | TR-A | TR-B | TR-C | TR-D | 小计 |
|---|---|---|---|---|---|
| fd 与文件 I/O | 8 | 6 | 0 | 1 | 15 |
| 元数据与目录 | 7 | 5 | 4 | 1 | 17 |
| 进程/线程/同步 | 4 | 3 | 6 | 4 | 17 |
| 时间 | 2 | 2 | 1 | 1 | 6 |
| stdio 与 libc | 0 | 3 | 5 | 1 | 9 |
| 网络 | 0 | 0 | 0 | 2 | 2 |
| **合计** | **21** | **19** | **16** | **9** | **65** |

**读法**: 接口条目是"常见 POSIX 名"的一次盘点(不是全集), 档位只看**前置是否在位**。
**TR-A + TR-B = 40 条**是"照着地基能写出来"的部分; 其中 **TR-B 的 19 条全部只需要
runtime/posix 自己那一层**(没有一条要动 native 面) —— 这是本代最值得先吃的一块。

> **实现状态(ADR-0014)**: TR-A + TR-B **已全部落地**(含 `link/symlink/readlink` 三条从
> TR-D 上移的)。仍缺的是 TR-C 的 16 条(每一族都挂着 P-1…P-9 里的一条前置)与 TR-D 的 9 条
> (结构性不做: `fork`/信号/`mmap`/`chmod` 之外的无权限面…)。

---

## 4. 建议的最小可交付切片(v1.0/M2, 不依赖 v2 网络)

| 序 | 切片 | 内容(TR-A/B 里挑) | 判据 | 前置 |
|---|---|---|---|---|
| **S1** | fd 主干 | `open/close/read/write/lseek/dup/dup2/fsync/ftruncate/unlink/stat/fstat/mkdir/rmdir/rename/opendir/readdir/closedir` + 访问模式执法 + 全局 errno | APP 经 POSIX 在 tmpfs 上往返 + `/dev/uart0` 读写; `[POSIXCONF]` 挂 selftest | 无(地基已齐) |
| **S2** | 相对路径 | `chdir/getcwd` + cwd 拼装 | 用相对路径打开同一个文件 | 无 |
| **S3** | 时间 | `clock_gettime(MONOTONIC)`/`sleep`/`usleep`/`nanosleep` + 粒度成文 | 单位校准用例 | **P-2**(成文可先于 tickless) |
| **S4** | 线程 | `pthread_create/join/exit/self/yield` + `mutex/cond/sem` + side table | 多线程读写同一 fd 表 | **P-4/P-5/P-6** |
| **S5** | stdio | `FILE*` 六件 + 提炼的 `vsnprintf` | `fopen/fprintf/fflush/读取` 冒烟 | **P-3**(libc 选型) |
| **S6** | 三方移植 | sqlite 模式 A(`pread/pwrite/usleep/gettimeofday/F_SETLK`) | 建表/插入/查询落盘 | S1+S3+S5 + P-1 + 第三方构建通道 |

**S1 是"最高性价比的一刀"**: 它只消费 `framework/file-table`(ADR-0012)+ `framework/vfs-core`
的**既有**面, 一条 native 原语都不用新增, 却能把 `1-04` §3.1 的"接口行 ❌"翻成 ◐,
并让 `iface-posix` 皮肤第一次有可再导出的东西可指。

---

## 5. errno 口径(与 ADR-0012 的衔接)

- 值域:**`errno` 编号 = Linux 内核编号**; core 的 int 返回是负值, runtime/posix **取负即 errno**
  (`errno = -ret`, 零转换)。原型侧的 `BR_E*` 已按内核 `errno-base.h` **整表** +
  `errno.h` generic 按需补齐(ADR-0012), 并有宿主门禁与 `<errno.h>` 逐码对拍。
- 域子集仍按域成文(SD-10): POSIX 面能返回哪些码, 由**各域的可用子集**决定, 不是"表里有
  什么就能返回什么"。本篇不重开该裁定, 只登记"值表已对齐, 子集尚未逐族成文"。
- **`-EACCES`/`-EPERM` 目前无来源**(P-8): 码有了, 权限模型没有 ⇒ 任何"按权限拒绝"的
  语义在 v1 都只能返回成功或 `-ENOTSUP`, 这一点必须在子集清单里写明(否则是对用户的谎言)。

---

## 6. 开放问题(需设计侧拍板)

| # | 问题 | 影响 |
|---|---|---|
| ~~**Q-1**~~ | ~~**fd 表归谁**: 设计把它写作 `runtime/posix` 的一部分; 原型抽成框架件 `framework/file-table`~~ ⇒ **✅ 已关闭(2026-xx)**: 设计侧已收口 —— `1-03` §1 的插件清单**新增 `framework/file-table` 一行**(v1.0 清单 17 → 18 件), `3-06` §1/§2 与 `11-01` §1 的"fd 表唯一主人"改注为"表在 `file-table`、`runtime/posix` 是消费者", `1-01` §7.2 规则 3 补"归属落在哪个插件由实现定" | — |
| **Q-2** | **墙钟纪元**: 补 RTC/平台熵源契约, 还是成文"纪元 = 启动"并让 sqlite 接受? | `gettimeofday`/`time`/`utime`; sqlite 模式 A |
| **Q-3** | **libc 选型**: picolibc / newlib / 自研子集? `sbrk` 挂接点(`1-01` §4.5 仍是 `[?]`) | 整个 stdio 族 + 三方移植 |
| **Q-4** | **per-thread 私有区**: 是 core 给一个通用槽, 还是 runtime/posix 自建 side table? | `pthread_key_*`/per-thread `errno`/`pthread_join` 的 `void**` |
| **Q-5** | **进程退出语义**: 单地址空间下 `exit()` 是什么?(退到 idle / 不提供) | `exit/atexit`; 也是"AB 正常收尾"的定义 |
| **Q-6** | **`pread/pwrite` 槽位**: vfs 加 `file_ops.pread/pwrite`, 还是 runtime/posix 用 lseek 模拟(接受并发下的不原子)? | 并发正确性; sqlite 模式 A |
| **Q-7** | **权限模型**: v1 是否需要 `mode/uid/gid`? 若不需要, `-EACCES` 的语义要显式声明"永不返回" | `open`/`chmod`/`access`/`umask` |
| **Q-8** | **POSIX 用例组**: `6-01` 需要新增 `TC-POSIX-*`(原型已有自编号的 20 条, 但按 id 检索对不上 ⇒ `br-wa-test-001`) | 验收与冻结(`br-posix.txt` 的第五批) |
| ~~**Q-10**~~ | ~~**插件改名与命名空间**: `svc-posix` → `runtime/posix`~~ ⇒ **✅ 已关闭(2026-xx)**: 设计侧已跟改 —— ① `brickie-v0.1` §8.3 ① 的 namespace 表**新增 `runtime` 行**(`ability` + `subkind = service`; 唯一一个 namespace 与 subkind 不一一对应的行), `4-02` §2 的集合同步为 `…\|service\|runtime`; ② 设计各篇的插件/服务名统一为 `runtime/posix`(D18 行保留原名 + 修订注; `comment/**` 的评审存档**不改写**); ③ golden 组名 `br-svcposix.txt` → **`br-posix.txt`**(短、与插件 short 同名; 同步 `3-01` §0/§1/§15 与 ADR-0001 的组名清单)。原型侧对应 ADR-0015 | — |
| **Q-9** | **组合器的 include 可见性**: 依赖方拿不到被依赖插件的 `include/`, 于是 `#include <unistd.h>` 在别的插件里链不上 ⇒ 这是 `iface-posix`(再导出皮肤)的前置: 工具侧要先决定"再导出的头文件路径怎么可见" | `iface-posix` 落地; `10-01` §2 |

---

## 7. 与既有文档的关系(不改写, 只引用)

| 文档 | 关系 |
|---|---|
| `11-01` §2 第 1 项 | 本篇是它的**现状版**; `11-01` 保留"M2 应当交付什么"的口径 |
| `1-01` §7.6 | 模式 A 的 POSIX 需求清单(`pread/pwrite/ftruncate/unlink/stat/fstat/usleep/gettimeofday/pthread_mutex/mmap(可关)`)在 §2 里逐条有了档位: 7 条 TR-A/B, 1 条 TR-C(P-1), 1 条 TR-D(mmap) |
| `1-04` §3.1「接口」行 | 该行判"`runtime/posix`/`iface-posix`/`iface-min` 全无"; 本篇给出**从哪一档开始补** |
| `3-01` §15 第五批 | `br-posix.txt` 的冻结批次 = M3; 冻结对象就是本篇 §2 的 TR-A/TR-B 集合(子集清单成文是升格前置) |
| `10-01` §2 第 2 项 | `iface-posix` 的子集清单应与本篇**同源**(它再导出 runtime/posix 的面) |
| 原型 ADR-0012 | 文件表(fd 表)与 errno 值域的实现裁定 |
| 原型 ADR-0014 | **本清单的实现裁定**: TR-A/TR-B 的落地方式、链接族的档位上移、六处语义偏离、`chmod/chown/access` 的空操作口径, 以及 §4 的欠账清单 |
| `1-03` §1 的插件清单 | ✅ 已补(Q-1 与 Q-10 关闭时) |

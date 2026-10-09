# runtime/posix

**POSIX 运行时**(设计 D18 的 `svc-posix`; 本树的插件名是 `runtime/posix`, 见 ADR-0015):
`open/read/write/stat/opendir/symlink/pthread/sem/stdio…` 的一面 POSIX 名字,
下面是 core + `framework/{vfs-core, file-table}`。

> 设计出处: `1-01` §7(D18 双角色拆分 + §7.6 三方移植双模式)、`11-01` §1/§2(POSIX 运行时
> 服务: fd 表主人 + `errno = -ret`)、**`11-02`(覆盖清单: TR-A/TR-B 的范围)**、
> `7-01` §1/§4.1(fd 表项 = `br_file_t*`)。
> 逐条裁定与欠账: `docs/decisions/0014-svc-posix-posix-runtime.md`(内容)与
> `docs/decisions/0015-runtime-posix-rename.md`(本插件的名字与命名空间)。

## 它是什么, 不是什么

| | |
|---|---|
| **是** | 一层**翻译**: ① 零转换委托 ② 形状转换 ③ 本服务自己的状态(cwd / errno / FILE* / 线程记录 / 记录锁) |
| **不是** | 第二个内核。它没有自己的文件系统、没有自己的线程、**没有自己的 fd 表** |

fd 表的唯一主人在 **`framework/file-table`**(ADR-0012)。本件只 **消费** 它 —— 于是
"共享状态单一主人"这条规则可以被 grep 出来: `br_ft_alloc` 在全树只出现在本件源码里。

## 覆盖范围(`11-02` 的 TR-A + TR-B)

| 族 | 已实现 |
|---|---|
| fd 与文件 I/O | `open/openat/creat/close/read/write/pread/pwrite/lseek/dup/dup2/fcntl/fsync/ftruncate/truncate/isatty/readv/writev/poll/select` |
| 元数据 | `stat/lstat/fstat/mkdir/chmod/fchmod/chown/fchown/umask/access` |
| 名字空间与链接 | `unlink/rmdir/remove/rename/link/symlink/readlink` |
| 目录 | `opendir/readdir/closedir/rewinddir` |
| cwd | `chdir/getcwd`(相对路径归本服务, vfs 只认绝对路径) |
| 时间 | `clock_gettime(MONOTONIC)/clock_getres/nanosleep/sleep/usleep` |
| 进程身份 | `getpid/getppid/getuid/geteuid/getgid/getegid` |
| pthread | `create/join/exit/self/equal/yield/detach/NULL-attr` + `mutex_*` + `cond_*` |
| semaphore | `sem_init/destroy/wait/trywait/timedwait/post/getvalue` |
| stdio(**流层**) | `fopen/fdopen/fclose/fread/fwrite/fseek/ftell/rewind/fflush/feof/ferror/clearerr/fileno/setvbuf/fgets/fputs/fputc/fgetc/puts/putchar` |
| string / stdlib | `str*/memchr/strerror` + `malloc/calloc/realloc/free/abs/labs/atoi/atol/abort/getenv` |

**明确不做**(子集诚实义务 —— 不声明 = 链接期暴露, 见 `11-02` 的 TR-C/TR-D):
`printf/scanf` 家族(libc 选型未拍)、`fork/exec/wait`、信号族、`gettimeofday/time`(无墙钟)、
`mmap`(vfs 恒 `-ENOTSUP`)、`seekdir/telldir/scandir`、`pthread_key_*`/`rwlock`/`barrier`、
`sem_open`、`exit/_exit`(无进程退出语义)。

## 三条承重口径

1. **`errno = -ret`(零转换)**: core 与 vfs 的错误码**就是**内核编号(ADR-0012 逐码对拍),
   所以实现里没有 errno 换算表 —— 只有 `-rc`。这直接来自 SD-10 与 D18。
2. **两条错误通道, 别混**(POSIX 自己就不一致, 照抄): 系统调用族失败返回 `-1` 并设 `errno`;
   `pthread_*` 族**把错误号当返回值**(不碰 `errno`)。实现里是两个函数(`svc_fail`/`svc_rc`),
   用错通道的症状极隐蔽, 所以 `TC-POSIX-017` 专门钉这一条。
3. **访问模式执法归本件**: vfs **刻意不执法**(ADR-0009 §5 遗留项 5 明写"留给 runtime/posix 的
   fd 层")⇒ "只读 fd 上 write ⇒ EBADF" 必须在这里有, 否则整个 POSIX 面没有权限语义
   (`TC-POSIX-003`)。

## 已知欠账(不在本插件目录可修)

| 欠账 | 影响 | 还债动作 |
|---|---|---|
| **`errno` 是全局的** | 多线程下"线程 A 的失败被线程 B 读走" | 等 core 给出 per-thread 通用槽位(`11-02` 的 P-4) |
| **`st_ino/st_uid/st_gid/st_*time` 恒 0** | 依赖 inode 号去重 / 时间戳的代码拿不到值 | vfs 的 `br_stat_t` 补字段(append-only) |
| **无墙钟** ⇒ `gettimeofday/time` 缺席, `CLOCK_REALTIME` ⇒ `-ENOTSUP` | 日期/超时(绝对时刻)无处安放 | 设计侧拍 Q-2(补纪元源 或 成文"纪元 = 启动") |
| **`printf` 家族缺席** | 无格式化输出 | 先提炼一个 `vsnprintf`(core 里有三份私有子集可收敛), 再做 stdio |
| **`pread/pwrite` 非原子** | 同 fd 并发 pread 会互相踩偏移 | vfs 的 `file_ops` 加 pread/pwrite 槽位(设计 Q-6) |
| **`pthread_detach` ⇒ `-ENOTSUP`** | detach 型线程用不了 | core 加"无 join 回收"路径 |
| **无权限模型** ⇒ `chmod/chown/access` 是空操作(按用户裁定) | 调用方可能以为权限生效 | 设计侧拍 Q-7; 在那之前靠 ADR 与 README 显式声明 |
| **消费者拿不到本插件的 include 路径** | 别的插件 `#include <unistd.h>` 链不上(include 路径按插件隔离) | 组合器把依赖方的 include 目录并入(工具侧改动) |
| `TC-POSIX-*` 是自编号 | 按 id 检索对不上设计用例表 | `br-wa-test-001`:`6-01` 需先补 POSIX 用例组 |

## 目录

```
plugin.toml                    人写   ← 插件级唯一真值(fd/元数据/线程… 的导出面)
include/                       人写   ← **POSIX 头文件面**(unistd.h / fcntl.h / sys/stat.h / …)
src/posix.c                人写   ← 全部实现(翻译 + 本服务的状态)
src/posix_selftest.c           人写   ← [POSIXCONF] 用例(只经 POSIX 头驱动)
README.md                      ← 本文件
tests/smoke.toml               ← 声明面用例骨架(与 in-image TC-POSIX-* 同 id)
```

## 一致性用例

`posix_selftest()` 在 `src/posix_selftest.c`, 逐例打印 `[POSIXCONF] PASS/FAIL`,
末尾 `[POSIXCONF] SUMMARY pass=%u fail=%u total=%u`。20 例 `TC-POSIX-001..020`:
fd 往返 / dup 共享偏移 / 访问模式执法 / pread+pwrite+readv+writev / stat 三兄弟 /
名字空间与 errno 映射 / 符号链接 / 硬链接 / 目录枚举 / chdir+getcwd / 权限空操作 /
openat+O_DIRECTORY / fcntl+记录锁 / poll+select / 时间 / pthread 线程 / pthread 同步 /
stdio / string+stdlib / errno 值域。

★ 本套件有一条与其他插件不同的纪律: **只经 POSIX 头文件驱动**(一行 `br_*` 都不调,
除打印与计时)。理由: 本件的产出就是"POSIX 面", 走 native API 测就等于**没测面本身** ——
而名字/签名/错误通道正是最容易错的地方。门禁 `posix-test` 逐条点名 20 个 tag。

# 0014 — svc-posix(POSIX 运行时服务)

> ★ **改名通知(ADR-0015)**: 本 ADR 落地的那个插件原名 `service/svc-posix`, 现已改名为
> **`runtime/posix`**(目录 `runtime/posix/`, 钩子 `posix_*`, 接口单元 `runtime/posix#posix`),
> `plugin_type`/`subkind`/`api_type`/相位**都不变**。下面的正文保留当时的名字(ADR 是记录,
> 不改写历史); 两者的对应关系与理由见 [`0015-runtime-posix-rename.md`](0015-runtime-posix-rename.md)。
> 设计侧的文档仍用 `svc-posix` —— 那笔收口登记在 ADR-0015 §4.1。

> 状态: **已落地**(QEMU virt aarch64; `[POSIXCONF] 20/0`; `[VFSCONF] 20/0`(13 → 20);
> `brickie test -j8` 全量门禁绿: 宿主 7 + QEMU 8 + 脚本 4)。
> 影响面(**新插件 1 件**): `service/svc-posix/**`(plugin.toml / include/**(12 个 POSIX 头)/
> src/svc_posix.c / src/posix_selftest.c / README.md / tests/smoke.toml);
> `tools/brickie/templates/descriptor/runtime_adapter/c/`(**新模板目录**);
> `product.toml`(`[select]` 追加 `service/svc-posix`); `tests/gates.toml`(新增 `posix-test`
> 门禁); `WORKAROUNDS.md`(扩写 `br-wa-test-001`)。
> **不含**链接族 —— `link`/`symlink`/`readlink` 属**文件系统**的语义(硬链接要"两个名字共享
> 同一份数据", 符号链接要"目标可被解析"), 那一刀是独立的 **ADR-0013**(vfs-core + tmpfs),
> 本 ADR 的 POSIX 面只是它的**第一个消费者**。
> 设计依据: `1-01` §7.2/§7.3/§7.4/§7.6(D18 双角色 + 依赖宪法 + 三方移植双模式)、
> `11-01` §1/§2(POSIX 运行时服务: fd 表主人 / `errno = -ret`)、
> **`11-02-svc-posix-subset.md`(本件的覆盖清单: TR-A/TR-B)**, `7-01` §1/§2/§4.1、
> `3-01` §2/§3/§4/§7/§11。
> 相关: ADR-0009(vfs 与 tmpfs; "访问模式执法归 svc-posix 的 fd 层"就在那里)、
> **ADR-0013**(符号链接与硬链接 —— 本 ADR 的 `link`/`symlink`/`readlink` 直接映射它)、
> ADR-0012(fd 表 = `framework/file-table`; errno 与内核对齐)、ADR-0010(自检统一驱动)。

## 1. 背景与本刀范围

设计 D18 把 POSIX 拆成两个身份: `svc-posix`(**实现**)与 `iface-posix`(**薄皮肤**)。
ADR-0012 先落了它唯一无法绕过的地基 —— **fd 表**(`framework/file-table`)。本刀落**实现**
本身: `11-02` 清单里的 **TR-A(18 条)+ TR-B(19 条)**。

**同一批需求里还有一条与 POSIX 面正交的**: 用户要求"文件系统支持 link/symbol link"。
那条落在文件系统(硬链接要两个名字共享同一份数据; 符号链接要目标可被解析), 单独立为
**ADR-0013**; 本 ADR 只做它的 POSIX 面(`link`/`symlink`/`readlink` 三个 1:1 映射), 并因此把
`11-02` §2.2 里判为 TR-D 的那三条在原型侧改档为 TR-A(设计侧回灌见 §4.8)。

## 2. 决策

### 2.1 覆盖范围 = `11-02` 的 TR-A/TR-B; **不做** TR-C/TR-D

`11-02` 已经逐族给出判定与前置(P-1…P-9)。本刀的实现范围**逐条照着那张表**走, 于是
"哪些没做、为什么没做"不需要在本 ADR 重述 —— 它在 `11-02` 里有档位、有前置编号。

**不声明 = 链接期暴露**(设计的"子集诚实义务", `1-01` §7.6 代价 2)。所以本刀**没有**
`printf` 家族、`fork/exec`、信号、`gettimeofday`、`mmap`、`pthread_key_*`/`rwlock`、
`seekdir`/`scandir`、`exit`。**没有"写了但返回 -ENOTSUP"的假面** —— 唯一的例外是那几处
POSIX **要求**存在、而本代确实语义不同的(逐条列在 §2.4)。

> ★ **后续(ADR-0019)**: `pthread_key_*`、`rwlock`、`barrier` 以及 mutex attr / once /
> 每线程 `errno` / `pthread_detach` **已补齐** —— 它们全部落在 `runtime/posix` 层内
> (本件自己的"每线程记录"表足以承载 errno/TLS 槽位, 不必等 core 的通用槽位), 于是
> 本节的 TR-C 边界收窄为: `printf` 家族 / `fork`·`exec` / 信号 / `gettimeofday` /
> `mmap` / `pthread_cancel` / robust mutex / rwlock 同线程递归读。上段的
> "`pthread_key_*`/`rwlock` 不做"按 ADR-0019 作废。

### 2.2 两条错误通道: 系统调用 `-1`+`errno` / pthread **返回错误号**

POSIX 自己在这一点上不一致, 而混用通道的症状极隐蔽: 调用方写
`if (pthread_mutex_lock(m) != 0)` 时, 若实现返回 `-1` 并设 `errno`, 它会把 -1 当成错误号
去查表(永远查不到), 而 errno 那条路也没人读。本刀的实现里是两个函数——
`svc_fail()`(设 errno + 返回 -1)与 `svc_rc()`(只返回错误号)——并且 `TC-POSIX-017`
专门钉这一条(trylock 忙 ⇒ **返回** EBUSY)。

**三处必须翻译的码**(核心的语义与 POSIX 不同, 不是漏写):
`br_mutex_lock_to(ZERO)` 忙 ⇒ `-ETIMEDOUT`(INV-1 的统一超时码), POSIX 的 trylock 要 `EBUSY`;
`br_sem_take(ZERO)` 忙 ⇒ `-ETIMEDOUT`, POSIX 的 `sem_trywait` 要 `EAGAIN`;
`br_cond_wait` 超时 ⇒ `-ETIMEDOUT`(与 POSIX 同)。除此之外**全部零转换**。

### 2.3 `errno = -ret`(零转换)是 ADR-0012 的直接红利

core 与 vfs 的负 errno **就是**内核编号(ADR-0012 已用宿主 `<errno.h>` 逐码对拍 49 个码)。
于是本刀的实现里**没有 errno 换算表** —— 只有 `-rc`。`<errno.h>` 里的每个 `E*` 也是
`BR_E*` 的**别名**(不是重新定义数值)⇒ 不可能出现"core 改了、POSIX 面没跟上"的漂移。

### 2.4 六处**语义偏离**(都写在头文件里, 并在用例里钉住)

| # | 偏离 | 为什么 | 判据 |
|---|---|---|---|
| 1 | `clock_gettime(CLOCK_REALTIME)` ⇒ **-ENOTSUP** | 没有墙钟源(`11-02` 的 P-1)。编一个"1970 年至今"的数字会让排序/超时静默错 | TC-POSIX-015 |
| 2 | `pthread_cond_timedwait` 的绝对时刻按 **MONOTONIC** 解释 | 同上 —— 唯一可用的时基就是单调钟(POSIX 默认是 REALTIME) | 头注释 + ADR |
| 3 | `getcwd` 缓冲不足 ⇒ `ERANGE`(不是 Linux 的 `ERANGE` 两种写法之一) | POSIX 允许 | TC-POSIX-010 |
| 4 | ~~`pthread_detach` ⇒ `ENOTSUP`~~ ⇒ **ADR-0019 起改为"惰性回收"**(detach 返回 0; 下一次 pthread 调用 join 掉已退出的 detached 线程) | 原理由: core 只有 `join` 一条回收 ZOMBIE 的路径; 假装成功会让 TCB 池悄悄漏光。现在语义成立、时机不精确(ADR-0019 §2.2) | TC-POSIX-026 |
| 5 | `openat(真 dirfd)` ⇒ `ENOTSUP` | 由 fd 反查目录路径不可能(句柄可能已被 rename/unlink) | TC-POSIX-012 |
| 6 | `fsync` 对 tmpfs ⇒ `ENOTSUP` 原样上传 | vfs 的 `fsync` 槽位是 BR_NULL; 假装"落盘了"会让那个判断失去依据 | 头注释 |

### 2.5 `chmod`/`chown`/`access` = **空操作返回 0**(用户裁定), 但 `access` 仍查存在性

**按用户裁定**: `chmod`/`fchmod`/`chown`/`fchown` 接受并忽略, 返回 0。
为什么这是**诚实**而不是偷懒: 本原型**没有**权限模型(vfs 没有 mode/uid/gid 存储面, 也没有
"当前用户")。与"返回 -ENOTSUP"相比, 空操作让按 POSIX 写的代码能跑完(`chmod 0644` 之后
功能不受影响); 真正的风险是"调用方以为权限生效了" —— 那靠**文档与 ADR 显式声明**兜,
不靠返回码兜(返回 -ENOTSUP 也拦不住这个误解, 只会让代码在无关的地方失败)。

**`access` 是个例外, 且刻意如此**: 权限位**一律放行**, 但**存在性照查**。
一个"文件不存在却回答可读"的 `access` 会让调用方的分支逻辑彻底失真 —— 那不是"空操作",
那是撒谎。`umask` 记录并返回旧值(不执法)。

### 2.6 链接族只有三个"1:1 映射", 语义全在 ADR-0013

`link`/`symlink`/`readlink` 在本插件里是**纯映射**(没有自己的一行状态):

| POSIX | 映射 | 注意 |
|---|---|---|
| `symlink(target, path)` | `br_symlink` | ★ 目标串**不做绝对化**: 符号链接的语义就是"按链接所在目录解释", 提前拼成绝对路径会把链接钉死在当前 cwd 上 |
| `readlink(path, buf, cap)` | `br_readlink` | 返回**全长**(可能 > cap, 照 POSIX); 非链接 ⇒ `EINVAL` |
| `link(old, new)` | `br_link` | 末级**不跟随**(POSIX `link(2)`); 目录 ⇒ `EPERM`; 跨挂载 ⇒ `EXDEV` |

`lstat`/`fstat` 同样是薄映射(`br_lstat`/`br_fstat`)。**为什么这些原语在 vfs 而不在本插件**:
目标可能是绝对路径(要重启挂载表匹配)或相对路径(要相对链接所在目录), 只有掌握挂载表与完整
路径的层做得到; 硬链接的"两份名字共享一份数据"更是 FS 的数据模型问题。逐条裁定见 **ADR-0013**
(它同时定了: 展开用重启而非递归、深度上界 8、相对目标的 `..` 不跨挂载、tmpfs 的数据体引用计数)。

### 2.7 `pread/pwrite` 用"保存偏移 + lseek + 读写 + 恢复偏移"模拟 —— **非原子**

vfs 的 `file_ops` 没有 pread/pwrite 槽位(那是设计 `11-02` 的开放问题 Q-6)。单 APP 下
模拟够用, 但**两个线程同时 pread 同一 fd 会互相踩偏移**。这是本刀明确的语义边界,
不是实现疏漏 —— 写进了 §4 的欠账, 也写在 `svc_posix.c` 的函数注释里。

### 2.8 访问模式执法落在 svc-posix(照 ADR-0009 的既定分工)

ADR-0009 §5 遗留项 5 明写"访问模式策略归 svc-posix 的 fd 层"。本刀**兑现**它:
只读 fd 上 `write`/`ftruncate` ⇒ `EBADF`; 只写 fd 上 `read` ⇒ `EBADF`(`TC-POSIX-003`)。
`fcntl(F_SETFL)` 只允许改状态位(`O_APPEND`/`O_NONBLOCK`), **访问模式不可改**(POSIX 亦然)。
另外 `O_APPEND` 有一个 fd 层兜底: `F_SETFL` 之后才置上的 `O_APPEND` 到不了 `br_file_t`
的打开标志(那是打开时固定的), 于是 `write` 在**普通文件**上自己先定位到末尾(设备不可定位,
对它们 `O_APPEND` 本无语义)。

### 2.9 工具链: `runtime_adapter` 的**描述符模板**是新增的

`brickie` 的模板按 `<api_type>/<plugin_type>/<lang>/` 分目录, 而 `svc-posix` 是全树第一个
`api_type = runtime_adapter` 的插件 ⇒ 它的描述符模板不存在(`gen` 立刻报"模板不可读")。
本刀补了 `tools/brickie/templates/descriptor/runtime_adapter/c/plugin_desc.c.tmpl`
(与 native 版逐字相同 —— 描述符的形状与 api_type 无关)。`BRV-TAX-0014`("模板维度未交付")
仍管**插件骨架**模板(`brickie new`), 那是另一件事, 本刀不碰。

## 3. 落地清单与判据

| 面 | 落点 |
|---|---|
| POSIX 头文件面(12 个) | `service/svc-posix/include/`: `unistd.h` `fcntl.h` `dirent.h` `poll.h` `pthread.h` `semaphore.h` `stdio.h` `stdlib.h` `string.h` `time.h` `errno.h` + `sys/{types,stat,uio,select}.h` |
| 实现 | `service/svc-posix/src/svc_posix.c`(翻译 + cwd/errno/FILE*/线程记录/记录锁) |
| 目标套件 | `src/posix_selftest.c` → `[POSIXCONF] 20/0`, 由 core 在全部 `start()` 后驱动(ADR-0010) |
| 链接族的映射 | `link`/`symlink`/`readlink` + `lstat`/`fstat` —— 底座是 **ADR-0013** 的 vfs 面(本 ADR 不含那些原语) |
| 声明面 | `plugin.toml`(`api_type = runtime_adapter`, 三条 `[[dep]]`, `[selftest] cases = 20`) |
| 门禁 | `tests/gates.toml`: 新增 `posix-test`(链接族自己的 7 条用例与门禁在 ADR-0013) |
| 用例骨架 | `service/svc-posix/tests/smoke.toml`(TC-POSIX-001..020 的声明面) |
| 债 | `br-wa-test-001` 扩写(第六组自编号 `TC-POSIX-*`) |

**验证(实测)**

```
$ brickie check                    # 0 错 0 警; 闭包 16 个插件, RAM 40 KiB
$ brickie test -j8                 # 宿主 7 + QEMU 8 + 脚本 4 全绿
  [POSIXCONF] SUMMARY pass=20 fail=0 total=20      ← 本 ADR
  [VFSCONF]   SUMMARY pass=20 fail=0 total=20      ← ADR-0013(含链接族 7 条)
  [FTCONF]    SUMMARY pass=11 fail=0 total=11
  [SELFTEST]  SUMMARY plugins=16 ran=13 skipped=? fails=0 errors=0
$ brickie iface status service/svc-posix#svc-posix   # 一致(hash sha256:dd31f4fb…)
```

> ⚠ **跑门禁前先看 `product.toml [selftest].enabled`**: 它是**生成期**开关(关掉 ⇒ 描述符写
> `BR_PLUGIN_NO_HOOK`, 测试代码被 `--gc-sections` 裁出镜像)。所有 QEMU 门禁的 `require` 里
> 都有 `\[SELFTEST\] SUMMARY … ran=[1-9]…` ⇒ 关掉时**所有 QEMU 门禁都会红**。本刀的实测是在
> `enabled = true` 下跑的(工作树里它是 `inherit`; 若被改成 `false`, 那是环境设置不是本刀)。

## 4. 未做 / 留给下一刀(诚实清单)

1. **`iface-posix` 皮肤**: 本刀只做实现。皮肤要点是"再导出 + stdio/errno 接线"(`10-01` §2),
   而 `brickie` 目前**只校验** `form = "skin"` 的声明、**不生成**再导出代码(没有运行实例)。
   ⇒ 皮肤是独立一刀(工具侧要先落再导出机制)。
2. **消费者拿不到本插件的 include 路径**: 组合器给每个插件的是**自己**的 `include/`, 于是
   别的插件写 `#include <unistd.h>` 链不上(找不到头)。本刀没用例暴露它(套件就在本插件内),
   但**这是 iface-posix 的前置**: 皮肤要再导出, 头文件路径必须先能被依赖方看见。
3. **`errno` 是全局的**(无 per-thread 通用槽位, `11-02` P-4)。
4. **`st_ino/st_uid/st_gid/st_*time` 恒 0**(vfs 的 `br_stat_t` 没有这些面);
   **目录的 `st_nlink` 恒 1**(没有"子目录数"计数)。
5. **链接族的边界**(相对目标的 `..` 不跨挂载、目录 `st_nlink` 恒 1、`mknod` 不占位)
   —— 全部登记在 **ADR-0013 §4**, 不在此重复。
6. **`pread/pwrite` 非原子**(§2.7); **`F_SETLKW` 阻塞式记录锁不做**;
   **`rewinddir` 是"关掉再开"**(vfs 的迭代器没有重置槽位)。
7. **`printf` 家族缺席**(libc 选型未拍); core 里有**三份私有的格式化子集**
   (`core/src/log.c`、`panic.c`、`service/hexdump`)—— 规范做法是先提炼一个 `vsnprintf`。
8. **设计侧回灌**(属设计仓库, 本 ADR 只登记):
   - `11-02` §2.2 把 `link/symlink/readlink` 判为 TR-D; 已实现 ⇒ **改档为 TR-A**
     (原型侧已改; `7-01` 的两个槽位与 `br_stat_t.nlink` 那几笔回灌登记在 **ADR-0013 §4.6**);
   - `11-02` §6 的 Q-1(fd 表归谁)已由 ADR-0012 落地为"框架件", 需在 `1-03` §1 的插件清单
     里补 `framework/file-table` 与 POSIX 运行时两行;
   - `11-02` 需新增 **Q-10**(插件改名: `svc-posix` → `runtime/posix`, 见 ADR-0015);
   - `3-01` §11 的 errno 表可指向 ADR-0012 的"值表按内核抄、子集按域裁"口径;
   - `6-01` 需补 `TC-POSIX-*` 用例组(否则永远是自编号, 与 `br-wa-test-001` 同源)。
9. **`11-02` 的 P-1/P-2/P-3/P-4/P-5 仍未还**: 墙钟、tickless、libc 选型、per-thread 槽位、
   `br_*_destroy`。本刀**没有**顺手补它们(那是 core 的面, 各有自己的裁定面)。

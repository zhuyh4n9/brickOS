# 0028 — `iface/posix` 与真 POSIX **一致**(可机械判定)

> 状态: **已落地**。修掉四处实测偏离(`dev_t`/`blksize_t` 宽度、`O_LARGEFILE`、
> 缺 `STD*_FILENO`); 新增门禁 `tools/check-posix-conformance.sh`(四条判据, 以宿主
> glibc 为对照)并入 `tests/gates.toml`。
> 前置: ADR-0014/0015(POSIX 运行时与 `iface/posix` 皮肤)、ADR-0018(声明依赖闭包 =
> 包含面)、ADR-0020(APP 只依赖 Interface)。

## 1. 背景: APP 看见的 POSIX 面此前**没人核对**

`iface/posix` 是 APP 唯一的 POSIX 入口(它把 `runtime/posix` 的头原样再导出)。但这些头
与真 POSIX 是否一致, **从来没有任何判据** —— 它们是人写的, 而"人写的 POSIX 头"最容易
在三个地方悄悄偏:

| 偏法 | 后果 | 为什么"读一遍头文件"发现不了 |
|---|---|---|
| **类型宽度**不同 | 同一份代码两边看到**不同布局** ⇒ 静默错值 | 宽度要看 `sizeof`, 肉眼看不出来 |
| **常量值**不同 | 同一个 flag 位**含义不同** | `O_TRUNC` 写成 `0001000` 还是 `0002000` 得算 |
| **少一个必需的名字** | 移植过来的 POSIX 代码**编不过** | 得知道"POSIX 要求它存在" |

本刀以宿主 glibc 为对照逐条实测, 查出**四处真实偏离**(见 §3)。

## 2. 决策: 把"一致"变成门禁, 而不是靠 review

`tools/check-posix-conformance.sh` —— 四条判据, **全部以宿主 POSIX 为对照物**:

1. **类型宽度**: `size_t/ssize_t/off_t/ino_t/mode_t/nlink_t/dev_t/blksize_t/blkcnt_t/
   uid_t/gid_t/pid_t/time_t/suseconds_t/useconds_t` 逐条与宿主 `sizeof` 相同;
2. **常量值**: APP 可能比较或传递的常量(`O_*` / `F_*` / `S_I*` / `DT_*` / `SEEK_*` /
   `STD*_FILENO` / `FD_SETSIZE`)与宿主相同;
3. **函数签名**: 本仓声明的每个 POSIX 函数, 其签名与宿主同名函数**赋值兼容**
   (做法: 只包含宿主头, 把**我们写的原型**转成函数指针类型去接宿主函数 ⇒ 不符则编译报错);
4. **结构体成员**: `struct stat` / `struct dirent` 的 POSIX 必需成员**在场**且**类型宽度**正确。

### D-1 判据 ④ **不**比字段偏移(这是刻意的)

POSIX **不规定** `struct stat` 的成员顺序 —— 那是实现自由。若拿 glibc 的 `offsetof`
当判据, 就会把"**glibc 的 ABI**"误当成"**POSIX 的要求**": 我们会被迫复刻 glibc 的
`__pad0` 与保留字段, 而那对"与 POSIX 一致"毫无贡献。⇒ 只查"成员在场 + 类型宽度"。

> 实测: 本仓 `struct stat` 的 `st_nlink`/`st_mode` 顺序与 glibc 相反(其余偏移因此整体
> 平移)。**按 POSIX 这不是偏离**, 所以门禁不判它红 —— 但把它记在这里, 免得后来者
> 把它当成"还没修完"。

### D-2 少数**有意偏离**必须走白名单 + 写明理由

唯一一条: `FD_SETSIZE` 本仓 64、宿主 1024。理由: 本仓 fd 表上界 `BR_FT_MAX = 32`
(fd 0..31), 64 已覆盖**每一个合法 fd** ⇒ 满足 POSIX 对 `FD_SETSIZE` 的要求
(须容纳实现能创建的所有 fd); 宿主 1024 是 Linux 的选择, 不是 POSIX 的下限。取更大
只会让 `fd_set` 占更多栈而无可用 fd 支撑。
**白名单在脚本里是显式表, 且理由写在代码里** —— 没有理由的偏离一律判红。

## 3. 修掉的四处实测偏离

| # | 位置 | 偏离 | 影响 | 修 |
|---|---|---|---|---|
| 1 | `sys/types.h` | `dev_t` = 4 B(宿主 8 B) | `struct stat.st_dev` 布局不同 | `unsigned long` |
| 2 | `sys/types.h` | `blksize_t` = 4 B(宿主 8 B) | `st_blksize` 布局不同 | `long` |
| 3 | `fcntl.h` | `O_LARGEFILE` = `0100000`(64 位宿主 **0**) | 同一个 flag 位含义不同 | `0` |
| 4 | `unistd.h` | **缺** `STDIN_FILENO`/`STDOUT_FILENO`/`STDERR_FILENO` | 用它们的 POSIX 代码**编不过** | 补上(0/1/2) |

**负例已实测**(证明门禁真的守得住): 把 `dev_t` 退回 4 B ⇒ 红, 报
`类型 dev_t: 本仓 4 B, POSIX 8 B`; 把 `O_LARGEFILE` 退回 `0100000` ⇒ 红, 报
`常量 O_LARGEFILE: 本仓 32768, POSIX 0`; 还原 ⇒ 绿。

## 4. 验证

- `bash tools/check-posix-conformance.sh` ⇒
  `ok POSIX 一致性: 类型 15 项 / 常量 53 项 / 签名 155 项 / 结构体 18 个成员`;
- 负例(见 §3)两条都判红;
- `brickie build` 全绿; `brickie test` ⇒ **门禁全绿(全部)**。

## 5. 与设计的关系

设计 `1-01` §7.4(skin)与 §7.3(APP 仅经 Interface)不变: 本刀**不改** `iface/posix`
"原样再导出 `runtime/posix`"的形态(单一真值), 只要求**那份真值本身**与 POSIX 一致。
⇒ 判据落在 `runtime/posix` 的头上, 因为那才是值的所在; 皮肤只是转发。

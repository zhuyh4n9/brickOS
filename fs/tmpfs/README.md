# fs/tmpfs

RAM rootfs: 挂 `/` 的管理类 FS —— 节点与文件数据全部出自 core 堆(`br_malloc`), 完整文件语义,
并在 mount 时预建 `/dev` `/data` `/tmp` 这层命名空间骨架。

> 设计出处: `7-03` §2(tmpfs: "节点与数据均出自 core 堆(`br_malloc`); 完整文件语义
> (read/write/lseek/truncate/unlink/rename/mkdir/stat)"; **挂载为 rootfs("/")** —— 命名空间骨架,
> `/dev`、`/data`、`/tmp` 等由 manifest **预建目录列表**生成; 用途 = rootfs 兜底 / `/tmp` 临时文件 /
> 无介质产品也能拥有完整 VFS 命名空间; 掉电不保持是**角色分工**不是缺陷)、`7-03` §6(挂载计划
> 第 1 条: `fs/tmpfs → /`, 预建三个目录; "挂载点在父 FS 缺失时自动 mkdir")、`7-01` §2(四层 ops,
> **路径走查在 vfs-core**; 目录 inode 的 file 面产出 `br_dirent_t`)、`7-03` §1(FS 契约总览:
> tmpfs 介质 = RAM(core 堆), 写路径 = 完整读写, 无自带缓存)。
> 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`(§2.2 挂载点自动 mkdir 与非空目录
> `-ENOTEMPTY` / §3 裁定 9 配额常量与生成链路 / §5 遗留项)。

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` | `ability` | FS 是能力插件 |
| `api_type` | `native` | 不抛对外头 |
| `subkind` | `fs` | 管理类 FS(`7-03` §1) |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ CORE |
| 依赖(init) | `framework/vfs-core` `>=0.1.0` | 挂载要经 `br_mount_register`(`8-01` §1.4: fs/tmpfs → vfs-core); 拓扑序保证"挂载表先能收挂载" |
| 导出面 | **无导出面**(manifest 里没有 `[[export]]`) | 设计 `7-03` §2 没给 tmpfs 任何函数面 —— 能力就是"一个挂载在 `/` 上的 FS" |
| 资源 | `ram` 4 KiB: 静态部分只有根节点与记账; 文件数据与目录节点在运行期从 core 堆取 | `[[res]]` |
| `[[mount]]` | `path = "/"`, `order = 1` | 设计 `7-03` §6 挂载计划第 1 条(★ 工具侧尚不消费, 见欠账) |
| `[build]` | `sources = ["src/*.c"]`, **无 `includes`** | 本件不抛对外头, 也不被别人 include |
| `sched_class` | `SAFE_PREEMPT` | v1 无锁; 单 APP 阶段可接受(ADR-0009 §5 遗留项 7) |

## 实现什么(行为全部经 vfs-core 的公开 API 被观测)

- **挂载 `/`**: CORE init 调 `br_mount_register("/", &s_fs_ops, &s_fs)`; 建根 inode 与预建目录都是
  **super 层 `mount` 槽位**的职责(由 `br_mount_register` 在规范化路径后调用), `tmpfs_init` 自己
  不建树。`mount` 里做一次记账清零(= "从这里开始只有一棵树")并校验 `fs_priv` 就是那份静态记账体
  (两套访问路径指向不同对象时日志会撒谎)。
- **预建 `/dev` `/data` `/tmp`**: 走**同一条** `tmpfs_mkdir` 路径(名字校验/节点配额/入链只有一处
  实现)。这也是 `fs/devfs` 能把 `/dev` 挂上的前提(挂载点已存在 ⇒ vfs 不再自动 mkdir)。预建失败
  半成品整棵丢弃, 不留纯泄漏。
- **节点模型(与"瞬态 inode"的差别)**: 节点是 `br_malloc` 出来的**持久对象**, 挂在父目录的 child
  单链上 ⇒ `lookup()` 直接交回既有节点、**走查零分配**; 节点指针持久化在 `head.fpriv`, vfs 在 open
  时把它交给 `br_file_t`。`free_inode` 是 **no-op**: vfs 只把瞬态 inode 交给它, 而 tmpfs 没有瞬态
  inode —— 顺手 `br_free` 会把活着的目录树撕掉。
- **完整文件语义**(`7-03` §2 的清单): inode 级 `lookup`/`create`/`unlink`/`mkdir`/`rmdir`/`rename`/
  `getattr`/`setattr`(两种节点**共用一张 iops 表**, 每槽自判类型); 文件面 `open`/`read`/`write`/
  `lseek`/`close`; 目录面 `read` 产出定长 `br_dirent_t`(`.` / `..` / 子项)。`ioctl`/`fsync`/`poll`/
  `poll_attach` **显式** `BR_NULL` ⇒ 调用方得 `-ENOTSUP`(内存文件没有落介质/等待面)。
- **配额记账**: 数据上界 `TMPFS_DATA_MAX = 128 KiB`(口径 = Σ 节点 **cap**, 不是 Σ size —— 占堆的
  是 cap), 节点上界 `TMPFS_NODE_MAX = 48`, 首次写入最小容量 `TMPFS_MIN_CAP = 64`(避免把顺序追加
  写成 O(n²)); 越界 ⇒ `-ENOSPC`; `truncate`/`unlink` 把 cap 交还, 不泄漏。`setattr` 只认
  `BR_STAT_SIZE`, 其余位 ⇒ `-ENOTSUP`(假装成功会让调用方以为改了)。非空目录 `rmdir` ⇒
  `-ENOTEMPTY`(与"挂载点不可删"的 `-EBUSY` 必须分开 —— ADR-0009 §2.2/§3 裁定 4)。

生命周期: `tmpfs_early_init`(EARLY, 堆还没认领 ⇒ 只允许声明, 空手返回)/ `tmpfs_init`(CORE,
注册挂载)/ `tmpfs_start`(START, 一行用量摘要作启动证据)。三个钩子是组合期契约, **不进
`[[export]]`**。

## 边界纪律 / 不做什么

- **不导出 API**: 没有对外头, 没有 `[[export]]`。它的行为只经 VFS 契约被**泛型地**驱动
  (存储域总套件 = vfs-core 的自检 `vfs_core_selftest()`, 由 core 驱动并在 `/` 上跑)。这与
  `app/hello` 的形态同类: 组合期契约(生命周期钩子)不进导出面, 而本件没有别的对外面。
- **不认挂载点**: `tmpfs_mount` **不**按 `TMPFS_MOUNT_PATH` 校验 (`TMPFS_MOUNT_PATH` 只在
  `tmpfs_init` 里当注册参数用): tmpfs 的节点与路径无关, 挂到别的点上也完全自洽(v2 的运行时挂载
  会用到); v1 的"只挂 `/`"由 manifest 挂载计划与拓扑序保证, 校验归组合期。
- **不掉电保持**: 数据落介质归 littlefs/EROFS(`7-03` §2 的角色分工); 本件不做介质、不做块层、
  不做磨损均衡。
- **不做访问模式执法 / 并发加锁**: 见 vfs-core 的同一条(ADR-0009 §5 遗留项 5/7); tmpfs 的树
  没有锁, SD-3(无 inode cache)的性能上限属设计风险 R-S3。
- **不做运行时挂载/卸载**: `mount` 是单次的(同一 path 二次注册由 vfs 以 `-EEXIST` 挡下);
  若 v2 支持卸载, 这里必须换成 `unmount` 配对释放整棵树(O-S3)。
- **`free_inode` 刻意是 no-op**(见上, 这是本件与 littlefs/devfs 形状上的关键差别)。
- **iops 填法与契约头的注释不同**: `br_vfs.h` 写"非目录可为 `BR_NULL`", tmpfs 让所有节点共用
  一张带 `lookup` 的表(槽位里再按类型自校验); 之所以安全, 是因为 vfs 判"是不是目录"只看
  `type`, 不看 `iops`(契约头已把这条写成注)。

## 一致性用例

本件**没有自己的套件** —— 它由 vfs-core 的自检 `vfs_core_selftest()`(13 例, 在
`src/vfs_selftest.c`, 由 core 在全部 `start()` 之后驱动)在 `/` 上**泛型地**验证:
`TC-VFS-003`(mkdir/冲突/`-EISDIR`/`-ENOTDIR`)、`TC-VFS-004`(文件往返)、
`TC-VFS-005`(close/stat 类型与长度)、`TC-VFS-007`(目录迭代: `.` + `..` + 1 个文件 = 3 条)、
`TC-VFS-008`(rename + truncate + `O_TRUNC`)、`TC-VFS-010`(unlink/rmdir 与重复操作 `-ENOENT`)
直接测的就是本件。逐例日志形如 `[VFSCONF] PASS <tag> <desc>`, 末尾
`[VFSCONF] SUMMARY pass=%u fail=%u total=%u`; 门禁 `fs-test` 要求 `pass=N fail=0`。
`tests/smoke.toml` 因此按 `TC-VFS-*` 的 id 写, 并在 `pre` 里写明"这些例子落在本件上"。

## 目录

```
plugin.toml        人写   ← 插件级唯一真值(本目录不改它)
src/tmpfs.c        人写   ← 节点模型 / 完整文件语义 / 配额 / 挂载与生命周期钩子
README.md          ← 本文件
tests/smoke.toml   ← 声明面用例骨架(借 vfs-core 的 TC-VFS-* id, 见上)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-fs-001`: **挂载点是代码常量** —— `src/tmpfs.c` 的 `TMPFS_MOUNT_PATH` 与 `plugin.toml`
  的 `[[mount]]` 是**两处真值**, 工具侧既不消费 `[[mount]]` 也不校验两者一致(与 `br-wa-mem-001`
  的 budget→region 同源: 声明面有了, 生成链路还没有)。退出条件 = 组合器按 `[[mount]]` 产出挂载
  计划(或把挂载点变成描述符字段由 FS 读回), 届时 `TMPFS_MOUNT_PATH` 随之消失。
  登记表见根目录 `WORKAROUNDS.md`。
- 配额(128 KiB 数据 / 48 节点)是**编译期常量**, 不是 manifest 预算生成的 —— ADR-0009 §3 裁定 9
  把它与 `br-wa-fs-001` 一起还(`[[res]].ram_kib` 已是声明面的预算真值, 缺的是"预算 → 代码"的
  生成链路), 不另立条目。
- `TC-VFS-*` 本身是自编号(`br-wa-test-001`): 设计 `6-01` 里没有存储域用例组。本件不持有
  `TC-*` 标记, 但被这套自编号用例覆盖。

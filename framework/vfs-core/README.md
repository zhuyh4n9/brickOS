# framework/vfs-core

纯 VFS: `br_file_t` 不透明句柄 + 挂载表单路由(最长前缀匹配)+ 四层 ops(super/inode/file,
dentry 预留)+ 逐级 `lookup` 走查 + 目录迭代。

> 设计出处: `7-01` §1(统一可打开模型 SD-1/D21: 一切可打开对象 = `br_file_t`, 单一原语
> `br_open` **只走挂载表**)、`7-01` §2(挂载与 ops 四层分层 SD-15/D23: super / inode / file /
> dentry 预留, **路径走查在 vfs-core**, inode v1 **瞬态**、无 cache —— SD-3)、`7-01` §3
> (poll 分期 SD-7: v1 只查就绪位, `poll_attach` 槽位预留、NULL ⇒ -ENOTSUP)、
> `7-01` §4.2 / `8-01` §1.3(框架件归属: `br_file_t`/`br_file_ops` 契约、`br_open` 挂载表·单路由、
> 挂载表、`br_fs_ops`、目录语义 —— **纯 VFS**, D21 撤销设备路由后依赖面收缩到 core)、
> `8-01` §2(dev-core 的 `open_file` 钩子返回 `{fops, fpriv}` —— 本头提供这两个类型)、
> `3-04-memory.md`(`br_open` 经 core 堆分配句柄)。
> 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`(§2.2 单路由 + 挂载点自动 mkdir +
> 挂载点不可删 / §2.3 走查与所有权规则 / §2.4 四层 ops 与"看得见打不开" / §2.5 自检落点 /
> §3 裁定 2 `fpriv` 增量、裁定 3 路径级便捷面、裁定 7 `br_open_err`)。

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` | `ability` | 能力框架件, 不是 service |
| `api_type` | `native` | 消费者是 native 插件 |
| `subkind` | `framework` | `1-01` §4.5 / D19"框架件 = 插件的身份, core 的纪律" |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ ② 完成点 = CORE |
| 依赖(init/type/runtime) | **无 `[[dep]]`**(纯 VFS 的设计结果, 不是漏写) | `8-01` §1.3: 依赖 = core; ADR-0009 §2.1"vfs-core 的 deps 为空" |
| 导出面 | 单元 `vfs`(`sha256:00a441f9…`, `unfrozen`/`experimental`): **20 宏 + 8 类型布局 + 28 函数**(自检入口已移出 `[[export]]`) | `br-vfscore.txt` 的候选面 |
| `[selftest]` | `cases = 13`; 钩子 `vfs_core_selftest()` 在描述符里, 由 core 在全部 `start()` 之后驱动 | `plugin.toml` `[selftest]`; ADR-0010 |
| 资源 | `ram` 8 KiB: 挂载表 8 槽(路径 64 B/槽)+ 句柄在 core 堆; 静态部分约 0.6 KiB | `[[res]]` |
| `sched_class` | `SAFE_PREEMPT` | 无睡眠的纯路径代码 |

四层 ops 的**结构布局**入 golden(D14: 后补字段 = 二进制不兼容): `br_file_ops_t`(9 槽, 含
`poll_attach` 预留)、`br_inode_ops_t`、`br_dentry_ops_t`(v1 全 NULL)、`br_fs_ops_t`。

## 实现什么(契约面 = `include/br/vfs/br_vfs.h`)

`src/br_vfs.c` 的文件头把本件的全部内容列成六条, 与实现一一对应(第 ⑥ 件自 ADR-0010 起与
生产分离, 落在 `src/vfs_selftest.c`):

1. **挂载表 + 最长前缀匹配**(`7-01` §1 的单路由): `br_mount_register` 校验绝对路径与 ops、
   规范化、建 cfg → `ops->mount` → 记录 `{path, ops, priv, root}`; 同一 path 重复挂载 ⇒
   `-EEXIST`, 表满(8 槽)⇒ `-ENOSPC`, 父挂载不存在 ⇒ `-ENODEV`; 非根挂载在**父挂载**里
   逐级自动 mkdir(`7-03` §6 原文); `br_mount_count`/`br_mount_path_at` 提供观测面。
2. **路径规范化 + 逐级 lookup 走查链** + 瞬态 inode 释放时机管理(v1 走查即弃, 无 cache)。
   匹配带**分量边界**: `/devx` 不会错配到 `/dev`(ADR-0009 §2.2)。
3. **文件句柄的分配/释放 + 层 3 派发**: `br_open`/`br_open_err` → 末级 inode → `fops->open`
   建会话; `br_file_read/write/lseek/ioctl/fsync/poll/close`; 槽位缺失 ⇒ `-ENOTSUP`;
   `f == NULL` ⇒ `-EINVAL`; `br_file_close(NULL)`/`br_closedir(NULL)` 幂等 0。
4. **目录迭代(泛型)**: `br_opendir` 就是对目录 inode 做一次 `O_RDONLY` 打开, `br_readdir`
   泛型地调 `fops->read` 取一条定长 `br_dirent_t`(返回 `1`/`0`/负 errno)—— 不需要任何
   "目录专用 ops"(D23 分层的收益)。
5. **路径级便捷面**: `br_stat`/`br_mkdir`/`br_rmdir`/`br_unlink`/`br_rename`/`br_truncate`
   —— "路径 → 父目录 inode + 名字"两段式的翻译层, 即 svc-posix 的 1:1 映射口(ADR-0009 §3 裁定 3)。
6. **存储域一致性用例** `vfs_core_selftest()`, 在 `src/vfs_selftest.c`(见下)。

句柄/inode 访问器(`br_file_inode`/`br_file_fpriv`/`br_file_set_fpriv`/`br_file_flags`/
`br_file_offset`/`br_file_set_offset`/`br_inode_priv`)是 fops 实现者读写私有状态的唯一入口
——`br_file_t` 不透明(D14), 布局只活在本件里。

## 边界纪律 / 不做什么

- **零设备知识(D21 的"纯 VFS")**: 本文件没有任何 `br_cdev`/`br_dev`/设备名分支 —— vfs 看到的
  只是"某个 FS 的某个 inode 带了 fops/fpriv"。设备经 `fs/devfs` 以 `/dev` 文件节点出现。
- **无 inode/dentry cache(SD-3)**: inode 是走查的中间产物, `free_inode` 即弃;
  `br_dentry_ops` v1 全 NULL, 槽位先占免得 v2 改布局。路径走查**零特判**, 一切经挂载表。
- **不做访问模式执法**: `flags` 只做合法性校验(未知位置位 ⇒ `-EINVAL`), 在只读句柄上 write、
  无写权限时 `O_TRUNC` 都不拒绝 —— 访问模式策略归 svc-posix 的 fd 层(ADR-0009 §5 遗留项 5)。
- **不做 cwd**: cwd 是 POSIX 概念, 归 svc-posix; native API 只认绝对路径(`7-01` §2)。
- **不做运行时挂载/卸载**: 挂载点本身 `unlink`/`rmdir` ⇒ `-EBUSY`(卸载语义不早于 v3, O-S3);
  `unmount`/`sync` 是签名先行的可空槽位。
- **不实现事件等待**: `poll_attach` v1 恒 NULL ⇒ `-ENOTSUP`; `br_file_poll` 只查就绪位(SD-7)。
- **不认 FS 名字、不替 FS 做策略**: `vfs_core_selftest()` 对"rootfs 是 tmpfs"这个事实只当作
  "挂载表里有一个 `/`"(契约头原话)。
- 并发未加锁: vfs-core 的挂载表/句柄表无锁(ADR-0009 §5 遗留项 7; 单 APP 阶段可接受)。

## 一致性用例

`vfs_core_selftest()` 在 `src/vfs_selftest.c`, 逐例打印 `[VFSCONF] PASS/FAIL <tag> <desc>`,
末尾一行:

```
[VFSCONF] SUMMARY pass=%u fail=%u total=%u
```

13 例 `TC-VFS-001..013`(挂载表 / 最长前缀 / 创建与冲突 / 文件往返 / open 负例 / 目录迭代 /
rename+truncate / lseek 边界 / unlink+rmdir / 空参健壮性 / 挂载点不可删 / 非根挂载的目录面)。
`sched`/`stdout` 侧的判据是门禁 `fs-test`(`tests/gates.toml`)要求
`\[VFSCONF\] SUMMARY pass=[0-9]* fail=0 ` 且禁止 `[VFSCONF] FAIL`。

★ **总套件由 core 驱动, 不由本件的 start 相跑**: `vfs_core_start()` 只打印一行挂载表摘要;
`vfs_core_selftest()` 是描述符的 `.selftest` 钩子, 由 `br_plugin_manager_selftest()` 在
**全部 `start()` 之后、`br_sched_run()` 之前**统一调用(返回失败项数; 失败不停机, 红绿由
门禁判)。它仍是跨插件的端到端验证 —— 挂载表(rootfs 由 tmpfs)、`/dev` 节点(devfs)、
设备打开(cdev-core 适配层)三者都由**别人**提供(ADR-0009 §2.5), 所以"在谁之后跑"是前提,
而"由谁驱动"是机制: 后者已收归 core, APP 不再认识本件(`app → framework/vfs-core` 的
`allow_edge` 随 ADR-0010 删除)。该节的 13 例在 `6-01` 里**整组不存在**, 属自编号。

## 目录

```
plugin.toml                    人写   ← 插件级唯一真值(本目录不改它)
include/br/vfs/br_vfs.h        人写   ← VFS 契约(本件的对外面)
src/br_vfs.c                   人写   ← 挂载表/走查/文件面/目录面/便捷面(只留机制)
src/vfs_selftest.c             人写   ← 存储域一致性用例 [VFSCONF](由 core 驱动)
src/vfs_internal.h             人写   ← 套件专用的插件私有访问器(不导出, 转调生产实现)
README.md                      ← 本文件
tests/smoke.toml               ← 声明面用例骨架(与 in-image TC-VFS-* 同 id)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-test-001`: `TC-VFS-001..013` 是**自编号** —— 设计 `6-01` 的用例表里没有存储域这一组
  (7-storage/8-device 两域在设计侧整组无用例表)。还债动作: 先补出 `6-01` 的存储域用例组,
  再把自编号改回正式编号(登记表见根目录 `WORKAROUNDS.md`)。

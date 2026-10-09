# fs/devfs

设备文件系统: 把 `framework/dev-core` 的注册表**投影**为 `/dev/<name>` 节点(实时枚举,
`open` 经条目自带的 `open_file` 钩子拿 `{fops, fpriv}`)。**只投影, 不拥有设备。**

> 设计出处: `7-03` §3("所有设备经 devfs 接入 VFS 管理" —— Linux devtmpfs / rCore DeviceFS
> 同型; 节点 = dev-core 注册表**实时枚举**, 注册即上线、无持久化; inode 映射 D23: 根 inode =
> 注册表投影, `lookup(name)` → 设备节点**文件 inode**({fops, fpriv} 由类 **open_file 钩子**给出),
> 再由 `fops->open` 建会话; readdir/stat 列设备名与类别; **unlink/mkdir → -ENOTSUP**, 设备生命
> 周期归驱动注册不归文件系统; devfs 依赖 dev-core + cdev-core(init, 保证钩子就绪)+ vfs-core
> (挂载) —— **不依赖任何子分类的 ops 形状**)、`7-03` §6(挂载计划第 2 条: `fs/devfs → /dev`)、
> `7-03` §1(介质 = dev-core 注册表; 无自带缓存)、`8-01` §2/§3(D21/D23 的 `open_file` 钩子 +
> cdev-core 的通用会话适配)。
> 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`(§2.3 "`type` 是语义唯一真值" /
> §2.4 "看得见打不开" / §3 裁定 6 flash 无文件面、裁定 11 设备节点的 iops 只填 getattr)。

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` | `ability` | FS 是能力插件 |
| `api_type` | `native` | 不抛对外头 |
| `subkind` | `fs` | 管理类 FS(`7-03` §1) |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ CORE |
| 依赖(init) | `framework/dev-core`, `framework/cdev-core`, `framework/vfs-core`, `fs/tmpfs` | 设计 `8-01` §1.4 给了前三条; **第四条 `fs/tmpfs` 是挂载点 `/dev` 的父 FS** —— `br_mount_register` 的自动 mkdir 走"父挂载", 而 `/dev` 由 tmpfs 的 CORE init 预建, 故 tmpfs 必须先挂上(设计 `7-03` §6 的挂载顺序) |
| 依赖(runtime) | `framework/dev-core`(symbol `br_dev_count`)、`framework/vfs-core`(symbol `br_mount_register`) | 只让"谁调谁"在声明面可见, **不参与拓扑**(kind = runtime) |
| 导出面 | **无导出面**(manifest 里没有 `[[export]]`) | 本件的行为经 vfs-core 的公开 API 被观测 |
| 资源 | `ram` 2 KiB: 静态部分只有根 inode + iops/fops 表; 设备节点 inode 在 `lookup` 时从 core 堆现造(约 48 B/次) | `[[res]]` |
| `[[mount]]` | `path = "/dev"`, `order = 2` | 设计 `7-03` §6 挂载计划第 2 条(★ 工具侧尚不消费, 见欠账) |
| `[build]` | `sources = ["src/*.c"]`, **无 `includes`** | 本件不抛对外头, 也不被别人 include |

## 实现什么(三条硬边界就是它的全部内容)

1. **节点 = dev-core 注册表的实时枚举**: `lookup(name)` → `br_dev_lookup(name)`, 命中则在
   `make_dev_node()` 里 `br_malloc` 一个**瞬态** `devfs_node_t`(`{br_inode_t head; char name[]}`),
   `type = BR_INODE_FILE`。readdir 的每一轮都重新问一遍注册表 ⇒ 注册即上线。
2. **设备节点 inode 携带 `{fops, fpriv}`**: 唯一的"设备知识"入口是条目自带的 `open_file` 钩子
   —— 钩子在就调用它拿 `{fops, fpriv}` 塞进 inode; **钩子缺失 = 本子分类不提供文件面**
   (v1 的 raw flash/bdev), 节点照样在、`br_open` 得 `-ENOTSUP`(ADR-0009 §2.4)。
3. **目录面**: 根 inode 是**静态**的(`s_root`, 挂载表持有、`free_inode` 特判), 根 iops =
   `lookup` + `getattr`, 根 fops = `open`(分配一个游标 `devfs_iter_t` 放 fpriv)/`read`
   (产出定长 `br_dirent_t`: 先 `.`、再 `..`、然后逐条枚举注册表名)/`close`(释放游标);
   `write`/`lseek`/`ioctl`/`fsync`/`poll`/`poll_attach` 留空。设备节点 iops = **只有 `getattr`**
   (`valid = BR_STAT_SIZE`、`type = ino->type`、`size = 0` —— 设备节点没有"字节数"这回事)。

**名字空间变更一律 `-ENOTSUP`**: 根的 `create`/`unlink`/`mkdir`/`rmdir`/`rename` 与设备节点的
全部变更槽位都**留空**(NULL), 由 vfs-core 翻译成 `-ENOTSUP` —— 不需要在这里写一行"返回不支持"
(设计 `7-03` §3 原话: "unlink/mkdir → -ENOTSUP")。

生命周期: `devfs_early_init`(EARLY)/ `devfs_init`(CORE, `br_mount_register("/dev", …)`)/
`devfs_start`(START, 一行"投影了 N 个设备"的启动证据)。钩子是组合期契约, **不进 `[[export]]`**。

## 边界纪律 / 不做什么

- **不拥有设备**: devfs 没有"设备表", 也没有生命周期 API —— 设备的存在与否由驱动注册决定;
  节点的上线/下线归驱动注册, **不归文件系统**(`7-03` §3)。
- **不认识任何子分类的 ops 形状(SD-13/D21)**: 本文件**不 include** `br_cdev.h`、不出现
  `br_cdev_ops`; 打开设备只经条目里的 `open_file` 钩子拿 `{fops, fpriv}`。这正是 D21 撤销
  "vfs-core → dev-core" 之后留给 devfs 的位置 —— 若把 cdev 适配放进 devfs, devfs 就得认识
  `br_cdev_ops`, 那正是被撤销的那条依赖(ADR-0009 §4 的否决项)。
- **不提供文件面本身**: 会话的建立/销毁、`read`/`write`/`ioctl`/`poll` 的语义全在
  `framework/cdev-core` 的通用适配层与驱动 ops 里; devfs 只管把二元组塞进 inode。
- **不做持久化**: 无介质、无缓存; 掉电即空(节点是注册表的投影)。
- **只有一层目录**: 任何 `dir` 都被当成根; `.` / `..` 返回根指针本身(非瞬态, 特判不释放)。
- **无注销 ⇒ 游标迭代安全**: v1 注册表只增不减, 所以 readdir 用"注册表下标游标"不会漏项也不会
  重项; 若将来有注销, 这里必须改成"按名字续读"(届时注册表要给出稳定的迭代键)—— 源码注释已写明。
- 并发未加锁(ADR-0009 §5 遗留项 7)。

## 一致性用例

本件**没有自己的套件** —— 它由 vfs-core 的 `[VFSCONF]` 在 `/dev` 上**泛型地**验证, 不认任何
设备名(认设备名是用例该在**器件插件**里做的事, 见 `io/uart-pl011`):

- `TC-VFS-013`: 找到非根挂载(`/dev`)→ `br_opendir`/`br_readdir` **可枚举**(条目数 ≥ 2, 含
  `.`/`..`)→ 每个节点 `br_stat` 成功 → `br_open(O_RDONLY|O_NONBLOCK)` 只回"成功(有文件面)"
  或 `-ENOTSUP`(没有文件面); 别的 errno 一律判红(不放宽)。
- `TC-VFS-012`: 挂载点本身不可 `rmdir`/`unlink` ⇒ `-EBUSY`(卸载不早于 v3, O-S3)。

逐例日志形如 `[VFSCONF] PASS <tag> <desc>`, 末尾 `[VFSCONF] SUMMARY pass=%u fail=%u total=%u`;
门禁 `fs-test` 要求 `pass=N fail=0`。`tests/smoke.toml` 因此按 `TC-VFS-*` 的 id 写, 并在 `pre`
里写明"这些例子落在 `/dev` 这个投影上"。

## 目录

```
plugin.toml        人写   ← 插件级唯一真值(本目录不改它)
src/devfs.c        人写   ← 注册表投影(lookup/readdir/getattr/free_inode)+ 挂载与生命周期钩子
README.md          ← 本文件
tests/smoke.toml   ← 声明面用例骨架(借 vfs-core 的 TC-VFS-* id, 见上)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-fs-001`: **挂载点是代码常量** —— `src/devfs.c` 的 `BR_DEVFS_MOUNT_PATH` 与
  `plugin.toml` 的 `[[mount]]` 是**两处真值**, 工具侧既不消费 `[[mount]]` 也不校验两者一致
  (与 `br-wa-mem-001` 的 budget→region 同源)。退出条件 = 组合器按 `[[mount]]` 产出挂载计划,
  届时该常量随之消失。登记表见根目录 `WORKAROUNDS.md`。
- 设备节点的**私有不属于本件**: 会话由 cdev-core 的适配层建/销; 本件的 `free_inode` 只
  `br_free` 自己现造的 inode(根特判)。
- `TC-VFS-*` 本身是自编号(`br-wa-test-001`): 设计 `6-01` 里没有存储域用例组。本件不持有
  `TC-*` 标记, 但被这套自编号用例覆盖。

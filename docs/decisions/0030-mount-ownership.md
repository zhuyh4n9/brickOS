# 0030 — 挂载归 **APP**: FS 只注册驱动, APP 用 POSIX `mount()` 按名挂载

> 状态: **已落地**。四个 FS 插件(tmpfs/devfs/fat/erofs)改为只登记驱动; vfs-core 增
> **驱动注册表**与 `br_vfs_mount()`; `runtime/posix` 实现 POSIX `mount()`/`umount()`
> 并新增 `<sys/mount.h>`; iface/posix 再导出; brickie 增 **APP 的 `[[mount]]` 声明面**
> 与"计划表 + 应用函数"生成; `app/hello` 声明 6 条计划并在 start 相执行。
> 前置: ADR-0009(VFS/挂载表)、ADR-0014/0015(POSIX 运行时与皮肤)、ADR-0018(包含面)、
> ADR-0020(APP 只依赖 Interface)、ADR-0028(POSIX 一致性门禁)。
> 取代: 设计 `7-03` §6「挂载顺序(**manifest 生成**, 各 FS 插件 init 时执行)」的**执行者**
> —— 计划仍在声明面, 但**由 APP 执行**, 不由 FS 插件自己执行。
> 顺带清掉欠债 `br-wa-fs-001`(挂载点是代码常量 + 两处真值)。

## 1. 背景: "谁挂载"错位导致的三个问题

上一版的形态是: **每个 FS 插件在自己的 init/start 相调 `br_mount_register("/挂载点", …)`**,
同时在 `plugin.toml` 里声明一份 `[[mount]] path = "/挂载点"`。于是:

1. **挂载计划有两个真值** —— 码里的常量一份、manifest 一份, 改一处不报错, 只会静默挂错
   地方(`br-wa-fs-001` 记的正是这条)。工具侧既不消费 `[[mount]]`, 也不校验两者一致。
2. **"挂在哪"写死在 FS 里** —— 同一个 tmpfs 只能挂在 `TMPFS_MOUNT_PATH`, 换挂载点要改 FS
   的源码; 介质类 FS 更糟: `uboot-fat` 把 **挂载点 ↔ 设备** 的三行对应表**硬编码在插件里**
   (`UB_FAT_MNT12/DEV12/…`), 也就是说"哪块卡上的 FAT 挂到哪"是**插件**的决定。
3. **APP 没有发言权** —— 需求方要的是"文件系统交由 app 进行挂载"; 而 APP 既看不到挂载
   表, 也没有挂载原语。

## 2. 决策

### D-1 FS 插件只**登记驱动**, 不挂载

`framework/vfs-core` 增一张**驱动注册表**: `fstype 名 → {ops, fs_priv}`。

```c
int br_fs_driver_register(const char *name, const br_fs_ops_t *ops, void *fs_priv);
```

FS 插件在自己的 start/init 相把自己的**能力**登记进去(`"tmpfs"`/`"devfs"`/`"fat"`/`"erofs"`),
不再调 `br_mount_register`, 也不再声明 `[[mount]]`。重名 ⇒ `-EEXIST`(**不覆盖**: 覆盖会让
先注册者的 `fs_priv` 失去持有者)。

### D-2 APP 用 POSIX `mount()` 按名挂载

```c
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);
```

| 形参 | 本仓语义 |
|---|---|
| `source` | 介质类 FS 的设备名(`"emmc0"`); 无介质(tmpfs/devfs)传 `NULL` |
| `target` | 挂载点(`"/"`、`"/fat12"`) |
| `filesystemtype` | **驱动名**(`"fat"`)—— 驱动注册表按它查 |
| `mountflags` / `data` | v1 **保留**: 非 0 / 非 NULL ⇒ `-EINVAL` |

`mount()` 是**纯转发**: 真正语义(路径规范化 / 父挂载 / 自动 mkdir / 表满 / 重复挂载)全在
vfs-core 的 `br_vfs_mount()`; 位置在 `runtime/posix`(与其它 POSIX 面同一层, 单一真值)。
`umount()` ⇒ `-ENOTSUP`: 运行期卸载属设计 O-S3(不早于 v3), **显式不实现**而非假装成功。

> ★ `mountflags`/`data` 为什么"拒绝"而不是"忽略": 收下不用会让 APP 以为选项生效了。
> 显式 `-EINVAL` 把"这不支持"变成可观测事实。

### D-3 挂载计划写在**APP 的**声明面(A+C)

APP 在自己的 `plugin.toml` 里声明:

```toml
[[mount]]
source = "emmc0"     # 介质类 FS 的设备名; 无介质不写
target = "/fat12"
fstype = "fat"
order  = 3           # 只用于生成期排序
```

brickie 把它生成到该 APP 的 `plugin_desc.c`:**一张表 + 一个应用函数**。

```c
int hello_mount_plan_apply(void);   /* APP 的 start 相调用 */
```

**为什么是"表 + 函数"而不是让 APP 自己遍历**: 计划元素的类型 `br_mount_plan_t` 属 core
契约, 而 **APP 不许 include core 头**(ADR-0020: APP 只依赖 Interface)⇒ 类型不能出现在
APP 源码里。所以表与遍历一起放进生成物, APP 只调一个入口。

**归属校验**: `[[mount]]` 只允许 `plugin_type = "app"` 声明; 非 APP 声明 ⇒ 报
`BRV-MF-0001` 并指路(把旧写法静默吃掉会让"以为挂了、其实没挂")。

### D-4 挂载点与设备**都由调用方给** ⇒ 修掉 `br-wa-fs-001`

- FS 的 mount 槽位从 `cfg->path` / `cfg->dev` 取挂载点与设备, **不再**拿它们与自己的常量
  比对(`uboot-erofs` 原先"挂载点必须等于 `/assets` 否则 -EINVAL"的校验已删)。
- `uboot-fat` 的三行 挂载点↔设备 表**取消**, 改为挂载时按 `(cfg->dev, cfg->path)` **认领**
  一个卷槽(`uboot_fat_vol_claim`)。同一对重复认领幂等; 槽用完 ⇒ `-ENOSPC`。
- ⇒ "哪个 FS 挂在哪"只剩**一个**真值(APP 的 `[[mount]]`), `br-wa-fs-001` 的"两处真值"
  这一半就此还清(另一半 —— 工具不消费 `[[mount]]` —— 也随 D-3 的生成一并解决)。

### D-5 一个必须记住的**生命周期**约束: `cfg->path` 必须长期存活

介质类 FS 会把 `cfg->path` / `cfg->dev` 记进自己的卷表, 挂载返回后在**自检相**还要用。
`br_vfs_mount()` 内部若把**栈缓冲**的规范化路径交给 FS, mount 一返回它就失效 ——
实测症状是"**挂载那一刻日志完全正常, 自检却报 `vol_info_failed` / `sync_rc=-2`**"。

⇒ `mount_with_dev()` 改为: **先把规范化路径写进挂载表的槽位**(生命周期 = 到关机), 再把
`cfg.path` 指过去; mount 失败时回退该槽位(不留"看起来已挂"的半状态)。
这是一条**框架契约**, 不是某个 FS 的局部处理: 任何"记下 cfg 指针"的 FS 都依赖它。

## 3. 落地与证据

启动日志(实测, 取自 `smoke` 门禁):

```
vfs: driver tmpfs          ← 四个 FS 只**登记驱动**
vfs: driver devfs
vfs: driver erofs
vfs: driver fat
mounted / (fstype=tmpfs)   ← APP 按 order 逐条 mount(); 先 "/"
mounted /dev (fstype=devfs)
vfs: mount /fat12 (dev=emmc0)   mounted /fat12 (fstype=fat)
vfs: mount /fat16 (dev=emmc1)   mounted /fat16 (fstype=fat)
vfs: mount /fat32 (dev=emmc2)   mounted /fat32 (fstype=fat)
vfs: mount /assets (dev=emmc3)  mounted /assets (fstype=erofs)
```

- 顺序证据: 驱动登记发生在各 FS 的 start/init; 挂载发生在 **APP 的 start** —— 而插件管理器
  保证"非 APP 的 start 全部跑完才轮到 APP", 所以 APP 挂载时**驱动已注册、块设备已就绪**。
- 介质类 FS 拿到的是 APP 给的源设备(`emmc0..3`), 不再是插件常量。
- `brickie test` 全绿(含 `smoke`/`fs-test`/`fat-test`/`erofs-test` —— 它们覆盖"挂载到位 +
  经 VFS 真的能读写"的端到端判据)。

## 4. 判据

`tools/check-mount-ownership.sh`(注册进 `tests/gates.toml`)—— 两条机械判据:

1. **FS 插件不许自挂载**: `fs/**` 与 `adapter/**` 的源码里不许出现 `br_mount_register`
   (唯一豁免: vfs-core 自己的实现与它的一致性套件);
2. **`[[mount]]` 只归 APP**: 除 `app/**` 外的 `plugin.toml` 里不许有 `[[mount]]`。

> 为什么要有这条门禁: 这两件事都是"改了也不报错、只是行为悄悄变了"的类型 ——
> 谁再往 FS 插件里写一行 `br_mount_register`, 挂载就又变成 FS 自己的决定, 而这在
> 功能测试里**看不出来**(照旧能挂上)。ADR-0030 之前正是这样。

## 5. 与设计的关系

设计 `7-03` §6 的原话是"挂载顺序(**manifest 生成**, 各 FS 插件 init 时执行)"。
本 ADR 保留它的**前半**(计划由声明面生成)与**精神**(静态组合、顺序确定),
只把**执行者**从"各 FS 插件"改成"**APP**" —— 依据是本轮的明确要求
"文件系统交由 app 进行挂载"。差异登记在此, 不重写设计原文。

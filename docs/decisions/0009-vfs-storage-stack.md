# 0009 — VFS 存储栈: framework/vfs-core + fs/tmpfs + fs/devfs(+ dev-core / cdev-core / io/uart-pl011)

> 状态: **已落地**(v0.2.0, QEMU virt aarch64; 存储/设备域四套用例全绿 ——
> `[VFSCONF] 13/0`、`[DEVCONF] 6/0`、`[CDEVCONF] 8/0`、`[IOCONF] 12/0`;
> `brickie test fs-test` 与既有六道门禁全绿)。
> 影响面(**新插件 6 件**): `framework/vfs-core/**`、`framework/dev-core/**`、
> `framework/cdev-core/**`、`fs/tmpfs/**`、`fs/devfs/**`、`io/uart-pl011/**`;
> `core/include/br/core/br_error.h`(**只加不破**: 补 5 个存储域错误码);
> `app/hello/src/main.c`(APP 的 start 接线存储域总套件);
> `product.toml`(`[select]` 追加 3 件 + `[lint].allow_edges` 追加 1 条);
> `tests/gates.toml`(新增 `fs-test` 门禁); `WORKAROUNDS.md`(新增两条 + 扩写 `br-wa-test-001`)。
> 设计依据: `7-01-vfs.md`(§1 SD-1/D21、§2 SD-15/D23、§3 SD-7)、`7-03-concrete-fs.md`
> (§2 tmpfs / §3 devfs / §6 挂载计划)、`8-01-device.md`(§1 体系与形态 A/B/C、§2 注册表与
> open_file 钩子、§3 子分类与 D22 预留、§4 错误模型、§5 ioctl 编码)、`1-03-roadmap.md` §1
> (v1.0 插件清单)、`1-01` §4.5(D19 框架件)/§7.2(依赖方向)/§7.3(依赖禁则)。
> 相关: ADR-0008(core 四阶段启动链: 本刀的 CORE init 都落在阶段 ④ 的 CORE 相)、
> ADR-0003/0004(内存与构建)、ADR-0005(插件管理器: 相位与首败即停机)、ADR-0006(调度器)。
> 登记表: `WORKAROUNDS.md` 新增 `br-wa-fs-001`(挂载点两处真值)与 `br-wa-io-001`(轮询 cdev ≠ 中断 tty)。

## 1. 背景与本刀范围

设计侧 v1.0 的存储栈是**能力框架 = 插件**(D19)的一个完整实例:

```
器件(io/uart-pl011)
  → 设备类(framework/cdev-core: br_cdev_ops / br_flash_ops)
    → dev-core(通用设备: 唯一扁平命名空间 + 子分类注册协议 + open_file 钩子)
      → fs/devfs(/dev 节点, 注册表的投影)
        → framework/vfs-core(br_file_t + br_open 挂载表单路由 + 四层 ops)
          → 消费者(APP / svc-posix)
```

本刀把这条链**从零建到能跑**, 包含 6 个插件(设计清单里的框架件 3 件 + FS 2 件 + I/O 1 件):

| 插件 | 类别 | 设计清单出处 |
|---|---|---|
| `framework/vfs-core` | framework | `1-03` §1 "vfs-core 框架件" |
| `framework/dev-core` | framework | `1-03` §1 "dev-core 框架件" |
| `framework/cdev-core` | framework | `1-03` §1 "cdev-core 框架件" |
| `fs/tmpfs` | fs | `1-03` §1 "fs/tmpfs: rootfs" |
| `fs/devfs` | fs | `1-03` §1 "fs/devfs: /dev 设备节点" |
| `io/uart-pl011` | io | `1-03` §1 "io/uart-pl011: M0 轮询 console → M2 注册 cdev" |

**为什么连框架件一起做**: `fs/devfs` 是 dev-core 注册表的**投影**, 打开设备经 cdev-core 的
`open_file` 钩子(设计 `7-03` §3 / `8-01` §2)。缺了 dev-core/cdev-core, `/dev` 永远是空目录,
"设备经 /dev 接入 VFS"(D21)这条设计主张**无法被验证** —— 只做 vfs-core+tmpfs 能通过用例,
但那验证的是"VFS 能用", 不是"设备与文件同一命名空间能用"。

**未做(明确不在本刀)**: 设计清单里的 `bdev-core`/`io/virtio-blk`/`fs/littlefs`/`svc-posix`/
`iface-posix`/`iface-min`/`service/dbg-bridge`/`platform/host` 仍缺(它们是 M2/M3 的其余部分)。
`fs/erofs` 属 v2.0。本刀**不**碰 core 的既有契约(除 `br_error.h` 的 append-only 补码)。

## 2. 决策

### 2.1 依赖方向: 严格按设计 §7.3 与 `8-01` §1.3/§1.4, 一条都没有放宽

| 边 | kind | 依据 |
|---|---|---|
| `fs/tmpfs → framework/vfs-core` | init | `8-01` §1.4: "fs/tmpfs(rootfs) → vfs-core(挂载 /)" |
| `fs/devfs → {dev-core, cdev-core, vfs-core, fs/tmpfs}` | init | `8-01` §1.4: "devfs → dev-core(枚举)+ cdev-core(钩子就绪)+ vfs-core(挂载)"; tmpfs 是挂载点 /dev 的**父 FS** |
| `framework/cdev-core → framework/dev-core` | init | `8-01` §1.3 依赖表 |
| `framework/cdev-core → framework/vfs-core` | **type** | 适配层返回 `br_file_ops_t`(O-S7 的类型成本) |
| `framework/dev-core → framework/vfs-core` | **type** | `open_file` 钩子签名引用 `br_file_ops`(设计 §1.3 原话"仅头文件") |
| `io/uart-pl011 → framework/cdev-core` | init | `8-01` §6: 驱动向 cdev-core 注册 |
| `io/uart-pl011 → framework/dev-core` | **type** | 用它的 ioctl 编码宏与命名规则 |
| `app/hello → framework/vfs-core` | init | **M0/M2 引导例外**, 见 §2.5 |

* **两条 `type` 边是刻意的**(裁定 1): 设计 `1-01` §7.2 明说 `dev-core → vfs-core` 必须写成
  `type` —— 写成 `runtime` 会按"runtime 参与闭包"把 vfs-core 拉进组合, 从而推翻 D19/O-S7
  ("设备框架本可独立于 VFS 成立"), 即 `8-01` §1.2 的**形态 B**。本刀保留了这条性质:
  `brickie check` 的闭包只包含 `init`/`runtime` 边, 所以把 `[select]` 里的 `fs/devfs` 与
  `fs/tmpfs` 去掉, vfs-core 不会因为 dev-core 而被拖进来。
* `vfs-core` 的 `deps` **为空**: D21 撤销设备路由后它是纯 VFS(依赖面收缩到 core)。
  这是设计结果, 不是漏写 —— 它的 source 文件头与 README 都把这条写成"边界纪律"。

### 2.2 挂载: 最长前缀匹配 + 挂载点自动 mkdir + 挂载点不可删

* **单路由**(SD-1/D21): `br_open` 只查挂载表; **最长前缀 + 分量边界**匹配(所以 `/devx`
  不会错配到 `/dev`);
* **挂载点自动 mkdir**(`7-03` §6 原文: "挂载点在父 FS 缺失时自动 mkdir —— 静态组合的便利性
  优先于显式 mkdir 仪式"): `br_mount_register("/dev", …)` 在**父挂载**(tmpfs 的 `/`)里逐级
  建出 `dev`;
* **挂载点本身不可 unlink/rmdir**(v1): 返回 `-EBUSY`。理由: 那需要一个"卸载"语义, 而运行时
  挂载属 O-S3("落地不早于 v3")。这条也是 tmpfs 的 `rmdir` 非空目录用 `-ENOTEMPTY` 而**不是**
  `-EBUSY` 的原因(裁定 4)—— 两个不同的失败必须有两个码。
* `mount` 与 `unmount`/`sync`: 后两者是 v1 的**签名先行**槽位(可 `BR_NULL`)。

### 2.3 走查与所有权: inode v1 瞬态, 释放点**只有一处**

* 走查链: 挂载表 → 根 inode → 逐级 `iops->lookup` → 末级 inode → `fops->open` 建会话;
  未命中且 `O_CREAT` ⇒ 父目录 `iops->create`(SD-3 的 handle-split VFS, **无 inode cache**)。
* **所有权规则是唯一容易错的地方**, 所以写在 `br_vfs.c` 的文件头并只用一个 `ino_put()`
  收敛: 挂载表的**根 inode 永不**交给 `free_inode`; `lookup`/`create` 产出的瞬态 inode 谁拿
  谁负责(中间层用完即弃、末级交给句柄、路径级便捷面用完立即释放)。
* **`type` 是语义唯一真值**, 不是 `iops == BR_NULL`(裁定 2 的推论): 各 FS 对 `iops` 的填法
  不同(tmpfs 让所有节点共一张表, devfs 分两张)—— 若 vfs 靠 `iops == NULL` 判目录, 同一个
  "文件当目录走查"在两种 FS 下会得到不同 errno(`-ENOENT` vs `-ENOTDIR`)。**实测踩到过**:
  devfs 的设备节点原本给 `iops = BR_NULL`, 于是 `br_stat("/dev/uart0")` 得 `-ENOTSUP`
  (`getattr` 属 inode 层), TC-IO-001 与 TC-VFS-013 同时红。

### 2.4 四层 ops 与"看得见打不开"

* 层 1 super(`br_fs_ops`: mount/free_inode/unmount/sync)、层 2 inode(`br_inode_ops`:
  lookup/create/unlink/mkdir/rmdir/rename/getattr/setattr)、层 3 file(`br_file_ops`: 9 槽)、
  层 4 dentry(`br_dentry_ops`: v1 **全 NULL**, 槽位先占免得 v2 改布局—— D23/D14)。
* **"每文件私有状态"用 `fpriv` 覆盖**, 不需要私有尾: inode 携带 {fops, fpriv}, `fops->open`
  可以覆写 `br_file_fpriv()`(把会话指针放进去), `close` 释放。与 Linux
  `file->private_data` 同型(cdev-core 的适配层就这么用)。
* **能力缺失一律 `-ENOTSUP`, 且"看得见打不开"比"看不见"诚实**: v1 的 raw flash 与 bdev 的
  裸块访问属 v2(`7-03` §3 原文), 所以 cdev-core 给 flash 条目填 `open_file = BR_NULL` ⇒
  `/dev/nor0` **会出现在 readdir 里**但 `br_open` 得 `-ENOTSUP`。这条有机械判据
  (TC-CDEV-008 断言"flash 条目的 open_file 恒 NULL"), 谁给 flash 接上文件面就会红。

### 2.5 自检落点: 存储域总套件在 APP, 设备域各件套件在各插件

* `[VFSCONF]`(13 例)是**跨插件**的端到端验证: 挂载表(rootfs 由 tmpfs 挂)、`/dev` 的节点
  (devfs 投影 dev-core 注册表)、设备打开(经 cdev-core 适配层)三者都由**别人**提供,
  只有消费者视角能一次看清全链路 ⇒ 它由 APP 的 start 调(与既有 4 套 conformance 并列)。
* `[DEVCONF]`/`[CDEVCONF]`/`[IOCONF]`(6/8/12 例)是**域内**验证 ⇒ 留在各插件自己的 start
  (与 platform 的 `[IRQCONF]`/`[MEMCONF]` 同型)。
* 代价: APP 多了一条 `allow_edges` 豁免(`app/hello → framework/vfs-core`)。它与既有的
  `app → platform` 同属 **M0/M2 引导例外**, 正解是 `iface-min`(M2)把这个面收进 Interface
  插件。豁免逐条列名, 使"还有几条例外"一眼可见。

### 2.6 错误码: append-only 补 5 个(不改既有语义)

SD-10 的存储域子集是 `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS`, 而 D23 的
lookup 链与名字空间变更还需要:

| 码 | 值 | 为什么不能用 SD-10 里的代替 |
|---|---|---|
| `-ENOENT` | 2 | lookup 未命中是"不存在", 不是"参数错"(`-EINVAL`), 更不是"设备不在"(`-ENODEV`) |
| `-ENOTDIR` | 20 | "路径分量不是目录" |
| `-EISDIR` | 21 | "对目录做文件操作" —— 与上一条压成一个码会让排障信息归零 |
| `-ENOTEMPTY` | 39 | rmdir 非空目录; 用 `-EBUSY` 会与"挂载点不可删"**撞码**(§2.2) |
| `-EROFS` | 30 | 本就在 SD-10 子集里, 只是 `br_error.h` 原先没定义(补定义, 非新增语义) |

数值沿用 Linux/aarch64 编号(与 `br_error.h` 的既有口径一致), 便于 svc-posix 零转换映射。

### 2.7 收口: `io/uart-pl011` 为什么是"轮询 cdev"而不是"中断 tty"

设计 `1-03` §1 写的是 "M0: 早期轮询 console(**不注册设备**)→ M2: 中断 tty(**注册 cdev**)"。
本刀交付: **注册 cdev + 轮询式会话**(不开 UART 中断)。

* **能做的部分**: `/dev/uart0` 真的出现、能 `br_open`、能 `write`(走 TX FIFO)、能 `ioctl`
  (3 个命令)、能 `poll`、有严格独占(`-EBUSY`)、阻塞 read 按 SD-7 的 v1 口径
  ("poll = 轮询 + `br_task_sleep`")。这足以验证 D21 的**完整形态 A**: 器件 → cdev-core →
  dev-core → devfs → VFS → 消费者。
* **欠的部分(登记 `br-wa-io-001`)**: 不开 UART 中断(绑定表里的 `BR_IRQ_UART0` 无人注册)、
  没有 RX 中断唤醒、没有 tty 层(line discipline / 行缓冲 / 回显)。理由: 中断 tty 需要
  bh/work-queue(设计把 bh 排在 v1.0 的 Stage 2), 而本刀的验收判据是"设备能被 `br_open`
  打开并读写", 不是 tty 功能齐全。
* 与 platform 的 `console_pl011.c` 是**同一块硬件的两种形态**(设计内, 不是重复): 后者是
  M0 早期 console(形态 C standalone —— 内核日志与 panic 通道, **不能**注册设备: panic 路径
  不许依赖插件栈); 前者是 M2 形态 A 侧。两者共用 PL011 ⇒ `SET_BAUD` 会连带改变日志速率,
  已在头文件与 workaround 条目里写明这个危险。

## 3. 设计缺口与逐条裁定

1. **`dev-core → vfs-core` 的 `type` 边会不会把 vfs-core 拉进镜像?**
   不会(裁定 1): `type` 边不进闭包, 只表达"编译期需要类型" —— 这正是设计 `1-01` §7.2 的
   措辞。但**头文件确实会被包含**(O-S7 已登记的"形态 B 的类型依赖成本"), 本刀如实保留:
   形态 B 的裁剪收益是"不链 vfs-core 的目标代码", 不是"不包含它的一个头"。
2. **`br_inode_t` 里加了 `fpriv` 字段(设计只写了 `fops`)**。
   设计 `8-01` §2 说 inode "携带 {fops, fpriv} 二元组", 但 `7-01` §2 的对象模型只把
   `fops` 放进了公共头。本刀补上 `fpriv`, 理由: vfs-core 必须在 `open` 时把 `fpriv` 交给
   句柄, 而它不许知道任何 FS 的私有尾布局; 少了它, 每个 FS 都得再补一个"取 fpriv"的 op
   (即又一处接口面)。布局入 golden(D14), 故登记为**增量**。
3. **路径级便捷面(`br_stat`/`br_mkdir`/`br_rmdir`/`br_unlink`/`br_rename`/`br_truncate`)
   是增量**。`7-01` §1 只写了 `br_open`/`br_file_*` 与 `br_mount_register`, 但目录的名字空间
   变更在 ops 表里是"父目录 inode + 名字"**两段式**, 而消费者**拿不到 inode**(瞬态走查产物)
   ⇒ 必须由 vfs 提供"路径 → 两段式"的翻译层。否则每个消费者(svc-posix 首当其冲)都要自己
   实现一遍走查, 那等于把 vfs-core 的职责复制出去。svc-posix 的 POSIX 面就是这一层的 1:1
   映射(svc-posix 落地时可直接对表)。
4. **`-ENOTEMPTY` 与"挂载点 `-EBUSY`"必须并存**(见 §2.2/§2.6): 实现期实测到两者会撞码 ——
   子 agent 按"非空目录 `-EBUSY`"实现后被一致性用例暴露(tmpfs 的非空 `rmdir` 与 vfs 的
   "挂载点不可删"要给出不同的 errno 才能分别判据)。这条是**用例发现的设计缺口**, 记在此。
5. **`[[mount]]` 声明面是"人读的", 工具侧尚不消费** ⇒ 挂载点是**两处真值**
   (`plugin.toml` 的 `[[mount]]` + 代码里的 `TMPFS_MOUNT_PATH`/`BR_DEVFS_MOUNT_PATH`),
   登记 `br-wa-fs-001`, 退出条件 = 组合器产出挂载计划(与 `br-wa-mem-001` 的
   budget→region 同源: 都是"声明面有了, 生成链路还没有")。
6. **flash 子型的文件面在 v1 不存在**(`open_file = BR_NULL`): 设计 `7-03` §3 只说 "bdev 的 raw
   块访问(/dev/blk0)钩子 = v2", 没提 flash 的 `/dev/nor0`。本刀**统一按同一口径**处理
   (v1 都不给文件面), 并用 TC-CDEV-008 把这条钉成机械判据 —— 将来给 flash 接文件面时,
   该改的是判据与文档, 不是偷偷放行。
7. **`br_open` 失败返回 `BR_NULL` 会丢 errno** ⇒ 增补 `br_open_err(path, flags, int *err)`
   (增量)。原型口径是"返回句柄", 而 svc-posix 必须能把失败原因映射回用户态 errno;
   两个入口共用一份实现, 没有第二处逻辑。
8. **`BR_NAME_MAX = 32` 是静态上界**(设备名规则 `[a-z][a-z0-9]*` 远小于它, 对 tmpfs 的文件名
   是真上界): 超长名字返回 `-ENOSPC`(而不是 `-ENAMETOOLONG` —— 后者不在 SD-10 子集里,
   为一个静态上界再加一个码不划算)。设计 `7-01` §1 只约束设备名, 没约束 tmpfs 文件名。
9. **tmpfs 的配额(128 KiB 数据 / 48 节点)是编译期常量**, 与 `br-wa-mem-001` 同源但**不另立
   条目**: `[[res]].ram_kib` 已经是声明面的预算真值, 缺的同样是"预算 → 代码"的生成链路,
   与 `br-wa-fs-001` 一起还。tmpfs 的 `README` 与源码注释都写了这个退出条件。
10. **`[[res]].stack` 不该给不开线程的驱动声明**: `io/uart-pl011` 初版声明了 1 KiB 栈,
    被 `brickie check` 判红("Σ stack_kib = 5 超出 platform 容量 4")—— 平台的栈容量是
    **给插件自己的线程**的预算, 而不开线程的驱动的 ops 全跑在**调用者的**栈上。已删掉该条。
    这条值得写进 `4-03` 的 manifest 语义(将来)。
11. **名字超长的 errno 有两道判据, 但对外只有一个口径**: vfs-core 在**分量进 lookup 之前**
    就拒 `>= BR_NAME_MAX`(`-ENOSPC`), 而 tmpfs 的 `tmpfs_name_ok()` 也有一道长度检查
    (返回 `-EINVAL`)。后者是**不可达的纵深防御**(前一道已经挡住), 所以端到端恒为 `-ENOSPC`;
    两道检查的**目的不同**: vfs 的是"路径分量上界"(命名空间级), tmpfs 的是"扫描不超过
    `BR_NAME_MAX` 字节"(防未终止串的越界读)。这不是不一致, 但读者容易误会 —— 故记在此。
12. **`getattr`/`setattr` 属 inode 层 ⇒ 每个支持 `br_stat` 的 inode 都要有 iops**:
    devfs 的设备节点因此带一张"只有 getattr"的表(与根 iops 分开 —— 两个对象的能力面不同)。
    这是 D23 分层(Linux 型 `i_op`/`f_op` 分工)的直接后果, 不是额外限制。实测踩到过(见 §2.3)。

## 4. 被否决的替代方案

| 方案 | 为什么不取 |
|---|---|
| 只做 vfs-core + tmpfs + devfs, dev-core/cdev-core 留骨架 | `/dev` 永远为空 ⇒ D21"设备经 /dev 接入 VFS"**无法验证**; 本刀的核心主张就是这条链, 骨架等于没做 |
| 把设备适配层放进 devfs(而不是 cdev-core) | devfs 就得认识 `br_cdev_ops` —— 正是 D21/SD-13 撤销的那条依赖(vfs-core 纯化的同一条理由) |
| 把"设备可直接按名 `br_open`"作为快捷路径 | 设计 D21 明确**撤销**了裸名设备路由("单路由 = 单一语义"); 保留它会让 `/dev` 成为可选装饰 |
| inode 加 cache(v2 行为) | SD-3 明说 v1 无 cache; 单 APP 文件规模下它是优化不是需求(R-S3 已把性能上限登记为风险) |
| `dev-core` 定义 ops 形状(省掉 cdev-core) | 设计 §3 的"通用与形状分离"就是为此; 且新增子分类(bdev/netdev)不该动 dev-core(SD-2) |
| 挂载点在代码里**校验**与 `[[mount]]` 一致 | 两处真值的正确解法是**消掉一处**(生成链路), 不是加一道"两处对不对得上"的运行期检查 |
| `br_open` 只有一个出口, 用全局 errno | 原型是单核协作式, 全局 errno 会在切换点被覆盖; 出参形式没有这个问题 |
| flash/bdev 也给 `open_file`(返回 `-ENOTSUP` 的 fops) | "看得见打不开"要靠 `fops == BR_NULL` 表达才**机械可判**(TC-CDEV-008); 给一个"只会返回 ENOTSUP 的 fops"会让判据消失 |
| 把存储域套件放进 `service/dump` 的 LATE init(像 DBGCONF 那样) | DBGCONF 是**调试域**自己的套件(它测的就是那几个调试插件); 存储域是**别的域**, 塞进 dump 会让"dump 拥有 VFS"这种假依赖出现在声明面 |
| 给 `io/uart-pl011` 打开 UART 中断 | 中断 tty 需要 bh/work-queue(Stage 2), 且本刀判据是"能经 devfs 打开"; 已登记 `br-wa-io-001`, 不假装做完 |
| `SET_BAUD` 直接重写 `CR` 常量 | platform 的 console 拥有同一根 UART 的线路配置; 读改写才能保住 console 的设置(实现已如此) |

## 5. 后果与遗留项(诚实清单)

**本刀交付的**
* 6 个插件进镜像(`.br_plugins` 段 8 → **14 条描述符**; `[PLGCONF] 6/0` 仍绿 —— 段条数与
  生成物计数一致)。
* 39 条新用例(13 VFS + 6 DEV + 8 CDEV + 12 IO)+ `fs-test` 门禁(逐 tag 点名)。
* 设计清单里 D21 的完整形态 A 首次**可运行**: `/dev/uart0` 经 devfs 出现、经 cdev-core
  的适配层被 `br_open` 打开、读写/ioctl/poll 全通。
* `1-01` §4.5 的"框架件 = 插件身份 + core 纪律"在原型里第一次有 3 个真实实例。

**仍在欠的(不要误报为已还)**
1. `br-wa-fs-001`: 挂载点是**代码常量**, `[[mount]]` 只是人读声明面(缺"manifest → 挂载计划"
   的生成链路)。
2. `br-wa-io-001`: PL011 是**轮询 cdev**, 不是中断 tty; `SET_BAUD` 会连带改日志速率。
3. `br-wa-test-001`: 四套新用例的 `TC-*` id 是**自编号**(7-storage/8-device 两域在 `6-01`
   里**整组无用例表**) ⇒ 需要先补出设计侧的用例组, 再把自编号改回正式编号。
4. tmpfs 的配额(128 KiB / 48 节点)是编译期常量, 与 #1 一起还。
5. **`br_open` 的 `flags` 只做"合法性校验", 不做"权限执法"**: 在只读句柄上 write、
   无写权限时 `O_TRUNC`, v1 都不拒绝 —— 设计 `7-01` §1 把访问模式策略留给 svc-posix 的
   fd 层, 而 svc-posix 尚未落地。**这是一条本刀新引入的口径, 值得单独记一笔**(它意味着
   在 svc-posix 到位前, 消费者可以绕开访问模式)。
6. **`poll` 只实现"查询就绪位"**(SD-7 的 v1 半), `poll_attach` 槽位预留(v2 wait-queue,
   与 preempt 同期 —— R-S1)。
7. **并发未加锁**: vfs-core 的挂载表/句柄表与 tmpfs 的树都没有锁。设计 `8-01` §4 的口径是
   "设备 ops 由驱动自锁; FS 侧并发 v1 未另行约定(单 APP + 驱动自锁)"。**但本刀的 uart
   适配层与 vfs-core 都是可被多线程调用的** ⇒ 在 APP 变成多线程消费者之前这是可接受的,
   之后必须补(与 `br-wa-sync-*` 类的债同源, 尚未登记条目 —— 建议主控决定是否单列)。
8. `service/dump` 的启动快照里**还没有挂载表一节**(它现在会打印 region 表与 trace 环);
   把 `br_mount_*` 接进 dump 属下一步(需要 dump 依赖 vfs-core, 会新增一条边 —— 建议随
   svc-posix/debug bridge 一起做)。

## 6. 需要主控执行/注意的动作

* `WORKAROUNDS.md`: 已新增 `br-wa-fs-001` / `br-wa-io-001`, 并把 `br-wa-test-001` 扩写到
  四套新用例; 同时**修正了该文件的表格结构**(`## 已注销` 标题原先插在活动欠债表的表头与
  行之间, 使 7 条活动欠债落在"已注销"表下)。`check-workarounds` 绿(9 标记 ↔ 9 条目)。
* 设计侧建议(本刀发现, 属设计仓库):
  1. `7-01` §2 的 `br_inode_t` 应补 `fpriv` 字段(§3 裁定 2);
  2. `7-01` §1 应补"路径级便捷面"一节(§3 裁定 3)与 `br_open_err`(§3 裁定 7);
  3. `6-01` 需要新增 `TC-VFS-*`/`TC-DEV-*`/`TC-CDEV-*`/`TC-IO-*` 四组用例表(§5 遗留项 3);
  4. `8-01` §4 的错误码子集应补 `-ENOENT/-ENOTDIR/-EISDIR/-ENOTEMPTY`(§2.6);
  5. `4-03` 的 manifest 语义应写明"`[[res]].stack` 只给**自建线程**的插件"(§3 裁定 10)与
     `[[mount]]` 的形状(§3 裁定 5)。
* `product.toml [lint].allow_edges`: 已追加 `["app/hello", "framework/vfs-core"]`(§2.5);
  它与 `app → platform` 一起应在 `iface-min` 落地后删除。
* `br_plugin.h` 文件头引用的 ADR 文件名不一致(`0005-plugin-manager-and-scheduler.md`)——
  ADR-0008 §5 已记, 仍未改(冻结件归主控)。

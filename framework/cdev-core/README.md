# framework/cdev-core

字符设备子分类框架: `br_cdev_ops` 会话式形状 + `br_cdev_register`, flash 子型
(`br_flash_ops`/`br_flash_register`/`br_flash_get`), 以及**通用 `br_file_ops` 会话适配层**
(devfs 经 dev-core 的 `open_file` 钩子取得它)。

> 设计出处: `8-01` §3(`br_cdev_ops` 会话式形状 + `br_cdev_register`; **flash 子型**
> `br_flash_ops`/`br_flash_register`/`br_flash_get` —— spi-nor/nand 对接于此, 与 littlefs
> `lfs_config` 1:1; **统一预留槽位 D22/SD-14**: `ioctl`/`suspend`/`resume`, NULL ⇒ -ENOTSUP;
> file 面的 `poll`/`close` 由本件的适配层提供)、`8-01` §2(`open_file` 钩子 = "设备 → 文件"
> 的适配入口, 返回 `{fops, fpriv}` —— D21/D23)、`8-01` §1.3(框架件归属与依赖:
> cdev-core → **dev-core + vfs-core(类型/适配钩子)**)、`8-01` §4(SD-6 阻塞语义 / SD-10 负 errno)、
> `7-03` §3(cdev-core 提供通用会话适配: open 建会话 / read/write / ioctl / poll / close 转发;
> **lseek/fsync 槽位 NULL ⇒ -ENOTSUP**)、`7-03` §4(flash 子型 ⇄ `lfs_config`)。
> 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`(§2.1 依赖边 / §2.4 "看得见打不开" /
> §3 裁定 6 flash 的 `open_file = BR_NULL`)。

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` | `ability` | 能力框架件 |
| `api_type` | `native` | 消费者是驱动(littlefs 与 `io/*`) |
| `subkind` | `framework` | D19 / `1-01` §4.5 |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ CORE |
| 依赖(init) | `framework/dev-core` `>=0.1.0` | 注册要走它的注册表(`8-01` §1.3 依赖表); 拓扑序保证 dev-core 先就绪 |
| 依赖(type) | `framework/vfs-core` `>=0.1.0` | 适配层返回 `br_file_ops_t`(O-S7 的类型成本); type 边**不进闭包** |
| 导出面 | 单元 `cdev`(`sha256:e2922485…`, `unfrozen`/`experimental`): **3 类型布局 + 5 函数**(无宏; 自检入口已移出 `[[export]]`) | `br-cdevcore.txt` 的候选面 |
| `[selftest]` | `cases = 8`; 钩子 `cdev_core_selftest()` 在描述符里, 由 core 在全部 `start()` 之后驱动 | `plugin.toml` `[selftest]`; ADR-0010 |
| 资源 | `ram` 2 KiB: cdev 槽 8 + flash 槽 4(约 0.5 KiB); 每文件会话 `cdev_file_t` 在 core 堆上(16 B/打开) | `[[res]]` |
| `sched_class` | `SAFE_PREEMPT` | 注册与适配层不睡眠(阻塞语义在驱动 ops 里) |

ops 布局入 golden(D14/D22: 后补字段 = 二进制不兼容 ⇒ 槽位一次占齐):
`br_cdev_ops_t` = `{open, read, write, ioctl, poll, close, suspend, resume}`,
`br_flash_ops_t` = `{read, program, erase, sync, ioctl, suspend, resume}`。

## 实现什么(契约面 = `include/br/dev/br_cdev.h`)

`src/cdev_core.c` 的文件头把本件收敛成三件职责(别的都不做):

1. **`br_cdev_register`**: 校验(`ops` 非空; **open/close 必填**, 缺 ⇒ `-EINVAL`)→ 在 dev-core
   以 `class_id = BR_CLASS_CDEV` 登记一条**并自动填上本件的 `open_file` 钩子**(驱动只实现 ops,
   不需要认识 VFS)。交给钩子的 `dev_priv` 是**本件的槽位指针**(不是驱动私有本身)——因为适配层
   要同时找到 ops 与驱动私有, 而钩子只拿到一个 `void *`。dev-core 侧失败则**回滚**槽位,
   不留"半注册"。`br_cdev_get` 是**形态 B**(不链 vfs-core)的直取口。
2. **通用 `br_file_ops` 适配层**(`7-03` §3 原文): `open` 建会话(`br_malloc` 一个
   `cdev_file_t` → 调 `ops->open` 拿不透明 `sess` → `br_file_set_fpriv()` **覆写** fpriv:
   从"设备槽位"变成"每文件会话"); `read`/`write`/`ioctl`/`poll`/`close` 转发, 驱动槽位为空
   ⇒ `-ENOTSUP`; `close` 释放 `cdev_file_t` 并调用驱动的 `close`; 刻意留空 `lseek`/`fsync`/
   `poll_attach` 三槽(字符设备无偏移语义、无落介质语义; `poll_attach` 是 v2 wait-queue 预留)。
3. **flash 子型**: `br_flash_register`(几何四项自洽 + `read`/`program`/`erase` 三个必填动作
   在**注册期**校验; 未注册取回 ⇒ `BR_NULL`)、`br_flash_get`、`br_flash_geom_get`
   (未注册 ⇒ `-ENODEV`, 出参空 ⇒ `-EINVAL`)。flash 的 `class_id` 同样是 `BR_CLASS_CDEV`
   (它是 cdev 的**子型**), 但 `br_cdev_get` 用"槽位是否落在本件的 cdev 池里"把两个形状分开。

**为什么适配层在 cdev-core 而不是 devfs**(ADR-0009 §4 的否决项): 设计 D21/SD-13 要求 devfs
**不依赖任何子分类的 ops 形状**, vfs-core 更是零设备知识。若把适配放进 devfs, devfs 就得认识
`br_cdev_ops` —— 那正是 D21 撤销的那条依赖。所以"设备语义 → 文件语义"的翻译只能在**子分类框架**
里发生: 它既认识设备形状, 也认识 `br_file_ops`(类型依赖, O-S7)。

**每文件私有状态放哪**(ADR-0009 §3 裁定 2 的落地): vfs-core 把 inode 携带的 `fpriv`(这里 =
设备槽位指针)交给 `br_file_t`; 适配层的 `open` 把它换成自己分配的 `cdev_file_t`(含会话指针),
`close` 释放 —— 于是"每文件一份会话"有落点, 不需要改 `br_file_t` 布局, 也不需要 FS 报"私有尾大小"。

## 边界纪律 / 不做什么

- **不认识任何具体器件**(uart/can/adc/gpio/display…): 那是对接本件的 `io/` 插件;
  形状解释权在本件, **器件语义在驱动**。
- **不做设备生命周期策略**: 注册 = 上线, **无注销**(v1 静态组合, SD-4/O-S3)。
- **不把 cdev 的"会话"概念泄漏给 dev-core**(D20: 通用与形状分离): dev-core 只看到
  `class_id` + `class_priv` + `open_file` + `dev_priv`, 不认识 `sess`。
- **不实现文件面语义**: 只做**转发**与槽位映射; `lseek`/`fsync`/`poll_attach` 恒空 ⇒
  `-ENOTSUP`(与驱动无关, 是"字符设备没有这个语义")。
- **v1 的 raw flash 不给文件面**: flash 条目的 `open_file` 恒 `BR_NULL` ⇒ `/dev/nor0` 出现在
  readdir 里但 `br_open` 得 `-ENOTSUP`(ADR-0009 §3 裁定 6; 设计 `7-03` §3 只说 bdev 裸块访问
  属 v2, 本件把 flash 统一按同一口径处理)。这条有机械判据 `TC-CDEV-008`, 谁给 flash 接上
  文件面就会红。
- 静态上界: `BR_CDEV_MAX = 8` / `BR_FLASH_MAX = 4`, 无动态池增长; 无抢占级锁(v1 单 APP)。

## 一致性用例

`cdev_core_selftest()` 在 `src/cdev_selftest.c`, 逐例打印 `[CDEVCONF] PASS/FAIL <tag> <desc>`,
末尾一行:

```
[CDEVCONF] SUMMARY pass=%u fail=%u total=%u
```

8 例 `TC-CDEV-001..008`(名字非法不污染注册表 / 必填槽位缺失 / 未注册取回不动出参 /
已注册 cdev 往返 / 重名 ⇒ `-EEXIST` / 适配层槽位形状 / flash 前置校验 / flash 无文件面)。
由钩子 `cdev_core_selftest()`(描述符 `.selftest`)在**全部 `start()` 之后**被
`br_plugin_manager_selftest()` 统一调用(返回失败项数; 失败不停机)。
TC-CDEV-004/005/006 的被测对象是内部机制本身, 经 `src/cdev_internal.h` 的两个访问器读
(它们**转调**生产实现, 不复制逻辑)。
门禁 `fs-test` 要求 `\[CDEVCONF\] SUMMARY pass=[0-9]* fail=0 ` 且禁止 `[CDEVCONF] FAIL`。

与 dev-core 同型, 用例刻意**不登记测试设备**(无注销 API ⇒ 一旦登记就永远出现在 `/dev` 里):
正面登记路径由真实驱动覆盖(`io/uart-pl011` 登记 `uart0`); 组合里没有 cdev 设备时
`TC-CDEV-004/005` 只走"不适用"分支并在日志里写明前提。

## 目录

```
plugin.toml                    人写   ← 插件级唯一真值(本目录不改它)
include/br/dev/br_cdev.h       人写   ← 字符设备契约(cdev 形状 / flash 子型)
src/cdev_core.c                人写   ← 注册前置校验 + 会话适配层 + flash 子型(只留机制)
src/cdev_selftest.c            人写   ← [CDEVCONF] 用例(由 core 驱动)
src/cdev_internal.h            人写   ← 套件专用的插件私有访问器(不导出, 转调生产实现)
README.md                      ← 本文件
tests/smoke.toml               ← 声明面用例骨架(与 in-image TC-CDEV-* 同 id)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-test-001`: `TC-CDEV-001..008` 是**自编号** —— 设计 `6-01` 的用例表里没有设备域这一组。
  还债动作: 先补出 `6-01` 的用例组, 再把自编号改回正式编号(根目录 `WORKAROUNDS.md`)。
- 设计 `8-01` §3 的 `suspend`/`resume` 是**设备级** PM 钩子(D22): v1 无统一调用方(O-S6),
  本件只把槽位占齐, 不提供调用路径。

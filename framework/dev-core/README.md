# framework/dev-core

通用设备注册表: 唯一扁平命名空间 + 命名规则执法 + 子分类注册协议 + `open_file` 钩子原样透传
+ ioctl 编码宏。**只管"是个设备"**。

> 设计出处: `8-01` §1.1/§1.2(体系分层与三种组合形态 A/B/C: 每层可独立存在)、`8-01` §1.3
> (框架件归属: 通用设备 = dev-core; 依赖 = core native + vfs-core(**类型**, 仅头文件
> —— `open_file` 钩子引用 `br_file_ops`))、`8-01` §2(注册表条目 `br_dev_class_entry_t` +
> `open_file` 钩子, D21/D23: 由"直接产出 `br_file_t`"演化为"返回 **{fops, fpriv}**"; 设备名
> `[a-z][a-z0-9]*` 无斜杠)、`8-01` §3(dev-core **不定义任何具体 ops 形状** —— 新增子分类不动本件)、
> `8-01` §4(错误模型 = 负 errno, 设备/存储域子集)、`8-01` §5(ioctl 用 Linux 兼容 32 位编码:
> `dir[31:30] size[29:16] type[15:8] nr[7:0]`)。
> 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`(§2.1 两条 `type` 边是刻意的 /
> §3 裁定 1 `type` 边不进闭包、形态 B 可裁剪 —— O-S7 的类型依赖成本如实保留)。

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` | `ability` | 能力框架件 |
| `api_type` | `native` | 消费者是子分类框架与驱动 |
| `subkind` | `framework` | D19 / `1-01` §4.5 |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ CORE |
| 依赖(type) | `framework/vfs-core` `>=0.1.0`, kind = **type** | `8-01` §1.3 原话"vfs-core(类型, 仅头文件)"; ADR-0009 §2.1: 写成 runtime 会把 vfs-core 拉进组合, 推翻 D19/O-S7 的形态 B |
| 依赖(init/runtime) | **无** | dev-core 不调用 vfs-core 的任何函数, 也不参与它的 init 顺序 |
| 导出面 | 单元 `dev`(`sha256:2c7a26ef…`, `unfrozen`/`experimental`): **7 宏 + 1 类型布局 + 6 函数**(自检入口已移出 `[[export]]`) | `br-devcore.txt` 的候选面 |
| `[selftest]` | `cases = 6`; 钩子 `dev_core_selftest()` 在描述符里, 由 core 在全部 `start()` 之后驱动 | `plugin.toml` `[selftest]`; ADR-0010 |
| 资源 | `ram` 2 KiB: 注册表 16 槽(名字 32 B + 条目 32 B/槽 ≈ 1 KiB); **无动态分配** | `[[res]]` |
| `sched_class` | `SAFE_PREEMPT` | 纯静态表操作, 不睡眠 |

## 实现什么(契约面 = `include/br/dev/br_dev.h`)

`src/dev_core.c` 的文件头把职责收敛成三条(R-S4 的防线):

1. **一张静态注册表 + 命名规则执法**: 16 槽 `s_devs[]`, entry 与 name 分开存(名字是注册表的
   **键**, 由 `br_dev_add(name, entry)` 单独给出 —— 设计 `8-01` §2 如此)。`br_dev_add` 校验:
   名字不合规 / entry 为空 / `class_id == BR_CLASS_NONE`(没有形状解释者)⇒ `-EINVAL`;
   重名 ⇒ `-EEXIST`; 表满 ⇒ `-ENOSPC`。`br_dev_name_valid` 实现 `[a-z][a-z0-9]*`、无斜杠、
   长度 1..`BR_DEV_NAME_MAX-1`。
2. **把 `br_dev_class_entry_t`(含 `open_file` 钩子)原样存下、按名/按下标取回**:
   `br_dev_lookup`(未注册 ⇒ `BR_NULL`, 不是错误码)、`br_dev_count`、`br_dev_name_at`、
   `br_dev_entry_at`(越界 ⇒ `BR_NULL`; 顺序 = 注册顺序, devfs 的 readdir 用它)。
3. **ioctl 编码宏**(在头文件里, 纯编译期): `BR_IOC/BR_IOR/BR_IOW/BR_IOWR/BR_IO` 与
   `BR_IOC_DIR/TYPE/NR/SIZE` 分解宏; `BR_IOC_NONE/WRITE/READ` 方向位。

子分类 id `BR_CLASS_NONE/CDEV/BDEV` 是 **append-only** 的 id, dev-core **只透传**,
形状解释权在子分类框架(D14)。重名在**运行期**也执法: 设备名是驱动运行期拼出来的字符串
(可能含 SoC 实例号), `brickie check` 的声明面看不到它 —— 与 `br_service_publish` 的
`-EEXIST` 同型("两个驱动抢同一个 `/dev/uart0`"是静默把一方废掉的错)。

**为什么注册表放在插件而不是 core**: D19 / `1-01` §4.5"能力框架成为插件" —— 不选设备框架的
产品不该为它付 RAM/代码; 而纪律(API 面进 golden/门禁)与 core 相同。所以这里 `br_*` 前缀是
**治理纪律**, 不是 core 本体。

## 边界纪律 / 不做什么

- **不定义任何 ops 形状**: 不定义 cdev/bdev/flash 的 ops 表 —— 形状归 `framework/cdev-core`
  (与将来的 bdev-core), 新增子分类不动本件(`8-01` §3 / SD-2)。本件甚至不知道
  cdev/bdev/flash 是什么, 也不调用任何子分类的 ops。
- **不做设备生命周期策略**: 设备在驱动 `*_register` 时出现、**无显式注销**(v1 静态组合,
  SD-4/O-S3); dev-core 只提供"登记/查找/枚举"三件事。
- **不做策略**: 命名唯一性 / 上限 / 错误码是**机制**, 就这些; 不替子分类解释 `class_priv`。
- **不拥有文件面**: `open_file` 钩子只是**原样透传**的入口 —— `BR_NULL` 表示"本子分类不提供
  文件面"(v1 的 raw flash 与 bdev 裸块访问), devfs 仍列出节点, 但 `br_open` 它会得
  `-ENOTSUP`(ADR-0009 §2.4:"看得见打不开"比"看不见"诚实)。
- `BR_DEV_MAX = 16`、`BR_DEV_NAME_MAX = BR_NAME_MAX` 是编译期静态上界(与 IRQ/PIC 池、TCB 池
  同一手法); 表**只增不减**(无注销), 所以"前 count 项有效"是不变量。

## 一致性用例

`dev_core_selftest()` 在 `src/dev_selftest.c`, 逐例打印 `[DEVCONF] PASS/FAIL <tag> <desc>`,
末尾一行:

```
[DEVCONF] SUMMARY pass=%u fail=%u total=%u
```

6 例 `TC-DEV-001..006`(命名规则反例枚举 / 非法登记不污染注册表 / 查找缺失 ⇒ `BR_NULL` /
枚举与查找同源往返 / 重名 ⇒ `-EEXIST` / ioctl 位布局)。由钩子 `dev_core_selftest()`
(描述符 `.selftest`)在**全部 `start()` 之后**被 `br_plugin_manager_selftest()` 统一调用
(返回失败项数; 失败不停机)。本件判据全部走公开 API, 因此**没有** `src/dev_internal.h`
(对照 vfs-core / cdev-core: 那种头只为"公开 API 表达不出来"的判据存在)。
门禁 `fs-test` 要求 `\[DEVCONF\] SUMMARY pass=[0-9]* fail=0 ` 且禁止
`[DEVCONF] FAIL`。

用例刻意**不造测试设备**: dev-core 没有注销 API, 一旦登记就永远出现在 `/dev` 里; 正面登记
路径由真实驱动覆盖(`io/uart-pl011` 在 CORE init 里登记 `uart0`), 本套件只确认
"坏输入进不来 + 条数不变"。表空时 `TC-DEV-005` 只走"不适用"分支并在描述里写明前提。

## 目录

```
plugin.toml                    人写   ← 插件级唯一真值(本目录不改它)
include/br/dev/br_dev.h        人写   ← 通用设备契约(注册表条目 / 命名 / ioctl 编码)
src/dev_core.c                 人写   ← 静态注册表 + 命名执法(只留机制)
src/dev_selftest.c             人写   ← [DEVCONF] 用例(由 core 驱动; 无内部头, 只走公开 API)
README.md                      ← 本文件
tests/smoke.toml               ← 声明面用例骨架(与 in-image TC-DEV-* 同 id)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-test-001`: `TC-DEV-001..006` 是**自编号** —— 设计 `6-01` 的用例表里没有设备域这一组
  (7-storage/8-device 两域在设计侧整组无用例表)。还债动作: 先补出 `6-01` 的用例组, 再把
  自编号改回正式编号(根目录 `WORKAROUNDS.md`)。
- `O-S7`(设计侧已登记): `open_file` 钩子签名引用 `br_file_ops` ⇒ 不选 vfs-core 的**形态 B**
  组合仍会拉进该头文件(仅类型, 无 init/call 依赖)。本件如实保留这条类型成本; 取舍(接受 /
  条件编译 / 把钩子下沉到 cdev-core)待真实裁剪收益出现再定。

# 8-01 — 设备管理(device management: dev-core / cdev-core / bdev-core)

> 章节: **8-device**(设备域, 独立成章)。存储侧(VFS / 块设备 / 具体 FS)→ 章节 `7-storage`。**本篇含设备体系总览与三种组合形态**(§1)。
> **契约归属(D19/D20)**: **dev-core 描述通用设备**——注册表(唯一扁平命名空间)、命名规则、调用语义/错误模型、ioctl 编码、**子分类协议**; **向下分为 cdev、bdev 等子分类框架**; **spi-nor/nand 对接 cdev-core**(flash 子型)。
> 版本: v1.0。本篇决策 SD-2/SD-5/SD-6/SD-10/SD-11/SD-12/SD-13/SD-14。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **形态 A / B / C** | — | §1.2 的三种组合形态: A = 器件→设备类→dev-core→VFS(全栈接入); B = 只到 dev-core(无 VFS); C = standalone(不入册) |
| **API** | Application Programming Interface | 应用程序接口 |
| **bbm** | bad block management | 坏块管理(NAND 之上需叠加, v2+) |
| **cdev / bdev / netdev** | character / block / network device | 设备子分类框架: 字符 / 块 / 网络(dev-core 之下的对称子分类) |
| **dev-core / cdev-core / bdev-core / vfs-core** | — | 四个框架件(插件形态的能力基础设施): 通用设备 / 字符设备子分类 / 块设备子分类 / 纯 VFS; 纪律同 core(golden/门禁) |
| **DeviceFS** | — | rCore 的设备文件系统(devfs 的同型参照) |
| **DMA** | Direct Memory Access | 直接内存访问(外设不经 CPU 读写内存) |
| **eMMC** | embedded MultiMediaCard | 嵌入式多媒体卡(块介质) |
| **EROFS** | Enhanced Read-Only File System | 只读压缩文件系统(代码/资产分区, v2.0) |
| **FAST / SLOW** | — | 级联中断域双上下文契约(CA-9): FAST = ISR 内可读寄存器, SLOW = 仅下半部(bh)可读 |
| **FTL** | Flash Translation Layer | 闪存转换层(把 flash 语义映射为块语义) |
| **ioctl** | input/output control | 设备控制原语(Linux 兼容 32 位编码, SD-5) |
| **IRQ** | Interrupt Request | 中断请求(硬件中断线) |
| **ISR** | Interrupt Service Routine | 中断服务例程(中断上下文中的处理函数) |
| **NAND** | Not-AND | 与非门闪存(块擦写、有坏块, 需 bbm/FTL) |
| **NOR** | Not-OR | 或非门闪存(可随机读 / XIP) |
| **OOB** | Out-Of-Band | 带外(闪存每页的备用区, 存 ECC/坏块标记) |
| **open_file 钩子** | — | 子分类框架经 dev-core 提供的"设备 → 文件"适配入口, 返回 {fops, fpriv}(D21/D23, §2) |
| **O-S1–O-S8** | — | 存储与设备域的开放问题编号 |
| **PM** | Power Management | 电源管理 |
| **PMIC** | Power Management IC | 电源管理芯片(典型级联中断域来源) |
| **POSIX** | Portable Operating System Interface | 可移植操作系统接口 |
| **QSPI** | Quad SPI | 四线 SPI(NOR flash 的高速接口形态) |
| **RAM** | Random Access Memory | 随机访问存储器 |
| **R-S1–R-S4** | — | 存储与设备域的风险编号 |
| **SD-1–SD-15** | — | 存储与设备域决策编号(7-01/7-02/8-01 §决策记录) |
| **VFS** | Virtual File System | 虚拟文件系统(统一可打开模型 + 挂载表) |

> **编号约定**: `SD-2`/`SD-5`/`SD-6`/`SD-10`–`SD-14` = 本篇 §7 决策; `SD-1`/`SD-3`/`SD-4`/`SD-7`/`SD-15` = 存储侧(7-01); `SD-8`/`SD-9` = 块设备(7-02); `O-S*`/`R-S*` = 开放问题 / 风险(§8); `D19–D26` = 全局决策; **形态 A/B/C** = §1.2 的三种组合形态(接 VFS / 只用设备框架 / standalone)。

## 1. 设备体系与三种组合形态(域总览)

### 1.1 体系分层(自下而上)

设备管理是一条**自器件到文件**的分层链, 每层只认识下一层的契约:

```
deviceXXX(具体器件)        uart-pl011 / qspi-nor / virtio-blk / PMIC / …
   │  实现 ops 表并注册
   ▼
具体设备类(class)          cdev-core(br_cdev_ops;flash 子型 br_flash_ops)
   │                       bdev-core(br_bdev_ops, 见 7-02-bdev §1)
   │  br_cdev_register / br_flash_register / br_bdev_register
   ▼
dev-core(通用设备)          唯一扁平命名空间、命名规则、调用语义/错误模型、
   │                       ioctl 编码、子分类注册协议(§2/§3)
   │  fs/devfs 枚举注册表 → /dev/<name> 节点 + open_file 钩子
   ▼
VFS(vfs-core)              br_open 挂载表 · br_file_t / br_file_ops(7-01-vfs §1/§2)
   │
   ▼
消费者                     APP(经 Interface 插件) / 服务·框架件(native)
```

要点:
- **每一层都可独立存在**: 器件不必注册(形态 C), 设备类不必上行到 VFS(形态 B), 只有需要 `br_open` 统一命名空间时才引 VFS(形态 A)。
- **层的职责不串**: dev-core 只管"是个设备"(命名/语义/注册表), 形状归子分类; devfs 只做**投影**(注册表 → 文件节点), 不拥有设备; vfs-core 零设备知识(D21)。
- 图中 VFS 及其以上**不是设备管理的必需部分**——本节的形态 B/C 说明其余两种合法组合。

### 1.2 三种组合形态(裁剪谱系)

同一条体系链, 按产品需求截断在不同高度, 得到三种合法形态:

**形态 A — 体系化接入 VFS(全栈)**: 器件 → 设备类 → dev-core → devfs → VFS。

- 组成: `deviceXXX` + 对应 `*-core` + `dev-core` + `vfs-core` + `fs/devfs`(+ `fs/tmpfs` rootfs 可选)
- 设备可见性: 以 **`/dev/<name>`** 出现; 打开/session 经子分类 **open_file 钩子**(cdev-core 的通用 `br_file_ops` 适配层)接入
- 消费者: APP 经 Interface 插件——POSIX 面经 svc-posix fd 表(`iface-posix`), native `br_open` 面经 `iface-min`(A-2; 见 `brickie` v0.1 §13.2); **服务/框架件之间的 native 调用不受此约束**
- 适用: **POSIX 产品、需要"文件与设备同一命名空间/同一句柄"的产品**(SD-1 单路由的完整收益)
- 代价: 链入 vfs-core + devfs; 每次打开多一层挂载表路由

**形态 B — 只用设备框架(无 VFS)**: 器件 → 设备类 → dev-core, 到此为止。

- 组成: `deviceXXX` + 对应 `*-core` + `dev-core`(**不链** `vfs-core` / `fs/devfs` / 任何 FS)
- 设备可见性: 消费者经**类 API 直取**——`br_cdev_*` 会话、`br_flash_get("nor0")`、`br_bdev_get("blk0")`
- 保留: 注册表与命名唯一性、调用语义/错误模型(SD-6/SD-10)、子分类纪律、ops 预留(D22)、golden/门禁(SD-11/SD-12)
- 失去: `/dev` 命名空间; cdev-core 的 `br_file_ops` 适配层与 `poll`/`close` 的 file 面**不再必需**
- 适用: **简易 OS / 无文件系统需求的产品**——裸机式控制件、只有 UART/GPIO/ADC 的设备、不需要路径概念的组合
- 这正是 **D19 "框架件 = 可裁剪插件"** 的直接兑现: 不选 vfs-core 就没有 VFS 成本

**形态 C — standalone device**: 器件**不入册**, 不经 dev-core 与任何子分类。

- 形式: ops 表由拥有者私下持有(platform 内建、某服务的私有件), 或驱动自带私有接口; 无注册调用
- 适用: 平台内建器件(早期轮询 console、时钟/中断控制器、PMIC 级联域控制器)、**单一消费者且无复用价值**的器件、以及 **M0 早期**(框架件尚未落地时, 如 `io/uart-pl011` 的 M0 轮询 console 不注册设备, 1-03 §1)
- **代价与风险**: 组合期看不见它——**设备名唯一性、类契约合规、统一错误模型都得不到检查**(IRQ/DMA/RAM 资源冲突仍由 manifest 声明覆盖); 对下游消费者是隐式耦合
- 仍受的纪律: ISR/DMA/cache 三条纪律(§6)、ops 预留(D22)、符号命名(3-01 §13.3/CA-10)
- 演进路径: 出现第二个消费者或需要 `/dev` 暴露时 → 升为形态 B(注册入类)→ 再按需升为形态 A

**判据表:**

| 判据 | A 全栈(接 VFS) | B 只用设备框架 | C standalone |
|---|---|---|---|
| 需要文件/设备统一命名空间(路径打开) | 是 | 否 | 否 |
| 需要 fd / POSIX 语义 | 是 | 可选(自带会话 API) | 否 |
| 链入 vfs-core | **是** | 否 | 否 |
| 链入 dev-core + 子分类 | 是 | **是** | 否 |
| 设备名唯一性/类契约受组合期校验 | 是 | 是 | **否(风险)** |
| 资源冲突(IRQ/DMA/RAM)受组合期校验 | 是 | 是 | 是(manifest 声明) |
| 典型场景 | POSIX 产品、存储产品 | 简易 OS、无 FS 控制件 | 平台内建件、M0 早期 |

> **形态选择是组合决策, 不是架构分叉**: 三种形态共用同一套器件 ops、同一份错误模型与同一条 ISR/DMA 纪律; 差别只在"链进哪些框架件、是否上行到 VFS"。因此同一驱动在形态 B→A 升级时**驱动代码零改写**, 只是新增注册与组合声明。

### 1.3 框架件归属与依赖(D19/D20)

**能力归属**: file/open/VFS 接口 = **vfs-core**; 通用设备 = **dev-core**, 向下子分类: 字符设备 = **cdev-core**(spi-nor/nand 的 flash 子型在此)、块设备 = **bdev-core**(**依赖 dev-core**); 具体文件系统(littlefs)依赖 **vfs-core**(另按介质绑定 cdev-core / bdev-core)。**所有设备经 /dev(devfs)接入 VFS 管理, tmpfs 挂载为 rootfs("/")**(D21, Linux devtmpfs/rCore DeviceFS 同型)。

| 框架件 | 提供(契约) | 依赖 |
|---|---|---|
| **dev-core** | **通用设备**: 注册表(唯一扁平命名空间)、命名规则、调用语义/错误模型、ioctl 编码、**子分类注册协议**(§2) | core native + vfs-core(类型, 仅头文件——open_file 钩子引用 `br_file_ops`, §2) |
| **cdev-core** | **字符设备子分类**: `br_cdev_ops`(会话形状)+ `br_cdev_register`; **flash 子型**: `br_flash_ops` + `br_flash_register`(**spi-nor/nand 对接于此**); **open_file 钩子实现**(D21) | **dev-core + vfs-core**(类型/适配钩子) |
| **bdev-core** | **块设备子分类**: `br_bdev_ops`/几何/可堆叠、分区映射器(SD-9) | **dev-core** |
| **vfs-core** | **`br_file_t`/`br_file_ops` 契约、`br_open` 挂载表·单路由、挂载表、`br_fs_ops`、目录语义** | **core(纯 VFS, D21: 撤销设备路由)** |

### 1.4 消费者的依赖声明(manifest)

| 消费者 | 依赖 |
|---|---|
| **fs/tmpfs(rootfs)** | **vfs-core**(挂载 "/") |
| **fs/devfs(/dev)** | **dev-core**(枚举注册表)+ cdev-core(钩子就绪)+ vfs-core(挂载) |
| littlefs | **vfs-core** + cdev-core(flash 子型绑定)/ bdev-core(QEMU bdev 适配)+ fs/tmpfs(挂载点父目录) |
| EROFS | vfs-core + bdev-core |
| svc-posix | **vfs-core**(open/fd 的底层原语) |
| uart / can / adc / gpio / display 驱动 | cdev-core(`br_cdev_register`) |
| **virtio-hsm(HSM host 链路, v1.x/M5)** | **cdev-core**(`br_cdev_register`; 注册 `hsm0` → `/dev/hsm0`, 消费者 = `service/hsm-host`) |
| **QSPI-NOR / NAND 驱动** | **cdev-core**(`br_flash_register`) |
| virtio-blk / SD 驱动 | bdev-core(`br_bdev_register`) |

![1.3 框架件归属与依赖(D19/D20)](pics/8-01-device-01.png)

> 源文件: [plantUML/8-01-device-01.puml](plantUML/8-01-device-01.puml)

**设备与存储栈全景**(形态 A 的完整视图; 形态 B 在该图 dev-core 处截断, 形态 C 在器件处截断):

![设备与存储栈全景](pics/8-01-device-02.png)

> 源文件: [plantUML/8-01-device-02.puml](plantUML/8-01-device-02.puml)

**框架件的治理身份**: **插件的身份, core 的纪律**——插件形态 ⇒ 可按组合裁剪(无存储产品不链 vfs-core/bdev-core); core 纪律 ⇒ API 面进 golden/门禁(`docs/1-architecture/1-02-api-contract-governance.md`, D12 机制), 不透明句柄(D14)。这是 core 的第三次收缩: POSIX→接口插件(v0.3), POSIX 运行时→服务(v0.5/D18), **能力框架→框架件(v0.6/D19)**; D20 进一步把设备侧框架**按子分类再切细**。

> **框架件的冻结批次(必读, 否则 release 会被永久阻断)**: 四件框架件的 API 面各有独立 golden 文件(`br-devcore.txt`/`br-cdevcore.txt`/`br-vfscore.txt`/`br-bdevcore.txt`), 其**冻结批次 = `docs/3-os-core/3-01-core-api-list.md` §15 第四批**(非 core 组; 每文件独立升格, M2 起 dev-core/cdev-core、M3 起 vfs-core/bdev-core, 随实现成熟分批)。这条是 `brickie` 的 `--profile release`(禁止依赖未冻结接口)能通过的前置: **有治理声明却无冻结批次的框架件会让所有 release 组合被永久阻断**。冻结粒度与插件级 `COMPAT_GEN` 的取法见 `3-01` §15 与 `brickie` v0.1 §5.5/§8.1(A-16/A-18)。

> **多域消费者(D24, v1.x/M5)**: HSM 完整样例是这套体系的**首个第二产品域消费者**——`io/virtio-hsm` 走**形态 A**(器件 → cdev-core → dev-core → devfs → VFS, `/dev/hsm0`), 消费者为 `service/hsm-host`; 样例的裁剪变体再给**形态 B**(不链 vfs-core, 经 `br_cdev_*` 会话直取)作对照。设备侧框架件在此**零改动**——这是 §1.2 "形态选择是组合决策" 的实测。详见 `docs/9-app/9-02-hsm-sample.md` §7.2/§10。

## 2. 设备注册表(dev-core)

- 唯一扁平命名空间: 设备名 `[a-z][a-z0-9]*`, 无斜杠(devfs 节点名 = 路径分量)
- **子分类注册协议**(框架件间协议, 不入 `docs/3-os-core/3-01-core-api-list.md` 通用清单): 子分类框架(cdev-core/bdev-core)的注册 API 经"依赖 dev-core"把条目入同一张表——`/dev/nor0`(经 devfs)、`br_flash_get("nor0")`、`br_bdev_get("blk0")` 看到同一对象

```c
/* dev-core: 通用注册表条目(子分类框架调用; 形状解释权在子分类框架) */
typedef struct {
    uint16_t class_id;     /* BR_CLASS_CDEV / BR_CLASS_BDEV / ... (append-only) */
    void    *class_priv;   /* 子分类框架的 ops 表指针(如 br_cdev_ops*) */
    /* D21/D23: 可文件化钩子——devfs 设备节点 lookup 时调用, 返回文件 ops 集与私有 */
    int (*open_file)(void *dev_priv, const br_file_ops **fops, void **fpriv);
    void *dev_priv;        /* 驱动私有 */
} br_dev_class_entry_t;
int br_dev_add(const char *name, const br_dev_class_entry_t *entry);
```

- **钩子语义演化(D23)**: 自 D21 的"直接产出 `br_file_t`"改为"返回 **{fops, fpriv}**"——配合 inode 走查模型(devfs 设备节点 inode 携带该二元组); cdev-core 提供通用会话适配 `br_file_ops`(open 建会话/read/write/…转发)
- 设备名唯一性 = manifest 组合校验主键(§6)
- dev-core **不定义任何具体 ops 形状**——形状归子分类框架(§3), 新增子分类不动 dev-core
- `open_file` 钩子签名引用 `br_file_ops`(vfs-core 类型)⇒ dev-core 对 vfs-core 为**类型依赖**(仅头文件, 无 init/call 依赖)

## 3. 设备子分类(D20: dev-core 通用, 向下分 cdev/bdev)

| 子分类 | 框架件 | ops 形状 | 注册 API | 例子 |
|---|---|---|---|---|
| **cdev**(字符设备) | **cdev-core** | 会话式 open 工厂(`br_cdev_ops`) | `br_cdev_register` | uart / can / adc / gpio / display |
| **cdev · flash 子型** | **cdev-core** | read/program/erase/sync(`br_flash_ops`) | `br_flash_register` | **spi-nor / nand** |
| **bdev**(块设备) | **bdev-core**(依赖 dev-core) | 扇区 read/write/flush + 几何 | `br_bdev_register` | virtio-blk / SD / eMMC |

**设计理由**:
- **通用与形状分离**: dev-core 只管"是个设备"(命名/语义/注册表), 形状归子分类 ⇒ 新增子分类(netdev, O-S5)不动 dev-core
- **spi-nor/nand 对接 cdev-core**(D20): flash 是**字符型介质**(按地址 program/erase, 无磁盘式扇区抽象——擦除以 block 为粒度, 见 `br_flash_geom_t`), 归字符设备子分类; 其 ops 形状是 cdev 的一个**子型**, 与 littlefs `lfs_config` 1:1(零胶水绑定)
- 磁盘型介质(无擦除、有 flush)语义不同 → bdev 子分类; 强行统一会把两类驱动都写别扭(SD-2 延续)
- **设备接入 VFS(D21)**: 注册表中的设备自动出现为 **/dev/<name> 节点**(fs/devfs 挂载于 /dev, 实时枚举); 打开经类 open_file 钩子——设备生命周期归驱动注册, 文件系统只做投影(`7-03-concrete-fs` §3)

```c
/* ---- cdev(cdev-core 契约): 会话式字符设备形状 ---- */
typedef struct br_cdev_ops {
    int     (*open)(void *dev_priv, uint32_t flags, void **sess);
    ssize_t (*read)(void *sess, void *buf, size_t n);
    ssize_t (*write)(void *sess, const void *buf, size_t n);
    int     (*ioctl)(void *sess, uint32_t cmd, void *arg);
    int     (*poll)(void *sess, uint32_t *events);
    int     (*close)(void *sess);
    /* D22: 电源管理(设备级, dev_priv——非会话级); NULL = 不支持(-ENOTSUP) */
    int     (*suspend)(void *dev_priv);
    int     (*resume)(void *dev_priv);
} br_cdev_ops;
int br_cdev_register(const char *name, const br_cdev_ops *, void *dev_priv);

/* ---- cdev · flash 子型(cdev-core 契约, 与 littlefs lfs_config 1:1) ---- */
typedef struct {
    uint32_t read_size, prog_size;   /* 最小读粒度 / 编程页大小 */
    uint32_t block_size;             /* 擦除块 */
    uint32_t block_count;
} br_flash_geom_t;
typedef struct br_flash_ops {
    int (*read)  (void *priv, uint32_t addr, void *buf, size_t n);
    int (*program)(void *priv, uint32_t addr, const void *buf, size_t n);
    int (*erase) (void *priv, uint32_t block_idx);
    int (*sync)  (void *priv);
    /* D22 预留: ioctl(NOR 深度下电/特性控制)+ suspend/resume(电源管理) */
    int (*ioctl)(void *priv, uint32_t cmd, void *arg);
    int (*suspend)(void *priv);
    int (*resume)(void *priv);
} br_flash_ops;
int br_flash_register(const char *name, const br_flash_ops *,
                      const br_flash_geom_t *, void *priv);
const br_flash_ops *br_flash_get(const char *name, void **priv);
```

**统一预留槽位(D22)**: 所有设备类别 ops 预留 `ioctl` / `suspend` / `resume`(suspend/resume 恒为设备级; ioctl 于 cdev 为会话级(`sess` 参数)、于 bdev/flash 为设备级); file 面向的 `poll` / `close` 由 **cdev-core** 通用 `br_file_ops` 适配层提供(devfs 经 open_file 钩子取得该适配)。NULL = -ENOTSUP。
**动机 = D14**: ops 结构布局入 golden——后补字段 = 布局变更 = 二进制不兼容; **预留即免破坏**。suspend/resume 为电源管理钩子: v1 无统一调用方, v2 由 service/pm(或平台 PM 流程)经注册表枚举调用(O-S6)。

(bdev 子分类契约见 `7-02-bdev` §1。)

## 4. 调用语义与错误模型(SD-6/SD-10)

- **阻塞语义**(SD-6): read/write 阻塞调用线程; 驱动内部异步(DMA + 信号量), 对外同步
- **ISR 禁令**: 设备/存储域全部 API 禁止在 ISR 上下文调用(白名单为空)——静态检查执法(D10 双保险之一; conformance 矩阵 = R1 执法); core native 的白名单见 `docs/3-os-core/3-01-core-api-list.md` §11
- **错误模型**(SD-10): int 返回, **负 errno**。SD-10 全集(两域并集) = `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT/-ENOSPC/-EROFS`, 各域取子集; **设备/存储域子集**: `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS`(core 域子集见 `docs/3-os-core/3-01-core-api-list.md` §11); svc-posix 直接取 `-ret` 作 errno(零转换)
- 并发: 设备 ops 由驱动自锁(v1 单锁即可); FS 侧并发 v1 未另行约定(单 APP + 驱动自锁)

## 5. ioctl 编码(SD-5: Linux 兼容)

```c
/* 与 Linux _IOC 位布局一致: dir[31:30] size[29:16] type[15:8] nr[7:0] */
#define BR_IOR(type, nr, size)   ...
#define BR_IOW(type, nr, size)   ...
#define BR_IOWR(type, nr, size)  ...
/* type = 设备/驱动族魔数字母(每驱动族一个, 全局唯一分配): 'u' uart, 'c' can, 'b' bdev, 'f' flash, 'd' display ... */
```

零学习成本(直觉与 Linux 一致); 编码可静态查表 → 组合器/文档生成器可自动列 ioctl 清单。

## 6. 驱动编写者契约(快速开发的设备侧落点)

驱动作者只需: **实现 ops 表 + 遵守三条纪律 + 声明资源**。

| 纪律 | 内容 | 执法 |
|---|---|---|
| ISR 纪律 | ISR 最小工作 → `br_work_submit`; 禁阻塞/malloc/持锁返回 | conformance + 评审清单 |
| DMA/cache 纪律 | `br_dma_alloc` 分配; 传输前后 `br_mm_cache_flush/invalidate`(R4) | conformance + 真硬件 M4 |
| 资源声明 | manifest: IRQ 号 / DMA 通道 / 引脚 / RAM 预算 | 组合期冲突检测(主文档 §6.4) |

- **框架件依赖声明(D19/D20, §1)**: uart/can/spi-nor/nand 驱动→**cdev-core**(`br_cdev_register`/`br_flash_register`), 块设备驱动→**bdev-core**, FS→vfs-core; init-DAG 保证框架件先于消费者初始化
- **级联驱动**(PMIC/GPIO 控制器): 实现 `br_irq_domain_ops` 而非裸 ISR; 子中断消费者用 `br_irq_register_child`——子 handler 契约随域类型(FAST=ISR 纪律, SLOW=线程上下文, `docs/3-os-core/3-01-core-api-list.md` §8.1)
- 平台差异(pinmux/时钟/中断号/region)全部由 Platform 插件数据提供(主文档 §8 三层模式)——**驱动代码板级无关**, 换板只换 Platform 插件

## 7. 决策记录(本篇)

| # | 决策 | 理由 |
|---|---|---|
| SD-2 | 设备子分类: dev-core=通用, cdev/bdev 为子分类框架; **flash = cdev 子型**(spi-nor/nand 对接 cdev-core) | littlefs 1:1 映射零胶水; flash 是字符型介质(无磁盘式扇区抽象); 新增子分类不动 dev-core |
| SD-5 | ioctl 用 Linux 兼容 32 位编码 | 零学习成本; 可静态枚举 |
| SD-6 | 对外同步阻塞 API; 驱动内部异步(DMA+信号量) | API 面最小; async 是 v2+ 优化 |
| SD-10 | 错误 = 负 errno(SD-10 全集 + 各域子集, 设备域 7 码见 §4), svc-posix 零转换 | 单一错误空间 |
| SD-11 | **框架件归属(D19)**: file/open/VFS = vfs-core; 通用设备 = dev-core; bdev = bdev-core(依赖 dev-core); littlefs → vfs-core | 用户指定; core 第三次收缩; 框架件 = 插件身份(可裁剪) + core 纪律(golden/门禁)——**可裁剪的具体形态见 §1.2(形态 A/B/C)** |
| SD-12 | **设备子分类框架化(D20)**: cdev-core 独立框架件(依赖 dev-core); spi-nor/nand → cdev-core(flash 子型); ~~vfs-core 依赖 dev-core + cdev-core~~ → **SD-13/D21 修订**: 撤销; netdev 的答案空间 = 第三个子分类框架(O-S5) | 用户指定; 通用/形状分离; 子分类对称可扩展 |
| SD-13 | **设备接入 VFS(D21)**: fs/devfs 把 dev-core 注册表发布为 /dev 节点; 类 open_file 钩子(devfs 不依赖子分类形状); **vfs-core 纯化**(撤销设备路由, 依赖收缩到 core); fs/tmpfs 挂载为 rootfs("/") | Linux devtmpfs/rCore DeviceFS 同型; 单路由 = 单一语义; 用户指定 |
| SD-14 | **设备 ops 统一预留(D22)**: `ioctl`/`suspend`/`resume` 槽位全类别预留(poll/close 由 cdev-core 通用 br_file_ops 适配层提供, devfs 经钩子取得); NULL → -ENOTSUP | D14: ops 布局入 golden, **预留即免二进制破坏**; PM 设备级钩子(suspend/resume); 用户指定 |

## 8. 风险与开放问题(本篇)

| # | 类型 | 说明 |
|---|---|---|
| R-S4 | 风险 | 框架件边界漂移——*-core 是"插件身份 + core 纪律", 警惕往框架件里塞策略(会胖成小内核); golden 面审查是防线 |
| O-S2 | 开放 | NAND: 对接 cdev-core flash 子型后, 坏块管理/OOB 需在其上叠加 bbm/FTL 层(v2+)——子型 ops 是否够用待真实硬件验证 |
| O-S4 | 开放 | flash 子型若膨胀(NAND OOB)可在 cdev-core 内扩展或对称拆出 flash-core(依赖 cdev-core)——真实需求出现再定 |
| O-S5 | 开放 | **netdev(v2.0 网络栈前置)**: D20 子分类模型给出答案空间——**netdev-core 作为第三个子分类框架**(依赖 dev-core, 对称 cdev/bdev); `service/lwip` 对接之; v2.0 设计前定(`docs/1-architecture/1-03-roadmap.md` v2.0 插件清单 ★) |
| O-S6 | 开放 | **PM 调用方(v2)**: suspend/resume 的统一调用方——service/pm 经 dev-core 注册表枚举, 或平台 PM 流程; 与 sched-tt/低功耗 idle 的组合语义(挂起顺序/失败回滚) |
| O-S7 | 开放 | **形态 B(无 VFS)的类型依赖成本**: dev-core 的 `open_file` 钩子签名引用 `br_file_ops`(vfs-core 类型)⇒ 不选 vfs-core 的组合仍会拉进该头文件(仅类型, 无 init/call 依赖, §2)。选项: (a) 接受——头文件级类型依赖不进镜像; (b) 钩子做成可选面(条件编译/弱声明); (c) 钩子下沉到 cdev-core(dev-core 零 vfs 类型)。取舍取决于形态 B 的真实裁剪收益(§1.2) |
| O-S8 | 开放 | **形态 C 的组合期可见性**: standalone 器件不入注册表 ⇒ 设备名唯一性与类契约合规无从校验(资源冲突仍由 manifest 覆盖)。是否需要"轻注册"(仅入册供校验、不上行 VFS→形态 B)作为 C 的推荐升级路径, 待真实组合评审后定 |

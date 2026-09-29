# 04-vfs-device 模块评审意见（文档表述质量）

> 评审对象: `tangramOS/docs/6-vfs-device/04-vfs.md`(162 行) / `05-bdev.md`(96 行) / `06-device.md`(236 行) / `07-concrete-fs.md`(106 行)
> 评审方式: 分模块 subagent 评审 + 主评审全文交叉复核（每条均已对照原文行号核实）
> 评审重点: 胡言乱语/技术性错误/前后矛盾/表达错误/格式引用
> 严重度: 高 = 误导设计决策或明显胡言乱语; 中 = 易误导读者或明显表达问题; 低 = 错别字/格式瑕疵

## 04-vfs.md

### 高

- **L109** [前后矛盾] 「- **依赖 vfs-core(D19)**: open()→`tg_open` 的底层原语; **设备路径由 vfs-core 路由至 dev-core**」
  - D19 时代的旧模型残留。D21 已明确撤销 vfs-core 的设备路由：本篇 L9「`tg_open` **只走挂载表**(D21 修订: 撤销裸名设备路由…)」、L40/L116「**vfs-core 成为纯 VFS**——零设备知识」、同小节 L111「路径分派: 零特判——一切经挂载表; `/dev/*` 由 devfs 承接(D21)」、主文档 D21 均与之直接矛盾。同一段内 L109 与 L111 自相打架。
  - 建议: 改为「设备路径经挂载表路由至 fs/devfs，由 devfs 经 open_file 钩子接入 dev-core 侧」。

### 中

- **L5** [前后矛盾] 「本篇决策 SD-1/SD-3/SD-4/SD-7(全局编号, 分篇列出)」
  - 头部决策清单漏了 **SD-15**：L46 章节标题含 SD-15，L154 决策表也收录（D23 四层 ops 是本篇核心决策）；README 索引亦写「SD-1/3/4/7/15」。清单滞后于正文。
  - 建议: 补为「SD-1/SD-3/SD-4/SD-7/SD-15」。
- **L30** [前后矛盾] 「`tg_file_t *tg_open(const char *name_or_path, uint32_t flags);`」
  - 参数名 `name_or_path` 暗示仍接受裸设备名，与 D21「撤销裸名设备路由、只走挂载表」（L9）矛盾，误导实现者以为 `tg_open("uart0")` 可用。
  - 建议: 参数名改为 `path`。
- **L44** [前后矛盾] 「驱动声明 `TG_CDEV_F_EXCLUSIVE` → 二次 open 返回 -EBUSY(uart 类典型)」
  - `TG_CDEV_F_EXCLUSIVE` 全仓库仅此一处出现；06-device 的 cdev 契约（`tg_cdev_register(name, ops, dev_priv)` 与 `tg_cdev_ops`）没有任何 flags 参数/字段可承载该「声明」，所述机制在契约中无落点。
  - 建议: 在 06 的 cdev 注册契约中补 flags 载体，或改述为「由 cdev `ops->open` 自行返回 -EBUSY」。
- **L103** [前后矛盾]（主评审补充）「**v2**: `file_ops` 增加 `poll_attach`(wait-queue 注册), 精确唤醒」
  - 与本文自持的 D14/D22/D23 纪律冲突：ops 布局入 golden 后「后补字段 = 布局破坏——预留即免破坏」（D22 原话）；D23 恰为 v2 dcache 给 `tg_dentry_ops` 预留了槽位（「槽位先占, D14 同理」），`tg_file_ops` 的 `poll_attach` 却计划 v2 事后追加，v1.0 冻结后即触发布局破坏。
  - 建议: v1 即预留 `poll_attach` 槽位（NULL → -ENOTSUP），与 `tg_dentry_ops` 同法。

### 低

- **L4** [表达错误] 「`tg_open` 挂载表单路由」
  - 「挂载表单」极易误读为 mount form，实为「挂载表 · 单路由」。该词在 06 L16/L40/L77 及主文档 §4.5 多处复用，属全局术语歧义。
  - 建议: 统一写作「挂载表·单路由」或「挂载表单一路由」。
- **L54** [表达错误] 「/* 卷级 sync(fsync 的上游) */」
  - 「上游」指代不明：卷级 sync 与文件级 fsync 是并列的两个入口（对应 sync(2)/fsync(2)），不存在上下游调用关系。
  - 建议: 改为「卷级 sync(对应 POSIX sync, 与文件级 fsync 并列)」。
- **L97/L162** [前后矛盾·存疑] 「运行时挂载(USB 热插拔)是 v2 议题(O-S3)」
  - 与 02-roadmap.md L135「运行时挂载/热插拔…若落地也在 v3 [?]」版本归属冲突（本篇与 07 L105 均说 v2，roadmap 说 v3）。
  - 建议: 统一版本口径（v2 事件模型 / v3 落地，需 roadmap 与 04/07 对齐）。

## 05-bdev.md

### 中

- **L52** [格式引用] 「EROFS 特例走 FS 级缓存(`07-concrete-fs` §3), 与本层正交」
  - 交叉引用错节：07 §3 是 devfs，EROFS 的「FS 级解压页缓存」在 **07 §5**（07 §5 反向引用「05-bdev §3」是对的）。
  - 建议: 改为 `07-concrete-fs` §5。

### 低

- **L5** [前后矛盾] 「本篇决策 SD-2(bdev 侧)/SD-8/SD-9」
  - 本篇 §6 决策表只有 SD-8/SD-9；SD-2 的决策记录在 06-device §7。头部清单口径与本篇表格不一致（「bdev 侧」限定词可意会，但清单应与表格对齐）。
  - 建议: 改为「本篇决策 SD-8/SD-9；另涉 SD-2 的 bdev 侧（决策记录见 06-device）」。
- **L51** [表达错误] 「**写穿优先**(v-x)」
  - 「v-x」版本写法全文仅此一处，与 L5「page cache vx.0」不一致，含义不明（疑为 vx.0 笔误）。
  - 建议: 改为「(vx.0)」。
- **L80** [表达错误·存疑] 「T -> T: 返回 n_lba」
  - `tg_bdev_ops.read` 签名返回 int（L19），SD-10 只定义负 errno，成功返回值语义（0 还是传输扇区数）契约未定义，时序图却明确「返回 n_lba」，两种读法不一致。
  - 建议: 在 §1 契约中注明成功返回值约定（如「成功返回传输扇区数」）。

## 06-device.md

### 中

- **L5** [前后矛盾] 「本篇决策 SD-2/SD-5/SD-6/SD-10/SD-11/SD-12。」
  - 头部清单漏 **SD-13、SD-14**：§7 决策表（L225/L226）两者俱在，且是 D21/D22 的核心落点。
  - 建议: 清单补为「SD-2/SD-5/SD-6/SD-10/SD-11/SD-12/SD-13/SD-14」。
- **L13 + L63 vs L125** [前后矛盾] 「| **dev-core** | … | **core native** |」（§1 依赖表/依赖图均只有 DEVC→COREN）
  - 与本文 L125「`open_file` 钩子签名引用 `tg_file_ops`(vfs-core 类型)⇒ dev-core 对 vfs-core 为**类型依赖**」直接矛盾：`tg_dev_class_entry_t`（L111–118）内含 `const tg_file_ops **` 参数。同表 cdev-core 行却列出了「vfs-core(类型/适配钩子)」，两行口径不一。
  - 建议: dev-core 依赖列补「+ vfs-core(类型, 仅头文件)」，依赖图补 DEVC→VFSC 边。
- **L177** [前后矛盾] 「所有设备类别 ops 预留 `ioctl` / `suspend` / `resume`(设备级); file 面向的 `poll` / `close` 由 devfs 适配层(cdev-core 通用 `tg_file_ops`)提供」
  - 两处问题: (a) 把「devfs 适配层」与「cdev-core 通用 tg_file_ops」划等号——按本文 L14/L122 与 07 §3 L30，该适配器由 **cdev-core** 提供，devfs「不依赖任何子分类形状」，两个组件被混为一谈（L226 SD-14 行「poll/close 由 devfs 适配层」同病，主文档 D22 同句式）; (b) 括注「(设备级)」概括不准——cdev 的 ioctl 是**会话级**（L147 参数为 `void *sess`），仅 suspend/resume 是设备级（L150 注释自证「非会话级」）。
  - 建议: 改为「file 面向的 poll/close 由 cdev-core 通用 tg_file_ops 适配层提供（devfs 经 open_file 钩子取得）; ioctl 于 cdev 为会话级、于 bdev/flash 为设备级, suspend/resume 恒为设备级」。
- **L187** [格式引用] 「并发: 设备 ops 由驱动自锁(v1 单锁即可); FS 侧见 `07-concrete-fs`」
  - 断裂引用：07 全文（§1–§9）没有任何 FS 侧并发/加锁的讨论，读者按图索骥落空。
  - 建议: 在 07 补 FS 侧并发小节，或改为「FS 侧并发 v1 未另行约定（单 APP + 驱动自锁）」。
- **L186** [前后矛盾]（主评审补充）「**错误模型**(SD-10): int 返回, 负 errno 子集: `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS`」
  - 与 08 §11 的「错误码(SD-10 统一)」集合（`-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT`）不是同一集合，两边都自称「SD-10 统一」；19-test R-4 又把 core-api 错误码不变量归到 SD-10。同名决策、三处口径。
  - 建议: 明确 SD-10 定义「设备/存储域错误码子集」，core native 域另立编号或显式声明「08 §11 为 core 域超集」，并在 19-test R-4 改引 08 §11（详见 `06-cross-doc-consistency-comment.md`）。

### 低

- **L23** [表达错误] 「cdev-core(钩子就序)」——「就序」为「就绪」之误（07 L56 即作「钩子就绪」）。
- **L157** [技术错误] 「uint32_t read_size, prog_size; /* 页大小 */」
  - 注释不准确：littlefs `lfs_config` 中 read_size 是**最小读取粒度**（NOR 上可为 1），并非页大小；只有 prog_size 对应编程页。既然 L156 声明「与 littlefs lfs_config 1:1」，注释应与其语义一致。
  - 建议: 改为「/* 最小读粒度 / 编程页大小 */」。
- **L196** [表达错误] 「/* type = 子分类字母: 'u' uart, 'c' can, 'b' bdev, 'f' flash, 'd' display ... */」
  - 自称「子分类字母」但举例混入设备类型字母（uart/can/display 是设备类型而非子分类），且 'c' 给了 can 之后与 cdev 潜在撞车，粒度混杂。
  - 建议: 改为「type = 设备类别魔数（每驱动族一字母）」并统一分配规则。
- **L137** [表达错误·存疑]（主评审补充）「flash 是**字符型介质**(按地址 program/erase, 无扇区抽象)」
  - 「无扇区抽象」与其后 `tg_flash_geom_t` 的 `block_size/block_count`（擦除块抽象）表述有张力；NOR 手册亦常以 sector 称擦除单元。意为「无磁盘式扇区 read/write 抽象」，但当前写法易被读成「无块/扇区概念」。
  - 建议: 改为「无磁盘式扇区抽象（擦除以 block 为粒度, 见 tg_flash_geom_t）」。

## 07-concrete-fs.md

### 中

- **L13** [前后矛盾] 总览表 devfs 列「写路径 | … | 只 open/poll(设备语义)」
  - 与本文 L30「cdev-core 提供通用会话适配 `tg_file_ops`(open 建会话/**read/write**/ioctl/poll/close 转发)」矛盾：uart 等字符设备的 read/write 是基本用法，总览表却写 devfs 节点「只 open/poll」。
  - 建议: 该格改为「n/a——read/write/ioctl 转发设备 ops(设备语义)」。
- **L32** [前后矛盾] 「unlink/mkdir → -EOPNOTSUPP(**设备生命周期归驱动注册, 不归文件系统**)」
  - SD-10 声明的错误码子集（06 L186）与本文 L30 均用 `-ENOTSUP`；`EOPNOTSUPP` 不在子集内（Linux 上两符号同值，musl/newlib 等嵌入式 libc 未必保证，契约文本应统一符号）。
  - 建议: 改为 `-ENOTSUP`。
- **L33** [前后矛盾] 「devfs 只依赖 dev-core(枚举/钩子协议)与 vfs-core(挂载)——**不依赖任何子分类形状**」
  - 与本文 L62「fs/devfs → {dev-core, **cdev-core**, vfs-core}」、06 L23 消费者表（devfs 依赖含 cdev-core「钩子就绪」）、02-roadmap L59 均冲突：「只依赖」把 init 依赖也排除了。
  - 建议: 改为「devfs 不依赖任何子分类的 **ops 形状**（init 依赖仍声明 cdev-core 以保证钩子就绪）」。
- **L49/L104** [前后矛盾·存疑] 「XIP(NOR 零拷贝执行)是诱惑但 cache 一致性代价未评估(O-S1)」「EROFS-on-NOR 的 XIP」
  - 与本模块的介质绑定矛盾：EROFS 全文只绑 bdev（L12「bdev 子分类」、L25；06 L25/L55/L95），而 NOR 在本架构走 cdev-core flash 子型（06 §3），全模块不存在 NOR→bdev 路径，「EROFS-on-NOR」场景暂无数据通路支撑。因标为开放问题，故存疑。
  - 建议: 在 O-S1 补一句通路前提（如「需 NOR 内存映射窗口直读 + 元数据经适配 bdev, 路径待设计」）。
- **L97** [格式引用] 「**SD-2/SD-12**(`06-device`): 子分类与 open_file 钩子是 devfs 的底座」
  - SD 编号引用错误：open_file 钩子是 **SD-13**（D21，06 L225）的决策内容；SD-12 是子分类框架化，与钩子无直接关系。
  - 建议: 改为「SD-2/SD-13」。

### 低

- **L94** [表达错误] 「设备 ops 统一预留 ioct/PM 钩子」——「ioct」漏字母，应为「ioctl」。
- **L62** [前后矛盾]（跨文档组补充）「init 顺序由 manifest 依赖声明保证: fs/littlefs → {vfs-core, fs/tmpfs, cdev-core}」——漏 bdev-core: 06-device L24 与 02-roadmap L62 的 littlefs 依赖均含「cdev/bdev-core(QEMU bdev 适配)」，本篇 L39 自己也写了该 QEMU 适配器。→ 依赖列表补 bdev-core（标注 QEMU 适配）。
- **L74** [前后矛盾] §7 时序图「PX -> LF: file_ops.write(file, buf, 512)」
  - svc-posix 直调 littlefs 的 `file_ops`，跳过了 04 §1 的 vfs-core 统一入口 `tg_file_write`（`tg_file_t` 为不透明句柄 D14，svc-posix 不应直达 fops；06 L26 依赖表也只声明 svc-posix→vfs-core）；与 04 §5 open 链路（vfs-core 作为参与者）详略不一致。
  - 建议: 时序图补 vfs-core 参与者（PX→VF: `tg_file_write` → VF→LF: `fops->write`）。

## 一致性核对通过项（供置信）

- SD-1~SD-15 全局编号无重复、无断号（04: 1/3/4/7/15；05: 8/9；06: 2/5/6/10–14），与主文档 D17/D19–D23 决策内容一致（问题仅在各篇头部清单滞后）。
- page cache「无感层」定位四处一致（04 §4.3、05 §3、06/主文档存储栈图、用户笔记 vx.0）；版本口径一致：VFS v1.0(M2)、Block Device v1.0(分区 v1.x)、EROFS v2.0、page cache vx.0。
- 06 §1 存储栈全景图与主文档 §10 逐行一致。
- 技术事实核对无误：EROFS 只读压缩、write→-EROFS；littlefs 掉电安全（COW+元数据对）、wear-leveling 在磁盘上无意义、`lfs_config` 1:1 绑定；tmpfs/devfs 语义；bdev 堆叠（provider=consumer）与分区映射器；ioctl 位布局与 Linux `_IOC` 完全一致（dir[31:30] size[29:16] type[15:8] nr[7:0]）；05 §5 virtio-blk 读流程（DMA 写内存后 invalidate）与 07 §7 flash 编程前 cache flush 的 cache 维护方向均正确；ISR→bh→信号量模型正确。

## 总体评价

该模块技术底子扎实：**未发现「胡言乱语」级错误**，littlefs/EROFS/tmpfs/devfs 特性、块设备堆叠与分区、DMA/cache 维护方向、ioctl 编码等关键技术表述均正确，且与主文档 D17/D19–D23 及用户笔记的版本 milestone 高度一致。主要质量债务来自决策快速迭代（D19→D21→D23）留下的**陈旧表述**——最严重的是 04 L109 仍写「设备路径由 vfs-core 路由至 dev-core」，直接反转了 D21「vfs-core 纯化」这一核心决策；其次是三篇头部决策清单滞后、devfs/cdev-core 适配器归属表述混乱、若干断裂交叉引用（05 L52 错节、06 L187 落空、07 L97 错号）和错误码符号不统一。建议做一轮「D21/D23 术语清扫」：以「vfs-core 纯 VFS、devfs 只投影、cdev-core 提供适配」为基准句式统一全文，并同步刷新各篇头部决策清单。

**问题统计: 高 1 / 中 15 / 低 13，共 29 条**（含主评审补充 3 条: 04 L103、06 L186、06 L137; 跨文档组补充 1 条: 07 L62）。

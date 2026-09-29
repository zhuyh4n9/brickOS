# 存储 · VFS · 设备 设计评审(04 / 05 / 06 / 07)

> 评审人: subagent(vfs-device 模块, 证据经父 agent 抽查核实)
> 评审对象: `tangramOS/docs/6-vfs-device/04-vfs.md`、`05-bdev.md`、`06-device.md`、`07-concrete-fs.md`
> 评审维度: 完备性(矛盾、技术点是否清晰、架构是否存在问题)
> 意见分档: P0 = 矛盾/架构缺陷(必须解决); P1 = 完备性缺口/技术不清晰(应当解决); P2 = 改进建议

## 模块总评

该模块的**架构骨架相当成熟**: D19–D23 的四框架件切分(dev-core 通用 / cdev/bdev 子分类 / vfs-core 纯化)加 D21 的 devfs/tmpfs 投影与 Linux devtmpfs/rCore 同型, 06-device §1 提供了完整的归属表+依赖图+消费者清单, 决策均带理由、风险与开放问题编号, 是文档库中质量较高的部分。主要缺口集中在**函数级契约层**: ops 四层的结构已定义, 但参数语义、错误通道、生命周期(会话私有数据、{fops,fpriv} 所有权、inode 释放时序)与并发契约大面积缺席, 且存在引用悬空(FS 并发契约指向 07-concrete-fs 中不存在的章节)。**两处成对矛盾必须先清理**: D19 决策行与 04 §4.1 残留的"vfs-core→dev-core 设备路由"D21 前旧文字; "无存储产品不链 vfs-core"的裁剪承诺与 D21"所有设备经 /dev 接入"冲突且无替代路径。**最大技术风险**是 D23 open 会话协议未闭合(tg_file_t 不透明且由 vfs-core 定长分配, 会话私有数据无处安放)与 vfs-core 对外 API 面不完整(readdir/lseek/元数据操作无入口), 二者直接决定 svc-posix 的 POSIX 面能否落地; bdev 可堆叠"无感"目标清楚, 但堆叠组合/命名绑定协议未定义, page cache 无感层按现有文档尚不足以实现。

---

## [P0] D19 决策行与 04 §4.1 残留"vfs-core→dev-core 设备路由"旧文字, 与 D21"vfs-core 纯化"直接矛盾
- **位置**: `00-architecture.md` §1 决策表 D19 行 / L56; `04-vfs.md` §4.1 / L109
- **问题**: 主文档 D19 行仍写"派生: vfs-core→dev-core(设备路由+适配)", 04 §4.1 仍写"设备路径由 vfs-core 路由至 dev-core"。二者均与 D21(L58: "**vfs-core 纯化**(设备依赖移出, 依赖面收缩到 core)")、SD-13、06 §1(L16: vfs-core 依赖 = "core(纯 VFS, D21: 撤销设备路由)")以及 04 自己的 §1/§4.2/L111("零特判——一切经挂载表; /dev/* 由 devfs 承接(D21)")矛盾。决策表是权威登记处, 其余被修订决策(如 D11)都带删除线+修订标注, D19 漏标。若实现者按 D19 建依赖, 还会与 06 §2 承认的 dev-core→vfs-core 类型依赖构成环。
- **证据**: "派生: vfs-core→dev-core(设备路由+适配), svc-posix→vfs-core"(主文档 D19); "设备路径由 vfs-core 路由至 dev-core"(04 §4.1)
- **建议**: D19 行加修订标注 `~~vfs-core→dev-core(设备路由+适配)~~ → D21 修订: 撤销, 设备经 fs/devfs 接入`; 04 §4.1 该句改为"设备路径经挂载表路由至 devfs(D21)"。

## [P0] "无存储产品不链 vfs-core"裁剪承诺与 D21"所有设备经 /dev"矛盾, 且无 vfs-core 组合的设备访问路径
- **位置**: `06-device.md` §1 / L103; `00-architecture.md` D21 行 / L58
- **问题**: 06 §1 宣称框架件可裁剪("无存储产品不链 vfs-core/bdev-core"), 但 D21 规定"所有设备经 /dev(devfs 插件)接入 VFS 管理"——任何带 uart/can 的产品(连 console 完整形态都算)必须链 vfs-core+devfs+tmpfs(rootfs)。替代路径缺失: bdev/flash 有消费者类 API(tg_bdev_get/tg_flash_get), **cdev 无任何文档化的非 VFS 会话 API**(06 §3 只定义了驱动侧 tg_cdev_register), 裁剪承诺无从兑现。
- **证据**: "插件形态 ⇒ 可按组合裁剪(无存储产品不链 vfs-core/bdev-core)"(06 §1); "所有设备经 /dev(devfs 插件)接入 VFS 管理"(主文档 D21)
- **建议**: 三选一并成文: (a) 声明"凡含设备产品最小组合必含 vfs-core+tmpfs+devfs"并改 L103 措辞; (b) 为 cdev-core 补 tg_cdev_get/会话类 API 作为 native 直达路径; (c) 显式声明 D21 的适用前提是"vfs-core 在组合内"。

## [P1] vfs-core 对外 API 面不完整: lseek/poll 无入口, 目录/元数据/名字空间操作无 native 入口
- **位置**: `04-vfs.md` §1 / L29-35(入口清单) vs L18-27(ops)、§2 / L60-79、L95
- **问题**: 入口仅 tg_open/tg_file_read/tg_file_write/tg_file_ioctl/tg_file_fsync/tg_file_close——**缺 tg_file_lseek 与 poll 入口**(ops 有 lseek/poll 槽, 但 tg_file_t 不透明(D14), svc-posix 无法直接触 ops); inode_ops 定义了 create/unlink/mkdir/rmdir/rename/getattr, 却没有任何路径级入口(tg_unlink/tg_mkdir/tg_stat/tg_rename/tg_truncate…)供 svc-posix 映射 POSIX 的 unlink()/mkdir()/stat()/ftruncate(); "readdir 由 svc-posix 映射"映射到什么原语未定义; tg_fs_ops.sync("卷级 sync")无任何调用方入口。
- **证据**: 入口清单仅 6 个函数; "目录枚举: …readdir 由 svc-posix 映射"; "int (*sync)(void *fs_priv); /* 卷级 sync(fsync 的上游) */"
- **建议**: 补全入口清单并写明 svc-posix POSIX 面与之一一映射: tg_file_lseek/tg_file_poll、tg_stat/tg_unlink/tg_mkdir/tg_rmdir/tg_rename/tg_truncate、tg_opendir/tg_readdir/tg_closedir、卷级 tg_sync。

## [P1] 目录枚举契约缺失: tg_dir_t 未定义、tg_file_ops 无 readdir 槽、目录 inode 的 file 面绑定规则不明
- **位置**: `04-vfs.md` §2 / L95; §1 / L18-27, L85-90
- **问题**: "目录 inode 的 file 面(opendir → tg_dir_t 迭代)"——tg_dir_t 类型从未定义; tg_file_ops 无 readdir/getdents 槽; tg_inode 注释"iops: 目录 inode / fops: 文件 inode"暗示互斥, 但 opendir 要求目录 inode 也携带 fops(目录 inode 是否双挂 iops+fops? opendir 是否走 fops->open? 迭代游标放哪?)未说明。
- **证据**: "opendir → `tg_dir_t` 迭代"; ops 结构仅有 open/read/write/lseek/ioctl/fsync/poll/close; "const tg_inode_ops *iops; /* 目录 inode */ const tg_file_ops *fops; /* 文件 inode */"
- **建议**: 定义目录文件面契约: tg_file_ops 增加 readdir 槽(游标在会话内)或独立 tg_dir_ops; 写明目录 inode 同时挂 iops(名字空间)与 fops(迭代)的规则及 opendir→closedir 生命周期。

## [P1] 会话私有数据无处安放: open(tg_file_t*, flags) 协议与 {fops, fpriv} 钩子协议未闭合
- **位置**: `04-vfs.md` §1 / L16, L19, L43; `06-device.md` §2 / L116-118, L122; `07-concrete-fs.md` §3 / L29-30
- **问题**: (a) tg_file_t 不透明(D14)且由 vfs-core 经 tg_malloc 定长分配, 而 fops->open(tg_file_t *f, flags) 建立的 per-open 会话(lfs_file_t、cdev sess)**没有任何文档化的存储槽或访问器**——FS/适配器作者无法按文档实现 open; (b) {fops, fpriv} 的 fpriv 内容、所有权与生命周期未定义(静态设备对象还是 per-lookup 堆分配? 瞬态 inode 释放后 fops 指针必须静态、fpriv 必须设备生命周期——所有权规则未写); (c) cdev 通用适配的 open 需要同时取到 tg_cdev_ops(在 entry->class_priv)与 dev_priv, 但钩子签名只传 dev_priv, 适配如何取回 ops 未闭合。
- **证据**: "typedef struct tg_file tg_file_t; /* 不透明(D14) */"; "tg_file_t 由 vfs-core 经 core 堆(tg_malloc)分配"; "int (*open)(tg_file_t *f, uint32_t flags)"; "返回 {fops, fpriv}"; "cdev-core 提供通用会话适配 tg_file_ops(open 建会话/read/write/…转发)"
- **建议**: 二选一并写死: tg_file_t 为定长结构含 `void *f_priv`(open 钩子写入、close 钩子释放), 或 CA-2 式公共头+FS 私有尾(改由 FS 分配); 定义 fpriv 所有权规则(设备/FS 生命周期对象、非 per-open); 写明 cdev-core 在 tg_cdev_register 时生成 {ops, dev_priv} 静态记录供钩子返回。

## [P1] inode 瞬态模型收尾未定义: 最终 inode 生命周期、lookup 错误通道、fstat(fd) 路径
- **位置**: `04-vfs.md` §2 / L52, L61, L67, L93-94; `07-concrete-fs.md` §4 / L38; `00-architecture.md` §7.6 / L506
- **问题**: (a) lookup 是四层 ops 中唯一无错误通道的(返回裸指针): NULL 是 ENOENT 还是 EIO? littlefs lookup=lfs_stat 会返回 LFS_ERR_IO, 无法与"不存在"区分; (b) 走查中间目录 inode 与最终文件 inode 的 free_inode 调用时机(每步? open 成功后? 失败路径谁释放)未定义, 实现极易泄漏; (c) "走查即弃"与 fstat 冲突: 文件 inode 释放后 fops 无 getattr 槽、tg_file_t 亦不持路径 ⇒ **fstat(fd) 无实现路径**(主文档明确 sqlite 子集需要 stat/fstat)。
- **证据**: "tg_inode_t *(*lookup)(tg_inode_t *dir, const char *name)"; "inode 是走查的中间产物, free_inode 即弃"; "fops = lf_file_ops"…无任何文件级 getattr
- **建议**: lookup 改 int 返回+出参(或显式约定 NULL=ENOENT、其余经负 errno 侧信道); 补"释放时序表"(成功/未命中/O_CREAT 各路径); 定义 fstat 路径(会话持 inode 引用, 或 fops 增加 getattr, 或声明 fstat 经路径重走查)。

## [P1] 文件偏移归属未声明, pread/pwrite 不可达
- **位置**: `04-vfs.md` §1 / L20-21; `00-architecture.md` §7.6 / L506
- **问题**: read/write 签名无偏移参数, 偏移存于会话(FS 维护)还是 tg_file_t(vfs-core 维护)、whence 解析归谁未写; 无偏移参数使 **pread/pwrite 无法实现**(lseek+read 两步在多线程共享 fd 下有竞态), 而主文档 sqlite 模式 A 明确需要 pread/pwrite。
- **证据**: "ssize_t (*read)(tg_file_t *f, void *buf, size_t n)"; "pread/pwrite/ftruncate/unlink/stat/fstat…"(主文档 §7.6)
- **建议**: 写明"偏移归会话所有, lseek 是唯一修改入口"; file_ops 增加 pread/pwrite 槽(v1 可 NULL→-ENOTSUP)或在 svc-posix 文档声明模拟实现及其一致性代价。

## [P1] 挂载表匹配/并发语义未定义: 分量边界、嵌套挂载、readdir 遮蔽、init 后只读性
- **位置**: `04-vfs.md` §1 / L11-13; §2 / L57, L97
- **问题**: (a) 未声明前缀匹配按路径分量对齐——字符串前缀下 "/data" 会错误匹配 "/database/x", 这是挂载表最经典的实现 bug, 契约必须写死; (b) 嵌套挂载(/data 与 /data/sub)下的走查与 readdir 遮蔽语义未定义(v1 计划虽平坦, 契约应声明禁止或定义行为); (c) 挂载表 init 后是否只读(静态挂载⇒无锁读的依据)未声明, O-S3 的 v2 动态挂载将推翻该前提。
- **证据**: "'/'→ tmpfs(rootfs)——最短前缀兜底"; "'/data/xxx'→ littlefs——挂载点最长前缀匹配"
- **建议**: 写明"前缀匹配按完整路径分量(前缀须为分量边界)"; 声明 v1 挂载表 init 后不可变、并发免锁; 声明 v1 禁止嵌套挂载或定义其走查/readdir 规则。

## [P1] FS 侧并发契约缺失, 06 §4 引用悬空
- **位置**: `06-device.md` §4 / L187; `07-concrete-fs.md` 全篇(无并发章节); `00-architecture.md` §2.1 / L67
- **问题**: 06 §4 写"FS 侧见 07-concrete-fs", 但 07 没有任何锁/并发内容——引用指向不存在的章节。单 APP 多线程(主文档明示"APP 内部仍可有并发")下: 两个线程同时对同一 littlefs 卷 lookup/create(lfs 非线程安全)、tmpfs 并发 rename/分配, 锁的归属(每挂载实例锁? vfs-core 全局锁? FS 自锁?)完全未定义。
- **证据**: "并发: 设备 ops 由驱动自锁(v1 单锁即可); FS 侧见 `07-concrete-fs`"
- **建议**: 在 07 增加"并发模型"节并修正 06 §4 引用: 声明 v1 每挂载实例单锁, 写明持有者(建议 vfs-core 在走查+ops 分派全程持锁, FS 实现可免锁)。

## [P1] dev-core 注册表并发与 devfs 实时枚举一致性契约缺失; 设备注销 API 缺位未声明
- **位置**: `06-device.md` §2 / L107-125; `07-concrete-fs.md` §3 / L28, §9 O-S3 / L105
- **问题**: v1 已支持运行时注册("注册即上线"、"v1 已动态"), 但注册表锁契约未定义: readdir 迭代中并发 tg_dev_add 的迭代器失效/快照语义、lookup 与 add 竞争; 06 §4 的"驱动自锁"只覆盖设备 ops 不覆盖注册表数据结构; 只有 tg_dev_add 无 remove——设备注销语义(open 后节点消失)悬空, 应显式声明 v1 无注销。
- **证据**: "节点 = dev-core 注册表实时枚举: tg_cdev/tg_flash/tg_bdev_register 即出现节点(注册即上线)"; "devfs 节点随注册出现(v1 已动态)"
- **建议**: 定义注册表互斥(devfs 枚举取快照或持锁迭代)与 readdir 一致性级别; 显式声明"v1 仅 tg_dev_add, 无注销, 设备生命周期=镜像生命周期"。

## [P1] 依赖声明不一致: dev-core→vfs-core 类型依赖在 06 §1 表/图与主文档 §6.3/§7.2 三处漏报
- **位置**: `06-device.md` §2 / L125 vs §1 表 L13、§1 图 L63; `00-architecture.md` §6.3 / L343、§7.2 图 / L431
- **问题**: 06 §2 承认"dev-core 对 vfs-core 为类型依赖(仅头文件)", 但 06 §1 表 dev-core 依赖只写"core native", 主文档 §6.3 写"依赖 core"、§7.2 图只有 DEVC2-->COREN。依赖宪法以 manifest 依赖图执法, 漏报意味着 dev-core 不带 vfs-core 组合的头文件可用性无契约保障, "框架件间单向"的官方图也不完整。
- **证据**: "open_file 钩子签名引用 tg_file_ops(vfs-core 类型)⇒ dev-core 对 vfs-core 为类型依赖(仅头文件, 无 init/call 依赖)" vs 表内"core native"
- **建议**: 在 06 §1 表、主文档 §6.3/§7.2 图统一标注 dev-core --类型依赖--> vfs-core(仅头文件); 或把 tg_file_ops 前向声明放中立头消掉该边; manifest 依赖语法增加 header-only 级别。

## [P1] devfs 依赖声明自相矛盾(不依赖子分类 vs 依赖 cdev-core), 且该 init 依赖技术上不必要
- **位置**: `07-concrete-fs.md` §3 / L33 vs §6 / L62; `06-device.md` §1 消费者表 / L23
- **问题**: 07 §3 说"devfs 只依赖 dev-core 与 vfs-core——不依赖任何子分类形状", 同文 §6 却写"fs/devfs → {dev-core, cdev-core, vfs-core}", 06 §1 也含"cdev-core(钩子就序)"——同文档内直接冲突。且该 init 依赖不必要: 钩子函数指针由子分类框架在 tg_*_register 时填入 entry, 而驱动注册本身已依赖 cdev-core, devfs 只在 lookup 时经 entry 间接调用。
- **证据**: "devfs 只依赖 dev-core(枚举/钩子协议)与 vfs-core(挂载)——**不依赖任何子分类形状**" vs "fs/devfs → {dev-core, cdev-core, vfs-core}"
- **建议**: 统一表述"代码级依赖 = dev-core + vfs-core, 钩子经 entry 函数指针晚绑定", 从依赖清单移除 cdev-core(或说明保留作 v1 保守排序的理由)。

## [P1] bdev 可堆叠协议不足以落地 page cache 无感层: 堆叠组合与命名绑定协议未定义
- **位置**: `05-bdev.md` §1 / L29-34, §3 / L49; `06-device.md` §2 / L108(名字唯一性), §1 图 / L96-98
- **问题**: (a) page cache 要"无感"垫在 FS 与真实 bdev 之间, 但 FS 绑定经 tg_bdev_get("blk0") 按名取 ops——cache 层不改名无法插入, 沿用同名又与"设备名唯一性 = manifest 组合校验主键"冲突; 分区有 child_name, page cache 没有任何组合/注册 API; (b) 堆叠层的 read/write 错误传播、部分完成、几何/对齐合成规则(上层暴露谁的 align/max_rw)无契约(只有 flush 转发有纪律); (c) 分区子设备是否入 dev-core 注册表(是否出现在 /dev 与未来 PM 枚举)未写。
- **证据**: "可堆叠语义: provider 同时可以是 consumer——上层看到同一 tg_bdev_ops…VFS/FS/驱动对堆叠层**无感**"; "设备名唯一性 = manifest 组合校验主键"; "BD --> PC : tg_bdev_ops(可堆叠)"
- **建议**: 定义堆叠组合协议(manifest 声明栈序如 blk0 = pagecache(hw_blk0), bdev-core init 期构造并按顶层名注册绑定); 写明堆叠层几何取最严、错误原样传播; 写明分区子设备的注册表投影规则。

## [P1] flash 子型设备的 /dev 打开语义未定义(bdev 有对声明, flash 没有)
- **位置**: `06-device.md` §3 / L156-175; `07-concrete-fs.md` §3 / L30-31; `05-bdev.md` §1 / L36
- **问题**: bdev 明确写了"v1 置 NULL(钩子), raw 文件访问 v2", 但 tg_flash_ops 只有 read/program/erase/sync——无会话形状, 不匹配 cdev 的"会话式 open 工厂"; flash 设备(/dev/nor0)的 open_file 钩子是否为 NULL、open 返回什么(-ENOTSUP?)两文档均未写, 是对称性缺口。
- **证据**: bdev: "v1 块设备经类 API(tg_bdev_get)供 FS 绑定; /dev/blk0 的 raw 文件访问 = v2(devfs 的 bdev open_file 钩子置 NULL)"; flash 子型仅 "read/program/erase/sync"
- **建议**: 07 §3 补一行与 bdev 对称: "v1 /dev/nor0 raw 访问 = 钩子置 NULL → -ENOTSUP(消费者经 tg_flash_get 绑定); v2 再议 ioctl/debug 文件面"。

## [P1] 契约类型未定义且 mount cfg 无来源: tg_fs_cfg_t/tg_stat_t/O_* 标志族
- **位置**: `04-vfs.md` §2 / L51, L57, L62, L67-68; §1 / L30
- **问题**: tg_fs_ops.mount 收 `const tg_fs_cfg_t *cfg` 但 tg_fs_cfg_t 全文档未定义, 且 tg_mount_register(path, ops, fs_priv) **没有 cfg 参数**——cfg 由谁构造、内容是什么(设备名? block_cycles?)悬空, FS 作者无法实现 mount; tg_stat_t(getattr/setattr 用)、TG_O_* 常量(tg_open 的 uint32_t flags)同样未定义。
- **证据**: "int (*mount)(void *fs_priv, const tg_fs_cfg_t *cfg, tg_inode_t **root)"; "int tg_mount_register(const char *path, const tg_fs_ops *, void *fs_priv)"; "tg_stat_t *st"
- **建议**: 定义 tg_fs_cfg_t(至少: 介质设备名、只读标志、FS 私有配置指针)并加进 tg_mount_register 签名; 补 tg_stat_t 与 TG_O_* 最小集(或显式声明"O_CREAT/O_EXCL/O_TRUNC/O_RDONLY/O_WRONLY/O_RDWR, 详见 svc-posix 头")。

## [P1] open 标志语义在 vfs-core 与 FS ops 间的分工未定义
- **位置**: `04-vfs.md` §1 / L19; §2 / L62, L93
- **问题**: O_CREAT/O_EXCL/O_TRUNC/O_APPEND 各自由谁检查执行未写: create 的 flags 参数是转发 open 标志还是仅创建类标志; O_TRUNC 在 open/create/setattr 哪一步; O_APPEND 与偏移/lseek 的交互; 打开目录路径的行为(O_DIRECTORY?)。这些是文件语义的核心, 全部缺席。
- **证据**: "int (*open)(tg_file_t *f, uint32_t flags)"; "int (*create)(tg_inode_t *dir, const char *name, uint32_t flags, tg_inode_t **out)"; "未命中且 O_CREAT → inode_ops.create"
- **建议**: 增加"标志分工表": vfs-core 负责走查与 O_CREAT 分派; FS 的 open/create 负责 O_EXCL 冲突与 O_TRUNC(或统一由 vfs-core setattr 截断); O_APPEND 声明为"每次 write 前原子置尾"并写明执行方。

## [P1] 错误码子集缺路径类错误, 且 06 与 08 两份"SD-10 统一"清单不一致、-EOPNOTSUPP/-ENOTSUP 混用
- **位置**: `06-device.md` §4 / L186; `07-concrete-fs.md` §3 / L32; `2-os-core/08-core-api-list.md` §11 / L289
- **问题**: (a) 06 的子集(-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS)**没有 -ENOENT/-ENOTDIR/-EISDIR/-ENAMETOOLONG**——路径走查最基本的错误无法在声明的子集内表达, 而 vfs 恰是本域核心; (b) 08 §11 也自称"错误码(SD-10 统一)"但清单不同(含 EAGAIN/ENOMEM/EEXIST/ETIMEDOUT, 无 ENOSPC/EROFS)——"单一错误空间"实际两套; (c) 07 用 -EOPNOTSUPP, 06 子集写 -ENOTSUP。
- **证据**: 06: "负 errno 子集: -EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS"; 08 §11: "错误码(SD-10 统一): …-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT"; 07: "unlink/mkdir → -EOPNOTSUPP"
- **建议**: 建一张权威错误码总表(按域分列、统一编号), VFS 域补 -ENOENT/-ENOTDIR/-EISDIR/-ENAMETOOLONG; 统一用词(建议 -ENOTSUP); 06 §4 与 08 §11 改为互引而非各自罗列。

## [P1] NAND 列为 v1.0 flash 子型对接对象但 v1 不可用; wear-leveling 归属未显式声明
- **位置**: `06-device.md` 篇首 L5(版本 v1.0)、§3 / L132、SD-2 / L219、O-S2 / L233; `07-concrete-fs.md` §4 / L37, L41
- **问题**: 篇首声明"版本: v1.0"且 flash 子型示例为"spi-nor / nand", 但 O-S2 承认 NAND 坏块管理/OOB 需 v2+ 叠加 bbm/FTL——littlefs 直接对接 raw NAND 在 v1 会因坏块静默损坏数据, 范围声明与开放问题矛盾; 另外 wear-leveling 的归属(littlefs 内部 block_cycles)从未显式写明, 读者可能以为 flash 子型或驱动要做 WL。
- **证据**: "tg_flash_register | **spi-nor / nand**"; "O-S2: NAND: 对接 cdev-core flash 子型后, 坏块管理/OOB 需在其上叠加 bbm/FTL 层(v2+)"; "manifest 配置: 挂载点(/data)/设备名/block_cycles"
- **建议**: 06 §3 表把 nand 标注"v2+(需 bbm 层, O-S2)", v1.0 flash 子型 = spi-nor only; 07 §4 加一句"磨损均衡由 littlefs(block_cycles)承担, flash 子型/驱动不做 WL; NAND 坏块归 bbm/FTL(v2+)"。

## [P1] "bdev 请求带 cache 维护职责标注"无 API 载体; DMA 一致性场景未分
- **位置**: `05-bdev.md` §4 / L57-59, §5 / L79; `00-architecture.md` §10 / L615
- **问题**: read/write 是裸 (lba, buf, n_lba) 签名, 无请求对象或标志位, "职责标注"没有任何机制承载, 两文档重复同一虚指; §4 同时说 tg_dma_alloc 提供"对齐/一致性"又说"传输前后 tg_mm cache 维护"——coherent 分配与显式维护何时各用(真实 SoC 非一致 DMA vs QEMU 一致)未分场景; §5 时序图把 "tg_mm_cache_invalidate + 拷贝" 并列为一步, 混淆直通(维护调用方缓冲)与 bounce(拷贝、免维护)两条路径。
- **证据**: "bdev 请求带 cache 维护职责标注(对齐主文档 R4)"; "驱动经 tg_dma_alloc 分配传输缓冲(对齐/一致性); 传输前后 tg_mm cache 维护"; "T -> T: tg_mm_cache_invalidate + 拷贝"
- **建议**: 删"标注"措辞或落实为 geom flags(如 TG_BDEV_F_COHERENT); 写明两分支: 调用方缓冲满足 align → 驱动 DMA 前后维护 cache; 否则驱动内部 bounce(tg_dma_alloc, 免维护); 修正 §5 图。

## [P2] 篇首决策列表过期: 04 漏 SD-15, 06 漏 SD-13/SD-14
- **位置**: `04-vfs.md` 篇首 / L5 vs §6 表 L148-154; `06-device.md` 篇首 / L5 vs §7 表 L217-226
- **问题**: 04 篇首写"本篇决策 SD-1/SD-3/SD-4/SD-7"但 §6 表含 SD-15; 06 篇首写"SD-2/SD-5/SD-6/SD-10/SD-11/SD-12"但 §7 表含 SD-13/SD-14(README 索引行反而是对的"SD-2~14")。
- **证据**: "本篇决策 SD-1/SD-3/SD-4/SD-7(全局编号, 分篇列出)" / "本篇决策 SD-2/SD-5/SD-6/SD-10/SD-11/SD-12"
- **建议**: 篇首与决策表同步(04 加 SD-15; 06 加 SD-13/SD-14)。

## [P2] 05 §3 EROFS 特例引用错章节
- **位置**: `05-bdev.md` §3 / L52
- **问题**: 写"EROFS 特例走 FS 级缓存(07-concrete-fs §3)"——07 §3 是 devfs; EROFS 的"FS 级解压页缓存"在 07 §5。
- **证据**: "EROFS 特例走 FS 级缓存(`07-concrete-fs` §3), 与本层正交"
- **建议**: 改为 `07-concrete-fs` §5。

## [P2] 08 §9 引用不存在的"04-vfs.md §0.1 消费者表"
- **位置**: `2-os-core/08-core-api-list.md` §9 / L262(指向被评审模块)
- **问题**: 04-vfs 只有 §1–§7, 无 §0.1, 也无消费者表; 消费者依赖表实际在 06-device §1——引用悬空。
- **证据**: "类型契约由服务方文档化(`docs/6-vfs-device/04-vfs.md` §0.1 消费者表即用法总览)"
- **建议**: 改指向 `06-device.md` §1 消费者依赖表。

## [P2] tg_inode 标注"不透明(D14)"与"公共头+FS 私有尾"矛盾
- **位置**: `04-vfs.md` §2 / L85-91
- **问题**: L85 注"不透明(D14)", L86 紧接着给出带公共字段的 struct 定义并注明"公共头; 私有尾: littlefs=路径前缀, devfs=设备条目"——CA-2 式公共头+私有尾不是不透明对象, 公共头必须对 FS 插件可见且布局应入 golden; 标注会误导实现者把定义藏进 .c, FS 无法构造私有尾。
- **证据**: "typedef struct tg_inode tg_inode_t; /* 不透明(D14) */" + "struct tg_inode { /* 公共头; 私有尾… */ }"
- **建议**: 删"不透明(D14)"或改为"对 APP 不透明; 公共头布局入 vfs-core golden(CA-2 同型)"。

## [P2] 预建目录列表与自动 mkdir 双机制并存, 优先级未定义
- **位置**: `07-concrete-fs.md` §2 / L20, §6 / L55, L61; `04-vfs.md` §2 / L97; `06-device.md` §1 / L24
- **问题**: tmpfs 有"manifest 预建目录列表"(预建 /dev /data /tmp), 挂载计划又有"挂载点在父 FS 缺失时自动 mkdir", littlefs 还额外依赖"fs/tmpfs(挂载点父目录)"——三处机制冗余, 冲突时(预建列表与挂载点不一致)谁赢未写; 有 auto-mkdir 时 littlefs 对 fs/tmpfs 的 init 依赖并不必要(只需父挂载先于自己)。
- **证据**: "/dev、/data、/tmp 等目录由 manifest **预建目录列表**生成"; "挂载点在父 FS 缺失时自动 mkdir"; "fs/tmpfs(挂载点父目录)"
- **建议**: 定主从(建议: 挂载点一律由挂载计划 auto-mkdir, 预建列表仅用于非挂载目录如 /tmp), 写明校验与优先级。

## [P2] tmpfs 承诺 truncate 但 ops 集无承载槽
- **位置**: `07-concrete-fs.md` §2 / L19; `04-vfs.md` §2 / L68
- **问题**: tmpfs 宣称"完整文件语义(…truncate…)", 但 file_ops 无 truncate 槽, inode_ops 只有 setattr(tg_stat_t 未定义)——"truncate=setattr(size)"的映射未写明, 与入口缺失叠加后 ftruncate 无路径。
- **证据**: "完整文件语义(read/write/lseek/truncate/unlink/rename/mkdir/stat)"; "int (*setattr)(tg_inode_t *ino, const tg_stat_t *st)"
- **建议**: 声明"truncate = setattr(size)"(含语义: 截断/扩展行为), 或显式加 truncate 槽。

## [P2] 无引用计数与 dup/unlink-while-open 的现实场景冲突, 语义未定义
- **位置**: `04-vfs.md` §1 / L43; `07-concrete-fs.md` §2 / L19
- **问题**: "v1 无引用计数"的论据是"单 APP 无 fork", 但没覆盖: svc-posix 的 dup/dup2(两 fd 指向同一 tg_file_t, close 一个即释放→另一 fd 悬挂)与 unlink 已打开文件(tmpfs 的 POSIX 语义=延迟删除 vs littlefs lfs_remove 打开文件行为)。
- **证据**: "v1 无引用计数(单 APP 无 fork)"; tmpfs "完整文件语义(…unlink…)"
- **建议**: 声明 v1 策略: svc-posix fd 表自持轻量引用计数(dup 不新建 tg_file_t); unlink-while-open 语义按 FS 分列(立即删除或 -EBUSY)。

## [P2] bdev read/write 返回值与请求拆分/越界契约不清
- **位置**: `05-bdev.md` §1 / L13, L19-20; §5 / L80
- **问题**: 返回 int 但时序图写"返回 n_lba"(n_lba 是 size_t, 大传输截断); "max_rw 单次传输上限"未声明由谁遵守(FS/堆叠层必须拆分?); lba+n_lba 越界检查归谁(ops 直调无 bdev-core 仲裁点); 部分完成返回部分数还是 -EIO 未定义。
- **证据**: "uint32_t max_rw; /* 单次传输上限(扇区) */"; "int (*read)(void *priv, uint64_t lba, void *buf, size_t n_lba)"; "T -> T: 返回 n_lba"
- **建议**: 契约写死: "调用方保证 n_lba ≤ max_rw 且 lba+n_lba ≤ n_sectors, 违者 -EINVAL; 成功返回 n_lba(或改 ssize_t), 失败负 errno, 无部分成功"。

## [P2] 跨挂载 rename 的 EXDEV 语义未分配
- **位置**: `04-vfs.md` §2 / L66
- **问题**: rename 可跨目录, ndir 属另一挂载(不同 FS 实例)时行为未写; 检查归 vfs-core(路径级)还是 FS 未分配; -EXDEV 不在 SD-10 子集。
- **证据**: "int (*rename)(tg_inode_t *dir, const char *name, tg_inode_t *ndir, const char *nname)"
- **建议**: 写明"vfs-core 分派前校验同挂载, 跨挂载返回 -EXDEV(补入错误码总表)"。

## [P2] devfs 拒绝操作只列了 unlink/mkdir, create/rename/rmdir 缺
- **位置**: `07-concrete-fs.md` §3 / L32
- **问题**: 只写"unlink/mkdir → -EOPNOTSUPP"; open("/dev/x", O_CREAT) 走 create、rename/rmdir 行为同样需要定义。
- **证据**: "unlink/mkdir → -EOPNOTSUPP(设备生命周期归驱动注册, 不归文件系统)"
- **建议**: 改为"create/unlink/mkdir/rmdir/rename 全部 -ENOTSUP"(统一用词)。

## [P2] QEMU bdev 适配器 erase=nop 缺 0xFF 预擦语义; "1:1 零胶水"表述过强
- **位置**: `07-concrete-fs.md` §4 / L37, L39
- **问题**: littlefs 假定擦除态=全 0xFF, 磁盘镜像默认 0x00 会导致 mount 失败/误判——适配器需在首次使用时将镜像填 0xFF(或模拟 erase), 文档未提; 另外 lfs_config 回调是 (block, off) 形参而 tg_flash_ops 是平面 addr, littlefs 无 sync 回调、还需 cache_size/lookahead 参数——"一一对应, 适配层近零"略有夸大(胶水小但不为零)。
- **证据**: "program→write, erase→nop(磁盘无擦除), sync→flush"; "与 littlefs 的 lfs_config 块设备回调一一对应, 适配层近零"
- **建议**: 适配器规格补"镜像初始化=全 0xFF"; "1:1"改为"近 1:1((block,off)→平面地址换算 + 几何参数直填)"。

## [P2] "devfs 适配层"术语所有者漂移 + 04 §4.2 保留 D21 旧措辞
- **位置**: `06-device.md` §3 / L177; `00-architecture.md` D22 / L59; `07-concrete-fs.md` §3 / L30; `04-vfs.md` §4.2 / L116
- **问题**: 适配器实际归 cdev-core(07 §3/06 §1 表), 但 D22 与 06 §3 称之为"devfs 适配层", 易被读成 fs/devfs 的代码; 04 §4.2 的"open_file 钩子(cdev-core 实现: 会话 → tg_file_t 适配)"是 D21 措辞, 与 D23"钩子返回 {fops, fpriv}"(06 §2)未同步; "poll/close 由 devfs 适配层提供"还易误读为设备侧无 poll/close, 而 tg_cdev_ops 实际有这两个槽(适配层是转发它们)。
- **证据**: "file 面向的 poll/close 由 devfs 适配层(cdev-core 通用 tg_file_ops)提供"; "cdev-core 实现: 会话 → tg_file_t 适配"
- **建议**: 统一命名"cdev-core 文件面适配层(经 open_file 钩子发布)"; 04 §4.2 改为"打开经 open_file 钩子取 {fops, fpriv}(D23), cdev-core 提供通用文件面适配"。

## [P2] TG_CDEV_F_EXCLUSIVE 无声明载体, 执法者未写
- **位置**: `04-vfs.md` §1 / L44; `06-device.md` §3 / L154
- **问题**: tg_cdev_register(name, ops, dev_priv) 无 flags 参数, tg_cdev_ops/tg_dev_class_entry_t 也无 flags 字段——"驱动声明 TG_CDEV_F_EXCLUSIVE"无处声明; 二次 open 检查需要 cdev-core 维护每设备 open 计数, 责任方未写。
- **证据**: "驱动声明 TG_CDEV_F_EXCLUSIVE → 二次 open 返回 -EBUSY(uart 类典型)"; "int tg_cdev_register(const char *name, const tg_cdev_ops *, void *dev_priv);"
- **建议**: tg_cdev_register 加 flags 参数(或 entry 加 flags 位); 写明"排他由 cdev-core 文件面适配层以 open 计数执法"。

## [P2] socket 等非路径对象与"fd 层零特判"的关系未在 VFS 文档闭合
- **位置**: `04-vfs.md` §4.1 / L110; `00-architecture.md` §7.3 / L458; `1-architecture/01-api-contract-governance.md` §3.2 / L222
- **问题**: 主文档与治理文档两处提到"socket 类插件向 owner 注册 file_ops(VFS provider 模式)", 但 04-vfs(拥有 tg_file 契约的文档)对该注册 API/对象形态完全缺席: socket fd 是否也是 tg_file_t、file_ops 向谁注册(svc-posix? vfs-core?)未定义——若不是 tg_file_t, "fd 表项 = tg_file_t*, 零特判"不成立。
- **证据**: "fd 表项 = `tg_file_t*`(SD-1: 设备/文件统一, fd 层零特判)"; "socket 类插件向 owner 注册 file_ops(VFS provider 模式)"
- **建议**: 04 增加占位节"非路径对象进入 fd 表": 声明 socket 等对象是否复用 tg_file_t/tg_file_ops 契约、注册面归属(v2 设计前锁定方向)。

## [P2] 06 §1 存储栈图把 bdev-core 框架与 bdev 实例混同
- **位置**: `06-device.md` §1 存储栈图 / L96-98(主文档 §10 同图)
- **问题**: 节点标注"bdev 子分类 = bdev-core 插件"而有向边 "BD --> PC : tg_bdev_ops(可堆叠)" 与 "PC --> DRV1"——读作"bdev-core 依赖 page cache", 实际是"FS 绑定的 bdev 实例可被 cache 层包装"; 框架件与设备实例混同会误导依赖分析(与堆叠组合协议缺口相关)。
- **证据**: "[bdev 子分类 = bdev-core 插件…]" + "BD --> PC : tg_bdev_ops(可堆叠)" + "BD ..> DRV1 : 未组合 cache 直连"
- **建议**: 图节点改为"bdev 实例栈(驱动 bdev + 可选堆叠层)", 或拆分 bdev-core(框架)与 blk0(实例)两个节点。

---

## 统计

| 严重度 | 数量 |
|---|---|
| P0(矛盾/架构缺陷) | 2 |
| P1(完备性缺口/技术不清晰) | 17 |
| P2(改进建议) | 15 |
| **合计** | **34** |

**核心修复优先级建议**: 先清两处 P0 矛盾(依赖方向文字), 再闭合 D23 open 会话协议(会话私有数据/{fops,fpriv} 所有权)与 vfs-core 入口 API 面, 然后补并发契约(FS 侧/注册表侧)与 bdev 堆叠组合协议。

## 核对无误的方面(供参考)

- D19–D23 四框架件切分与 D21 devfs/tmpfs 投影的架构骨架自洽; 06 §1 归属表/依赖图/消费者清单完整
- 技术事实正确: littlefs COW+元数据对掉电安全、lfs_config 近 1:1 绑定、EROFS 只读压缩、tmpfs/devfs 语义、bdev 堆叠(provider=consumer)与分区映射器、ioctl 位布局与 Linux _IOC 一致、virtio-blk 读流程(cache invalidate 方向)与 flash 编程前 flush 方向均正确
- SD-1~SD-15 全局编号无重复无断号, 与主文档 D17/D19–D23 决策内容一致(问题仅在各篇头部清单滞后)
- page cache"无感层"定位四处一致; 版本口径一致(VFS v1.0(M2)/分区 v1.x/EROFS v2.0/page cache vx.0)

# 跨文档一致性专项评审意见(全 21 篇横切)

> 评审人: subagent(跨文档一致性专项, 行号经父 agent 抽查核实; 主文档行号以 695 行现行版本为准——评审期间 §3 图被用户并行重排, 687→695 行)
> 评审对象: `tangramOS/docs/` 全部 21 篇的横切一致性——交叉引用正确性 / 术语与命名 / 版本与里程碑归属 / 决策转述一致性 / native API 面一致性 / 生命周期与启动序列 / 旧评审意见吸收核对
> 意见分档: P0 = 直接矛盾/断链; P1 = 描述漂移/过时; P2 = 表述不一致风险

## 总评

文档树整体一致性水平较高——全局编号体系(D/SD/CA/R-S/O-S/R-1~6/TC)运转良好, 21 份文档中绝大多数交叉引用、D 决策转述(D17/D18/D20/D21/D22/D23 均一致)、重点版本里程碑(虚拟内存分期/调度器三版本/bottom half/littlefs/EROFS/page cache/动态加载/crypto/Rust 插件/SMP/fault 路径)、native API 48 函数+3 宏的清点与归属、README 索引状态标记与分工声明, 逐项核对均一致; 旧评审意见 v0.3 全部 22 条与 v0.4 一条均有明确落点。问题集中在三类: ①拆分文档的**章节级错链**(唯一 P0 为 08→04 的 §0.1 断链); ②v0.5–v0.9 快速演进在子文档/决策表留下的**过时转述**(D19 决策行、生命周期"三相"、debug 版本表两处冲突); ③并行写作造成的**符号级漂移**(`.tg_plugin` 段名、TRACE_EVT 宏名、sched_ops 槽位名、TG_CDEV_F_EXCLUSIVE)。

---

## [P0] 08 §9 引用 04-vfs "§0.1 消费者表"——目标章节不存在且内容在别处
- **位置A**: `08-core-api-list.md` L262: "core 只管**名字 → 指针**, 不解释 ops 类型——类型契约由服务方文档化(`docs/6-vfs-device/04-vfs.md` §0.1 消费者表即用法总览)"
- **位置B**: `04-vfs.md` L7: "## 1. 统一可打开模型(SD-1, D21 修订)"(04-vfs 无 §0/§0.1, 章节自 §1 起, 全篇亦无"消费者表")
- **位置C**: `06-device.md` L18: "**消费者的依赖声明(manifest):**"(真正的消费者表在 06-device §1)
- **问题**: 引用的章节(§0.1)与所述内容(消费者表)在 04-vfs 中均不存在, 实际位于 06-device §1。
- **建议**: 改引 `docs/6-vfs-device/06-device.md` §1。

## [P1] 09-int "06 §8 三层模式"错链
- **位置A**: `09-int.md` L14: "**PIC 抽象**: prio/trigger 解释权在 platform(**06 §8 三层模式**); core 只传递"
- **位置B**: `06-device.md` L228: "## 8. 风险与开放问题(本篇)"(06 §8 是风险表, 非三层模式)
- **位置C**: `00-architecture.md` L523: "## 8. 平台能力三层模式…"; `08-core-api-list.md` L178: "## 8. 中断(tg-irq 组; PIC 实现 = platform 三层模式)"
- **问题**: 按 2-os-core 系文档"NN §M"编号约定(同系列 `11-memory.md` L22 "06 §6 三纪律"为正确用法), "06 §8"指向 06-device §8(风险表), 与"三层模式"不符; 三层模式在主文档 §8(09-int 自己 L4 即引"主文档…§8")或 08 §8。疑为 "00 §8"/"08 §8" 笔误。
- **建议**: 改为"主文档 §8"; 并建议 2-os-core 各篇对跨文档章节引用统一加文档号限定(08 L105 "(§5.2)"等裸引同类)。

## [P1] 05-bdev 引 07 §3 指称 EROFS FS 级缓存——实际在 §5
- **位置A**: `05-bdev.md` L52: "EROFS 特例走 FS 级缓存(`07-concrete-fs` **§3**), 与本层正交"
- **位置B**: `07-concrete-fs.md` L24: "## 3. devfs(v1.0, /dev 设备节点 — D21)"(§3 是 devfs)
- **位置C**: `07-concrete-fs.md` L44/L48: "## 5. EROFS(v2.0)" / "**FS 级解压页缓存**: …与 bdev 层 page cache 正交(`05-bdev` §3)"
- **问题**: EROFS 解压页缓存内容在 07 §5, 引用误写为 §3。
- **建议**: 05-bdev L52 改引"07-concrete-fs §5"。

## [P1] 生命周期被转述为"三相", 子相位 PRE_ARCH/PIC/ARCH 全树无定义
- **位置A**: `12-plugin-mgr.md` L13: "**生命周期**(§6.2): EARLY(**PRE_ARCH/PIC/ARCH**)/CORE/LATE **三相**; 启动序列见主文档 §9"
- **位置B**: `00-architecture.md` L343: "`EARLY → CORE → LATE → APP`, 同阶段内按依赖拓扑序。"(四相; §9 图 L565-575 亦无 PRE_ARCH/PIC/ARCH)
- **位置C**: `11-memory.md` L32: "堆的启动时序: 需要在插件 init 前可用 ⇒ 早期初始化点(**§9 的 PRE_ARCH 后**)"
- **问题**: 12-plugin-mgr 把 §6.2 转述为"三相"(丢 APP)并发明 EARLY 子相位名 PRE_ARCH/PIC/ARCH(全树无定义); 11-memory 又把 PRE_ARCH 当作主文档 §9 的既有阶段引用, 而 §9 中不存在该阶段名。
- **建议**: 12-plugin-mgr 改为四相转述并删除未定义子相位; PRE_ARCH 若确需保留, 先在主文档 §9 定义, 11-memory 同步。

## [P1] 描述符 start 回调无相位落点, 资源前提与 §9 开中断时序矛盾
- **位置A**: `00-architecture.md` L331-333: "int (*early_init)(void); /* 无堆/无线程/关中断 */ … int (*start)(void); /* **中断可用, 可创建线程** */"
- **位置B**: `00-architecture.md` L570-574: ":LATE / Service → Interface 依序 init; / **:全局开中断**; / :app.start() 创建 APP 线程"(§9 无任何相位调用 start; 全局开中断在 LATE 之后)
- **位置C**: `14-app.md` L12: "启动序列(§9): 全部插件 init 完 → 开全局中断 → **进入 APP main**"; `12-plugin-mgr.md` L13(三相转述, 亦无 start)
- **问题**: start 回调("中断可用")在 §6.2/§9 的阶段模型中没有落点: 若排入 LATE, 则违反 §9 的"全局开中断在 LATE 之后"(中断仍关); 若排在开中断之后, §9/§6.2 未画出。所有子文档转述(12/14)也只提 init。
- **建议**: 在主文档 §6.2/§9 明确三回调→四相位的映射(如 EARLY→early_init、CORE→init、开中断后/APP 相位→start), 12-plugin-mgr 与 14-app 同步转述。

## [P1] 描述符链接段名两写: .tg_plugins vs .tg_plugin
- **位置A**: `00-architecture.md` L338: "__attribute__((used, section(\`.tg_plugins\`), aligned(4)))"(另 L227 "扫 \`.tg_plugins\` 链接段"、L564 "扫描 .tg_plugins")
- **位置B**: `08-core-api-list.md` L335: "#define TG_PLUGIN_SECTION __attribute__((used, section(\`.tg_plugin\`)))" 与 L338: "extern const tg_plugin_t __start_.tg_plugin[], __stop_.tg_plugin[];"
- **位置C**: `12-plugin-mgr.md` L21: "描述符二进制细节: 段位置(\`.tg_plugin\`)+ \`__start/__stop\` 边界枚举(机制见 08 §13.3)"
- **问题**: 同一链接段名, 主文档 3 处写 ".tg_plugins"(复数), 08/12 共 4 处写 ".tg_plugin"(单数)。附带: 08 用 \`static const\`+边界符号枚举收集, 主文档 §6.1 宏却生成非 static 的 \`_tg_plugin_<name>\` 全局符号——两处收集机制描述也不一致。这是 D14/golden 纪律下的链接级契约名, 照两文档分别实现会直接冲突。
- **建议**: 统一段名(建议随主文档 §4.4/§9 用 ".tg_plugins"), 并统一收集机制(建议采纳 08 §13.3 的 static+边界枚举, 主文档宏同步改)。

## [P1] trace 宏名两写: TRACE_EVT vs TG_TRACE_EVT
- **位置A**: `03-debug.md` L23: "API: \`TRACE_EVT(id, a, b, c)\` 宏; 未启用时编译期整层移除(**零开销**)"
- **位置B**: `08-core-api-list.md` L284: "|\`TG_TRACE_EVT\` 宏(\`docs/4-debug/03-debug.md\` §1) | 中断内追踪 |"(ISR 白名单契约行)
- **问题**: 同一宏两个名字; 08 的白名单(契约级)用 TG_TRACE_EVT, 03 的定义用 TRACE_EVT。按本树命名规范(08 §12 "宏/常量 \`TG_*\`"), TG_TRACE_EVT 才合规。
- **建议**: 03-debug §1 统一为 \`TG_TRACE_EVT\`。

## [P1] sched_ops 槽位名漂移: create/switch_out/yield vs thread_ready/thread_block
- **位置A**: `08-core-api-list.md` L61/63/64: "|\`tg_task_create\`|…| core 构造 + \`sched_ops.create\`|" / "|\`tg_task_exit\`|…| core + \`sched_ops.switch_out\`|" / "|\`tg_task_yield\`|…|\`sched_ops.yield\`|"
- **位置B**: `00-architecture.md` L272-283: tg_sched_ops 草案字段为 thread_ready/thread_block/pick_next/on_tick/thread_sleep/mutex_lock/mutex_unlock/sem_take(无 create/switch_out/yield)
- **问题**: 08 语义规格引用的调度插件 ops 槽位(create/switch_out/yield)在主文档 tg_sched_ops 定义中不存在(后者的"…"省略号也从未列出它们); 两文档对同一 ops 表的槽位命名不一致(如任务入列, 08 用 create, 主文档语义对应 thread_ready)。
- **建议**: 以 10-sched 的 tg_sched_ops 完整规格(DoD 第 3 项)收口时统一槽位命名; 收口前 08 §2.1 至少标注"槽位名暂定, 以 10-sched 为准"。

## [P1] tg_mm 的"TLB 维护"在 08 权威清单中缺失
- **位置A**: `00-architecture.md` L215: "\`tg_mm\` 接口: region 描述(base/size/attrs)、map/unmap、**TLB 维护**、cache 维护 API(R4: M2 起定好签名)"(另 L543: "| 内存 map 管理 | \`tg_mm\`(region/attrs/map/unmap/TLB) |…")
- **位置B**: `08-core-api-list.md` L164-176: tg-mm 组仅 tg_mm_region_add/tg_mm_map/tg_mm_unmap/tg_mm_cache_flush/tg_mm_cache_invalidate(L26: "| \`tg-mm.txt\` | MMU/cache(§7) | **5** |…")
- **问题**: 主文档两处声明 tg_mm 契约含"TLB 维护", 但 08(native API 完整清单与签名的权威落点)的 5 函数清单无任何 TLB 维护 API——第一契约面与主文档描述不一致。
- **建议**: 或在 08 §7 补 TLB 维护函数(计入 CA-5 的 48+3 面预算), 或主文档 §4.3/§8 删去"TLB 维护"字样(说明其归 ISA 库/内部实现)。

## [P1] debug bridge 版本归属: v1.x(00/03) vs v1.0·M3(02)
- **位置A**: `00-architecture.md` L630: "| debug bridge | … | **v1.x 最小集** |"; `03-debug.md` L27: "## 2. debug bridge(类 adb, **v1.x 起最小集**)"
- **位置B**: `02-roadmap.md` L43(v1.0 小节): "| debug | **trace 插件**…+ **最小 debug bridge**(memread / trace 流) |" 与 L65: "|\`service/dbg-bridge\`|…| M3 |" 及 L172: "| M3 | **bridge 最小集** + … | **v1.0 完整化** + native API 冻结启动(D15) |"
- **问题**: 02-roadmap 把 bridge 最小集列为 v1.0 的 M3 验收项("v1.0 完整化"), 而 00 §11/03 §2 标 v1.x; 按本树 v1.x 语义(级联域/分区/红区/M4 均标 v1.x, 02 L70 "M4(v1.x)"), v1.x 指 v1.0 之后的 minor——两处版本归属冲突。
- **建议**: 以 02 的 M 里程碑为准(M3∈v1.0), 00 §11 与 03 §2 改"v1.0(M3) 最小集"。

## [P1] TLSF 红区/金丝雀版本: v1.x(00/03) vs v2.0 行(02)
- **位置A**: `00-architecture.md` L632: "target: **TLSF 红区+金丝雀(v1.x)**、per-plugin arena 泄漏记账(v2.0)…"; `03-debug.md` L49: "| target 便宜替代 | TLSF 红区…+ 栈 canary | **v1.x** |"
- **位置B**: `02-roadmap.md` L81(v2.0 小节 debug 行): "| debug | **mini ramdump** + target 侧 memleak(arena 记账) + **红区/金丝雀** |"(v1.0 debug 行 L43 无此项, 全篇仅此处提及红区)
- **问题**: 红区/金丝雀在 00/03 归 v1.x, 02 只在 v2.0 debug 行出现——同一能力两处版本归属不同。
- **建议**: 以 00 §11/03 §4 为准(v1.x), 02 v2.0 行删去"红区/金丝雀"或移至 v1.0 行并注明 v1.x。

## [P1] D19 决策行推论未随 D21 修订——决策表内自相矛盾
- **位置A**: `00-architecture.md` L56(D19 行): "…**派生: vfs-core→dev-core(设备路由+适配)**, svc-posix→vfs-core; …"
- **位置B**: `00-architecture.md` L58(D21 行): "**vfs-core 纯化**(设备依赖移出, 依赖面收缩到 core)"
- **位置C**: `06-device.md` L224(SD-12): "~~vfs-core 依赖 dev-core + cdev-core~~ → **SD-13/D21 修订**: 撤销" 与 L16: "| **vfs-core** | … | **core(纯 VFS, D21: 撤销设备路由)** |"
- **问题**: D21 已撤销 vfs-core→dev-core 依赖(06 的 SD-12 用删除线标注了修订, D11 行也对 D18 修订做了删除线标注), 但主文档决策表 D19 行仍保留"vfs-core→dev-core(设备路由+适配)"且无修订标注——只读 D19 行会得到已被废弃的依赖拓扑。
- **建议**: D19 行仿 D11/SD-12 的做法, 改为"~~vfs-core→dev-core~~ → D21 修订: 撤销(vfs-core 纯化)"。

## [P1] devfs 的 manifest 依赖清单三处说法不同
- **位置A**: `07-concrete-fs.md` L33: "devfs **只依赖 dev-core**(枚举/钩子协议)**与 vfs-core**(挂载)——**不依赖任何子分类形状**"
- **位置B**: `06-device.md` L23(消费者的依赖声明表): "| **fs/devfs(/dev)** | **dev-core**(枚举注册表)+ **cdev-core(钩子就序)**+ vfs-core(挂载) |"
- **位置C**: `02-roadmap.md` L59: "|\`fs/devfs\`| FS | … | **dev-core + cdev-core + vfs-core** | M2 |"
- **位置D**: `00-architecture.md` L443-444(§7.2 图)仅有 "DFS2 --> DEVC2 : 枚举/钩子协议"(无 devfs→cdev-core 边); 而 `06-device.md` L57(§1 图)有 "DFS ..> CDEVC : open_file 钩子实现方"
- **问题**: devfs 的依赖集合三处不一致——06/02 为三项(dev-core + cdev-core + vfs-core), 07 明言"只依赖"两项; 两张依赖图(00 §7.2 vs 06 §1)的边也不一致。
- **建议**: 以 06 §1/02 的 manifest 清单为准(含 cdev-core 作 init 时序前置), 07 L33 改为"不依赖任何子分类**形状**(init 时序上需 cdev-core 钩子就绪)", 并统一两张图的边。

## [P1] "D8 是最后一个未落定的架构决策"与待定决策表矛盾
- **位置A**: `09-int.md` L29: "| **D8** | **中断线程化**(**最后一个未落定的架构决策**): …"
- **位置B**: `00-architecture.md` L694-695(§17 待定决策点): "| D7 | Service 插件确切边界 | … |" 与 "| D8 | 中断线程化 | … |"(D7、D8 两项均待定)
- **位置C**: `17-service-mgmt.md` L28: "| **D7** | …——**最后未定的决策之一** |"(02-roadmap §5 第 1 项亦将 D7/D8 并列为待收口)
- **问题**: 09-int 称 D8 是"最后一个"未落定决策, 但主文档 §17 与 17-service-mgmt/02-roadmap 均表明 D7 与 D8 两项均未落定。
- **建议**: 09-int 改为"未落定的架构决策之一(D7/D8, 见主文档 §17)"。

## [P1] TG_CDEV_F_EXCLUSIVE 在 cdev-core 契约中无定义
- **位置A**: `04-vfs.md` L44: "驱动声明 \`TG_CDEV_F_EXCLUSIVE\` → 二次 open 返回 -EBUSY(uart 类典型)"
- **位置B**: `06-device.md` L143-154: tg_cdev_ops 全字段(open/read/write/ioctl/poll/close/suspend/resume)与 "int tg_cdev_register(const char *name, const tg_cdev_ops *, void *dev_priv);"(无 flags 参数; 06 全文无 TG_CDEV_F_* 定义)
- **问题**: 04-vfs(vfs-core 契约篇)引用的字符设备互斥打开标志 TG_CDEV_F_EXCLUSIVE, 在其属主契约文档 06-device(cdev-core)中既无定义, 注册 API 也无携带 flags 的途径——该机制在全树无落点。
- **建议**: 在 06-device §3 定义 TG_CDEV_F_*(如为 tg_cdev_register 增加 flags 入参), 或 04-vfs L44 改为 manifest 声明式表述。

## [P1] TC-MM-003 期望 -ENOSYS, 不在 08 错误码子集且违反 19 自身 R-4
- **位置A**: `19-test.md` L99: "| TC-MM-003 | map/unmap(v1) | \`-ENOSYS\`(签名在, 实现未到——R4 冻结验证) | ALL | target |"
- **位置B**: `08-core-api-list.md` L289: "负 errno 子集 \`-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT\`"(无 -ENOSYS; `06-device.md` L186 设备域子集 \`-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS\` 亦无)
- **位置C**: `19-test.md` L16: "| **R-4** | 错误码 ∈ SD-10 负 errno 子集(扫描验证) |"; `04-vfs.md` L18: "不支持的槽位 = NULL, 返回 -ENOTSUP"
- **问题**: 用例期望值 -ENOSYS 不在任何已文档化的错误码子集中, 直接违反 19-test 自己的 R-4 不变量(扫描会判红), 且与本树"未支持 → -ENOTSUP"的既有约定不一致。
- **建议**: 期望改 -ENOTSUP, 或在 08 §11 子集增补 -ENOSYS 并写明其语义(未实现 vs 不支持)。

## [P1] 08 §11 错误码子集与 06 §4 SD-10 子集两处清单不同, 关系未说明
- **位置A**: `08-core-api-list.md` L289: "错误码(**SD-10 统一**): …\`-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT\`"(core 域, 9 项)
- **位置B**: `06-device.md` L186: "错误模型(**SD-10**): …\`-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS\`"(设备域, 7 项)
- **问题**: 两处都引 SD-10("单一错误空间", 06 L222)但给出不同子集(08 含 EAGAIN/ENOMEM/EEXIST/ETIMEDOUT, 06 含 ENOSPC/EROFS), 未写明"统一空间=并集、各域取子集"的关系; 19-test R-4"错误码 ∈ SD-10 负 errno 子集"指向因此不明。
- **建议**: 在 SD-10(06 §7)或 08 §11 写明全集与域子集的归属关系。

## [P1] 07 §6 littlefs init 依赖漏 bdev-core
- **位置A**: `07-concrete-fs.md` L62: "init 顺序由 manifest 依赖声明保证: fs/devfs → {dev-core, cdev-core, vfs-core}; **fs/littlefs → {vfs-core, fs/tmpfs, cdev-core}**"
- **位置B**: `06-device.md` L24: "| littlefs | **vfs-core** + cdev-core(flash 子型绑定)/** bdev-core(QEMU bdev 适配)**+ fs/tmpfs |"; `02-roadmap.md` L62: "…| **vfs-core + cdev/bdev-core + fs/tmpfs** | M2 |"
- **问题**: 07 的 littlefs 依赖集漏掉 bdev-core(QEMU bdev 适配所需), 与 06/02 清单不一致。
- **建议**: 07 补为 {vfs-core, fs/tmpfs, cdev/bdev-core(按介质二选一)}。

## [P1] vfs-core 类型依赖的归属: 06 §2 说 dev-core, 各表/图都归 cdev-core
- **位置A**: `06-device.md` L125: "\`open_file\` 钩子签名引用 \`tg_file_ops\`(vfs-core 类型)⇒ **dev-core** 对 vfs-core 为**类型依赖**(仅头文件, 无 init/call 依赖)"
- **位置B**: `06-device.md` L13-14(§1 表): dev-core 依赖 = "**core native**"; cdev-core 依赖 = "**dev-core + vfs-core**(类型/适配钩子)"
- **位置C**: `02-roadmap.md` L54-55: "|\`dev-core\`|…| core | M2 |" / "|\`cdev-core\`|…| **dev-core + vfs-core(类型)** | M2 |"; `00-architecture.md` L436-439(§7.2 图): "CDEVC2 --> VFSC2 : 类型/适配钩子"(无 DEVC2→VFSC2 边)
- **问题**: open_file 钩子位于 dev-core 的 tg_dev_class_entry_t, 故 06 §2 把类型依赖记给 dev-core; 但 06 §1 表、02 清单、主文档 §7.2 图一致把"类型/适配钩子"依赖只记在 cdev-core 名下——归属表述不一(dev-core 的头文件级 vfs-core 依赖在各依赖表中缺失)。
- **建议**: 统一表述, 如 06 §1 表 dev-core 依赖补"(+vfs-core 头文件级类型依赖)", 或 §2 L125 改述为该类型依赖由 cdev-core 侧承担。

---

## [P2] 三篇"本篇决策"篇头清单与篇内决策表不一致
- **位置A**: `04-vfs.md` L5: "本篇决策 SD-1/SD-3/SD-4/SD-7(全局编号, 分篇列出)。" vs 同篇 L154 决策表含 "SD-15 | **VFS ops 分层(D23)**…"(篇头漏 SD-15)
- **位置B**: `06-device.md` L5: "本篇决策 SD-2/SD-5/SD-6/SD-10/SD-11/SD-12。" vs 同篇 L225-226 表中另有 SD-13/SD-14(篇头漏两项)
- **位置C**: `05-bdev.md` L5: "本篇决策 SD-2(bdev 侧)/SD-8/SD-9。" vs 同篇 §6 表(L88-89)仅 SD-8/SD-9(SD-2 权威定义在 06 §7)
- **位置D**: `README.md` L22: "06-device.md 设备管理(域总览/子分类/**SD-2~14**)"(区间式写法, 与 04 条目"SD-1/3/4/7/15"的显式风格不一致, 且字面含属 04/05 的 SD-3/4/7/8/9/15)
- **问题**: D21/D22/D23 落地时新增的 SD-13/SD-14/SD-15 未回填篇头"本篇决策"声明; README 用易误读的区间式编号。
- **建议**: 各篇头补齐; README 改显式列举(如"SD-2/5/6/10~14")。

## [P2] 11-memory 引 DoD"第 1 项"应为第 2 项
- **位置A**: `11-memory.md` L37: "\`docs/1-architecture/02-roadmap.md\` §5 **第 1 项**(D7/D8 之外的收敛项): native API 头文件级规格的内存部分已在 08"
- **位置B**: `02-roadmap.md` L188-189: 第 1 项为 "D7/D8 收口", 第 2 项为 "**native API 头文件级规格**…"
- **问题**: 所指内容(native API 头文件级规格)是 DoD 第 2 项; "第 1 项"与括注"D7/D8 之外"自相矛盾(同系列引用 10-sched"第 3 项"、12"第 4 项"、13"第 4/5 项"均正确)。
- **建议**: 改"第 2 项"。

## [P2] 07 §8 "ioct"拼写错误
- **位置A**: `07-concrete-fs.md` L94: "D22(主文档): 设备 ops 统一预留 **ioct**/PM 钩子(\`06-device\` §3, SD-14)"
- **位置B**: `06-device.md` L226: "| SD-14 | …\`ioctl\`/\`suspend\`/\`resume\` 槽位全类别预留…"
- **建议**: 07 改 "ioctl"。

## [P2] 05-bdev "(v-x)"版本记法非标准
- **位置A**: `05-bdev.md` L51: "**写穿优先**(**v-x**); **flush 必须转发到下层**"
- **位置B**: `00-architecture.md` L595: "page cache 无感层 **vx.0** 选配(堆叠 bdev)"; `05-bdev.md` L5: "page cache **vx.0**"
- **问题**: 同一 page cache 写穿约束的版本标注写作"v-x", 全树其余处均为"vx.0"。
- **建议**: 统一"vx.0"。

## [P2] iface-autosar-ish 引"主文档 §7.4"——目标节无此名称
- **位置A**: `02-roadmap.md` L156: "\`iface-autosar-ish\`(**主文档 §7.4** [?])"
- **位置B**: `00-architecture.md` L473-475(§7.4 表仅 iface-posix/iface-min/iface-pkcs11; L479 仅泛化论述"任何域标准 API 都是一个插件")
- **位置C**: `15-interface.md` L15: "未排期: iface-autosar-ish(车规生态)"
- **问题**: 02 把主文档 §7.4 当作该插件出处, 但 §7.4 未提及此名。
- **建议**: 02/15 改引"主文档 §7.4 泛化论述", 或在 §7.4 表补一行[?]。

## [P2] 插件名前缀风格与 CLI 命令集未收拢
- **位置A**: `02-roadmap.md` L63: "|\`svc-posix\`| Service |…"(无类别前缀) vs L64: "|\`service/trace\`| Service |…" / L65: "|\`service/dbg-bridge\`| Service |…"(带前缀, 同表风格不一)
- **位置B**: `00-architecture.md` L650: "tg add platform/qemu-aarch64 sched/sched-coop **iface/posix** io/uart-pl011 fs/littlefs service/trace"(CLI 路径式, 与插件名 \`iface-posix\` 拼写不同)
- **位置C**: CLI 命令集各处不同: `00-architecture.md` L649-654(init/add/run/test/build)、`03-debug.md` L32(\`tg dbg\`)、`01-api-contract-governance.md` L174(\`tg api-dump\`)、`13-toolchain.md` L20(大纲仅 "add/remove/build/run/check/flash", 未收拢 init/test/dbg/api-dump)
- **建议**: 13-toolchain 成文时统一插件命名规范(类前缀或连字符二选一)与 CLI 命令全集。

## [P2] tg_thread/tg_mutex(结构体 tag)与 tg_thread_t/tg_mutex_t(typedef)混用
- **位置A**: `00-architecture.md` L272-283: "void (*thread_ready)(**tg_thread** *t); … int (*mutex_lock)(**tg_mutex** *m, …)"
- **位置B**: `08-core-api-list.md` L33: "typedef struct tg_thread **tg_thread_t**;" 与 L43: "int tg_task_create(**tg_thread_t** **t, …)"
- **问题**: 主文档 §5.1 ops 签名用结构体 tag 名, 08 契约签名用 typedef 名——同一类型两种拼写风格。
- **建议**: 统一用 typedef 名(08 风格), 主文档 §5.1 同步。

## [P2] sched_class 同名双值域无消歧
- **位置A**: `00-architecture.md` L269(tg_sched_ops): "uint32_t sched_class; /\* **PREEMPT | COOP | TT** (§5.3) \*/"(调度器自身类别)
- **位置B**: `10-sched.md` L12: "**sched_class 声明**(D10): **SAFE_PREEMPT / COOP_ONLY / TT_SAFE**——插件声明…"(插件描述符字段值域; 主文档 §5.3/L326 同)
- **问题**: tg_sched_ops.sched_class 与 tg_plugin_t.sched_class 同名但值域不同(PREEMPT|COOP|TT vs SAFE_PREEMPT/COOP_ONLY/TT_SAFE), 且 §5.1 注释引 §5.3 加剧混淆。
- **建议**: §5.1 注释改"调度器自身类别(COOP/PREEMPT/TT), 区别于插件声明的 §5.3 兼容类别", 或改字段名(如 sched_mode)。

## [P2] TG_MALLOC_ALIGN 无契约定义
- **位置A**: `19-test.md` L85: "| TC-MEM-001 | malloc/free 往返 + 对齐 | ≥ **TG_MALLOC_ALIGN** | ALL | host |"
- **位置B**: `08-core-api-list.md` L131-156(§6 tg-mem 组全部函数/宏清单, 无 TG_MALLOC_ALIGN; 11-memory 亦未定义)
- **问题**: 用例断言所依据的对齐常量在 API 清单与内存篇中均无定义。
- **建议**: 在 08 §6(或 11-memory 成文时)定义 malloc 对齐保证。

## [P2] tg_fs_cfg_t / tg_stat_t / tg_dir_t / "tg_fs" 引用而无定义
- **位置A**: `04-vfs.md` L51: "int (*mount)(void *fs_priv, const **tg_fs_cfg_t** *cfg, tg_inode_t **root);"(同篇 L67-68 getattr 的 tg_stat_t、L95 的 tg_dir_t 亦无定义)
- **位置B**: `12-plugin-mgr.md` L20: "生成物(\`**tg_fs_cfg**\` 等)"(同样引用而未定义)
- **位置C**: `00-architecture.md` L512: "| B: os_tangram.c VFS 后端(~600 行) | … | 纯 native(**tg_fs**) |"(native 文件 API 符号族应为 tg_file_*/tg_open, 见 04 §1)
- **问题**: 多个契约类型在签名中被引用但全树无定义; 主文档 §7.6 的 "tg_fs" 更是非已定义符号族的简写。
- **建议**: 04 §2 补 tg_fs_cfg_t 最小定义(挂载点/设备名等), 主文档 §7.6 改"tg_file_*"。

## [P2] 评审意见相对路径不解析(已自我声明"仓库外")
- **位置A**: `00-architecture.md` L10: "吸收评审意见(\`../comment/00-architecture-comment.md\`, 仓库外)"
- **位置B**: `02-roadmap.md` L3: "依据: \`../comment/00-architecture-comment.md\`(仓库外)评审意见"
- **问题**: 相对路径 \`../comment/\` 自文档位置(tangramOS/docs/1-architecture/)解析为 tangramOS/docs/comment/(不存在); 实际文件在工作区根 \`comment/\`。虽有"(仓库外)"声明, 字面路径仍误导。
- **建议**: 改为说明式引用(如"工作区根 comment/00-architecture-comment.md, 仓库外")。

## [P2] 02 README 预案"文档导览(00–03)"过时
- **位置A**: `02-roadmap.md` L197: "README 内容预案(届时一次写完): 定位一句话、**文档导览(00–03)**、三域示例、QEMU 快速上手、许可证(WTFPL)。"
- **位置B**: `README.md` L6-27(现有 docs/README.md 索引已覆盖 00–19 共 20 篇文档)
- **问题**: 预案的导览范围仍停留在只有 00–03 四篇的旧规模; 且"README 在 v1.0 设计完成后编写"(L184)与已存在的 docs/README.md(索引)未作命名区分, 易误读为同一文件。
- **建议**: 预案改"文档导览(docs/README.md 索引, 00–19)", 并注明所指为仓库根 README 而非 docs/README.md。

---

## 第 7 项: 旧评审意见吸收核对(comment/00-architecture-comment.md)

| 旧意见 | 状态 | 落点/缺口 |
|---|---|---|
| 战略: 实验性质但可靠性/可移植性/性能不放松; 量产基于 rCore/unikraft 二次开发 | 已吸收 | `00-architecture.md` §0 持久资产表(L24-30)+ `02-roadmap.md` §0(L11 近乎逐句对应)+ R9(00 L684) |
| OS 命名(uk=unikernel 简称, 求命名) | 已吸收 | D16(00 L34 命名段 + L53 决策行): TangramOS, tg_/TG_/tg/.tg_* |
| 虚拟内存: 恒等映射 v1.0 / 重定位 v2.0 / MPU vx.0 | 已吸收 | 00 §4.3 分期表(L208-212); 11-memory L8; 02 v1.0/v2.0 域行 |
| int: bottom half v1.0 | 已吸收 | 00 §4.2(L202 "v1.0 即实现 bottom half(评审要求)"); 02 L36; 08 §5(L127) |
| sched: coop v1.0 / preempt v2.0 / tt v3.0 | 已吸收 | D2(00 L41); 00 §5.2(L297-299); 02 §1/§2; 10-sched L11 |
| memory map 管理能否作为插件 | 已吸收 | 00 §8 三层模式(L523-550, region 表归 Platform 插件) |
| 中断(终端)控制器管理能否作为插件 | 已吸收 | 00 §8(tg_pic ops 填表归 Platform 插件; §8 标题即该提问原文) |
| 插件依赖如何管理 / 环形依赖如何管理 | 已吸收 | 00 §6.5(L367-383, init 依赖 vs 调用依赖 + 四种解法); §6.4 环检测硬错误(L361); 12-plugin-mgr |
| 插件数据结构需增加版本管理 | 已吸收 | 00 §6.1 描述符 v2(ver[3]/api_rev/abi_id, L322-334) |
| trace 插件 v1.0 | 已吸收 | 00 §11(L629); 03-debug §1; 02 L43/L64(trace M2) |
| 类 adb debug bridge | 已吸收(版本归属有漂移, 见 P1) | 00 §11(L630); 03 §2; 02 L43/L65(dbg-bridge M3)——00/03 标 v1.x vs 02 标 v1.0/M3 |
| mini ramdump v2.0 | 已吸收 | 00 §11(L631); 03 §3; 02 L81/L95(service/ramdump) |
| ASan/memleak 如何实现 | 已吸收(红区版本有漂移, 见 P1) | 00 §11(L632); 03 §4 四层表(host 白捡/红区金丝雀 v1.x/arena v2.0/完整 ASan vx)——红区在 02 v2.0 行(L81)另有一说 |
| 密码学插件 v2.0 | 已吸收 | 02 L79/L91(service/crypto); 00 §12 依赖链(L644); 16-service L23 |
| Page Cache vx.0 无感层 | 已吸收 | 00 §10(L595/L621); 05-bdev §3(SD-8); 02 vx.0 小节(L145 io/pagecache) |
| Block Device v1.0 | 已吸收 | 02 L42(block 层)+ L61(io/virtio-blk M2); 05-bdev L5(v1.0) |
| VFS v1.0 | 已吸收 | 02 L42; 04-vfs L5(v1.0, M2 起); 02 L56(vfs-core M2) |
| EROFS v2.0 | 已吸收 | 02 L80/L94; 07 §5; 00 §10(L592)/D17(L54) |
| 动态插件加载 v3.0 + 鉴权机制 | 已吸收 | D1(00 L40); 00 §12(L636-644); 02 v3(L106/L133 service/modload + ed25519); 12-plugin-mgr §2.7; 16-service L24 |
| language: core C v1.0→C+Rust v4.0; 插件 C v1.0/Rust v2.0 | 已吸收 | D5(00 L44); 02 v2.0(L82 Rust 插件)/v4.0(L138-139 core 迁移) |
| SMP: v1.0 单核 / v2.0 SMP | 已吸收 | D6(00 L45); 02 L45/L77; R8 v2a/v2b 分期(00 L683, 02 L84) |
| ISA: aarch64 | 已吸收 | D9(00 L46); 02 L34 |
| D10: 双保险 | 已吸收 | D10(00 L47); 00 §5.3(L315); 19-test §4(L127) |
| v0.4: "Service 插件应该在 Interface 插件上, APP 插件下" | 已吸收(经 D18 转化) | D18/v0.5(00 L9、L55): svc-posix 服务化, 三方/服务可声明依赖 POSIX 运行时; 接口插件成为严格叶子(§7.2/§7.5/§7.6)。缺口: 字面层级(Service 位于 Interface 之上)被有意反转(接口=叶子, 服务在其下), 反转理由已成文(§7.2 L458)但未显式标注"该 v0.4 意见的处置结论" |

---

## 统计

| 严重度 | 数量 |
|---|---|
| P0(断链/直接矛盾) | 1 |
| P1(描述漂移/过时) | 15 |
| P2(表述不一致风险) | 16 |
| **合计** | **32** |

**核对一致(无发现)的重点项**: D17/D18/D20/D21/D22/D23 转述; 虚拟内存三期、bottom half v1.0、调度器三版本、trace v1.0、ramdump v2.0、host ASan v1.0/target ASan vx、littlefs v1.0、EROFS v2.0、page cache vx.0、动态加载 v3、crypto v2、Rust 插件 v2、SMP v2(v2a/v2b)、fault 路径 v2、级联域 v1.x/M4、分区 v1.x; native API 48 函数+3 宏清点与 golden 分组、CA-1~10、ISR 白名单四件、框架件符号归属(tg_open/tg_file_*/tg_bdev_*/tg_flash_* 归框架件)、插件清单计数(v1.0 17 件/v2.0 7 件/v3.0 2 件)与 M0–M4 里程碑; README 索引 20 篇全存在、状态标记与各篇自述一致、末尾分工声明与实际一致。

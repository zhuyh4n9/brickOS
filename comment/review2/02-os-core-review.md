# OS Core(内核核心)模块评审意见(08/09/10/11/12/17)

> 评审人: subagent(os-core 模块, 证据经父 agent 抽查核实)
> 评审对象: `tangramOS/docs/2-os-core/` 下 `08-core-api-list.md`、`09-int.md`、`10-sched.md`、`11-memory.md`、`12-plugin-mgr.md`、`17-service-mgmt.md`
> 评审维度: 完备性(矛盾、技术点是否清晰、架构是否存在问题)
> 意见分档: P0 = 矛盾/架构缺陷(必须解决); P1 = 完备性缺口/技术不清晰(应当解决); P2 = 改进建议

## 模块总评

该模块整体成熟度较高: 决策记录纪律(D/CA/SD 编号 + `[?]` 待定标注)、契约治理机器(golden/三态/三层 CI)、以及"48 函数 + 3 宏"的克制 API 面都显示出真实的 unikernel/RTOS 设计功底, 五篇骨架也全部兑现了 README 承诺的"范围 + 大纲 + 继承决策"结构。最大风险有三: **其一**, 08 自称"第一契约的完整形态"、01 §2.6.1 把它的 §2–§9 登记为每函数规格的权威落点, 但除 §2 外其余 7 组(41/48 个函数)没有按自身 L7 约定给出错误/阻塞/归属契约, 契约细节正在向 19-test 用例漂移, 而 §15 计划在 M3 冻结的第一批恰是这些缺规格的组; **其二**, core↔调度插件的关键内部接口 `tg_sched_ops` 在 08 与主文档 §5.1 两处不一致, TCB/锁对象的分配与静态定义机制悬空; **其三**, 上下文模型只承认 ISR/线程"唯二世界", 启动/init 上下文缺位, 导致"调度器注册前能否用锁""coop 下 bh 跑在谁的栈上"这类机制问题在全文档无答案。骨架篇另有继承走样(12 的"三相")、大纲缺核心项(11 的三池、10 的 coop 语义)与 3 处错误交叉引用。

---

## P0 — 矛盾/架构缺陷

### [P0] 08 违反自身"每组两小节"约定: §3–§9 缺每函数契约与实现设计, 契约正漂移到测试文档
- **位置**: `08-core-api-list.md` L7 / §3–§9; 交叉: `01-api-contract-governance.md` L116、`02-roadmap.md` L189、`19-test.md` L99/L105/L85
- **问题**: L7 声明"每组两小节"的详细设计约定, 但只有 §2(任务)交付了 2.1 语义规格 + 2.2 实现设计; §3(同步)/§4(时间)/§5(work)/§6(内存)/§7(MMU)/§8(中断)/§9(服务)共 41/48 个函数无每函数的错误码、阻塞、ISR 约束、超时、归属规格, 也无实现设计小节。后果已经发生: API 契约正在被测试文档发明——`tg_irq_register` 重复注册返回 `-EBUSY`、`tg_mm_region_add` 重叠返回 `-EINVAL`、`tg_mm_map` v1 返回 `-ENOSYS`、malloc 对齐保证 `TG_MALLOC_ALIGN` 都只存在于 19-test 用例中, 08 本体只字未提。01 §2.6.1 把"每函数规格"的权威位置登记为"08 §2–§9", 而 08 §15 又计划 M3 第一批冻结 tg-mem/tg-mm/tg-irq/tg-svc——恰好是缺规格的四组, 冻结将发生在语义未钉死的面上。
- **证据**: "详细设计约定: 每组两小节——**语义规格**(每函数: 错误/阻塞/归属)+ **实现设计**(数据结构/不变量/竞态)"(08 L7); "| 每函数规格 | `docs/2-os-core/08-core-api-list.md` §2–§9 | 人写(文档跟随) |"(01 L116); "TC-IRQ-002 | 重复 register | `-EBUSY`"(19 L105)、"TC-MM-003 | map/unmap(v1) | `-ENOSYS`"(19 L99)。
- **建议**: 按 L7 约定补齐 §3–§9 每组的语义规格表(错误码/阻塞/ISR-safe/超时/归属五列)与实现设计小节; 把 19-test 中已发明的契约(`-EBUSY`/`-EINVAL`/`-ENOSYS`/`TG_MALLOC_ALIGN`/无虚假唤醒)回填 08 并注明来源; 在 §15 第一批冻结前完成 tg-mem/tg-mm/tg-irq/tg-svc 四组规格, 否则调整冻结批次。roadmap L189 称 §2 为"函数规格样例", 若"其余组随后补"是既定计划, 应在 08 头部显式声明"§3–§9 规格待补"并挂到 DoD, 而不是以"成文/已完成"状态示人。

### [P0] `tg_sched_ops` 接口在 08 与主文档 §5.1 两处矛盾; TCB 分配协议缺失
- **位置**: `08-core-api-list.md` §2.1 L61–64、§10 L269 vs `00-architecture.md` §5.1 L259–281、§4.1 L172–183
- **问题**: 08 §2.1 的归属列引用了 `sched_ops.create`、`sched_ops.switch_out`、`sched_ops.yield` 三个槽位; 但 08 §10 声明"权威定义 = 主文档 §5.1", 而主文档 §5.1 的 ops 结构只有 `thread_ready/thread_block/pick_next/on_tick/thread_sleep/mutex_lock/mutex_unlock/sem_take` + "…"——create/switch_out/yield 不在其中, 实现者无法从权威定义得知这些槽位是必备契约。同时 08 §2.2 规定"TCB = core 公共头 + 调度插件私有尾, `sizeof` 来自调度插件头", 那么**谁分配 TCB、core 如何获知插件侧尺寸**(ops 表中无 `tcb_size` 或分配钩子)完全没有机制。另外 `switch_out` 作为调度器 ops 与主文档 §4.1"上下文切换原语(汇编 save/restore)归 core"的切分相抵触——切换是 core 的, 调度器只应做选择(pick), 命名把职责搅浑了。
- **证据**: "core 构造 + `sched_ops.create`"(08 L61)、"core + `sched_ops.switch_out`"(08 L63)、"`sched_ops.yield`"(08 L64); "void (*thread_ready)(tg_thread *t); … tg_thread *(*pick_next)(void);"(00 L265–267); "| `tg_sched_register(const tg_sched_ops *)` + `tg_sched_ops` | 调度插件(EARLY, 恰一次, 二次→panic) | 主文档 §5.1 |"(08 L269); "上下文切换原语(每 ISA 一段汇编 save/restore)| 任何调度器都要做切换, 与策略无关"(00 §4.1)。
- **建议**: 在 10-sched 成文时定稿唯一 ops 全集(含 create/join/exit/yield/sleep/锁/work 槽位, 以及 TCB 尺寸获取方式——`tcb_size` 字段或由调度插件分配 TCB), 08 §2.1 归属列改引定稿后的槽位名; 明确调用方向约定"切换 asm 归 core、就绪/选择归插件", 将 `switch_out` 改名为 `pick_next`+core 切换或等价表述; 在此之前不要让 08 的 `sched_ops.*` 引用与主文档结构体并存于两份"成文"文档。

## P1 — 完备性缺口/技术不清晰

### [P1] 上下文模型缺"启动/init 上下文": "thread-only"标签与启动期调用事实矛盾, 调度器注册前的同步原语行为未定义
- **位置**: `08-core-api-list.md` L53、L286、§3 vs `00-architecture.md` §6.1 L324–326、§9 L560–568、§6.2 L335
- **问题**: 08 把白名单之外全部标为"thread-only", 但插件 init 运行在任何线程存在之前(CORE/LATE 相, boot 栈, 中断关闭), 且必须能调 `tg_malloc`("堆可用")、app.start() 必须能调 `tg_task_create`(在 `tg_sched_run()` 之前)——"thread-only"这个标签与事实不符, 真实规则应是"非 ISR + 生命周期阶段可用性矩阵", 而该矩阵(哪些 API 在 EARLY 无堆/CORE 堆可用/start 中断可用各阶段可调)无处成文; `tg_task_self()` 在 init 上下文返回什么未定义; 更尖锐的是: EARLY 相内按依赖拓扑排序, 而没有任何插件声明对调度器的依赖(18-plugin-dev 路由表), 调度器是否保证最先注册未规定——**调度器注册前调用 `tg_mutex_lock`/`tg_sem_take` 是 panic、返回错误还是空指针解引用, 全文档无答案**。
- **证据**: "全部 **thread-only**(ISR 禁令, §11)"(08 L53)、"其余**全部 thread-only**——conformance 静态扫描执法"(08 L286); "int (*init)(void); /* 堆可用, 调度器已锁定, 中断仍关 */"(00 L324); ":CORE 各插件 init(堆可用) … :app.start() 创建 APP 线程(main 语义); :tg_sched_run()"(00 §9)。
- **建议**: 在 08 §11 增加 init/boot 上下文并给出"生命周期阶段 × API"可用性矩阵(create/malloc/publish/region_add 在 init 期可用; sleep/yield/join/cond/sem_take 仅线程上下文); 在 08 §3 明确"调度器注册前调用同步/阻塞原语 = panic"的失败语义; 规定调度插件在 EARLY 相内的注册次序保证(如组合期强制其拓扑序最先); conformance 静态扫描规则同步扩展为三上下文。

### [P1] coop 下 bottom half 的执行上下文与分派机制未定义; 10-sched 大纲未兑现 DoD 第 3 项的"sched-coop v1 语义"
- **位置**: `08-core-api-list.md` §5 L127; `00-architecture.md` §5.2 L295; `10-sched.md` §2 L19–25; `02-roadmap.md` §5 第 3 项 L190
- **问题**: coop 下 work = "事件入队、主循环分派"——**谁的"主循环"? work 回调跑在哪个栈、哪个线程上下文**(idle 循环? 调度插件内部 worker 线程——但线程创建时序未定? APP 主循环——但 48 函数中无分派 API)? 回调内可否阻塞、`tg_task_self()` 是什么, 均未定义。这是 v1.0 即上线的首要延迟路径。同时 roadmap DoD 第 3 项明确承诺"tg_sched_ops 完整规格 **+ sched-coop v1 语义(yield 点、事件队列、work queue 行为)**", 而 10-sched 大纲第 1 项只引用了前半句, 大纲 7 项中没有"sched-coop 语义/事件队列"条目, 第 5 项"work queue(bh)实现归属: 调度插件(08 §5)"又把皮球踢回 08——承诺与大纲脱节。
- **证据**: "coop → 事件入队、主循环分派(v1 形态)"(00 L295); "v1 无 work 对象/cancel——fn+arg 裸提交; v1.0 即上线(评审要求: coop 下=事件队列)"(08 L127); "**tg_sched_ops 完整规格** + sched-coop v1 语义(yield 点、事件队列、work queue 行为)"(roadmap L190) vs 10 大纲无对应条目。
- **建议**: 10-sched 大纲补独立条目"sched-coop v1 语义: 事件队列结构、work 分派点与执行上下文、work 回调契约(可否阻塞/可否再 submit)"; 08 §5 补一句"work 回调运行于线程上下文(bh 契约)"或按实际设计声明; 若 coop 需要 idle 循环代分派, 在 10 大纲第 3 项(idle/WFI)中显式关联。

### [P1] CA-2 静态定义宏的解析机制未说明; 二进制插件与调度器布局耦合未声明
- **位置**: `08-core-api-list.md` §3 L85–104、§13.5 L349–353; 交叉: `18-plugin-dev.md` L23–32
- **问题**: 锁对象 = core 公共头 + 调度插件私有尾, "宏展开的 sizeof 由所选调度插件头决定"——但插件/驱动作者必须调度器无关(18 的八类路由表中没有任何插件依赖调度器), 那么 `TG_MUTEX_DEFINE` 宏定义在哪个头、组合器如何让插件源码解析到**所选**调度器的布局(伞头文件? 构建期生成头? `-include`?)完全未说明。更深一层: 任何内嵌 `tg_mutex_t` 的插件(静态宏或嵌入自有结构体), 其数据布局随调度器变化 ⇒ D14 二进制分发(.a)的插件实际是"每调度器一份", 这个矩阵代价未声明, abi_id 是否包含调度器指纹也未定——与"为车规 BSP 厂商 .a 交付预留通路"的承诺直接相关。
- **证据**: "**CA-2 对象布局**: core 公共头(状态字)+ 调度插件私有尾(PI 字段等); `TG_*_DEFINE` 宏展开的 `sizeof` 由所选调度插件头决定——静态组合天然成立; 二进制分发时公共头入 core golden, 私有尾入调度插件 golden(abi_id, D14)"(08 L104)。
- **建议**: 在 08 §13.2/§13.5 写明机制(如组合器生成 `tg_sched_sel.h` 指向所选调度器布局头); 在 D14/abi_id 一节明确"含静态/内嵌锁对象的二进制插件绑定调度器 ABI"并纳入版本矩阵 CI 维度; 或者改用"core 分配不透明锁对象(`tg_mutex_create/destroy`)"方案解耦插件布局与调度器——三选一, 但必须成文。

### [P1] v2b SMP 下 `tg_irq_lock` 语义弱化无预警; 跨核互斥/自旋锁原语无归属
- **位置**: `08-core-api-list.md` §0 L18、§8 L189; `00-architecture.md` §5.2 L289、D6/R8
- **问题**: v1 单核下 `tg_irq_lock` = 全局互斥; v2b SMP 下它只关本核中断。coop 的"mutex ≈ irq 锁包装"以及一切用 `tg_irq_lock` 保护共享数据的插件代码, 在 SMP 组合下**静默失去互斥性**; 而 08 §0 把"SMP 原语(per-CPU/IPI)"刻意排除在 native 面外, 自旋锁(ISR 上下文的跨核保护)没有任何文档认领归属——v2b 将被迫在已冻结面之外新增 API, 这与"预留即免破坏"(D22)的哲学相悖。
- **证据**: "SMP 原语(v2: per-CPU/IPI)"(08 §0 刻意不在清单); "锁退化为 irq 锁(极便宜)"(00 §5.2); "tg_irq_state_t tg_irq_lock (void); /* ISR-safe(嵌套计数在 core) */"(08 L189); "v1.0 单核; v2.0 SMP(与 preempt 同期)"(D6)。
- **建议**: 在 08 §8 明确声明"tg_irq_lock 的互斥效力仅限单核; SMP 下只保证本核 ISR 屏蔽与抢占禁止"; 为 v2b 的自旋锁/抢占关闭原语预先立项决策记录(新面规划); conformance 静态扫描增加规则: 禁止"以 irq_lock 做跨线程数据互斥"的代码进入 SMP 组合(引导用 mutex)。

### [P1] 中断退出→调度检查路径(抢占点)在 09 与 10 两篇大纲中均缺失
- **位置**: `09-int.md` §2 L18–23; `10-sched.md` §2 L19–25
- **问题**: preempt(v2) 下 ISR 退出必须检查 need_resched 并可能切换——`tg_sem_give`(白名单内)与超时到期都会在 ISR 上下文标记待调度, 该机制是中断框架(core)与调度插件的交界。09 大纲 6 项(向量/注册表/临界区/bh/fault/SMP)与 10 大纲 7 项(ops/切换/超时轮/锁矩阵/work/SMP/tt)**均无"IRQ 退出→调度检查"条目**, 有"09 认为归调度、10 认为归中断"的两不管落空风险; v1 coop 虽不需要, 但 `tg_sched_ops` 定稿(10 大纲第 1 项)时必须同时定义"哪些 ops 可在 ISR 上下文调用、ISR 内标记的待调度由谁在何处消费"。
- **证据**: 09 §2 大纲 1–6 项与 10 §2 大纲 1–7 项的完整列表中无中断退出调度点条目(09 L18–23、10 L19–25)。
- **建议**: 在 10-sched 大纲第 2 项(上下文切换)或 09 大纲第 1 项(向量与入口)明确"IRQ 退出→need_resched→切换"的归属与规格条目; `tg_sched_ops` 完整规格中定义 ISR 上下文可调用的 ops 子集与调用方向。

### [P1] DMA attrs 的"cache 属性"在 v1 恒等映射下机制不通; `tg_dma_buf_t` 无一致性指示
- **位置**: `08-core-api-list.md` §6 L150–155、§7 L170; `11-memory.md` §2 L22
- **问题**: `tg_dma_alloc` 的 attrs 含"cache 属性", 但 v1 是恒等映射且 region 表只在 platform early_init 静态声明——同一 VA 不可能有两种属性, 逐缓冲改 cacheability 与恒等模型冲突; 该属性到底意味着"从预声明非缓存 region 中分配"还是"运行时改 PTE"未说明。同时 `tg_dma_buf_t` 只有 vaddr/dma_addr/size, 驱动无从得知返回缓冲是否 cache 一致(QEMU 一致、真实 SoC 常不一致)——06 §6 的"传输前后 flush/invalidate"纪律在一致缓冲上是浪费、在非一致缓冲上是必须, **判定依据缺失**。R4 要求 M2 冻结 tg-mm 签名, 此问题必须在此之前定案。
- **证据**: "attrs: TG_DMA_F_*(对齐/cache 属性, append-only); v1 恒等: dma_addr==vaddr(CA-6)"(08 L153); "tg_mm_region_add … platform early_init 声明恒等区(§9)"(08 L170); "DMA 区: `tg_dma_alloc` 属性(对齐/一致性)、与驱动契约(06 §6 三纪律)"(11 L22)。
- **建议**: v1 删除 per-buffer cache 属性(一致性 = region 表属性, 平台声明 coherent/非 coherent 区), 或保留属性但明确"仅允许命中预声明 region"; `tg_dma_buf_t` 增加 coherence 标志(append-only 字段)或提供 `tg_dma_is_coherent()` 查询; 11-memory 大纲第 4 项展开该契约。

### [P1] 内存屏障原语缺失, cache 维护 API 的 barrier 语义未定义
- **位置**: `08-core-api-list.md` §7 L174–175; `06-device.md` §6 L208
- **问题**: 全文档无 DMB/DSB/编译屏障 API(grep 证实唯一"barrier"出现在 page cache flush 语境)。aarch64 上"写描述符 → cache clean → 启动 DMA"的顺序必须由屏障保证, `tg_mm_cache_flush/invalidate` 是否内含必要屏障(ARM 推荐的 clean+DMB 序列)未声明——M4 真实 SoC(R4 自认的风险项)上这是数据损坏级问题, 且签名 M2 就要冻结。
- **证据**: "int tg_mm_cache_flush (void *addr, size_t size); /* 驱动 DMA 前后(R4) */"(08 L174); "DMA/cache 纪律 | `tg_dma_alloc` 分配; 传输前后 `tg_mm_cache_flush/invalidate`(R4)"(06 L208)。
- **建议**: 在 08 §7 声明 cache_flush/invalidate 的完整序(含前后 DMB/DSB)与编译器屏障保证; 若驱动还需要独立屏障(如 MMIO 描述符环发布), 增加 `tg_mm_dmb()/wmb()` 或在 R4 冻结记录中说明屏障由 ISA 库内联提供并如何被驱动引用。

### [P1] 级联域 demux 的 pending/ack 竞态与多 work 合流未定义; 域 ops 全 void 无错误通道
- **位置**: `08-core-api-list.md` §8.1 L205–253
- **问题**: (a) SLOW 域时序 = pending 读位图 → 逐子分发 → ack; 若某子中断在"读"与"ack"之间再次置位, ack 会清掉**未分发**的事件(边沿型直接丢失)——"处理中新子中断触发 → 线重新置位 → 新 IRQ 循环"只覆盖处理中, 未覆盖读-ack 窗口。(b) 每次 IRQ submit 一个 demux work 还是单飞合流(处理完再复查 pending)未定义——多 work 并发会撞 I2C 总线并冲击队列深度。(c) `pending/mask/unmask/ack` 全部 void 返回, I2C 读失败(PMIC 场景常态)无错误上报通道, 只能静默丢中断。该 API 声明"定稿于本清单"(L253), 形状问题必须在实现(M4)前修。
- **证据**: "void (*pending)(void *priv, uint32_t *bits, size_t nwords); … void (*ack)(void *priv, uint32_t sub);"(08 L206–211); "处理中新子中断触发 → 线重新置位 → 新 IRQ 循环"(08 L243–244); "D -> W: SLOW 域: tg_work_submit(I2C 读不能在 ISR)"(08 L236)。
- **建议**: 规定 demux 协议(mask→pending→unmask→分发→ack, 或要求 ack 幂等/状态 sticky + 电平语义); 规定单飞 demux(同一域同时至多一个 work, 完成后复查 pending); ops 返回改 int(-EIO), 失败策略(重试/线保持)写入 §8.1。

### [P1] `tg_irq_attr_t` 的 prio/trigger 编码未定义, "解释权在 platform"与驱动板级无关矛盾
- **位置**: `08-core-api-list.md` §8 L181、L196; `06-device.md` §6 L213
- **问题**: attr 的 trigger(边沿/电平/极性)与 prio 取值域未定义; 若"解释权在 platform、core 只传递", 同一常量在不同 PIC 下语义漂移, 驱动(如 uart-pl011)调用 `tg_irq_register` 时无法写出可移植的 attr——与 06 §6"驱动代码板级无关, 换板只换 Platform 插件"直接冲突。
- **证据**: "typedef struct { uint8_t prio; uint8_t trigger; uint32_t flags; } tg_irq_attr_t;"(08 L181); "prio/trigger 的解释权在 platform(PIC ops), core 只传递"(08 L196); "**驱动代码板级无关**, 换板只换 Platform 插件"(06 L213)。
- **建议**: core 定义抽象编码(`TG_IRQ_TRIG_EDGE_RISING/LEVEL_HIGH…`、`TG_IRQ_PRIO_*`), PIC ops 负责翻译到硬件编码; "解释权"改为"翻译权在 platform, 语义在 core"; 编码值进 golden(append-only)。

### [P1] mutex 递归语义未定义
- **位置**: `08-core-api-list.md` §3 L92–94、§11 L289
- **问题**: `tg_mutex_lock` 的递归策略(可递归? 递归加锁返回错误还是自死锁?)未声明; 错误码子集无 `-EDEADLK`; 19-test 的 TC-SYNC 组也没有递归用例。这是冻结前必须钉死的语义——PI 互斥(v2 preempt)与 coop 退化实现(irq 锁包装)在递归场景下行为完全不同。
- **证据**: "int tg_mutex_lock (tg_mutex_t *m); /* 阻塞至获得 */"(08 L92); 错误码列表"-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT"无 EDEADLK(08 L289)。
- **建议**: 声明"非递归; 递归加锁 = -EDEADLK(或自死锁 UB, 二选一并成文)"; 19-test 补 TC-SYNC 递归用例; 若允许递归需说明与 PI 的交互代价。

### [P1] 无 detach 语义: 未 join 的线程 ZOMBIE 永久泄漏; 无终止原语
- **位置**: `08-core-api-list.md` §2 L43–50、§2.2 L72–76
- **问题**: 状态机"ZOMBIE →(join 回收)"是唯一回收路径; 不被 join 的线程(长期 worker 类)TCB+栈永不回收, `TG_TASK_F_*` 中无 DETACHED 之类的自动回收选项(且该 flags 的 v1 值域本身未定义); 也无 terminate/cancel(挂死线程只能整机重启)。这是刻意最小化的合理取舍, 但未记录决策, 面上留了洞。
- **证据**: "NEW → READY ⇄ RUNNING → BLOCKED(等待队列+超时表) → ZOMBIE →(join 回收)"(08 L72); "join 回收: ZOMBIE 立即回收; create 时 `stack==NULL` 的栈经 `tg_free` 归还(仅此路径)"(08 L76)。
- **建议**: 增加 `TG_TASK_F_DETACHED`(exit 时自动回收, 对其 join 返回 -EINVAL), 或记录"不提供 detach, 线程必须被 join"的决策与理由; 终止原语的"刻意不做"写入决策记录; `TG_TASK_F_*` 的 v1 值域在 §2 列出。

### [P1] 线程栈"8 字节对齐"与 aarch64 AAPCS64 的 16 字节 SP 对齐冲突
- **位置**: `08-core-api-list.md` §2.2 L75
- **问题**: aarch64 过程调用标准要求公共接口处 SP 16 字节对齐(违例时 SIMD/16 字节访存路径未定义, 可能直接 fault); 08 规定栈"8 字节对齐"不足, create 构造初始栈帧若按此实现会埋雷。
- **证据**: "栈约束: `stack_size ≥ TG_STACK_MIN`(2K [?]), 8 字节对齐; 栈 guard = vx(红区)"(08 L75)。
- **建议**: 改为"SP 16 字节对齐(aarch64 AAPCS64)"并声明 stack 基址/大小的对齐取整规则; `TG_STACK_MIN` 的 2K [?] 一并落定。

### [P1] 错误码子集缺 -ENOSYS; `tg_mm_map` 的 v1 行为只存在于测试文档
- **位置**: `08-core-api-list.md` §11 L289 vs `19-test.md` L99(TC-MM-003)、L16(R-4)
- **问题**: TC-MM-003 期望 `tg_mm_map/unmap`(v1) 返回 `-ENOSYS`("签名在, 实现未到"), 但 08 §11 的错误码子集没有 -ENOSYS; R-4 不变量"错误码 ∈ SD-10 负 errno 子集"引用的 SD-10 子集(06 §4: -EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS)同样没有。API 的 v1 行为(map = -ENOSYS)只写在测试用例里, 08 §7 未声明——又一例契约漂移。
- **证据**: "TC-MM-003 | map/unmap(v1) | `-ENOSYS`(签名在, 实现未到——R4 冻结验证)"(19 L99); "负 errno 子集 `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT`"(08 L289)。
- **建议**: 08 §11 补 -ENOSYS(或统一改用 -ENOTSUP 并修订 19-test); 08 §7 为 tg_mm_map/unmap 补 v1 语义行("v1 恒等组合下返回 -ENOSYS, v2 实现"); R-4 的引用改为明确的并集子集。

### [P1] 共享 IRQ: 09 列为开放问题, 08 已"明确非目标", 两篇矛盾
- **位置**: `09-int.md` L19、L30 vs `08-core-api-list.md` §8.1 L252
- **问题**: 08 §8.1 已裁定"共享线…= 明确非目标——每根物理线恰一属主, 复用一律走域"; 09 仍把"共享 IRQ(v1 是否支持 [?])"列为大纲待定项和开放问题。骨架未继承已定决策, 读者会以为该问题还悬着; 且 09 的开放问题表也没有收录 08 §8.1 留下的另一个开放问题"嵌套域"。
- **证据**: "共享 IRQ(v1 是否支持 [?])"(09 L19); "共享 IRQ(v1: 不支持? 嵌入式单用途引脚通常独占)"(09 L30); "**共享线**(无状态寄存器的多设备并线)= 明确非目标——每根物理线恰一属主, 复用一律走域"(08 L252)。
- **建议**: 09 的该项改为"已决: 不支持(08 §8.1), 复用走域", 大纲第 2 项注明"每线恰一 handler, 重复注册 -EBUSY"; 开放问题表补"嵌套域(GPIO 扩展器挂 PMIC 之后)"; 若 09 作者认为传统多 handler 共享与"共享线"是两个概念, 需先回 08 补决策再回填。

### [P1] 12-plugin-mgr 生命周期表述与主文档矛盾: "三相" vs 四相, PRE_ARCH/PIC/ARCH 为幻影术语
- **位置**: `12-plugin-mgr.md` L13; `00-architecture.md` §6.2 L335; `11-memory.md` L32
- **问题**: 主文档 §6.2 是"`EARLY → CORE → LATE → APP`"四相; 12 写成"EARLY(PRE_ARCH/PIC/ARCH)/CORE/LATE 三相"——丢了 APP 相, 且 PRE_ARCH/PIC/ARCH 三个子相位名在全文档库无定义(仅 12 与 11 引用), 11-memory 还写"早期初始化点(§9 的 PRE_ARCH 后)"而主文档 §9 根本没有这个词。骨架把继承决策抄走了样。
- **证据**: "**生命周期**(§6.2): EARLY(PRE_ARCH/PIC/ARCH)/CORE/LATE 三相; 启动序列见主文档 §9"(12 L13); "`EARLY → CORE → LATE → APP`, 同阶段内按依赖拓扑序"(00 L335); "堆的启动时序: 需要在插件 init 前可用 ⇒ 早期初始化点(§9 的 PRE_ARCH 后)"(11 L32)。
- **建议**: 12 改回四相; PRE_ARCH/PIC/ARCH 要么删除、要么作为 EARLY 相的子序在 12 大纲正式立项定义; 11 的引用同步修正。

### [P1] 插件 start 回调在启动序列中无对应槽位: 描述符契约与 §9 序列矛盾, 12 大纲未承接
- **位置**: `00-architecture.md` §6.1 L326 vs §9 L546–570; `12-plugin-mgr.md` §2 L23
- **问题**: 描述符定义了 `start` 回调("中断可用, 可创建线程"), 但 §9 启动序列里只有 CORE/LATE 的 init 与 app.start(), **没有任何"各插件 start"阶段**——start 的执行时点、与"全局开中断"的先后、哪些类插件允许实现 start 均无定义; 12-plugin-mgr 作为生命周期的主人, 大纲第 3 项"生命周期状态机: 注册→init→运行"也没有承接 start。
- **证据**: "int (*start)(void); /* 中断可用, 可创建线程 */"(00 L326); ":LATE Service → Interface 依序 init; :全局开中断; :app.start() 创建 APP 线程"(00 §9); "3. 生命周期状态机: 注册→init→运行; 错误路径(init 失败 = 启动失败)"(12 L23)。
- **建议**: 12-plugin-mgr 定稿"回调 ↔ 相位"映射表(early_init→EARLY, init→CORE/LATE 按类与依赖, start→全局开中断后、app.start 前后的确切位置), 并修订主文档 §9 或描述符注释使两者一致; start 阶段可用的 API 集与 P1"上下文矩阵"联动成文。

### [P1] 11-memory 大纲缺失"三池"核心实现设计项
- **位置**: `11-memory.md` §2 L19–26; `docs/README.md` L14; `08-core-api-list.md` §6 L158
- **问题**: README 承诺本篇覆盖"三池", 08 §6/CA-7/CA-8 已把 heap/contig/页池三分 + manifest 预算定为关键决策, 但 11 的大纲 7 项(布局/TLSF/arena/DMA 区/cache/重定位/MPU)中**没有 contig 池分配器、页位图分配器、manifest 预算如何驱动 region 划分**的条目——本篇最核心的实现设计在大纲中缺位。
- **证据**: "1. 布局… 2. TLSF 堆… 3. arena 模型… 4. DMA 区… 5. cache 一致性…"(11 L19–26, 无三池条目); "三池划分: platform region 表把 RAM 划为 heap / contig 池 / 页池, 比例 = manifest 预算(§6.4 资源总账)"(08 L158); README L14 "内存(三池/DMA/tg_mm/布局)"。
- **建议**: 大纲补"三池模型"条目: region 划分协议(platform 表 × manifest 预算的落地机制)、contig 池分配器设计(定长块? buddy?)、页位图分配器、三池的 debug 记账; 与 13-toolchain 的 manifest 资源声明格式对齐。

## P2 — 改进建议

### [P2] 09 错误交叉引用: "06 §8 三层模式"应为"主文档 §8"
- **位置**: `09-int.md` L14
- **问题**: 三层模式在主文档 §8; 06-device §8 是"风险与开放问题", 与 PIC 无关。
- **证据**: "**PIC 抽象**: prio/trigger 解释权在 platform(06 §8 三层模式); core 只传递"。
- **建议**: 改为"主文档 §8 三层模式"(09 自己的 L4 来源行引用是对的)。

### [P2] 08 §9 错误引用"04-vfs.md §0.1 消费者表"
- **位置**: `08-core-api-list.md` L262
- **问题**: 04-vfs.md 只有 §1–§7, 无 §0.1; "消费者的依赖声明(manifest)"表实际在 06-device.md §1。
- **证据**: "类型契约由服务方文档化(`docs/6-vfs-device/04-vfs.md` §0.1 消费者表即用法总览)"。
- **建议**: 改引 `06-device.md` §1 的消费者表。

### [P2] 11-memory 的 DoD 引用错误("§5 第 1 项")
- **位置**: `11-memory.md` L36
- **问题**: roadmap §5 第 1 项是"D7/D8 收口", native API 头文件级规格是第 2 项; 11 的"第 1 项(D7/D8 之外的收敛项)"指代自相矛盾。
- **证据**: "`docs/1-architecture/02-roadmap.md` §5 第 1 项(D7/D8 之外的收敛项): native API 头文件级规格的内存部分已在 08"。
- **建议**: 改为"§5 第 2 项"。

### [P2] 插件段名两处不一致(`.tg_plugin` vs `.tg_plugins`), 且 08 示例的 C 标识符非法
- **位置**: `08-core-api-list.md` L335/L338、`12-plugin-mgr.md` L21 vs `00-architecture.md` L219/L330/L556
- **问题**: 主文档用 `.tg_plugins`(复数), 08/12 用 `.tg_plugin`(单数)——段名是 D16 级全局约定, 必须唯一; 且 `__start_.tg_plugin` 含 '.', 不是合法 C 标识符, 实际代码必须用 `__asm__("__start_.tg_plugin")` 标签技巧。
- **证据**: "section(\".tg_plugins\")"(00 L330); "TG_PLUGIN_SECTION __attribute__((used, section(\".tg_plugin\")))"(08 L335); "extern const tg_plugin_t __start_.tg_plugin[], __stop_.tg_plugin[];"(08 L338)。
- **建议**: 统一段名(建议 `.tg_plugins`); 08 示例改用 asm 标签声明; 12 大纲第 2 项补"链接脚本 KEEP() 以对抗 gc-sections"(00 §4.4 明确用 gc-sections 裁剪)。

### [P2] 主文档 §5.1 的类型名违反自身命名规范
- **位置**: `00-architecture.md` §5.1 L259–277 vs `08-core-api-list.md` §12 L293
- **问题**: 08 §12 规定"类型 `tg_*_t`", 主文档 ops 表用裸名 `tg_thread`/`tg_mutex`/`tg_sem`; golden 冻结时必须二选一。
- **证据**: "void (*thread_ready)(tg_thread *t);"(00 L265); "函数 `tg_<对象>_<动作>`; 类型 `tg_*_t`"(08 L293)。
- **建议**: 主文档 §5.1 统一为 `tg_thread_t`/`tg_mutex_t`/`tg_sem_t`。

### [P2] ISR 白名单中的 trace 宏命名两处不一致
- **位置**: `08-core-api-list.md` L284 vs `03-debug.md` L23
- **问题**: 08 称 `TG_TRACE_EVT`, 03-debug 定义为 `TRACE_EVT`(无 TG_ 前缀, 也不符合 D16 宏命名); 该宏是白名单成员, 名字是契约。
- **证据**: "| `TG_TRACE_EVT` 宏(`docs/4-debug/03-debug.md` §1) | 中断内追踪 |"(08 L284); "API: `TRACE_EVT(id, a, b, c)` 宏"(03 L23)。
- **建议**: 统一为 `TG_TRACE_EVT` 并修 03-debug。

### [P2] `tg_clock_now` 时间基准未定义; 墙钟无来源
- **位置**: `08-core-api-list.md` §4 L114–118; `00-architecture.md` §7.6 L506、§8 L533–539
- **问题**: clock_now 的起点(boot?)与量程未声明(19-test 只测单调不减); 而 svc-posix/sqlite 需要 gettimeofday(00 §7.6 的 sqlite POSIX 子集), native 面与平台三层表(内存/中断/console/timer 四行)都没有墙钟/RTC 来源。
- **证据**: "tg_time_t tg_clock_now(void);"(08 L114); "usleep/gettimeofday"(00 L506)。
- **建议**: 08 §4 声明"单调、起点 = boot"; 墙钟定位为服务/平台职责(RTC 数据经 Platform 插件或服务), 在 00 §8 表或 16-service 补一行。

### [P2] `tg_task_sleep` 的 -EINVAL(负值)对无符号参数无意义; sleep(INF) 语义未定义
- **位置**: `08-core-api-list.md` §2.1 L65–66、§4 L111
- **问题**: `tg_time_t` 是 uint64, "负值"错误条件永不触发; `TG_TIMEOUT_INF`(UINT64_MAX) 传入 sleep 是"永久挂起"还是非法未定义。
- **证据**: "| `tg_task_sleep` | 挂超时表切走, 到期唤醒(不早醒, 晚到无上界) | `-EINVAL`(负值) | 是 |"(08 L65)。
- **建议**: 改为明确的有效域规则(如 "rel_us == TG_TIMEOUT_INF → -EINVAL" 或 "INF = 永久挂起并成文"); sleep_until 的 abs 边界(含饱和值, 参见 TC-TIME-003)同补。

### [P2] `tg_task_join` 无超时变体
- **位置**: `08-core-api-list.md` §2.1 L62
- **问题**: join 永久阻塞, 目标挂死时 joiner 无法带超时退出; 全系统其他阻塞 API 均有 CA-4 超时约定, join 是例外但未声明理由。
- **证据**: "| `tg_task_join` | 等待目标退出并回收 TCB/栈 | `-EINVAL`(自 join / 已被 join) | 是 |"(08 L62)。
- **建议**: 补 `tg_task_join_to` 或记录"join 无超时"决策(与 detach 决策联动考虑)。

### [P2] sem 计数上界与 give 溓出行为未定义
- **位置**: `08-core-api-list.md` §3 L89、L96–97
- **问题**: `tg_sem_init(s, initial)` 无 max; 空闲 give 使计数无界增长, 与"资源计数"语义(应封顶)冲突; 溢出策略(封顶? -EAGAIN?)未定。
- **证据**: "int tg_sem_init (tg_sem_t *s, unsigned initial);"(08 L89)。
- **建议**: 定义 max 与 give 到顶行为; 19-test 补用例。

### [P2] 同步对象无 destroy; 带等待者销毁 = UB 未声明
- **位置**: `08-core-api-list.md` §3 L88–101
- **问题**: 有 init 无 destroy; svc-posix 的 pthread_mutex_destroy 需要底层语义; 销毁时有等待者的行为(UB? -EBUSY?)未定义。
- **证据**: "int tg_mutex_init (tg_mutex_t *m); /* DEFINE 之外的动态路径 */"(08 L88)。
- **建议**: 声明"对象生命周期归分配者; destroy 为 no-op(无内核资源)或提供 destroy(带等待者 → -EBUSY)", 写入 §3 语义规格。

### [P2] cond"无虚假唤醒"契约只在测试文档
- **位置**: `19-test.md` L59 vs `08-core-api-list.md` §3 L99
- **问题**: TC-SYNC-006 期望"0(无虚假唤醒)"——这是强实现约束(preempt+PI 下实现代价高), 但 API 文档 08 §3 未声明; 契约位置颠倒。
- **证据**: "TC-SYNC-006 | cond signal → wait 返回 | 0(无虚假唤醒)"(19 L59)。
- **建议**: 把"无虚假唤醒"上移到 08 §3 语义规格(或改为"允许虚假唤醒, 相对超时按剩余时间计"并同步改用例)。

### [P2] `TG_TASK_F_*`/`TG_PAGE_F_*`/`TG_DMA_F_*` 值域未定义
- **位置**: `08-core-api-list.md` L40、L146、L153
- **问题**: 三组 append-only 标志的 v1 值集未列出; golden 收录 ENUM 需要初值, 空值域也应显式声明。
- **证据**: "uint32_t flags; /* TG_TASK_F_* (append-only, D14) */"(08 L40)。
- **建议**: §2/§6 列出 v1 已定义值(或声明"v1 无已定义值, 保留"), 进 golden。

### [P2] 队列/消息原语不在 API 面也不在"刻意不在"清单
- **位置**: `08-core-api-list.md` §0 L18 / §2–§5
- **问题**: 生产者-消费者在 RTOS 中通常有 queue/msgq 原语(FreeRTOS queue、Zephyr k_msgq 是核心面); 本设计只有 sem+work+自管环形缓冲, 但这一取舍未进 §0 的排除清单, 也无决策记录; svc-posix 未来 pipe/mqueue 的实现底座不明。
- **证据**: "**刻意不在本清单上**(防面膨胀, 同样是契约): fd/文件/挂载…、SMP 原语(v2: per-CPU/IPI)、async I/O(v2+)、per-plugin arena…"(08 L18, 无队列项)。
- **建议**: 在 §0 排除清单补"消息队列/事件原语(v1 用 sem+work 组合, 替代模式成文)", 或立项决策记录。

### [P2] 复位/电源原语缺失
- **位置**: `00-architecture.md` §8 L533–539; `08-core-api-list.md` §6–§9
- **问题**: 平台三层表只有 内存/中断/console/timer 四能力; native 面无 reboot/poweroff/watchdog-kick; panic"最后一口气"路径之后系统无受控复位手段——嵌入式产品的必备原语(14-app 的看门狗约定 [?] 也无处挂接)。
- **证据**: "| timer | `tg_clock` tickless 语义 | arch timer | 频率、校准 |"(00 §8 表, 无 reset 行)。
- **建议**: 00 §8 表加 reset 行(platform 提供 reset 实现), 经平台 ops 暴露(如 `tg_platform_reset()`); 与 14-app 的看门狗约定衔接。

### [P2] work 队列满时 ISR 侧处置未定义; 队列深度的声明主体不明
- **位置**: `08-core-api-list.md` §5 L123–126; `00-architecture.md` §6.4
- **问题**: -EAGAIN 返回给 ISR 后该做什么(丢弃+计数+trace? panic 策略?)未定; "队列深度 = manifest 静态声明"但归谁声明(全局? 调度插件资源项? 每插件?)未明。
- **证据**: "int tg_work_submit(void (*fn)(void *), void *arg); /* ISR-safe; 队列满 → -EAGAIN */"(08 L123); "队列深度 = manifest 静态声明(资源预算, §6.4)"(08 L126)。
- **建议**: 09 大纲第 4 项成文时定义 ISR 侧策略(建议: 溢出计数器 + trace 事件, debug 构建断言)与深度声明主体(建议: 调度插件的资源声明)。

### [P2] `tg_mem_free_contig`/`tg_page_free` 的 size 参数无防错机制
- **位置**: `08-core-api-list.md` §6 L139–147
- **问题**: contig/页池按 (ptr, size) 回收且无每块元数据, size 传错会静默破坏池; 03-debug 的红区/金丝雀只覆盖 TLSF 堆。
- **证据**: "void tg_mem_free_contig (void *ptr, size_t size);"(08 L140)。
- **建议**: debug 构建加 size 台账校验(分配记录、释放断言), 或在 11-memory 大纲第 4 项注明风险与防护手段。

### [P2] 运行时优先级调整原语缺失, 未记录决策
- **位置**: `08-core-api-list.md` §2 L39; `10-sched.md` §3 L32
- **问题**: prio 仅 create 时设置; preempt(v2) 下常见的动态提优先级(boost 类)无 API; 10 的开放问题只谈"优先级范围与反转策略", 未谈运行时调整。
- **证据**: "uint8_t prio; /* sched-preempt 使用; coop 忽略(§5.3) */"(08 L39)。
- **建议**: 记录"v1 静态优先级, 不提供运行时调整"决策; v2 需要时按新增面走决策记录流程。

### [P2] core footprint 目标与功能面的口径未定义
- **位置**: `00-architecture.md` §4 L168
- **问题**: "~4–16KB flash / 2–8KB RAM"与 48 API + TLSF + 三池 + 级联域 + plugin_manager + 注册表的面相比偏紧, 且未定义测量口径(是否含 ISA 库、调度插件、域支持), [?] 标注之外无验证计划。
- **证据**: "目标 footprint: ~4–16KB flash / 2–8KB RAM(不含堆与线程栈)[?]"(00 L168)。
- **建议**: 定义口径(如 libtgcore.a 指定段求和), M1/M2 实测后修订; 级联域/MMU 等按 feature flag 分别报告。

### [P2] 09 大纲遗漏: 中断嵌套/优先级策略
- **位置**: `09-int.md` §2 L18–23
- **问题**: 09 大纲无"中断嵌套与优先级策略"条目(GIC 下高优先级 IRQ 是否嵌套低优先级 ISR、`tg_irq_lock` 期间的行为、EL1 下 IRQ/FIQ 配置)——这是中断框架的基本设计项; 08 §8.1 的"嵌套域"开放问题也未进入 09 开放问题表。
- **证据**: 09 §2 大纲 1–6 项(向量与入口/IRQ 注册表/临界区/bottom half/fault/SMP)中无嵌套策略条目。
- **建议**: 大纲补"中断嵌套与优先级策略"条目, 至少声明 v1 立场(如"v1 不嵌套, 全部 IRQ 同级, irq_lock 即全局关断")。

---

## 统计

| 严重度 | 数量 |
|---|---|
| P0(矛盾/架构缺陷) | 2 |
| P1(完备性缺口/技术不清晰) | 17 |
| P2(改进建议) | 20 |
| **合计** | **39** |

**优先处理顺序建议**: 先解 P0-2(tg_sched_ops 定稿, 它阻塞 10-sched 成文与 TCB/锁对象机制)与 P0-1(§3–§9 每函数规格补齐, 它阻塞 M3 第一批冻结); 再按"M2 签名冻结(R4)前必须定案"的顺序处理 DMA attrs、内存屏障、irq_attr 编码三项; 骨架篇的 P1(12 相位/11 三池/10 coop 语义)在各自成文时消化即可。

## 核对无误的方面(供参考)

- 决策记录纪律(D/CA/SD + [?] 标注)在 08 内部一致; "48 函数 + 3 宏"分组计数(21/11/5/9/2)复核相符
- 五篇骨架全部兑现 README 承诺的"范围 + 大纲 + 继承决策"结构; 12/17 的分工声明与 README 分工一致
- 08 §11 ISR 白名单四件与 09 §1 继承一致; CA-1~10 的决策理由链完整
- 三层模式(平台能力)在 09/11 的引用方向正确(仅 09 L14 一处笔误)

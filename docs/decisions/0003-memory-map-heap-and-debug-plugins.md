# 0003 — 内存映射(4KB 恒等映射)+ core 堆 + 调试插件(trace/backtrace/dump/hexdump/memleak)

> 状态: 已落地(v0.1.0, QEMU virt aarch64)。设计出处: `3-os-core/3-04-memory.md`(三池 +
> region 表 + v1 恒等映射政策)、`3-os-core/3-01-core-api-list.md` §6/§7/§13.6、
> `5-debug/5-01-debug.md`(§1 trace / §2 bridge / §3 ramdump / §4 memleak)、
> `1-03-roadmap.md` §1(v1.0 插件清单: `service/trace`、`iface-min`)。
> 工具侧: `2-toolchain/brickie/brickie-v0.1.md` §3.4(特权分级)/§7.3(分类学禁则)/
> §8.1(manifest schema)/§9.1.1(写路径纪律)。

## 1. 背景

三件事一起做, 因为它们**互相咬合**: 调试插件要"倾倒现场", 现场就是 region 表 + 堆账;
堆要"从某块 RAM 长出来", 而那块 RAM 是 region 表划的池; region 表要落地就必须有页表
(v1 的政策是**恒等映射 + 属性隔离**, `3-04 §1`)。

此前原型的实际状态(见 `WORKAROUNDS.md`): **没有 MMU、没有 region 表、没有堆**
(`MainLoop` 不碰内存管理), 调试侧只有一个 core 内的 trace 环。

## 2. 决策

### 2.1 API 面的账: native 面零增量, 新增面全部**分类登记**(CA-5 的诚实义务)

`3-01 §1` 的 native 面是 48 函数 + 3 宏(CA-5: 目标 ≤50)。本次落地**严格复用**其中的
`br-mem`(11)与 `br-mm`(5), 函数名与签名与文档逐字一致:

| 组 | 落地 | 出处 |
|---|---|---|
| br-mem(11) | `br_malloc/calloc/realloc/free`、`br_mem_alloc_contig/free_contig`、`br_page_alloc/free`、`br_dma_alloc/free`、`br_heap_usage` | `3-01 §6` |
| br-mm(5) | `br_mm_region_add`、`br_mm_map`、`br_mm_unmap`、`br_mm_cache_flush`、`br_mm_cache_invalidate` | `3-01 §7` |

**新增的是三类, 都不是 native 面**, 因此不动 CA-5 的 48 预算, 但必须登记:

| 类别 | 函数 | 为什么不能藏在 core 内部 |
|---|---|---|
| 初始化(1) | `br_mem_init` | `3-04 §3` 明示"初始化点 = core.init"; 由 platform 在 region 声明之后调用 |
| 观测契约(6) | `br_heap_stats_get`、`br_heap_walk`、`br_heap_check`、`br_page_stats`、`br_mem_layout`、`br_mm_region_{count,get,find,count_kind}` | 消费方是 **debug 插件**(memleak/dump)⇒ 与 `3-02 §17.4` 处置 `br_irq_stats_get` 同型: 跨模块观测契约不能 hidden |
| 记账契约(4) | `br_heap_owner_register/name/get/set` | `5-01 §4` 的"per-plugin arena 记账"在 v1.x 的**可落地形态**(见 2.6) |
| 平台面(5) | `br_mm_ops_t`、`br_mm_register`、`br_mm_activate`、`br_mm_active`、`br_mm_set_attrs`、`br_mm_query` | `3-01 §10` 的"平台侧接口"行(同 `br_pic` ops 的手法): 接口形状在 core, 构造在 platform |

### 2.2 三池的**认领机制**在 core, 划分数据在 platform(单一真值)

`3-01 §6` 原文: "platform region 表把 RAM 划为 heap / contig 池 / 页池, 比例 = manifest 预算"。
原型的落点:

- `br_mm_region_t.attrs` 的**位 8..11 = region 种类**(`BR_MM_KIND_{IMAGE,STACK,HEAP,CONTIG,PAGE,DMA,MMIO,RESERVED}`)。
  设计只给了 `3-01 §13.6` 的取值域(`heap|contig|page|dma|mmio|reserved`), **没定义载体** ——
  这是原型新增的一个位域, 语义与设计取值域一一对应(append-only, 位 4..7 保留)。
- `br_mem_init()` 按种类认领: HEAP/CONTIG/PAGE **必需**, DMA 可选(缺失 ⇒ `br_dma_alloc/-free`
  返回 `-ENODEV`, 不静默退化); 每个池的 base/size 必须 4 KiB 对齐与页粒度, 否则 `-EINVAL`。
- region 表的**重叠禁则**(`-EEXIST`)带来一个真实约束: 镜像尾与栈底只差十几字节, 拆成两条
  会因页对齐而重叠 ⇒ platform 把它声明为**一条** `IMAGE` 覆盖"镜像 + 启动栈", 池基址从
  `ALIGN_UP(&__stack_top, 2 MiB)` 起算。

⚠ **欠债(登记为 `br-wa-mem-001`)**: 比例(1 MiB / 256 KiB / 1 MiB / 256 KiB / 16 KiB)目前
**写死在 platform 的 region 表里**, 未经 manifest 的 `[budget]`/`[[res]]` 生成 —— v0.1 没有
"manifest → core 生成物"的链路。还债动作 = 组合器把预算生成 region 表。

### 2.3 TLSF 的落地参数与"红区/毒化"的便宜层

- **TLSF**(`CA-8` 的堆池实现): 一级 32 类 + 二级每类 32 细分, 两级位图定位最小可用类;
  块头含 `size|free` 与 `prev_phys` 大小; 分裂 + 立即合并; 空闲块双向链。宿主用例直接压测
  TLSF 本体(见 2.7)。
- **`BR_MALLOC_ALIGN = 16`**(设计未给值, 此处裁定): AAPCS64 要求 SP 16 对齐, 结构体自然
  对齐上界是 16; 取 4/8 会做出"malloc 出来的 buffer 放不下 `aligned(16)` 对象"的坑。
- **用户块布局**(包装层, `mem.c`): `[块头 32B: magic|size|block|owner|caller|seq|state|next]`
  `[用户区 n]` `[红区 0xA5(补到 8 字节)]` `[尾部 8B: magic2|size 副本]`。
  - 红区越界 ⇒ `br_heap_check()` 检出(`redzone_hits`);
  - 头部魔数/尾部副本 ⇒ `canary_hits`;
  - 双重释放 ⇒ 检出并**直接返回**(不二次入链, 堆保持完好); 释放非本堆指针 ⇒ `bad_free++`。
  这三件正是 `5-01 §4` 表里 "target 便宜替代(TLSF 红区 + freelist 毒化)" 的落地。
  **未做**: 栈 canary(需要编译器插桩, 归 v1.x 后续)。
- **页池**(`CA-8`): 静态位图 + 连续 run 首次适配; 位图按 `BR_PAGE_MAX_PAGES = 4096`
  静态预置(**16 MiB** 池上界, 位图 512 B; 本板页池 1 MiB = 256 页, 余量充足);
  `BR_PAGE_F_ZERO` 支持; 重复释放被忽略并计数。
- **contig 池**(`CA-7`): 有序空闲链 + 首次适配 + 对齐空洞切块还链 + 释放时前后合并。
  DMA 建于其上(`CA-6`): `dma_addr == vaddr`(**INV-6**)。

### 2.4 4 KiB 恒等映射的页表构造(platform 侧)

| 项 | 裁定 | 理由 |
|---|---|---|
| 粒度/级数 | 4 KiB(TG0=0b00)、4 级、仅 TTBR0(`EPD1=1`)、T0SZ=25(39 位 VA)、IPS=40 位 | 4 KiB 是 v1 的页粒度(`BR_PAGE_SIZE`); 39 位 VA 足够 |
| 低 1 GiB | 整段 512 条 **2 MiB Device-nGnRnE 块** | QEMU virt 的外设都在低 1 GiB, 且 GICR 的 8 帧一直排到 `0x081A0000` —— 只映射 GICD 附近 1 MiB 会漏掉 GICR 第 4 帧之后的访问(实测过的行为差异) |
| RAM 窗口 | `0x40000000 + 8 MiB`, L1[1] → L2 → **4 张 L3 表逐页 4 KiB 映射** | 三池 + 页表 + 镜像都在窗口内; 逐页映射使 `set_attrs`(RO/NX)可按页生效 —— 这正是 v2 重定位要用的通路 |
| 窗口之外 | **invalid**(不映射) | 给出一个"确定可取翻译 fault"的地址空间: 用例用它造可恢复 fault(见 2.5) |
| MAIR | Attr0 = 0xFF(Normal WB)、Attr1 = 0x00(Device-nGnRnE) | RAM 可缓存 / 设备严格序 |
| SCTLR | 读改写, 置 `M|C|I`, 显式**清** `SA`/`SA0`, RES1 位置 1 | 清 SA 使 Normal 内存上的非对齐访问不再取对齐 fault(设计口径"对齐检查关") |
| 页表内存 | 7 张静态 4 KiB 表(`.bss`, 28 KiB) | 静态、无运行期分配; 与"捕获/启动路径不分配"一致 |

`br_mm_activate()` 的状态机(未注册 → 已注册 → 已激活)归 core, 页表构造归 platform 的
`br_mm_ops.activate` —— 这是"三层模式"在状态归属上的落点。`br_mm_query()` **只报页表里
真实生效的属性**, region 表回答"这块是什么"; 两处不互相冒充(单一真值)。

### 2.5 v1 的 `br_mm_map` 是 `-ENOTSUP`, 而**属性改写**是真的

- `br_mm_map`/`br_mm_unmap` 在 v1 返回 `-ENOTSUP` —— 这不是偷懒, 是设计写明的期望:
  `TC-MM-003` 的期望列就是 "`-ENOTSUP`"(签名先行, 实现归 v2 重定位; 主文档风险 R4)。
- 运行期**改属性**走 `br_mm_set_attrs` → `ops.set_attrs`(逐页改写描述符的 AP/UXN 位)。
  一致性用例用它做**真保护验证**: 把保留区一页设成 RO, 再用 extable 写探针去写 ⇒ 取
  permission fault 并被修复成 `-EFAULT`; 改回 RW 后正常可写。NX 只做**描述符读回**验证
  (真的跳过去执行会走 instruction abort, 而 `3-02 §10.3` 的 extable 只覆盖 data abort)——
  这条边界写在用例注释里, 不假装测过。
- **可恢复 fault 的构造换锚点**: MMU 关着时靠"非对齐访问取 Alignment fault"(`P-IRQ-16`),
  MMU 开着 + Normal 属性 + SA=0 之后非对齐访问**不再 fault**。于是 `irq_conf.c` 的 extable
  用例改为读**窗口外未映射地址**(`0x42000000`)取 translation fault。原注释已预告过这次
  切换("将来开了 MMU…这一条随 MMU 工作一起改")。

### 2.6 调试插件: 5 个 service 插件 + 一个编排者

| 插件 | 形态 | 职责 | 依赖 |
|---|---|---|---|
| `service/trace` | ability/service | core 16B 事件环的**唯一消费方**(drain/解码/计数/呈现)+ 动态事件名 + marker | core |
| `service/backtrace` | ability/service | x29 帧链走查(栈边界/深度/单调三道护栏)+ 现场快照; **不做符号化** | core |
| `service/hexdump` | ability/service | 16 字节/行的地址+hex+ASCII 原语(缓冲与 console 两条路径; snprintf 式截断语义) | core |
| `service/memleak` | ability/service | 按**归属标签**出泄漏账 + 红区/魔数/毒化违约计数 | core |
| `service/dump` | ability/service | 编排现场(regions/heap/leaks/trace/backtrace)+ **调试域 conformance 入口** | 上面四件 |

- **环与 ISR 发射点留在 core**(`br_trace_emit`): CA-3 的 ISR 白名单要求中断/fault 路径
  不依赖"服务是否已 init"; 而 `5-01 §1` 要的"trace 是可整层移除的插件"由**消费侧**满足 ——
  插件缺席时环照旧记录, 只是没人取走。这条分工写进了 `br_trace_svc.h` 的文件头。
- **backtrace 只捕获不符号化**: `5-01 §3` 把"栈回溯分析"划给 host 侧(离线); 原型因此不生成
  也不携带符号表(那要两遍链接, 归 v1.x)。**编译期前提** = `-fno-omit-frame-pointer`
  (已进 CFLAGS; 这是本次唯一的全局编译选项改动, 理由是"崩溃时能走栈"值那几条 stp/ldp)。
- **hexdump 的格式是契约**(用例逐字节断言): 每行 87 字节, 详见头文件里的样例串。
  它不依赖 `br_log` 的格式器, 自己渲染 —— 否则"格式契约"会随 log 的实现漂移。
- **dump 的边界判定 = region 表**: 未声明地址 ⇒ `-EINVAL`(除非 `BR_DUMP_F_FORCE`,
  那是 P4/machine 语义, 留给 panic 路径)。`BR_DUMP_MAX_BYTES` 进 `3-01 §13.6` 的 ops 三轴
  ⇒ **发现一个分级模型缺口**: 该轴只有 `alloc/free/map/protect/flush/invalidate/unmap`,
  **没有"读/inspect"**, 所以 `service/dump` 只能声明 P0。回灌项(见 §4)。
- **调用链与分类学**: 按 `brickie-v0.1 §7.3`, app 只许依赖 interface。原型里 app 直调
  `service/dump` 与 `platform/qemu-aarch64`, 两条都在 `product.toml [lint].allow_edges` 里
  **逐条列名豁免** —— 这是 `1-03 §1` 给的 **M0 引导例外**(iface-min 属 M2, 运行期插件管理器
  属 M0), 属 `WORKAROUND(br-wa-boot-001)` 的欠债, 不是静默放行。
- **`service/*` 之间是合法边**(ability → ability): dump → {trace, backtrace, hexdump, memleak}
  四条 `[[dep]]` 都是真的调用边, `brickie check` 的 deps 域零错误。

### 2.7 验证: 宿主侧跑算法性质, 目标侧跑真地址空间

`make mem-test`(新增, **不需要交叉工具链/QEMU**)把 `tlsf.c/mem.c/page.c/mm.c` 编成宿主
可执行, 跑 20 万次随机交错分配/释放/校验 + 参考模型对拍。理由: TLSF 的合并/分裂/碎片、
页位图的 run 分配、region 重叠判定都是**算法性质**, 在宿主上几秒钟能跑上百万次操作; 同一批
性质在 QEMU 上只能靠几条用例撞运气。这就是 `1-03` 的"host 平台插件: CI 秒级 + ASan 白捡"的
精神(完整 ASan 归 host 平台插件, 本原型先用 `-O2` + 断言 + 对拍)。

`make dbg-test`(target)则要求 `[MEMCONF]` 与 `[DBGCONF]` 两个摘要 `fail=0`, 且**逐个用例
tag 一个不缺** —— 与 `irq-test` 同一纪律: 被裁掉的用例也算红。

### 2.8 归属记账: v1.x 便宜层 + 一个"标签"近似, 不假装是 v2 arena

`5-01 §4` 的 memleak 是 **v2.0** 的 "per-plugin arena 记账"。本次给的是:

- **有的**: 分配时把"当前归属标签"记进块头(`br_heap_owner_set/get`)+ 分配点返回地址
  (`__builtin_return_address(0)`, 供 host 侧 `addr2line` 离线解码)+ 分配序号;
  memleak 按标签归并出账。
- **没有的**: 预算上限、强制归属、OOM 策略 —— 所以它回答"谁没还、在哪分配的",
  **不能**回答"某插件超预算"。这条差异登记为 `br-wa-debug-001`。

### 2.9 呈现通道: 直写 console, 不经 bridge(欠债)

`5-01 §2` 的 debug bridge(COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面, M3)尚未存在,
所以 dump/trace 的输出**直写早期 console**, 并自觉遵守 bridge 的同类约束(只用静态缓冲、
不碰堆、只读)。登记为 `br-wa-debug-002`。

## 3. 被否决的替代方案

| 方案 | 为什么否 |
|---|---|
| 把 trace 环搬进 `service/trace` 插件 | CA-3 的 ISR 白名单要求中断/fault 路径可留痕且不依赖服务 init; 环在 core、消费在插件是唯一不破白名单的分法 |
| 镜像里嵌符号表(backtrace 自己解符号) | 需要两遍链接(先生成 ELF → 抽符号 → 再链接); `5-01 §3` 已把符号化划给 host 离线侧 ⇒ 收益不抵复杂度 |
| 用 2 MiB 块映射整个 RAM(省页表) | `set_attrs` 无法按页生效 ⇒ RO/NX 保护与 v2 重定位的通路就没了; 8 MiB 窗口用 4 KiB 页只要 28 KiB 页表 |
| 让 `br_mm_map` 在 v1 就先"能用" | 违背 `TC-MM-003` 的期望与 `3-01 §7` 的"签名先行"; 且没有重定位需求时它只是多余的运行期入口 |
| `br_page_alloc` 从 TLSF 堆切页 | 违背 `CA-7`/`CA-8` 的三池划分(页表/页池共用位图, 与堆碎片解耦) |
| 页表构造放 core(用 `.S`/内联汇编) | 违背三层模式: 接口在 core、构造在 ISA 层/platform; core 里不该出现 aarch64 页表描述符编码 |
| 给 app 加 iface-min 皮肤来消掉 allow_edges | 方向对(设计的 M2 就是这条), 但它是一次**接口层**的扩面, 不属本次三件事; 记入留待项 |
| dump 用 `br_log_info` 拼 hex 行 | 格式契约会随 log 的实现漂移; hexdump 自己渲染才可逐字节断言 |

## 4. 后果与留待项

- **留待(设计回灌)**: ① `3-01 §13.6` 的 ops 轴缺"读/inspect"(dump 的内存读只能声明 P0);
  ② region 种类位(`attrs[11:8]`)是原型新增, 设计侧"region 表"一节需承认载体;
  ③ 调试域的用例组 `TC-DBG-*`(6-01 只有 TASK/SYNC/TIME/WORK/MEM/MM/IRQ/SVC/HSM 九组)
  需并入 `6-01-test.md`; ④ `br_heap_stats_get/br_heap_walk/br_heap_check/owner_*` 等观测与
  记账函数应登记进 `3-02 §17.4` 那张"跨模块观测契约"表(与 `br_irq_stats_get` 同列)。
- **未实现(诚实)**: 重定位(v2)、MPU 目标(vx)、栈 canary、memleak 的 per-plugin arena
  预算(v2)、`br_fault_handler_register`(v2)、debug bridge(M3)、SLOW 级联域(Stage 2)。
- **已知的观测面缺口(各工作流提报, 未擅自扩冻结契约)**:
  ① `br_heap_stats_t` 没有 contig 侧错误计数(`bad_free` / `size_mismatch` 只有 core 内部
  计数 + 日志)—— 观测不到; ② `br_mem_layout_t` 没有"DMA 池是否就绪"的显式标志
  (靠 `dma.size > 0` 隐含, 因为 `size == 0` 已被 `-EINVAL` 挡住, 语义可判但不直白);
  ③ TLSF 的单实例接口是 **core 私有**(`core/src/mem/tlsf_internal.h`), 不对外
  —— 宿主用例直接 include 它, 别的插件不许用; ④ `br_heap_walk` 的回调内**不得**
  alloc/free(实现用"原地反转存活链"换 O(n) 升序, 零额外内存)。
- **已知的覆盖缺口(用例没走到, 不是实现没有)**: ① contig 的"前导空洞切块"分支
  (宿主池基址 4 KiB 对齐 + `align=4096` ⇒ 空洞恒为 0); ② `br_malloc` 对
  `n > 0xFFFFFFF0` 的拒绝(块头 `size` 是 u32 的布局约束); ③ DMA 池缺失时的
  `-ENODEV` 分支(宿主用例只覆盖了 DMA 池存在的情形); ④ 页池上界 16 MiB 与
  本板 1 MiB 池之间的余量没有用例(只需 `br_mem_init` 层面的上界检查)。
- **留待(设计回灌)**: dump 的 `BR_DUMP_F_FORCE` 在 v1 **不真读**未声明地址
  (需要 extable 保护的 probe —— 即 `5-01 §2` `MEMRD` 的完整形态);
  `service/memleak` 未注册 trace 事件名(它没有 `service/trace` 的 `[[dep]]`,
  不造未声明的调用边)。
- **欠债登记**: `br-wa-mem-001`(池比例未经 manifest)、`br-wa-debug-001`(标签 ≠ arena)、
  `br-wa-debug-002`(console 直写, 未经 bridge); 另有 `br-wa-entry-001`/`br-wa-isa-001`/
  `br-wa-boot-001` 三条的**范围扩大**(新增文件与调用点)。

## 5. 目标侧门禁抓到的真问题

与 ADR-0002 §5 同一纪律: 本节只记"**只有真跑才会暴露**"的问题 —— 它们全部通过了
`-Werror` 交叉编译、通过了宿主侧 81 项语义用例。结论先写在前面: **"编得过 + 宿主全绿"
是两层虚假信心** —— 宿主用例链的是 libc, 而 target 链的是内核自己的实现。

### 5.1 freestanding 的 libcall 义务(链接期才暴露)

`br_mem_layout()` 里一句结构体赋值被 GCC 降成 `bl memcpy`, 而 `-nostdlib` 没有 memcpy
⇒ 链接直接红:`undefined reference to memcpy` / `relocation truncated to fit: R_AARCH64_CALL26`。
`-ffreestanding -fno-builtin` 关掉的是**内建识别**, 不是**中端 lowering** —— 这条区别只有
链接器会告诉你。
**修**: 新增 `core/src/string.c`(memcpy/memmove/memset/memcmp 四件 + 私有头
`string_internal.h`), 定位是"编译器支持例程"而非 libc(不是 native API, 不进 48 函数面;
将来 `svc-posix` 交付 POSIX 符号面时应删掉本文件以归一符号)。

### 5.2 ★ `memcpy` 自递归(本 ADR 最有价值的一条)

`string.c` 第一版用 `__builtin_memcpy(&w, s + i, sizeof w)` 做 8 字节搬运, 预期它内联成
`ldr/str`; 实际在本文件的编译组合下(`-ffreestanding -fno-builtin` **加上**函数上的
`optimize("no-tree-loop-distribute-patterns")` 属性), GCC 16 把它降成**对 `memcpy` 自己的
libcall**:

```
4008200c: bl 40081f80 <memcpy>     ; __builtin_memcpy(&w, s+i, 8)
4008201c: bl 40081f80 <memcpy>     ; __builtin_memcpy(d+i, &w, 8)
```

于是只要 `n >= 8` 且两个指针 8 字节对齐就**无限递归**。症状与真因隔了三层, 所以值得完整记下:

| 层次 | 现象 | 为什么具有误导性 |
|---|---|---|
| 表面 | 控制台打完 pool 摘要后**再无输出**, QEMU 挂住(rc=124) | 看起来像"MMU 开箱即死", 第一嫌疑是页表 |
| 第二层 | gdb 挂上后 `PC = br_plat_vectors+512`(同步异常向量), `SP = X29 = 0x400a1040` —— 已落进 `s_l2_dev`/`s_l1` 页表 | 像"页表把栈覆盖了"或"栈溢出进 .bss"; 而"为什么栈会掉下去"才是根因 |
| 第三层 | `SP` 比 `__stack_bottom` 低 0x2AA0; `-d in_asm,int` 显示异常前最后翻译的块是 `br_mm_activate → br_plat_mmu_init → br_plat_mmu_info → br_mm_region_count → br_plat_mmu_init → br_mem_layout → memcpy`, `ESR=0x96000045`(EC=0x25, DFSC=5 level-1 translation fault), `ELR=memcpy+0` | 到此才确认: 是 `memcpy` 无限递归压穿栈, 不是页表算错 |
| 第四层 | 谁在调 `memcpy`? → `br_mem_layout()` 的结构体赋值(合法 C!) | 页表构造器一行没写错也照样挂 |

**修**: 8 字节搬运改成**显式宽访问** —— `typedef br_u64 br_word_u __attribute__((aligned(1), may_alias));`
后直接解引用(对齐已由 `is_word_aligned()` 保证, `may_alias` 避开严格别名 UB, 二者都不产生调用)。
**并且钉成门禁 `make check-string`**: 四个例程内部**不得有对自身的 `bl`**
(判据挂在 `all` 的依赖上, 以后 `make` 会直接红)。这类错误"编得过、宿主也跑得对",
**只有反汇编看得见** —— 所以它必须是机械判据, 不能靠人记得。

### 5.3 panic 打印再 fault ⇒ 无界递归(同一事故的第二层伤害)

排查中发现: panic 的输出走轮询 console; 一旦 console 也不可用,
"panic 打印 → 再 fault → 又走 panic → 再 fault"会**无界递归**, 每次压 0x140 B 异常帧 ⇒
第三次吃穿 64 KiB 启动栈(这次落进的是页表), 结果是**原始 fault 现场彻底丢失** ——
这正好解释了 5.2 里"为什么第一层现场看起来完全不像 memcpy 的问题"。
**修**: `core/src/panic_internal.h`(core 私有)提供 `br_panic_in_progress()` /
`br_panic_halt()`; `panic.c` 在打印**之前**置重入标志; `irq_fault.c` 在 `in_fault++`
之后、任何留痕/打印/handler **之前**判定 —— panic 输出期间再 fault ⇒ **静默停机**
(不打印、不取锁、不碰任何可能已损坏的设施)。行为退化为
"第一句 panic 打得出就打, 打不出就安静停住": 诊断价值最大化, 破坏最小化。

### 5.4 最终门禁结果(真构建: `make clean-brickos` 后从零)

| 门禁 | 结果 |
|---|---|
| `make brickie-check` / `--profile release` | 0 错误 / 0 警告 / 0 提示(dev 与 release 两档都过) |
| `make mem-test`(宿主) | `[HOSTTEST] SUMMARY pass=81 fail=0 total=81`(20 万次随机操作, 不变量恒 0) |
| `make string-test`(宿主, **本次新增**) | `[STRINGTEST] SUMMARY pass=6 fail=0 total=6` |
| `make`(交叉, `-Werror` 全开) | 零警告; `check-string` 通过; `build/brick.elf` = 80 KiB text / 53 B data / 59 KiB bss |
| `make irq-test` | `[IRQCONF] SUMMARY pass=67 fail=0 total=67` |
| `make dbg-test` | `[MEMCONF] SUMMARY pass=35 fail=0 total=35`; `[DBGCONF] SUMMARY pass=9 fail=0`(编排者断言)+ 各插件自报(memleak 12 等); 必需的 17+15 个 tag 一个不缺 |
| `make smoke` | PASS, 568 行日志(含三套摘要与启动现场) |
| `make tools-test` | 通过 542 / 失败 0 |
| `make check-build` / `check-workarounds`(7 标记 ↔ 7 登记)/ `tools-prebuilt-check` | PASS |

现场证据(摘): `mem: identity map on (SCTLR=0x30d51825, regions=9, heap=1024 KiB, page pool=256 pages)`;
`[TRACE] total=461 drained=264 overrun=8 ids=3 live=197`;`[LEAK] summary live=1 bytes=123 owners=1 corrupt=0`
(memleak 用例**故意**泄漏那一块的现场, 它随后自己释放);`[BT] frames=4`;`dbg: steady-state trace drained=40`
(第 2 拍才取走 —— 里面是 timer PPI 在**中断上下文**落下的事件, 即"trace 在 ISR 里可用"的活证据)。

`make string-test` 是本次新增的**宿主语义**用例, 验证的正是 5.2 的四个例程(语义 + 越界哨兵 +
参考实现对拍)。它与 `check-string` 分工互补: 一条看**反汇编**(有没有自递归), 一条看**行为**
(搬对了没有、越界了没有)。加它的理由很直接: **自递归在宿主上会立刻爆栈**, 所以这条用例
本身就是那个 bug 的回归判据。

### 5.5 声明面与实现不一致的两处(由复核/用例抓出, 已修)

① `br_hexdump.h` 的样例地址字段(`0000000000400800`)与它标注的基址(0x40080000)不符;
② `br_bt.h`/`br_hexdump.h` 的 `*_init` 注释声称"注册动态 trace 事件名", 而实现(正确地)
不跨插件建未声明的结构依赖边;③ `br_bt.h` 里 `br_bt_init`("复位栈范围")与
`br_bt_set_stack_bounds`("platform 在 early_init 调用")在时序上互斥。
三处都按**实现**修正了声明面, 并把"为什么本原型没有合法调用方"写进了注释
(platform → ability 是 §7.3 的非法边)。

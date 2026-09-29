# Debug + Test 模块评审意见(03-debug / 19-test)

> 评审人: subagent(debug+test 模块, 证据经父 agent 抽查核实)
> 评审对象: `tangramOS/docs/4-debug/03-debug.md`、`tangramOS/docs/5-test/19-test.md`
> 评审维度: 完备性(矛盾、技术点是否清晰、架构是否存在问题)
> 意见分档: P0 = 矛盾/架构缺陷(必须解决); P1 = 完备性缺口/技术不清晰(应当解决); P2 = 改进建议

## 模块总评

两篇文档的自我认知是诚实的(19-test 明标"运行基建…为骨架", 03-debug 的 ASan 分层自称"诚实工程"), 组织质量在同类设计中属上乘: 用例目录以六条不变量统摄、五元组与"机械判定"原则明确, debug 侧的"panic 通道独立""host 火力全开/target 只做便宜而确定的"分层策略方向正确, 且"用例先行"流程有实绩(19 §1 记录了 -ETIMEDOUT 契约缺口的发现与回填)。但精读发现 **5 处硬矛盾**: trace 的 Service 插件形态与其承担的 core 内建探针/ISR 白名单职责在初始化时序与依赖方向上不成立; "定长 16B 记录"与实际 20B 结构体冲突; TC-MM-003 期望的 -ENOSYS 违反自家 R-4 错误码子集; "host 完整 ASan 白捡"与 host 上跑真实 TLSF 的 TC-MEM 组互斥; 红区/金丝雀版本归属在 03/主文档(v1.x)与路线图(v2.0)间分裂。**最大风险**: debug 体系被定位为"隔离的替代品", 但其地基——trace 形态、panic 原语、host 执行模型——恰好是矛盾与未定义最集中的地方; 验证体系自身目前尚未通过它为别人设定的"机械可判定"标准。

---

## [P0] trace 的"Service 插件"形态与其 core 内建探针/白名单职责在架构上矛盾
- **位置**: `4-debug/03-debug.md` §1 L8/L24; `1-architecture/00-architecture.md` §3 L116、§9 L557–563、§7.2 L445; `2-os-core/08-core-api-list.md` §11 L284; `02-roadmap.md` L64
- **问题**: trace 声明为 Service 插件(总图 L3A 层), 但其"内建探针"的发射点是 core(int 模块)与 EARLY 相注册的调度插件, 且 08 §11 把 trace 宏列入 core 的 ISR 白名单契约。三个后果均无设计: (a) "插件生命周期"探针在 EARLY/CORE/LATE 期间发射, 早于/交错于 trace 自身作为 Service 的 LATE 相 init(启动序列: EARLY→CORE→LATE"Service → Interface 依序 init")——除非环是零初始化即可用的纯 BSS 结构(未声明), init 前事件必丢; (b) L1/L2 调用 L3A 的实现, 与依赖宪法"依赖箭头只许从'靠近 APP'指向'靠近 core'"方向相反; 路线图中 trace 的依赖边是 trace→core, 掩盖了实际存在的 core→trace 调用; (c) trace 的 init() 究竟做什么、"编译期可整层移除"时 core/调度器探针的编译出路(配置宏? 弱符号?)未定义。
- **证据**: "形态: Service 插件, `SAFE_PREEMPT`, **ISR 可用**"; "内建探针: 调度切换…IRQ 进出、插件生命周期、work 提交/执行"; 00 §3 "lwIP · crypto · trace"(L3A Service); 00 §9 ":LATE Service → Interface 依序 init"; 08 §11 "`TG_TRACE_EVT` 宏(`docs/4-debug/03-debug.md` §1)"列入白名单; 00 §7.2 "单向流: 依赖箭头只许从'靠近 APP'指向'靠近 core'"。
- **建议**: 二选一并写明机制: (1) 环缓冲与写入路径下沉为 core 设施(零初始化 BSS 环、core 拥有), trace Service 只负责泄放与解码元数据导出——与 00 §6.5"注册表中介"解法一致; (2) 保留 Service 形态但声明特例: EARLY 相 init、纯静态零初始化环、core 经编译期配置宏绑定, 并在 03 §1 写清"init 前事件不丢"的成立条件与整层移除的编译路径。

## [P0] "定长 16B 记录"与结构体实际 20 字节矛盾
- **位置**: `4-debug/03-debug.md` §1 L10 vs L13–19; `1-architecture/00-architecture.md` §11 L621; `02-roadmap.md` L64
- **问题**: 记录字段求和 4(tsc)+2(evt)+1(ctx)+1(narg)+12(arg[3]) = **20 字节**(packed 与否均为 20), 与三处文档的"16B"声明直接矛盾。环容量、manifest 预算("4–16KB 级")、解码器与 MPSC 原子性论证都会按错误数字设计。
- **证据**: "**定长 16B 记录**(环无碎片、解码简单)"; 结构体 "uint32_t arg[3]"; 00 §11 "定长 16B 事件环形缓冲"; 02-roadmap "16B 事件环形缓冲"。
- **建议**: 定稿其一: 若 20B 是真意, 全局改"20B"并重算预算(4–16KB = 204–819 条); 若 16B 是硬约束(对齐/缓存行友好), 砍为 arg[2] 或将 evt/ctx/narg 压进 4B 头。同时补一句记录的对齐要求(影响写路径原子性, 见环策略条)。

## [P0] TC-MM-003 期望 -ENOSYS 违反 R-4/SD-10 错误码子集, 且 SD-10 本身存在两张不一致的表
- **位置**: `5-test/19-test.md` §3.6 L99 vs §1 L16; `2-os-core/08-core-api-list.md` §11 L289; `6-vfs-device/06-device.md` §4 L186
- **问题**: R-4 规定"错误码 ∈ SD-10 负 errno 子集(扫描验证)", 但 TC-MM-003 期望 -ENOSYS, 而 08 §11 的子集(-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT)与 06-device §4 的 SD-10 子集(-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS)**都不含 -ENOSYS**。附带发现: 两处自称"SD-10 统一/单一错误空间"的子集互不一致(08 有 EAGAIN/ENOMEM/EEXIST/ETIMEDOUT 而无 ENOSPC/EROFS, 06 相反), R-4 的扫描基准到底是哪张表是歧义的。
- **证据**: "TC-MM-003 | map/unmap(v1) | `-ENOSYS`(签名在, 实现未到——R4 冻结验证)"; "R-4 | 错误码 ∈ SD-10 负 errno 子集(扫描验证)"; 08 L289 "负 errno 子集 `-EIO/-EAGAIN/…/-ETIMEDOUT`"; 06 L186 "负 errno 子集: `-EIO/-ENODEV/-ENOSPC/…/-EROFS`"。
- **建议**: TC-MM-003 改期望 -ENOTSUP(与 D22"NULL → -ENOTSUP"的全系统惯例一致); 同时把 SD-10 收敛为一张权威表(或明确 core 域子集与设备域子集的包含关系), 让 R-4 扫描器有唯一基准——这恰是 19 §1 自夸的"用例设计即契约评审"流程应当抓出的那类缺陷。

## [P0] "host 完整 ASan 白捡"与 host 上跑真实 TLSF 的 TC-MEM 组互斥
- **位置**: `4-debug/03-debug.md` §4 L48; `1-architecture/00-architecture.md` §11 L624; `5-test/19-test.md` §2 L26 + §3.5 L85–91; `2-os-core/08-core-api-list.md` §6 L133–136
- **问题**: ASan 的堆红区/use-after-free 检测只覆盖经其拦截分配器(宿主 malloc)分出的内存。19 把 TC-MEM 全组(含 TC-MEM-002 "TLSF 有界碎片")排在 host, 意味着 host 上跑的是真实 TLSF(否则该组对 conformance 无意义); 那么 tg_malloc 的内存来自 TLSF 池, ASan 完全看不见——"完整 ASan 白捡"对系统最主要的动态内存不成立(栈/全局仍受益)。反之若 host 把 tg_malloc 映射为宿主 malloc, ASan 成立但 TC-MEM-001/002/007 测的是 glibc 而非 TLSF, 矩阵空转。host 分配器映射本就是未完成设计(02-roadmap §5 DoD 第 6 项), 但两篇文档当前各自声称的收益不能同时成立。
- **证据**: "host 平台插件跑 Linux 进程, `-fsanitize=address` **直接可用, 白捡**"; 00 §11 "**host 完整 ASan 白捡**"; 19 §2 "全部语义用例(除 ISR/真时序类) | CI 秒级 + 完整 ASan 白捡(02)"; TC-MEM-002 "碎片压力: 交错 alloc/free 后最大块仍可分配 | TLSF 有界碎片 | ALL | host"。
- **建议**: 在 03 §4 与 19 §2 明确 host 分配器策略并量化 ASan 边界, 三选一: (1) TLSF 内建 ASan 毒化钩子(`__asan_poison_memory_region` 于红区/free 块)——这是设计工作, 应删除"白捡"措辞; (2) host 双档: conformance 档跑真 TLSF(无堆 ASan), ASan 档 tg_malloc→宿主 malloc(TC-MEM 组标注仅 conformance 档); (3) 承认边界并成文: "host ASan 覆盖栈/全局与宿主分配内存, 不覆盖 TLSF 池内破坏", 以 target 红区作补偿。

## [P0] TLSF 红区/金丝雀版本归属分裂(v1.x vs v2.0), bridge 版本表述同样不齐
- **位置**: `4-debug/03-debug.md` §4 L49; `1-architecture/00-architecture.md` §11 L624 vs `02-roadmap.md` §1 v2.0 L81; 附带 `03-debug.md` L27 / `00-architecture.md` L622 vs `02-roadmap.md` L65/L172
- **问题**: 红区+毒化+栈 canary 在 03 与主文档 §11 标 **v1.x**, 在路线图 v2.0 debug 行("mini ramdump + target 侧 memleak(arena 记账) + 红区/金丝雀")标 **v2.0**。这直接影响"v1 量产候选镜像有没有廉价的堆/栈破坏检测"——对一个"无隔离、debug 即隔离替代品"的系统是必须钉死的归属。同批问题: bridge 标"v1.x 起最小集"而路线图把 service/dbg-bridge 排在 M3(=v1.0 完整化), 表述不齐。
- **证据**: "TLSF 红区(malloc header/footer guard)+ freelist 毒化 + 栈 canary | v1.x"; 00 §11 "target: TLSF 红区+金丝雀(v1.x)"; 02-roadmap v2.0 "debug | **mini ramdump** + target 侧 memleak(arena 记账) + 红区/金丝雀"; 03 L27 "debug bridge(类 adb, v1.x 起最小集)"。
- **建议**: 以 03/00 的 v1.x 为准修订路线图 v2.0 debug 行, 或反向收敛; bridge 改"v1.0(M3)最小集"。建议在主文档 §11 维护唯一"能力 × 版本"权威表, 其余文档只引用。

## [P1] panic/断言原语未定义, v1.0 硬件 fault 行为悬空
- **位置**: `5-test/19-test.md` §2 L30; `4-debug/03-debug.md` §2 L33、§3 L37–38; `2-os-core/09-int.md` §2 L22; `08-core-api-list.md` §8 L192
- **问题**: 19 的失败上报模型("断言失败 → panic → bridge 独立通道上报")与 03 的"显式 panic"都依赖一个 panic/断言原语, 但全文档 grep 无 tg_panic/TG_ASSERT 定义(08 的 48 函数清单也不含), 其语义(关中断? 现场快照? 进入最后一口气循环? 可重入?)无处规定。同时 fault 路径整体标 v2(09-int "fault 路径(v2)"; 08 "/* v2: tg_fault_handler_register() */"), 意味着 v1.0 期间野指针数据中止没有定义行为——03 §3"默认 handler 只打印最小信息"被归在 v2.0 小节内, 对 v1 不适用; 而此时 conformance 恰恰开始在 QEMU target 上跑。
- **证据**: "断言失败 → panic → **bridge 独立通道上报**"; "触发: aarch64 同步异常…+ 显式 panic"; 09-int "5. fault 路径(v2): tg_fault_handler_register(同步异常钩子)"。
- **建议**: 在 03 增设"panic 原语"小节: TG_ASSERT/TG_PANIC 的 API 形态与进入路径(关中断→现场快照→panic 通道)、v1 最小 fault handler(打印 ESR/FAR/LR + 倒 trace 环, 不必等 v2 ramdump); 并明确 tg_panic 是否入 native API 清单(影响 CA-5 的 ≤50 计数)。

## [P1] bridge 协议关键细节缺失(握手/流控/通道复用/帧参数)
- **位置**: `4-debug/03-debug.md` §2 L29–33
- **问题**: 协议仅一句"COBS + 16-bit CRC, 请求/响应带 echo id"加命令名列表。缺: (a) 同步/握手——host 如何获知波特率/协议版本/在线状态, COBS 流损坏后的重同步策略; (b) 流控——TRACE_READ 流式输出期间 host 来新命令、UART RX 溢出如何处理; (c) **"console 复用"的线上复用方式**——裸 console 文本与二进制 COBS 帧同一条 UART, host 如何区分(console 字节是否也封装为帧?); (d) 帧参数——最大帧长、echo id 宽度、CRC 多项式、重传策略; (e) MEMWR 安全策略(无隔离系统上任意写, 有无地址范围校验/量产只读档)。
- **证据**: "通道: UART 先行(console 复用)"; "成帧: COBS + 16-bit CRC, 请求/响应带 echo id"; "命令集: `GETINFO` / `MEMRD` / `MEMWR` / `TRACE_READ`(流式)…"。
- **建议**: 02-roadmap §5 第 8 项已把"bridge 最小命令集定稿"指派给 03-debug, 应补一节协议规范: 帧格式图、console 复用规则(建议 console 输出封装为独立通道帧)、同步序列、命令级流控(停/续)、MEMWR 校验与只读档。

## [P1] panic"最后一口气"路径的实现机制未落地
- **位置**: `4-debug/03-debug.md` §2 L33、§1 L25
- **问题**: 原则正确("不依赖任何插件栈/调度器/堆"), 但机制未设计: (a) 这段代码归属哪个模块(bridge 插件内? core? platform?)——若在 bridge Service 插件里, 如何做到不依赖插件栈; (b) panic 时 UART 通常处于 io/uart-pl011 的中断 tty 模式(FIFO/中断使能状态未知), 轮询路径如何接管与重置; (c) 该路径实际仍依赖平台早期 console 的轮询 putc, "不依赖任何插件栈"措辞过强, 应精确为"仅依赖平台轮询 console + 静态代码"; (d) §1"panic 时 console 倒出"(自动)与 panic 通道 TRACE_READ(host 拉取)是两条路径, 关系与取舍未说明。
- **证据**: "**panic 通道独立** —— 致命态(IRQ 锁死)下走'最后一口气'路径: 轮询 UART、不依赖任何插件栈/调度器/堆, 只允许 GETINFO / MEMRD / TRACE_READ"; "泄放途径: panic 时 console 倒出 / bridge 实时流 / host 工具解码"; 00 §8 "console 双形态: platform 早期 console(轮询, init 链打印)→ I/O 插件完整 tty(中断驱动)"。
- **建议**: 在 03 §2 补"panic 路径"小节: 代码归属(建议 core 拥有入口 + 平台轮询 putc 表)、UART 接管步骤(关中断→查询 FIFO→切轮询)、自动倒出 vs host 拉取的默认策略与 manifest 开关。

## [P1] trace 时间源与回绕锚点未定义
- **位置**: `4-debug/03-debug.md` §1 L14; `08-core-api-list.md` §4 L110/L118
- **问题**: 记录只有"时间戳低 32 位, 回绕由解码器处理"。未定义: tsc 的单位与频率(arch timer 原始计数? 微秒?)、与 tg_time_t(CA-1: uint64 微秒)的换算关系; 解码器处理回绕需要频率已知 + 一个全宽锚点(周期性 64 位 sync 事件或记录序号), 均未提供——32 位在常见频率下几十秒至几十分钟回绕一次, 且环溢出产生事件空洞后按序 unwrap 必然失准。host 工具承诺"线程时序对照 trace", 无时间基准定义则无法对照。
- **证据**: "uint32_t tsc; /* 时间戳低 32 位, 回绕由解码器处理 */"; 08 §4 "typedef uint64_t tg_time_t; /* 微秒(CA-1) */"。
- **建议**: 定死 tsc = 微秒(与 CA-1 同源, 换算收敛在 core); 并规定环内每 N 条插入一条内建 SYNC 事件携带全宽 64 位时钟与环序号, 或记录附带序号使解码器可 unwrap。

## [P1] trace 环写入策略与多核演进未定义
- **位置**: `4-debug/03-debug.md` §1 L9/L16
- **问题**: (a) 满时策略未定: 覆盖最老(drop-oldest, 丢崩溃前导)还是丢弃新事件(drop-new, 保现场)——对"panic 时倒出"这是关键取舍; (b) 丢事件无计数机制(记录无 seq 字段, 解码器无法发现空洞); (c) "MPSC 无锁写"实现约束未定: 单核下 ISR 抢占线程写一半时的原子性靠什么(head 预留 CAS? 对齐单写原子性? irq_lock?), 热路径开销未量化; (d) 记录无 CPU id 字段, v2 SMP(D6)到来时 ctx 无法区分核, 而定长格式 + host 解码器是长期契约, 届时改格式即破坏兼容。
- **证据**: "存储: 静态环形缓冲(manifest 定尺寸, 4–16KB 级), MPSC 无锁写"; "uint8_t ctx; /* 0=ISR, 1..n=线程 id */"。
- **建议**: 写明 drop-oldest(推荐, 理由: 保崩溃前导)+ 溢出计数事件; 定义写路径原子性方案与 ISR 开销预算; 在 ctx 或保留位预留 2–3 bit CPU id, 或现在声明 v2 的格式演进策略。

## [P1] trace 离线解码链路未闭环(id 分配方案/线程 id 来源/导出格式)
- **位置**: `4-debug/03-debug.md` §1 L15/L16/L22/L24; `02-roadmap.md` §5 L195
- **问题**: 路线图 DoD 第 8 项"trace 事件 id 分配方案…定稿入 docs/4-debug/03-debug.md", 但 03 只有一句"事件 id(由 manifest 分配)": 全局空间还是按插件分段? 组合期冲突检测? 插件升版增删事件后旧 trace 文件如何解码? "id→名字的映射表随插件元数据导出"的格式未定义(13-toolchain 提到"trace id 表(03)随镜像分发"但无格式)。另外 ctx 的"1..n=线程 id"没有 API 来源: tg_task_attr_t/TCB 均无 id 字段, 线程是运行时创建的, 静态导出表覆盖不了; 内建探针清单也没有线程 create/exit 事件, 解码器无法把 ctx 映射到线程名。
- **证据**: "uint16_t evt; /* 事件 id(由 manifest 分配) */"; "id→名字的映射表随插件元数据导出, 离线解码"; "uint8_t ctx; /* 0=ISR, 1..n=线程 id */"; 02-roadmap "trace 事件 id 分配方案 + bridge 最小命令集定稿 | 定稿入 `docs/4-debug/03-debug.md`"。
- **建议**: 补齐 DoD #8: id 空间方案(建议按插件分段 + manifest 独占校验)、导出表格式(随镜像 sidecar, 含插件版本)、内建探针增加 thread_create/exit(携带 id 与 name), 并定义线程 id 的分配点(create 时从 core 领号)。

## [P1] 运行时统计/栈高水位等基础观测能力缺失
- **位置**: `4-debug/03-debug.md` 全文(仅 trace/bridge/ramdump/ASan 四节); 对照 `00-architecture.md` §11 L626、`08-core-api-list.md` L75、`19-test.md` L32
- **问题**: 对宣称"debug 体系就是隔离的替代品"的系统, 缺嵌入式最廉价的两类常开观测: (a) 运行时计数器/统计——per-IRQ 计数与延迟、work 队列深度/高水位、每线程 CPU 时间、堆高水位(08 只有 tg_heap_usage 一个总量); (b) 栈高水位——栈溢出是无隔离系统最常见破坏源, 而栈 guard 推到 vx("栈 guard = vx(红区)"), v1.x 栈 canary 仅函数返回时报警, 事后无法回答"谁把栈用爆"。另 19 §2 要求 R-2 精度用例"输出分布统计(min/avg/max)", 承载它的统计设施同样无设计。
- **证据**: 03 目录仅 "## 1. trace…## 4. ASan"; "总原则: 单异常级无硬件隔离 ⇒ **debug 体系就是隔离的替代品**"; 08 L75 "栈 guard = vx(红区)"; 19 L32 "精度类用例(R-2)输出**分布统计**(min/avg/max)"。
- **建议**: 在 03 增加"廉价常开统计(v1.x)"一节: 栈水位(pattern 填充 + conformance 收尾/关机扫描)、per-IRQ/线程/工作队列计数器(定长表, bridge 新增 GETSTAT 上报), 并把 19 的分布统计出口挂到它上面。

## [P1] ramdump 的格式/栈回溯/持久化/预算/传输命令未定义
- **位置**: `4-debug/03-debug.md` §3 L39–42
- **问题**: (a) dump 容器格式未定义(magic/版本/节表/CRC), host 工具无从解析; (b) "栈回溯"机制未定(aarch64 是否强制 frame pointer? 无 FP 时用 unwind 表? 编译选项归属); (c) "预留 dump 区(manifest 声明)"的持久化语义未定——RAM 预留区只有配合"热复位不清该区 + 下次启动取回"的流程才有意义, QEMU 上还需启动参数配合, 该流程未写; (d) 捕获"TCB 全集 + 各线程栈"的静态缓冲与 LZ4 窗口无预算估算; (e) "或经 bridge 直传"没有对应命令——v1.x 命令集与 panic 白名单(GETINFO/MEMRD/TRACE_READ)均无 dump 传输命令。
- **证据**: "捕获集: 寄存器组、TCB 全集 + 各线程栈、trace 环、内存 region 表、插件表、arena 统计"; "输出: 预留 dump 区(manifest 声明)LZ4 压缩, 或经 bridge 直传"; "host 工具: 离线分析(线程时序对照 trace、fault 解码、栈回溯)"。
- **建议**: 补容器规范(节表+CRC+目标信息块)、规定 debug 构建编译纪律(-fno-omit-frame-pointer 默认或 unwind 段随镜像分发)、写明热复位持久化与取回流程(含 QEMU 验证方法)、给出静态缓冲预算公式, 并在 bridge 命令集补 DUMP_READ(或声明经 MEMRD 分块读)。

## [P1] core-api 用例覆盖存在可枚举缺口(对照 08 的 48 函数面)
- **位置**: `5-test/19-test.md` §3 各组 vs `08-core-api-list.md` §2–§8
- **问题**: 43 用例整体覆盖良好, 但有具体缺口: **tg_dma_free**(TC-MEM-006 仅测 alloc 的 dma_addr==vaddr)、**tg_mm_cache_invalidate**(仅 TC-MM-002 测 flush)、**TG_SEM_DEFINE/TG_COND_DEFINE 静态 vs init 等价**(CA-2 布局对三类对象同样要紧, 仅 mutex 有 TC-SYNC-009)、**tg_task_sleep(-EINVAL 负值)**(08 §2.1 明确承诺)、**tg_task_create(-ENOMEM)**、**TG_TIMEOUT_INF 阻塞-释放语义**(CA-4 三态只测了 ZERO)、**sleep_until(未来期限)正向到期**(TC-TASK-005 只测已过期)、**deadline_from_now 正向构造**(TC-TIME-003 只测饱和)。
- **证据**: 08 §2.1 "`-EINVAL`(负值)"(sleep 行); 08 §4 "#define TG_TIMEOUT_INF UINT64_MAX"; TC-TASK-005 "sleep_until(已过期期限)"; TC-MEM-006 仅 "dma_alloc | `dma_addr == vaddr`"。
- **建议**: 按清单补 8–10 个用例(TC-SYNC-010/011 DEFINE 等价、TC-TASK-008 sleep 负值、TC-TIME-004 deadline 正向、TC-MEM-008 dma_free 往返、TC-MM-004 invalidate 等); 并在 §5 增加"golden 符号 ↔ 用例 id 覆盖核对表", 使覆盖缺口 CI 可查(与"新 API 用例先行"门禁闭环)。

## [P1] tg-sched 冻结门禁与版本路线不闭合(矩阵全绿需要 v3.0 的 sched-tt)
- **位置**: `5-test/19-test.md` §4 L125–127; `08-core-api-list.md` §15 L374; `01-api-contract-governance.md` §2.6.2 L149; `02-roadmap.md` §1(v2.0/v3.0)与 §3 L172
- **问题**: tg-sched 组(21 函数: 任务/同步/时间/work, 第一契约的主体)升格前置是"{sched-coop, sched-preempt, sched-tt} × 同一套语义测试全绿"。但 preempt 是 v2.0、tt 是 v3.0: 按 08 §15"第二批(M3 后)"字面, tg-sched 最早要到 v3.0 才能冻结, M3 的"native API 冻结启动(D15)"对最大的 API 组实际是空启动。三份文档均未写出这一后果, 也无分期方案。
- **证据**: "必须 `{sched-coop, sched-preempt, sched-tt} × 同一套语义测试` 全绿后升格"; 19 §4 "矩阵必须全绿——**矩阵是门禁, 不是事后报告**"; 02-roadmap "sched-preempt … v2.0"、"sched-tt … v3.0"; M3 "v1.0 完整化 + native API 冻结启动(D15)"。
- **建议**: 二选一: (1) 分期冻结——M3 后以 coop 矩阵全绿为前置冻结 tg-sched, 契约中写明"preempt/tt 落地时矩阵重跑是发布门禁"(修订 01 §2.6.2 前置条件为"已有调度器全绿 + 未到调度器的重验契约"); (2) 接受 tg-sched 实验期延至 v3.0, 但把后果写进 08 §15 与 D15 说明, 避免"M3 冻结启动"被读成覆盖全部 API。

## [P1] 调度列标注失真: TC-TASK-006 的 coop 专属期望标 ALL; tt 下用例集可行性未定义
- **位置**: `5-test/19-test.md` §3 L36/L47、§4 L128–129; `08-core-api-list.md` §2 L47; `00-architecture.md` §16 R5 L679
- **问题**: (a) TC-TASK-006 期望"交错发生(coop 强制切换点)"却标 ALL——08 §2 明确 "tg_task_yield: coop: 强制切换点; preempt: 提示", 提示不保证切换(对端优先级更低时 preempt 可不选它), 该用例在 preempt 矩阵会挂或抖动; (b) 全部 43 个用例均标 ALL, "标注者只在对应调度器下有意义"的机制空转; (c) tt(v3)是静态调度表, 动态 create/join、10k 次循环计数这类用例能否在其上运行未定义——主文档 R5 自认"时间表模式下动态线程语义受限", 19 却默认全套用例三矩阵通跑, conformance APP 在 tt 下的调度表由谁生成也无说法。
- **证据**: "TC-TASK-006 | yield 语义: 就绪队列有另一任务时交错 | 交错发生(coop 强制切换点) | ALL"; 08 "void tg_task_yield(void); /* coop: 强制切换点; preempt: 提示 */"; 00 R5 "sched-tt 与 pthread 语义摩擦: 时间表模式下动态线程语义受限"; 19 L36 "**ALL** = 三调度器矩阵全跑"。
- **建议**: TC-TASK-006 期望调度器条件化(拆为 coop 必然交错 + preempt"让出后 self 不再运行直至对端 yield"两个用例并标注); 为 tt 定义用例子集与执行模型(调度表由 conformance APP manifest 生成, 动态线程类用例标注 tt-不适用), 并在 §4 列出"哪些不变量在 tt 下仍必须成立"。

## [P1] host 执行模型的关键声称不成立(bridge/trace 上报在 Linux 进程上没有落点)
- **位置**: `5-test/19-test.md` §2 L30–31、L3
- **问题**: §2 失败上报模型是"断言失败 → panic → bridge 独立通道上报"与"结果经 trace 事件上报", 但 host 平台插件是 Linux 进程, 没有 UART/bridge: host CI 如何采集结果、panic 在 host 上映射为什么(进程 abort? 哪个退出码?)、一个用例失败是否拖死同进程其余用例(隔离模型: 每用例 fork?)均无说法。状态行自称"运行基建…为骨架", 但这两句是 §2 正文声称而非待定项。此外 conformance APP 所需 manifest 夹具(work 队列深度 D、栈尺寸、堆大小、trace 环尺寸)未定义——TC-WORK-003 依赖已知深度 D。
- **证据**: "断言失败 → panic → **bridge 独立通道上报**(`docs/4-debug/03-debug.md` §2)"; "结果经 trace 事件上报(03 §1), 用例 id 进 trace"; "状态: **用例目录成文**; 运行基建/矩阵细则为骨架"; TC-WORK-003 "队列满(深度 D) | 第 D+1 次 `-EAGAIN`"。
- **建议**: §2 拆 host/target 两条上报路径: host = 每用例 fork 隔离 + 进程退出码 + 结果文件(由 13-toolchain "conformance 运行器…报告格式"承接); target = 现有 bridge/trace 路径。并定义 conformance APP 的 manifest 夹具清单(固定 D/栈/堆/trace 尺寸, 纳入用例前置)。

## [P1] R-3 的"debug 运行时断言"机制无设计归属
- **位置**: `5-test/19-test.md` §1 L15、§3.7 L109; `08-core-api-list.md` §11 L286; `4-debug/03-debug.md` 全文
- **问题**: 19 把 ISR 白名单执法定义为"静态扫描红 + debug 运行时断言", TC-IRQ-005 直接期望"ISR 内调 malloc(debug 构建) → 运行时断言"。但 08 §11 只写了"conformance 静态扫描执法(D10/R1)", 03-debug 四节中没有 ISR 上下文运行时检测的设计: core 如何判定当前在 ISR(全局 in-ISR 标志?)、thread-only API 在 debug 构建下如何加检查(每函数入口断言? 包装层?)、开销与编译开关均无人认领——一个 target 用例依赖不存在的机制。
- **证据**: "R-3 | ISR 白名单执法: 白名单外调用 = 静态扫描红 + debug 运行时断言 | CA-3"; "TC-IRQ-005 | ISR 内调 malloc(debug 构建) | 运行时断言(R-3 执法)"; 08 §11 "其余**全部 thread-only**——conformance 静态扫描执法(D10/R1)"。
- **建议**: 在 03-debug 增加"debug 构建的运行时执法"一节: in-ISR 判定(int 模块维护)、TG_DEBUG 下 thread-only API 入口断言的展开方式、开销预算与编译开关; 08 §11 同步补"运行时断言(debug 构建)"并指向 03。

## [P1] 被治理面大于被测面: svc-posix/框架件语义用例与组合期校验负向测试无归属
- **位置**: `5-test/19-test.md` §1/§3(仅 core-api); `01-api-contract-governance.md` §2.3 L71–72; `00-architecture.md` §7.6 L512; `02-roadmap.md` §3 M0 L169; `7-toolchain/13-toolchain.md` §2
- **问题**: 治理文档承诺 svc-posix 的 POSIX 符号面"从'口头承诺'变成**被测试的契约**"、框架件四件 API 面是"第三类被治理对象——同一门禁机制", 主文档 §7.6 要求 POSIX"子集诚实义务…conformance 覆盖"; 但 19 的六不变量与八个用例组全部是 core native API, POSIX 面与框架件面的语义用例没有任何文档认领(18-plugin-dev §6 还把 19 当作全部插件的用例目录引用, 加剧名不副实)。同样, 组合期校验(环检测/预算超限/符号碰撞/sched_class 拒绝)的负向测试只有 M0 一句"环检测用例(故意造环看组合器报错)", 13-toolchain 大纲只有"schema + 校验器"没有"校验器的测试"。
- **证据**: 01 §2.3 "POSIX 子集从'口头承诺'变成被测试的契约"; "框架件(dev-core/cdev-core/vfs-core/bdev-core)的 API 面是第三类被治理对象"; 00 §7.6 "实现/未实现必须成文 + conformance 覆盖"; 02-roadmap M0 "**环检测用例**(故意造环看组合器报错)"; 19 §1 "用例目录围绕六条不变量组织"。
- **建议**: 在 19 定位节明确测试版图与归属: 本篇 = core native; svc-posix 语义用例 → 16-service 或独立篇; 框架件用例 → 04–07 各自的 conformance 节; 组合器负向夹具 → 13-toolchain(把 M0 环检测扩成负向夹具集: 环/预算/碰撞/sched_class 各一)。否则"同一门禁机制"对后两类对象只有层 1 没有 层 2。

## [P2] TRACE_EVT 与 TG_TRACE_EVT 命名失配(白名单引用断链)
- **位置**: `4-debug/03-debug.md` §1 L23 vs `08-core-api-list.md` §11 L284、§12 L293
- **问题**: 03 定义 "API: TRACE_EVT(id, a, b, c)", 而 08 §11 的 ISR 白名单写 "TG_TRACE_EVT 宏(docs/4-debug/03-debug.md §1)"——被引用文档里找不到这个名字, 且违反 08 §12 命名规范"宏/常量 `TG_*`"。白名单是 CA-3 冻结轨道契约, 名称失配会让按符号执法的静态扫描工具两头落空。
- **证据**: "API: `TRACE_EVT(id, a, b, c)` 宏"; 08 "`TG_TRACE_EVT` 宏(`docs/4-debug/03-debug.md` §1)"; 08 §12 "宏/常量 `TG_*`"。
- **建议**: 统一为 TG_TRACE_EVT, 并顺带定义 arity 变体(TG_TRACE_EVT0/1/2/3)与 narg 字段的对应关系。

## [P2] 部分用例通过判据不可机械判定, 违反自订规范
- **位置**: `5-test/19-test.md` §3.5 L86、§3.1 L47、§3.2 L59; §5 L134
- **问题**: §5 要求"期望必须可机械判定", 但 TC-MEM-002 的"TLSF 有界碎片"未定义分配模式、规模与"有界"的数值判据; TC-TASK-006 的"交错发生"无可观测判据; TC-SYNC-006 的"无虚假唤醒"无观察时长/轮次界。
- **证据**: "TC-MEM-002 | 碎片压力: 交错 alloc/free 后最大块仍可分配 | TLSF 有界碎片"; §5 "期望必须可机械判定"。
- **建议**: TC-MEM-002 写明固定伪随机分配脚本 + 数值判据(如"最大可分配块 ≥ 总空闲的 X%"); TC-TASK-006 用双计数器判据; TC-SYNC-006 限定"N 轮 wait/signal 循环零异常唤醒"。

## [P2] 执法闭环的机制细节未定义(用例先行 PR 检查、矩阵结果→组合期拒绝)
- **位置**: `5-test/19-test.md` §4 L127、§5 L135
- **问题**: "coop 下跑'抢占安全'用例失败 ⇒ 声明为假, 组合期拒绝"——组合期只能读声明, conformance 结果如何变成组合期可见的机器事实(发布资格库? 描述符标注?)无机制; "用例 PR 与升格 PR 的先后由 CI 检查"——跨 PR 先后关系 CI 如何判定也无机制。
- **证据**: "声明为假, 组合期拒绝"; "用例 PR 与升格 PR 的先后由 CI 检查"。
- **建议**: 定义两个机械产物: (1) 用例覆盖表(golden 符号→用例 id, 随 19 维护, 升格 PR 必须更新); (2) 插件发布产物中的矩阵结果戳(组合器校验"声明 SAFE_PREEMPT ⇒ 结果戳含 preempt 全绿")。

## [P2] TC-IRQ/TC-MM 组部分期望缺乏 08 规格背书
- **位置**: `5-test/19-test.md` §3.7 L106、§3.6 L97–98; `08-core-api-list.md` §7/§8
- **问题**: TC-IRQ-002 期望"重复 register → -EBUSY"、TC-MM-001 期望"region_add 重叠/重复 → -EINVAL"、TC-MM-002 期望"cache_flush 于 DEVICE 区 = 0(无操作)"——08 §7/§8 没有这些函数的错误/行为规格(§2.1 式每函数表只覆盖任务组)。用例先行本身是流程优势, 但 -ETIMEDOUT 先例(19 §1 注)表明流程要求"已补入 08", 这几条无回填记录。
- **证据**: "TC-IRQ-002 | 重复 register | `-EBUSY`"; 08 §8 无 tg_irq_register 错误表; 19 §1 "已补入 08 §11"。
- **建议**: 按 -ETIMEDOUT 先例把这三条(及 TC-IRQ-003 的 disable 语义)回填进 08 §7/§8 每函数规格, 或在用例表加"契约来源"列标"用例先行, 待回填"。

## [P2] TC-TIME-002 的硬上界与 R-2"晚到无上界不判红"表述冲突
- **位置**: `5-test/19-test.md` §3.3 L69 vs §1 L14
- **问题**: R-2 承诺"晚到无上界承诺(统计上报, 不判红)", TC-TIME-002 却给 sleep(1s) 判定区间"0.9–2.0s"——2.0s 是会判红的上界; 且 0.9s 下界允许了 10% 早到, 与"不早醒"不一致(未注明测量容差来源)。
- **证据**: "TC-TIME-002 | 单位校准: sleep(1s) 对 host 参考钟 | 0.9–2.0s(CA-1 微秒)"; "R-2 | 不早醒: sleep/超时到期不早于期限; 晚到无上界承诺(统计上报, 不判红)"。
- **建议**: 期望改为"≥ 1.0s(R-2); 上界 2.0s 仅作 sanity 告警不判红", 并注明下界容差定义。

## [P2] ramdump 触发仅定义 aarch64, ISA 范围未声明
- **位置**: `4-debug/03-debug.md` §3 L37; `00-architecture.md` §2.2 L74–76
- **问题**: 触发机制只写 aarch64 同步异常, 而主文档 §2.2 目标 ISA 还含 Cortex-M/R(恒特权)与 RISC-V(M-mode)。v2 是 aarch64 尚可, 但文档未声明"ramdump v2 范围 = aarch64; M/RISC-V 的 fault 源(HardFault/mcause+mtval)留待 vX", 易被读成机制天然多 ISA。
- **证据**: "触发: aarch64 同步异常(向量表分槽, `ESR_EL1`/`FAR_ELx` 区分 fault 与 IRQ)"; 00 §2.2 表含 "ARM Cortex-M/R | 恒特权态"、"RISC-V | M-mode"。
- **建议**: 在 §3 加一句范围声明与 M/RISC-V 机制占位, 或在 09-int fault 路径大纲按 ISA 分列。

## [P2] debug 设施自身无测试归属
- **位置**: `4-debug/03-debug.md` §2 L31(SELFTEST); `5-test/19-test.md` §3(无 debug 组)
- **问题**: bridge 有 SELFTEST 命令, 但 trace(环溢出计数、MPSC 在 ISR+线程并发下的正确性)、bridge(COBS 回环/坏帧重同步)、panic 通道可达性这些"隔离替代品"自身的正确性无用例也无归属——debug 设施损坏时, 最后的真相来源就没有了。
- **证据**: "命令集: … `SELFTEST`"; 19 §3 仅 TASK/SYNC/TIME/WORK/MEM/MM/IRQ/SVC 八组。
- **建议**: 在 19 增加 TC-DBG 组(或声明各 debug 插件 SELFTEST 的覆盖范围): trace 环溢出与并发写一致性、bridge 回环与重同步、panic 通道可达性(v1 即测, 不等 v2 ramdump)。

---

## 数量统计

| 严重度 | 数量 |
|---|---|
| P0(矛盾/架构缺陷, 必须解决) | 5 |
| P1(完备性缺口/技术不清晰, 应当解决) | 14 |
| P2(改进建议) | 7 |
| **合计** | **26** |

**优先处置建议**: 先解决 5 个 P0(其中 16B/20B 与 -ENOSYS 是一行级修复但属硬矛盾; trace 形态、host ASan 边界、红区版本归属需要设计决策), 再补 panic 原语与 v1 fault 入口(P1 第 1 条)——它是 19-test 失败上报模型与 03 panic 通道共同的地基, 当前悬空影响面最大。

## 核对无误的方面(供参考)

- 03/19 与主文档 §11 的能力-版本大表基本一致; R-1/R-2/R-5/R-6 不变量与 08 的 CA 决策对应关系正确
- 19 §1 的 -ETIMEDOUT 回填 08 §11 的流程实绩(用例先行的价值证明)真实存在且被引用
- host 平台插件 + conformance APP 的三层运行基建思路与 01 §2.3 层 2 对齐

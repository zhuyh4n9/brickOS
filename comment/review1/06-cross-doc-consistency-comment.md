# 06 — 跨文档一致性评审意见（系统性主题）

> 评审范围: `tangramOS/docs/` 全部 21 篇的横切一致性——版本归属 / 命名契约 / 决策语义 / 错误码口径 / 交叉引用 / 数字口径 / 术语统一
> 评审方式: 5 个分模块 subagent 的交叉核对 + 主评审对 D/SD/CA 编号、版本号、宏名、段名的全库 grep 复核
> 说明: 文件级明细见 `01`–`05` 号评审文件; 本篇只收口**跨 ≥2 篇**的系统性问题, 不重复文件级细节

## A. 版本归属冲突（同一特性, 两处版本号不同）

- **A1 [高] trace 记录长度**: `03-debug.md` L10、主文档 §11 L629、`02-roadmap.md` L64 三处写「定长 **16B** 事件环形缓冲」, 但 `03-debug.md` L13–19 的 `tg_trace_evt` 结构体实为 **20 字节**（tsc 4 + evt 2 + ctx 1 + narg 1 + arg[3] 12）。结构体与三处文字冲突。→ 二选一: `arg[2]`+宏收两参（真 16B）, 或全链口径改 20B（同步改主文档 §11 与 roadmap）。
- **A2 [中] 红区/金丝雀版本**: `02-roadmap.md` L81 把「红区/金丝雀」排进 **v2.0** debug 行; 主文档 §11 L632 与 `03-debug.md` L49 均为 **v1.x**。→ 修 roadmap（03 无需改）。
- **A3 [中] 运行时挂载版本**: `04-vfs.md` L97 与 `07-concrete-fs.md` L105 写「v2 议题(O-S3)」; `02-roadmap.md` L135 写「若落地也在 **v3** [?]」。→ 统一口径。
- **A4 [低] dbg-bridge 版本**: 主文档 §11 L630「bridge | … | **v1.x 最小集**」; `02-roadmap.md` L65 将 `service/dbg-bridge` 列入 **v1.0** 插件清单（M3）。→ 统一（建议: v1.0(M3) 最小集）。
- **A5 [中] 「冻结」时点三种口径 + 术语混用**: `08` L26「签名 **M2 起冻结**」/ `08` L171、`05-bdev` L57、`11-memory` L14「**v1** 签名冻结」 vs `08` §15 L373「第一批(**M3**)」 vs D15/`01` §2.1「**M3 之前不冻结任何东西**」; 且「冻结」混用「设计定稿」（主文档 L215 R4 原话是「M2 起定好签名」）与「升格 frozen 进 golden」两义。→ 全部改「M2 起定稿/定好签名(R4)」, 「冻结」保留给 D15 流程。
- **A6 [中] tg-sched 冻结前置与版本路线冲突**: `08` L24/L374 与 `01` D15 行（L234）均要求「{coop, preempt, tt} 三调度器 conformance 矩阵全绿」才能升格 `tg-sched`, 但按路线 **preempt = v2.0、tt = v3.0**——tg-sched（任务/同步 API, v1.0 核心）实际最早 v3.0 才能冻结, 与「M3 起分批冻结」整体节奏冲突。→ 明示「第二批实际时点 ≥ v3.0」或放宽为「已有调度器矩阵」。

## B. 命名与符号级契约漂移（组合式架构最怕的契约面失真）

- **B1 [中] 链接段名单复数**: 主文档 §4.4 L227/§6.1 L338/§9 L564 为 **`.tg_plugins`**; `08` L335/L338/L367 与 `12-plugin-mgr` L21 为 **`.tg_plugin`**。段名是二进制级链接契约。→ 全局统一（建议随主文档 `.tg_plugins`）。
- **B2 [低] 附: `__start_.tg_plugin` 非法标识符**: `08` L338 `extern const tg_plugin_t __start_.tg_plugin[]`——C 标识符不允许含 `.`, 无法编译; GNU ld 仅对「合法 C 标识符的段名」自动生成 `__start_/__stop_` 边界符号, 带点段名（`.tg_plugin`/`.tg_plugins` 均是）需链接脚本 PROVIDE 别名（如 `__tg_plugins_start`）。
- **B3 [中] trace 宏一宏两名**: `03-debug` L23 `TRACE_EVT` vs `08` L284 `TG_TRACE_EVT`——后者符合 D16「宏 `TG_*`」与 `08` §12 命名规范。→ 统一 `TG_TRACE_EVT`（改 03）。
- **B4 [低] 内联访问器宏名**: `08` L314 `TG_API_INLINE` vs `01` §2.4 L90/§2.6.3 L161 `TG_API_INLINE_FROZEN`（后者含「布局入 golden、每例需 CA 记录」硬约束, 08 引用的正是 01 §2.4 却用了另一个名字, 丢失 FROZEN 语义）。→ 统一。
- **B5 [中] `tg_task_create` 参数顺序**: `08` L43「`tg_task_create(tg_thread_t **t, const tg_task_attr_t *attr, entry, arg)`」（句柄在前） vs `01` L63 golden 示例「`FUNC tg_task_create (const tg_task_attr*, tg_thread**)`」（attr 在前）。golden 示例与规格矛盾。→ 以 08 规格为准修 01 示例（或反之, 但必须一致）。
- **B6 [中] `sched_ops` 幽灵槽位**: `08` L61/L63/L64 引用 `sched_ops.create`/`switch_out`/`yield`——主文档 §5.1 `tg_sched_ops` 无此三名（「入列」对应 `thread_ready`）; 10-sched 全字段表未成文, 属无出处前向引用。
- **B7 [中] 生命周期相位术语**: `12-plugin-mgr` L13「EARLY(**PRE_ARCH/PIC/ARCH**)/CORE/LATE **三相**」、`11-memory` L32「§9 的 PRE_ARCH 后」 vs 主文档 §6.2「**EARLY → CORE → LATE → APP 四相**」——`PRE_ARCH/PIC/ARCH` 在全部 21 篇中无定义。→ 对齐四相; 子相若保留需先在主文档落定义。
- **B8 [中] 生命周期环境与启动时序矛盾**: 主文档 §6.1 L331 `early_init`「**无堆**/无线程/关中断」 vs 主文档 §9 L561–563 `core.init`（含**堆**）先于 EARLY 执行——EARLY 回调运行时堆已存在。→ 统一（改 §6.1 为「不使用堆」, 或调整 §9 时序表述）。
- **B9 [中] 插件 `start` 回调悬空**: 主文档 §6.1 L333 定义 `start`（「中断可用, 可创建线程」）, 但 §9 启动序列在「全局开中断」与「app.start()」之间**没有任何插件 start 调用点**; `12-plugin-mgr` L22 状态机「注册→init→运行」也漏 `start`/`early_init`。→ §9 补「各插件 start()」步骤, 12 状态机同步补齐。
- **B10 [低] `init DAG` vs `init-DAG`**: 主文档 L410（§7.2 PlantUML 标签）写「init DAG 无环」, 全库其余 11 处均为「init-DAG」。→ 统一连字符。

## C. 决策语义漂移（同一决策号, 不同内容）

- **C1 [中] D10「双保险」**: 主文档 D10 = **组合期校验 + 静态分析**; `10-sched` L12 写成「插件声明 + conformance 双保险」; `19-test` L127 把矩阵执法当 D10 内容并写「组合期拒绝」（时序混淆: 矩阵失败是 CI 期发现, 组合期校验恰识别不出声明撒谎）; `09-int` L11 写「conformance 静态扫描执法(D10/R1)」。→ 以主文档 D10 为准统一（conformance 矩阵为 R1 的执法载体, 不是双保险成员）。
- **C2 [中] D8「最后一个未落定」**: `09-int` L29 称 D8 为「最后一个未落定的架构决策」——但 D7 同样待定（主文档 §17 并列两行; `17-service-mgmt` L28 称 D7「最后未定的决策**之一**」; roadmap L188 将 D7/D8 并列收口）。→ 改「最后两个未落定决策之一」。
- **C3 [中] D18 引用错位**: `14-app` L11「APP **可声明依赖服务**(sqlite 类经 svc-posix, **D18**)」——D18 的内容是「**三方中间件**可声明依赖 svc-posix」, 非 APP; 且主文档 §6.3/§7.2 中 APP 只依赖 Interface（或直调 native）, 服务走注册表按名取用。→ 修正引用与依赖模型（详见 `05-peripheral-comment.md`）。
- **C4 [中] 「严格叶子」逻辑龃龉**: 主文档 L170/L406 与 `15-interface` L8「**没有任何插件依赖它们**」 vs 主文档 L355/L453「**仅被 APP 依赖**」——而主文档 L68「APP 本身也是一个插件」, 字面自相矛盾。→ 改「**除 APP 外**没有任何插件依赖它们」（主文档与 15 一并修）。
- **C5 [中] abi_id 时点**: `12-plugin-mgr` L12「**v2** 描述符带 `abi_id`(D14)」 vs 主文档 §6.1 现行描述符**已含** `abi_id` 字段、D14「**Day1** 按二进制兼容设计」。（另一种读法「描述符 v2 带 abi_id」与 §6.1 标题一致——歧义本身即问题。）→ 删「v2」或明确「字段 Day1 存在、v2 二进制分发起生效」。

## D. 错误码集合口径（同号不同表 + 游离符号）

- **D1 [中] SD-10 同号两表**: `06-device` L186（设备域 7 码: EIO/ENODEV/ENOSPC/EINVAL/ENOTSUP/EBUSY/EROFS） vs `08` L289（core 域 9 码: EIO/EAGAIN/EINVAL/ENOMEM/ENODEV/ENOTSUP/EBUSY/EEXIST/ETIMEDOUT, 自称「SD-10 统一」）; `19-test` R-4 又按「SD-10 子集」扫描 core 错误码——按 06 份会与 R-1 的 -ETIMEDOUT、TC-WORK-003 的 -EAGAIN、TC-SVC-002 的 -EEXIST 直接打架。→ SD-10 定义全集（两处并集）+ 各域注明取子集, 或分别为设备域/core 域立号。
- **D2 [高] `-ENOSYS` 游离**: `19-test` L99 TC-MM-003 期望 `-ENOSYS`——不在任何一份子集内, 且违反本篇 R-4（详见 `03-plugin-debug-test-comment.md`）。
- **D3 [中] `-EOPNOTSUPP` 游离**: `07-concrete-fs` L32 用 `-EOPNOTSUPP`, 全库其余处统一 `-ENOTSUP`（Linux 上两符号同值, 嵌入式 libc 未必保证, 契约文本应统一符号）。
- **D4 [低] 「errno 映射」 vs 「零转换」**: `16-service` L19「errno 映射(-ret)」 vs `06-device` L186/`08` L289「直接取 `-ret` 作 errno(零转换)」。→ 统一「errno = -ret(零转换)」。

## E. 断裂/错误交叉引用（索引清单）

| # | 位置 | 问题 | 应为 |
|---|---|---|---|
| E1 | `09-int` L14 | 「06 §8 三层模式」——06-device §8 是「风险与开放问题」 | 主文档 §8（06 L213 自己写「主文档 §8 三层模式」） |
| E2 | `08` L262 | 「04-vfs.md §0.1 消费者表」——04 无 §0.1 | 06-device §1（「消费者的依赖声明」表） |
| E3 | `05-bdev` L52 | 「EROFS 特例…(07 §3)」——07 §3 是 devfs | 07 §5（EROFS FS 级解压页缓存） |
| E4 | `11-memory` L37 | 「roadmap §5 **第 1 项**…native API 头文件级规格」 | 第 **2** 项（第 1 项是 D7/D8 收口） |
| E5 | `06-device` L187 | 「FS 侧见 07-concrete-fs」——07 全文无 FS 并发讨论 | 补节或改「FS 侧并发 v1 未另行约定」 |
| E6 | `07-concrete-fs` L97 | 「SD-2/**SD-12**: 子分类与 open_file 钩子」 | SD-2/**SD-13**（钩子是 SD-13 的内容） |
| E7 | `04-vfs` L5 / `06-device` L5 | 头部「本篇决策」清单漏 SD-15 / SD-13、SD-14 | 补齐（README L20/L22 为准） |
| E8 | `08` L39/L105 等 | 裸「§5.3」「§5.2」——08 自身也有 §5/§6, 歧义; 实指主文档且 §5.3 指错（prio 差异在 §5.2） | 「主文档 §5.2」式全限定引用 |
| E9 | `02-roadmap` L156 | 「iface-autosar-ish(主文档 §7.4 [?])」——§7.4 无此插件（仅泛化注） | 改引 15-interface L15, 或在 §7.4 补域标准示例 |
| E10 | 主文档 L10 / roadmap L3 | 「`../comment/00-architecture-comment.md`(仓库外)」——相对路径解析为 docs/comment/, 不存在 | `../../comment/…` 或注明绝对位置 |
| E11 | `18-plugin-dev` L4 / `README` L31 | 「驱动/FS/服务/接口/平台/调度器/APP 全部是插件。**八类**」——枚举仅 7 项, 漏「框架件」 | 枚举补「框架件」或改「八类(含框架件)」 |

## F. 数字口径

- **F1 [低] 测试用例数**: 实际 **43** 条 TC- 用例（逐组清点: TASK 7 + SYNC 9 + TIME 3 + WORK 4 + MEM 7 + MM 3 + IRQ 7 + SVC 3）; `19-test` L121 写「~40 用例 × 3 调度器 ≈ 120」（43×3=**129**）; `README` L19 写「40 用例」（去掉了「约」）。→ 写实数「43 × 3 = 129」。
- **F2 [高] 16B vs 20B**: 见 A1（数字硬伤, 三处文字 vs 一处结构体）。

## G. 术语歧义与不统一

- **G1 [低] 「挂载表单路由」**: `04`/`06`/主文档 §4.5 多处——「挂载表单」易误读为 mount form, 实为「挂载表 · 单路由」。→ 统一「挂载表·单路由」。
- **G2 [中] 「冻结」一词两义**: 见 A5——「设计定稿」与「升格 frozen(D12 治理流程)」混用, 已产生实质冲突（M2/M3 之争）。→ 术语表区分。
- **G3 [低] R 双编号体系**: `19-test` 不变量 R-1…R-6 vs 主文档风险 R1–R9（仅一连字符之差）; `08` L289 用「R-1」、L26/L171/L174 又用「R4」, 同篇混用。→ 19 的评审项改前缀（如 INV-*）或首现处注明出处。
- **G4 [低] 「超时轮」**: `10-sched` L21 同一行「轮」（时间轮）与「队列」两个数据结构术语混用。→ 统一「超时队列」。

## 通过项（跨文档层面, 供置信）

- **用户手写笔记（`../00-architecture-comment.md`）中的决定已全部落实且跨文档一致**: coop v1.0 / preempt v2.0 / tt v3.0（D2/roadmap/10-sched）；虚拟内存 v1 恒等 / v2 重定位 / vx MPU（主文档 §4.3/11-memory）；bottom half v1.0（§4.2/08 §5/roadmap）；trace v1.0、类 adb bridge、mini ramdump v2.0（§11/03-debug）；littlefs v1.0 / EROFS v2.0（D17）；page cache vx.0（05-bdev/roadmap）；动态加载 v3.0 + 鉴权（D1/§12）；crypto v2.0（roadmap）；core C(v1.0)→C+Rust(v4.0)、插件 C(v1.0)/Rust(v2.0)（D5）；aarch64（D9）；SMP v2.0（D6）；双保险（D10）；插件版本管理（§6.1 `ver[3]`）；依赖/环管理（§6.5）；许可证 WTFPL（LICENSE 实为 WTFPL, 与 roadmap §5 一致）。
- **版本 milestone 全集一致**（除上列 A1–A6 冲突点外）: 动态加载 v3、EROFS/ramdump/crypto/lwip/pkcs11 v2.0、Rust 插件 v2.0、page cache/MPU/完整 ASan vx.0, 各篇无矛盾。
- **决策号体系无越界引用**: D1–D23（D7/D8 在主文档 §17 待定）、SD-1–SD-15、CA-1–CA-10, 全库无 D24+/SD-16+/CA-11+。
- **全局编号 00–19 连续无跳号**, README L3「全局编号 = 文档稳定身份」声明被遵守; README 状态列（成文/骨架）20 篇全部相符; 所有 `.md` 引用路径存在。
- 06-device §1 存储栈全景图与主文档 §10 逐行一致; `_IOC` 位布局与 Linux 完全一致; `lfs_config` 1:1、shadow≈RAM/8 等技术事实正确。

## 总体建议

1. **一轮「契约名词对齐」清账**（骨架深化前做）: 以 08 + 主文档为真值, 优先修 5 组——错误码集合（D1–D4）、链接段名与边界符号（B1/B2）、生命周期相位与 start 回调（B7–B9）、「冻结」术语（A5/A6）、trace 记录长度（A1）。这五组都是会直接进实现/CI 门禁的契约面。
2. **建一份术语表**（glossary）: 冻结/定稿、严格叶子、init 依赖/调用依赖、挂载表·单路由、双保险——每个术语一个权威定义 + 一句标准句式, 骨架篇转述时照抄。
3. **交叉引用规范化**: 「文档号 §N」全限定写法（如「主文档 §5.2」「06-device §3」）, 消灭裸「§N」; 决策号引用建议加一处索引页（D/SD/CA → 定义文件 §）。

**问题统计: 本篇收口系统性主题 30 项（高 2 / 中 17 / 低 11）**, 文件级明细共 94 条见 `01`–`05` 号评审文件（存在有意重叠: 同一问题在模块文件记明细、在本篇记系统视图, 修复时以模块文件行号为准）。

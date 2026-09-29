# review1 · 文档表述质量走查（本系列总览）

> 评审对象: `tangramOS/docs/` 全部 21 篇设计文档（00–19, 约 2700 行）
> 评审重点: **文档表述质量**——是否存在胡言乱语、技术性错误、前后矛盾、表达错误、格式与引用问题
> 评审方式: 5 个分模块 subagent 并行评审 + 主评审通读全部 21 篇逐条复核（每条意见均核对原文行号; 用户手写笔记 `../00-architecture-comment.md` 作为决策对照基准）
> 评审轮次: v0.9 文档稿（D1–D23 / SD-1–15 / CA-1–10 决策体系）

## 总体结论

**文档集整体质量高, 未发现成体系的胡言乱语。** 决策号体系（D/SD/CA）无越界引用、版本 roadmap 跨 21 篇基本一致、全局编号 00–19 无跳号、README 索引状态列全部准确、用户笔记中的全部既定决策（coop v1/preempt v2/tt v3、虚拟内存分期、littlefs v1/EROFS v2 等）均已落实。技术事实层面（aarch64 启动、_IOC 编码、lfs_config 绑定、ASan shadow、DMA/cache 维护方向、EROFS/littlefs 特性）绝大多数正确。

主要质量债务集中在两类:
1. **「改一半」的决策演进残留**——D20–D23 快速迭代后, 主文档 D19 决策行、§7.1 归属表、04-vfs 的设备路由描述等仍保留旧模型, 与现行决策直接矛盾（含 1 条高危）;
2. **符号与数字级的契约自相矛盾**——trace 记录 16B vs 结构体 20B、错误码子集同号两表、`-ENOSYS`/`-EOPNOTSUPP` 游离于子集外、链接段名单复数、栈 8 字节对齐违反 AAPCS64、APP「多线程不存在」反转主文档——这类问题会直接进实现与 CI 门禁。

## 意见文件索引

| 文件 | 覆盖范围 | 高 | 中 | 低 | 合计 |
|---|---|---|---|---|---|
| [`01-architecture-comment.md`](01-architecture-comment.md) | 00-architecture / 01-api-contract-governance / 02-roadmap | 0 | 16 | 24 | 40 |
| [`02-os-core-comment.md`](02-os-core-comment.md) | 08-core-api-list / 09-int / 10-sched / 11-memory / 12-plugin-mgr / 17-service-mgmt | 1 | 19 | 15 | 35 |
| [`03-plugin-debug-test-comment.md`](03-plugin-debug-test-comment.md) | 18-plugin-dev / 03-debug / 19-test | 2 | 7 | 10 | 19 |
| [`04-vfs-device-comment.md`](04-vfs-device-comment.md) | 04-vfs / 05-bdev / 06-device / 07-concrete-fs | 1 | 15 | 13 | 29 |
| [`05-peripheral-comment.md`](05-peripheral-comment.md) | 13-toolchain / 14-app / 15-interface / 16-service / README | 1 | 4 | 6 | 11 |
| [`06-cross-doc-consistency-comment.md`](06-cross-doc-consistency-comment.md) | 跨文档系统性主题（版本归属/命名契约/决策语义/错误码/引用/术语） | 2* | 17* | 11* | 30* |
| **合计（文件级 01–05）** | | **5** | **61** | **68** | **134** |

\* 06 号文件为系统视图, 与模块文件有意重叠（同一问题在模块文件记明细、在 06 记主题）, 不重复计入合计。

## 高优先级问题 Top 5（建议立即修复）

1. **[08-core-api-list L75] 线程栈「8 字节对齐」**——D9 定 aarch64 唯一 ISA, AAPCS64 要求 SP 16 字节对齐; 按此实现的初始栈帧可致 `stp/ldp` 对齐异常。→ 改「初始 SP 16 字节对齐（栈大小为 16 的倍数）」。
2. **[03-debug L10 vs L13–19] trace「定长 16B 记录」vs 结构体 20 字节**（tsc 4+evt 2+ctx 1+narg 1+arg[3] 12）——主文档 §11 与 roadmap L64 同写 16B, 三处文字与一处结构体冲突, 直接影响缓冲尺寸与记录布局。→ `arg[2]`+宏收两参, 或全链改 20B。
3. **[19-test L99] TC-MM-003 期望 `-ENOSYS`**——不在任何一份错误码子集内, 且违反本篇自己的不变量 R-4（「错误码 ∈ SD-10 子集」）, 扫描规则会判自己的用例红。→ 改 `-ENOTSUP` 或走决策记录补码。
4. **[04-vfs L109]「设备路径由 vfs-core 路由至 dev-core」**——D19 旧模型残留, 与 D21「vfs-core 纯化」（同篇 L9/L40/L116/L111 及主文档）直接矛盾, 反转核心决策。→ 改「经挂载表路由至 fs/devfs, 由 devfs 经 open_file 钩子接入 dev-core 侧」。
5. **[14-app L30]「APP 内多线程/多进程语义不存在」**——与主文档 §2.1「APP 内部仍可有并发（多线程）——这是调度框架存在的理由」直接相反, 且与本篇自己的「多线程约定」自相矛盾。→ 改「多进程语义不存在（无 fork/exec）; 多线程存在」。

## 系统性主题（详见 06 号文件）

- **错误码集合口径**（SD-10 同号两表 + ENOSYS/EOPNOTSUPP 游离 + 「映射」vs「零转换」）
- **命名与符号契约漂移**（`.tg_plugin`/`.tg_plugins`、`TRACE_EVT`/`TG_TRACE_EVT`、`TG_API_INLINE`/`TG_API_INLINE_FROZEN`、`tg_task_create` 参数顺序、`sched_ops` 幽灵槽位、`__start_.tg_plugin` 非法标识符）
- **生命周期与启动时序**（三相 vs 四相、PRE_ARCH 无定义、early_init「无堆」vs §9、start 回调悬空、BSS 清零顺序）
- **「冻结」时点与 tg-sched 升格前置**（M2/M3/v1 三口径; 三调度器矩阵全绿 vs preempt v2/tt v3）
- **版本归属冲突**（红区/金丝雀 v1.x vs v2.0、运行时挂载 v2 vs v3、debug bridge v1.0 vs v1.x、trace 16B/20B）
- **断裂引用清单**（E1–E11: 「06 §8 三层模式」、「04-vfs §0.1」、「07 §3」EROFS、DoD 编号、SD-12/13 错号等）

## 通过项亮点（这些是对的）

- 用户手写笔记（`00-architecture-comment.md`）的全部决定已在文档中落实且跨文档一致（18 项逐一核对）;
- 版本 milestone 全集一致: coop v1.0/preempt v2.0/tt v3.0、SMP v2b、动态加载 v3.0+鉴权、EROFS/ramdump/crypto/lwip v2.0、Rust 插件 v2.0、page cache/MPU/完整 ASan vx.0;
- 08 的「48 函数 + 3 宏」经独立点数**准确**（21+11+5+9+2）; CA-1~10 决策表与正文一致; `-ETIMEDOUT` 补录与 19-test 互证一致;
- 技术事实核对无误: `_IOC` 位布局与 Linux 完全一致、`lfs_config` 1:1、ASan shadow≈RAM/8、virtio-blk 与 flash 的 cache 维护方向正确、ISR→bh→信号量模型正确、EROFS 只读/littlefs 掉电安全表述正确;
- README 索引: 20 篇「成文/骨架」状态全部准确, 所有 `.md` 引用路径存在, 全局编号无跳号。

## 建议修复批次

1. **第一批（进实现前必须清零）**: Top 5 高危 + 错误码集合统一（SD-10 全集/子集方案）+ 链接段名统一。
2. **第二批（骨架深化前）**: 「契约名词对齐」清账——以 08 + 主文档为真值, 修 06 号文件 B/C 两组（命名漂移、决策语义漂移、生命周期相位、start 回调）; 建议同时建术语表（冻结/定稿、严格叶子、挂载表·单路由、双保险）。
3. **第三批（文档卫生）**: 断裂引用 E1–E11、各篇头部决策清单刷新（SD-15/13/14）、版本归属对表（roadmap vs 主文档 §11）、低危错别字与表达问题。

---
*评审方法说明: 5 个 subagent 分别负责 ①架构与路线 ②内核核心 ③插件/调试/测试 ④VFS/设备 ⑤外围模块+跨文档一致性, 主评审独立通读全部 21 篇后对每条 subagent 意见核对原文行号, 并补充自有发现; 严重度定义——高 = 误导设计决策或明显胡言乱语, 中 = 易误导读者或明显表达问题, 低 = 错别字/格式瑕疵; 标注「存疑」的条目为需要作者澄清意图的表述。*

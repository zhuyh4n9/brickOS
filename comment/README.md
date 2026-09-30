# TangramOS 设计文档评审(comment/ 总览)

> **⚠ 名称变更(2026-08, 晚于本存档)**: OS 已由 **TangramOS** 更名为 **brickOS**(D16 修订, 原名与既有项目命名冲突), 符号前缀 `tg_`→`br_`、宏 `TG_`→`BR_`、CLI `tg`→`br`、链接段 `.tg_*`→`.br_*`。**本目录是历史存档, 正文原样保留旧名与旧前缀**, 阅读时按上述映射替换即可; 现行文档见 [`docs/README.md`](../docs/README.md)。

> 评审对象: `tangramOS/docs/` 全部 21 篇设计文档(00–19, ~2730 行, 主文档 v0.9a)
> 评审任务: **设计是否完备(是否矛盾、技术点是否清晰、架构是否存在问题)**
> 评审方法: 6 个 subagent 分模块评审(架构治理路线图 / OS Core / 存储 VFS 设备 / Debug+Test / 插件生态 / 跨文档一致性专项) + 父 agent 全文精读交叉核实每条证据; 另有 subagent 自组织的一轮"文档表述质量"分模块走查
> 行号说明: 评审期间 `00-architecture.md` §3 图被并行重排(687→695 行); 架构/跨文档评审行号已按 695 行新版复核, 其余模块评审对主文档的行号引用有约 ±8 行漂移, 节号(§)不受影响; 各骨架/领域文档未被修改
> **编号变更提示(2026-08)**: 本目录是**历史评审存档**, 通篇引用**旧编号**(全局文档号 00–22 + 旧分类目录名)。现行编号见 [`docs/README.md`](../docs/README.md)。旧→新文档号: `00`→`1-01`, `01`→`1-02`, `02`→`1-03`, `03`→`5-01`, `04`→`7-01`, `05`→`7-02`, `06`→`8-01`, `07`→`7-03`, `08`→`3-01`, `09`→`3-02`, `10`→`3-03`, `11`→`3-04`, `12`→`3-05`, `13`→`2-01`, `14`→`9-01`, `15`→`10-01`, `16`→`11-01`, `17`→`3-06`, `18`→`4-01`, `19`→`6-01`, `20`→`4-02`, `21`→`4-03`, `22`→`4-04`。旧→新目录: `2-os-core`→`3-os-core`, `3-plugin-dev`/`3-plugin`→`4-plugin`, `4-debug`→`5-debug`, `5-test`→`6-test`, `6-vfs-device`→`7-storage`(其中 `06-device.md` 独立为 `8-device`), `7-toolchain`→`2-toolchain`, `8-app`→`9-app`, `9-interface`→`10-interface`, `10-service`→`11-service`。

## 目录结构

```
comment/
├── README.md                       # 本文件(总览)
├── review2/                        # ★ 设计完备性评审系列(主任务交付, P0/P1/P2 分级)
│   ├── 01-architecture-review.md   # 架构+契约治理+路线图(24 条: P0×3)
│   ├── 02-os-core-review.md        # OS Core: native API/中断/调度/内存/插件管理/服务(39 条: P0×2)
│   ├── 03-vfs-device-review.md     # 存储 VFS 设备: vfs/bdev/device/具体 FS(34 条: P0×2)
│   ├── 04-debug-test-review.md     # Debug + Test: trace/bridge/ramdump/ASan/用例矩阵(26 条: P0×5)
│   ├── 05-plugin-ecosystem-review.md # 插件开发/工具链/APP/接口/服务(25 条: P0×3)
│   ├── 06-cross-doc-consistency-review.md # 跨文档一致性专项+旧意见吸收核对(32 条: P0×1)
│   └── 07-absorption-check.md      # 旧评审意见(v0.3/v0.4)落实核对: 17/17 全部落实
└── review1/                        # 文档表述质量走查系列(subagent 自组织产出, 高/中/低 分级)
    ├── README.md                   # review1 系列总览(含 Top 5 高危)
    ├── 01-architecture-comment.md  # 00/01/02(40 条)
    ├── 02-os-core-comment.md       # 08–12/17(35 条)
    ├── 03-plugin-debug-test-comment.md # 18/03-debug/19-test(19 条)
    ├── 04-vfs-device-comment.md    # 04–07(29 条)
    ├── 05-peripheral-comment.md    # 13/14/15/16/README(11 条)
    └── 06-cross-doc-consistency-comment.md # 跨文档系统性主题收口(30 项)
```

两系列分工: **review2 = 设计层**(矛盾/完备性缺口/技术不清晰/架构问题, 按模块独立成篇、互相去重, 06 为跨文档收口); **review1 = 表述层**(技术性错误/前后矛盾/表达/引用, 按文档类别走查, 每条已核对行号)。约 20 条硬矛盾(16B/20B、段名、相位等)两系列有意各自收录(角度不同), 修复时以 review2 的行号为准。

历史输入: 原根目录的 `00-architecture-comment.md`(v0.3/v0.4 评审意见)在本次评审结束前被移出本目录; review2/07 的"落实核对"与 review1 各篇的"决策对照基准"即以它为对象, 引用仍可读。

## 总体结论

**设计的骨架是健康的**: D1–D23 决策表 + D12–D15 契约治理机器(golden/三态/三层门禁) + 依赖宪法四规则 + v1.0 插件清单/DoD 形成了完整闭环; D18 POSIX 双角色拆分、D21 设备经 devfs 接入、"core 收缩为 native API+注册表、一切能力皆插件"的演进路线自洽; 旧评审意见(v0.3 16 组 + v0.4 1 条)17/17 全部落实且可追溯; 版本里程碑、决策编号(D/CA/SD)、48 函数 API 清点在 21 篇间高度一致。

**主要风险集中在四个层面**:

1. **决策演进"改一半"**(最高优先): D20–D23 快速迭代后, 旧模型的残留文字与现行决策直接矛盾——D19 决策行/04 §4.1 仍写"vfs-core→dev-core 设备路由"、总体分层图 Platform 依赖方向画反(Platform→Scheduler)、§7.3 host 直通违反 D18 唯一主人、"无存储不链 vfs-core"承诺与 D21 冲突。
2. **元契约漂移**: 同一契约在两份"成文"文档中各持一套——链接段名 `.tg_plugin(s)`、`tg_sched_ops` 槽位名、生命周期相位(四相/三相/幻影 PRE_ARCH)、trace 宏名、错误码两张 SD-10 表、golden 生成器机制(头文件解析 vs 构建产物真值)。这些都处于"进 golden/进实现"的路径上, 是最贵的漂移。
3. **函数级契约大面积缺席**: 08 的 §3–§9(41/48 函数)无每函数错误/阻塞/ISR 规格且契约正在向 19-test 用例漂移; vfs-core 的会话私有数据({fops,fpriv} 所有权)、目录枚举、偏移归属、并发模型无定义; debug 侧 trace 的 Service 形态与 ISR 白名单/内建探针的时序矛盾。
4. **运行期错误面无设计**: 启动失败路径、v1.0 fault/panic 入口、看门狗全部悬空——对"新产品零开发只做组合"的 OS, init 失败的默认结局是静默挂死。

## 跨模块 P0 硬矛盾清单(去重后 12 项, 建议第一批修复)

| # | 矛盾 | 位置 | 详情 |
|---|---|---|---|
| 1 | Platform 在分层图中依赖方向画反(Platform→Scheduler), 与 §6.3"最底层"/§8 三层/§9 启动序冲突; 依赖宪法四规则不含 Platform/core 回调关系 | 主文档 §3 图 L159–160 | review2/01 P0-1 |
| 2 | D19 决策行 + 04-vfs §4.1 残留"vfs-core→dev-core 设备路由", 与 D21"vfs-core 纯化"直接矛盾(决策表是权威登记处, 漏修订标注) | 主文档 §1 D19 行 + 04 §4.1 | review2/01 P0-2, review2/03 P0-1 |
| 3 | §7.3 "host 直通: iface-posix 可直通宿主 open()" 违反 D18"POSIX 唯一实现 + fd 表唯一主人" | 主文档 §7.3 L467 | review2/01 P0-3 |
| 4 | "无存储产品不链 vfs-core"裁剪承诺与 D21"所有设备经 /dev"矛盾, cdev 无非 VFS 会话 API, 承诺无从兑现 | 06-device §1 L103 | review2/03 P0-2 |
| 5 | trace 声明为 Service 插件(LATE init), 但内建探针(插件生命周期/调度/IRQ)发射点在 core 与 EARLY 调度插件, 且 trace 宏在 core 的 ISR 白名单契约中——init 时序与依赖方向都不成立 | 03-debug §1 | review2/04 P0-1 |
| 6 | "定长 16B 记录"与 `tg_trace_evt_t` 实际 20 字节(4+2+1+1+12)三处冲突 | 03-debug §1 / 主文档 §11 / roadmap | review2/04 P0-2, review1 全系 |
| 7 | TC-MM-003 期望 -ENOSYS 不在任何错误码子集, 违反 19-test 自身 R-4; SD-10 存在两张不同子集表(core 域 9 码/设备域 7 码)却都称"统一" | 19-test L99 / 08 §11 / 06 §4 | review2/04 P0-3, review2/06 P1 |
| 8 | "host 完整 ASan 白捡"与 host 上跑真实 TLSF 的 TC-MEM 组互斥(或 ASan 不见 TLSF 池, 或矩阵测 glibc) | 03-debug §4 + 19-test §2 | review2/04 P0-4 |
| 9 | TLSF 红区/金丝雀版本归属分裂: v1.x(00/03) vs v2.0(roadmap); bridge 同病 v1.x vs v1.0(M3) | 00 §11 / 03 §4 vs 02-roadmap | review2/04 P0-5 |
| 10 | 生命周期"相位"模型三处矛盾: 主文档四相 vs 12-plugin-mgr 三相+幻影子相(PRE_ARCH/PIC/ARCH 全树无定义); 描述符 start 回调在启动序列中无执行点; 回调↔相位映射无定义 | 12-plugin-mgr L13 / 主文档 §6.2/§9 | review2/05 P0-1, review2/02/06 |
| 11 | golden 生成器机制与已定决策矛盾: 13-toolchain 写"头文件解析", 已定机制是"构建产物符号表(nm)+abidiff, 真值是二进制, CI 独立重生成防造假"(CA-10/01 §2.6.4) | 13-toolchain §2 L23 | review2/05 P0-2, review2/06 |
| 12 | APP 依赖规则三说不一: 主文档 §6.3 允许"直调 native", 18 路由表/14-app 写必须依赖 Interface, 14-app 内部两句自相矛盾 | 14-app L8/L11 / 主文档 §6.3 | review2/05 P0-3 |

## 统计

| 系列 | 范围 | P0/高 | P1/中 | P2/低 | 合计 |
|---|---|---|---|---|---|
| review2/01 架构+治理+路线图 | 00/01/02 | 3 | 11 | 10 | 24 |
| review2/02 OS Core | 08/09/10/11/12/17 | 2 | 17 | 20 | 39 |
| review2/03 存储 VFS 设备 | 04/05/06/07 | 2 | 17 | 15 | 34 |
| review2/04 Debug+Test | 03-debug/19-test | 5 | 14 | 7 | 26 |
| review2/05 插件生态 | 18/13/14/15/16 | 3 | 13 | 9 | 25 |
| review2/06 跨文档一致性 | 全 21 篇横切 | 1 | 15 | 16 | 32 |
| **review2 小计** | | **16** | **87** | **77** | **180** |
| review1/01–05 表述质量 | 全 21 篇分模块 | 5 | 61 | 68 | 134 |
| review1/06 跨文档主题收口 | ≥2 篇系统主题 | 2 | 17 | 11 | 30(与模块文件有意重叠, 不计入合计) |
| review2/07 旧意见落实核对 | v0.3+v0.4 | — | — | — | 17/17 全部落实 |

## 建议修复批次

1. **第一批(进实现/骨架深化前必须清零, 约半天)**: 上表 12 项 P0 硬矛盾 + 错误码全集统一(补 -ENOENT/-ENOTDIR/-EISDIR/-EXDEV 等 VFS 域码、统一 -ENOTSUP 用词) + 链接段名统一(`.tg_plugin` vs `.tg_plugins` 二选一全局替换, 并修 `__start_.tg_plugin` 非法 C 标识符问题)。
2. **第二批(契约面定稿, 阻塞 M2/M3)**: 08 §3–§9 每函数规格补齐并把 19-test 已发明的契约回填; `tg_sched_ops` 唯一全集定稿(含 TCB 尺寸/分配协议); D23 open 会话协议闭合(fpriv 所有权 + 目录枚举 + 偏移归属); trace 形态裁决(core 设施 vs EARLY 特例); 生命周期"回调×相位×可用资源"矩阵。
3. **第三批(设计补课, v1.0 DoD 扩充)**: 启动失败路径 + v1 最小 fault 入口 + panic 原语; 镜像与启动契约(DoD 第 9 项); 性能预算表; 静态分析器(D10 后半)立项或降级声明; 依赖宪法"插件类别"字段(规则 1 可执法化)。
4. **第四批(文档卫生)**: 断裂引用(E1–E11)、各篇头部决策清单刷新(SD-13/14/15)、版本归属对表(roadmap vs 主文档 §11)、错别字。

## 通过项亮点(这些是对的)

- 旧评审意见(v0.3 22 条 + v0.4 1 条)全部落实且跨文档一致, 吸收痕迹可追溯(版本头部"吸收评审意见"标注、各节"评审要求/排序/修订"标注)
- 版本 milestone 全集一致: coop v1.0/preempt v2.0/tt v3.0、SMP v2b、动态加载 v3+鉴权、EROFS/ramdump/crypto/lwip v2.0、Rust 插件 v2.0、page cache/MPU/完整 ASan vx.0
- 08 的"48 函数+3 宏"经独立清点**准确**(21+11+5+9+2); CA-1~10 决策链完整; ISR 白名单四件跨文档一致
- 技术事实核对无误: `_IOC` 位布局与 Linux 一致、`lfs_config` 近 1:1、ASan shadow≈RAM/8、DMA/cache 维护方向、ISR→bh→信号量模型、EROFS/littlefs 特性
- README 索引 20 篇状态标注全部准确, 全局编号无跳号, 所有 .md 引用路径存在(仅引用节号有个别错链, 见 review1/06)

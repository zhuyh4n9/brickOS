# brickOS 设计文档索引

> **编号规则**: 文件名 = `<章节号>-<章节内序号>-<文档名>.md`, 目录 = 章节 `<章节号>-<章节名>`。
> **章节号** = 文档在 11 个章节中的归属(章节调整 ⇒ 目录与文件名前缀一起改); **章节内序号** = 该篇在本章节内的顺序(01 起, 新增文档顺排到本章末尾)。
> **toolchain 为第 2 章**(2026-08 调整): 契约优先于实现, 组合器 + CI 门禁是仅次于架构的持久工程资产, 先于内核各子系统阅读。
> **storage 与 device 分立为第 7/8 章**(2026-08 调整): 原 `vfs + device` 一章按"面向文件 vs 面向设备"拆开——存储域(VFS / 块设备 / 具体 FS)与设备域(设备体系与管理)。理由见下「为什么 storage 与 device 分立」。
> 状态: **成文** = 已有实质内容; **骨架** = 范围/大纲/来源已定, 待深化(来源 = 主文档现有章节); **骨架(预留)** = 新占位: 范围与分工已声明, 内容待首次细化; **讨论稿** = 备选方案/权衡/开放问题已列出, 结论待拍板。
> **缩略词**: 每篇正文前有「缩略词」小节, 只列**本篇实际用到**的缩写与编号前缀(主文档 `1-01` 最全); 新增文档时同样补一节, 条目按字母序。

| 章节 | 分类 | 文档 | 状态 |
|---|---|---|---|
| 1 | **architecture**(架构与路线) | [`1-01-architecture.md`](1-architecture/1-01-architecture.md) 主文档(D1–D26 决策表 + §14.1 域支撑矩阵) | 成文 |
| | | [`1-02-api-contract-governance.md`](1-architecture/1-02-api-contract-governance.md) 契约治理(D12–D15 + §2.6 管理机制) | 成文 |
| | | [`1-03-roadmap.md`](1-architecture/1-03-roadmap.md) 版本路线图 + DoD 清单 | 成文 |
| 2 | **toolchain**(工具链) | [`2-01-toolchain.md`](2-toolchain/2-01-toolchain.md) 工具链总纲(CLI/manifest 工具实现/golden-CI) | 骨架 |
| | | [`2-02-br-arch.md`](2-toolchain/2-02-br-arch.md) **br 工具架构设计**(备选方案/权衡/开放问题) | 讨论稿 |
| 3 | **os core**(内核核心) | [`3-01-core-api-list.md`](3-os-core/3-01-core-api-list.md) native API 清单与详细设计(CA-1~10) | 成文 |
| | | [`3-02-int.md`](3-os-core/3-02-int.md) 中断管理(IRQ 框架/PIC 抽象/生命周期/屏蔽三层/级联域/fault/IR-1~15) | 成文 |
| | | [`3-03-sched.md`](3-os-core/3-03-sched.md) 调度框架(sched_class/br_sched_ops) | 骨架 |
| | | [`3-04-memory.md`](3-os-core/3-04-memory.md) 内存(三池/DMA/br_mm/布局) | 骨架 |
| | | [`3-05-plugin-mgr.md`](3-os-core/3-05-plugin-mgr.md) **插件管理器**(描述符/生命周期/init-DAG 执行; manifest 语义 → 4-03) | 骨架 |
| | | [`3-06-service-mgmt.md`](3-os-core/3-06-service-mgmt.md) 服务管理(注册表语义/单一主人/D7) | 骨架 |
| 4 | **plugin**(插件体系) | [`4-01-plugin-dev.md`](4-plugin/4-01-plugin-dev.md) 插件开发指南(八类路由/通用流程/纪律/合规) | 骨架 |
| | | [`4-02-plugin-layout.md`](4-plugin/4-02-plugin-layout.md) 插件布局(目录组织/命名/发现约定/物理映射) | 骨架(预留) |
| | | [`4-03-plugin-manifest.md`](4-plugin/4-03-plugin-manifest.md) manifest 管理(格式/schema/真值裁定/校验输入) | 骨架(预留) |
| | | [`4-04-plugin-deps.md`](4-plugin/4-04-plugin-deps.md) 依赖管理(声明/版本区间/init-DAG 规则/分类学约束/闭包) | 骨架(预留) |
| 5 | **debug** | [`5-01-debug.md`](5-debug/5-01-debug.md) trace/bridge/ramdump/ASan | 成文 |
| 6 | **test** | [`6-01-test.md`](6-test/6-01-test.md) **测试框架与 core-api 兼容性用例**(49 用例 × 三调度器矩阵; 含 TC-HSM) | 成文(用例目录) |
| 7 | **storage**(存储域) | [`7-01-vfs.md`](7-storage/7-01-vfs.md) VFS(ops 四层/挂载/SD-1/3/4/7/15) | 成文 |
| | | [`7-02-bdev.md`](7-storage/7-02-bdev.md) 块设备(堆叠/分区/page cache) | 成文 |
| | | [`7-03-concrete-fs.md`](7-storage/7-03-concrete-fs.md) 具体文件系统(tmpfs/devfs/littlefs/EROFS) | 成文 |
| 8 | **device**(设备域) | [`8-01-device.md`](8-device/8-01-device.md) 设备体系与三种组合形态(形态 A/B/C)/ 子分类 / 驱动契约(SD-2~14) | 成文 |
| 9 | **app**(应用) | [`9-01-app.md`](9-app/9-01-app.md) 单 APP 模型/入口/生命周期 | 骨架 |
| | | [`9-02-hsm-sample.md`](9-app/9-02-hsm-sample.md) **HSM 完整样例**(v1.x/M5, D24–D26: 第二产品域纵向切片 + 域支撑矩阵/资产边界) | 成文 |
| 10 | **interface management** | [`10-01-interface.md`](10-interface/10-01-interface.md) 接口插件(严格叶子/再导出) | 骨架 |
| 11 | **service**(自研与三方服务) | [`11-01-service.md`](11-service/11-01-service.md) svc-posix/trace/lwip/三方移植 | 骨架 |

**引用写法**: 跨文档引用用**文件号**(如 `3-01 §13.4`、`docs/8-device/8-01-device.md`), 不要只写名字——章节调整后名字不变而号会变, 号是唯一身份。

**为什么 storage 与 device 分立**: 原 `vfs + device` 一章混装两种读者视角——**面向文件**(VFS/挂载/具体 FS)与**面向设备**(注册表/子分类/驱动契约)。二者共用一条体系链(deviceXXX → 具体设备类 → dev-core → VFS), 但**不是同一条依赖链**: 设备框架可以在没有 VFS 的组合里独立成立(简易 OS), standalone 器件甚至连 dev-core 都不需要。所以拆成第 7 章 storage(VFS 与 FS 之上)与第 8 章 device(设备体系本身); `8-01` §1.2 把这条谱系显式写成**三种组合形态**(A 体系化接入 VFS / B 只用设备框架 / C standalone), 使"框架件可按组合裁剪"(D19)从口号变成可勾选的形态。bdev-core 是设备子分类、但其文档随块存储视角留在第 7 章(`7-02-bdev`), 设备章只引用不复制。

**debug 与 test 分立**: 5-01-debug = 观测与诊断设施(trace/bridge/ramdump/ASan); 6-01-test = **验证体系**(层 2 语义用例目录 × 三调度器矩阵)——测试失败经 debug 设施上报(6-01 §2), 但两者职责正交: debug 回答"出了什么事", test 回答"行为是否还是契约"。

**9-02 为什么与 9-01 分开**: 9-01 = APP **类别的通用契约**(入口/生命周期/manifest/多线程约定); 9-02 = **HSM 完整样例**(v1.x/M5, D24–D26), 是"新产品 OS 零开发, 只做组合"(§0)的**可运行判例**——它跨 8-device/7-storage/11-service/10-interface 四章, 需要自己的范围/插件清单/验收标准(§11 A1–A6)。两者关系 = 通用契约 vs 首个完整消费者(策略在 APP、协议在服务)。样例的第二功能是**缺陷发现器**: 熵源契约(O-H1)、`-EPERM` 错误码(O-H8)、crypto ops 是否入 golden(O-H7)三个缺口均由它前置暴露。

**为什么 4-plugin 独立成类**: 所有功能扩展均依赖插件——驱动/FS/服务/接口/平台/调度器/框架件/APP 全部是插件。八类契约分散在领域文档, 但**作者流程与纪律共享**, 本类是插件作者的横切入口(4-01); 同时承载插件体系的**声明面语义**: 布局(4-02)/ manifest(4-03)/ 依赖(4-04)——运行期管理面在 3-05, 工具实现在 2-01; 各类纵深契约在 3/7/8/10/11 类。

**为什么 toolchain 是第 2 章**: 组合器把"插件组合"变成"可启动镜像", CI 门禁把契约变成执法——工具链是**契约的执行者**, 其形态决定其余各章的写法(manifest 格式、golden 划分、conformance 矩阵都落在工具侧)。放在第 2 章 = 读完架构先读工具链, 再进内核各子系统。

**骨架文档的分工**: 主文档(`1-01`)保留全局决策与各子系统概要; 骨架文档是"拆出去深化"的落点——每篇声明范围、大纲、从主文档继承的决策, 待逐篇填实后主文档对应章节收缩为指针(与 7-01–7-03 + 8-01 的拆分模式相同)。

**3-os-core 内的分界**: `3-05-plugin-mgr` = 插件体系运行期管理面(描述符/生命周期/init-DAG 执行); `3-06-service-mgmt` = 服务注册表语义与规则; `2-01-toolchain` = 组合器与 CI 的工具实现(manifest/依赖求解的工具侧); **manifest 与依赖的语义** → `4-03`、`4-04`; **br 工具自身的架构** → `2-02`。

# TangramOS 设计文档索引

> **组织原则**: 目录 = 分类(10 类); **全局编号 = 文档稳定身份**(引用不因分类调整而断链, RFC 风格)。
> 状态: **成文** = 已有实质内容; **骨架** = 范围/大纲/来源已定, 待深化(来源 = 主文档现有章节)。

| # | 分类 | 文档 | 状态 |
|---|---|---|---|
| 1 | **architecture**(架构与路线) | [`00-architecture.md`](1-architecture/00-architecture.md) 主文档(D1–D23 决策表) | 成文 |
| | | [`01-api-contract-governance.md`](1-architecture/01-api-contract-governance.md) 契约治理(D12–D15 + §2.6 管理机制) | 成文 |
| | | [`02-roadmap.md`](1-architecture/02-roadmap.md) 版本路线图 + DoD 清单 | 成文 |
| 2 | **os core**(内核核心) | [`08-core-api-list.md`](2-os-core/08-core-api-list.md) native API 清单与详细设计(CA-1~10) | 成文 |
| | | [`09-int.md`](2-os-core/09-int.md) 中断管理(IRQ 框架/ISR 契约/bh/D8) | 骨架 |
| | | [`10-sched.md`](2-os-core/10-sched.md) 调度框架(sched_class/tg_sched_ops) | 骨架 |
| | | [`11-memory.md`](2-os-core/11-memory.md) 内存(三池/DMA/tg_mm/布局) | 骨架 |
| | | [`12-plugin-mgr.md`](2-os-core/12-plugin-mgr.md) **插件管理器**(描述符/生命周期/init-DAG/manifest) | 骨架 |
| | | [`17-service-mgmt.md`](2-os-core/17-service-mgmt.md) 服务管理(注册表语义/单一主人/D7) | 骨架 |
| 3 | **plugin development**(插件开发) | [`18-plugin-dev.md`](3-plugin-dev/18-plugin-dev.md) 插件开发指南(八类路由/通用流程/纪律/合规) | 骨架 |
| 4 | **debug** | [`03-debug.md`](4-debug/03-debug.md) trace/bridge/ramdump/ASan | 成文 |
| 5 | **test** | [`19-test.md`](5-test/19-test.md) **测试框架与 core-api 兼容性用例**(43 用例 × 三调度器矩阵) | 成文(用例目录) |
| 6 | **vfs + device management** | [`04-vfs.md`](6-vfs-device/04-vfs.md) VFS(ops 四层/挂载/SD-1/3/4/7/15) | 成文 |
| | | [`05-bdev.md`](6-vfs-device/05-bdev.md) 块设备(堆叠/分区/page cache) | 成文 |
| | | [`06-device.md`](6-vfs-device/06-device.md) 设备管理(域总览/子分类/SD-2~14) | 成文 |
| | | [`07-concrete-fs.md`](6-vfs-device/07-concrete-fs.md) 具体文件系统(tmpfs/devfs/littlefs/EROFS) | 成文 |
| 7 | **toolchain**(工具链) | [`13-toolchain.md`](7-toolchain/13-toolchain.md) CLI/manifest 工具实现/golden-CI | 骨架 |
| 8 | **app**(应用) | [`14-app.md`](8-app/14-app.md) 单 APP 模型/入口/生命周期 | 骨架 |
| 9 | **interface management** | [`15-interface.md`](9-interface/15-interface.md) 接口插件(严格叶子/再导出) | 骨架 |
| 10 | **service**(自研与三方服务) | [`16-service.md`](10-service/16-service.md) svc-posix/trace/lwip/三方移植 | 骨架 |

**debug 与 test 分立**: 03-debug = 观测与诊断设施(trace/bridge/ramdump/ASan); 19-test = **验证体系**(层 2 语义用例目录 × 三调度器矩阵)——测试失败经 debug 设施上报(19 §2), 但两者职责正交: debug 回答"出了什么事", test 回答"行为是否还是契约"。

**为什么 3-plugin-dev 独立成类**: 所有功能扩展均依赖插件——驱动/FS/服务/接口/平台/调度器/框架件/APP 全部是插件。八类契约分散在领域文档, 但**作者流程与纪律共享**, 本类是插件作者的横切入口(18); 各类纵深契约在 2/6/9/10 类。

**骨架文档的分工**: 主文档(`00`)保留全局决策与各子系统概要; 骨架文档是"拆出去深化"的落点——每篇声明范围、大纲、从主文档继承的决策, 待逐篇填实后主文档对应章节收缩为指针(与 04–07 的拆分模式相同)。

**2-os-core 内的分界**: `12-plugin-mgr` = 插件体系管理面(描述符/生命周期/init-DAG/manifest 语义); `17-service-mgmt` = 服务注册表语义与规则; `13-toolchain` = 组合器与 CI 的工具实现(manifest 的工具侧)。

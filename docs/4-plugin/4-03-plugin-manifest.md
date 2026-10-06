# 4-03 — manifest 管理(plugin manifest)

> 章节: **4-plugin**。状态: **骨架(预留, 待细化)**。
> 来源: 主文档 §6.4(组合期校验六项)/§6.1(deps/res 描述符字段)/§7.3(双层粒度 D13)/§10(page cache 静态预算)/§13(brickie CLI); 7-03 §6(挂载计划); 3-05 §2 第 1 项(manifest 定稿原归属, **迁本篇**); 2-01 §2 第 1/3 项(校验器/生成物)。
> 分工: 本篇 = manifest 的**语义与格式**(schema/真值裁定/校验输入); 工具实现(校验器/闭包求解/生成物)→ `docs/2-toolchain/2-01-toolchain.md`; 组合期校验规则清单与运行期编排 → `docs/3-os-core/3-05-plugin-mgr.md`; 作者用法 → 本类 `4-01-plugin-dev.md` §1。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **CI** | Continuous Integration | 持续集成(三层门禁的落地处) |
| **CLI** | Command-Line Interface | 命令行界面 |
| **DMA** | Direct Memory Access | 直接内存访问(外设不经 CPU 读写内存) |
| **DoD** | Definition of Done | 完成定义(此仓库指设计阶段收敛清单) |
| **DSL** | Domain-Specific Language | 领域专用语言(manifest 表达格式候选之一) |
| **IRQ** | Interrupt Request | 中断请求(硬件中断线) |
| **page cache** | — | 页缓存: 堆叠于 bdev 之上的无感层(SD-8) |
| **RAM** | Random Access Memory | 随机访问存储器 |
| **TOML** | Tom's Obvious Minimal Language | 配置文件格式(manifest 表达格式候选) |
| **YAML** | YAML Ain't Markup Language | 配置文件格式(manifest 表达格式候选) |

> **编号约定**: `D13` = 双层粒度(模块级声明 + 符号级真值); `§6.4` = 组合期校验六项。

## 1. 范围与现状(预留)

manifest = **产品级组合输入**(选哪些插件/怎么组合/预算多少), 与描述符(插件自描述, `BR_PLUGIN` 宏, 3-05)相对。

已有事实(散点, 待收口):

- manifest 承载(4-01 §1): 依赖声明 / 资源声明(IRQ/DMA/引脚)/ RAM 预算; 挂载计划(7-03 §6); page cache 静态预算"manifest 定死"(§10)
- **双真值问题**: deps/res 在描述符(§6.1)与 manifest 两处声明, 权威与同步机制未定义——本篇是收口落点
- 双层粒度(D13, §7.3): manifest 模块级声明+版本(人读); 符号级碰撞检测以链接器符号表为唯一真值
- 生成物(2-01 §2 第 3 项): manifest.c/h、链接脚本片段、挂载计划表(`br_fs_cfg` 等)——方向: manifest → 生成物
- 表达格式: YAML / TOML / 自定义 DSL [?](原 3-05 §3 开放问题, 迁本篇)
- RAM 预算 arena 记账(v2)(4-01 §5); Σ(插件 RAM/栈) ≤ platform 提供量(§6.4-5)

## 2. 大纲(预留, 待细化)

1. **格式选型**: YAML / TOML / 自定义 DSL 取舍 [TODO: 结论 + 理由 + 工具依赖面]
2. **schema 定稿**: 字段全集与语义 [TODO: 插件选择/版本区间/资源/挂载计划/sched_class 引用/预算/产品参数]
3. **真值裁定**: 描述符 vs manifest 的权威边界; 不一致时以谁为准; `brickie check` 一致性校验(类比 1-02 §2.6.1 单一事实源纪律) [TODO]
4. **生成物契约**: manifest → manifest.c/h / 链接脚本片段 / 挂载计划表 [TODO]
5. **校验挂接**: §6.4 六项组合期校验中 manifest 侧输入与责任划分(与 3-05 的边界) [TODO]
6. **版本与演化**: manifest 自身版本字段/前向兼容 [TODO; 与 4-04 版本区间联动]

## 3. 开放问题(预留)

| # | 问题 |
|---|---|
| — | 表达格式(YAML? TOML? DSL?)[?] |
| — | 双真值: 权威方 + CI 同步检查形态 [?] |
| — | 分层: 产品 manifest 与插件级声明是否两级(选择 vs 自述) [?] |
| — | 挂载计划 schema 归属: 本篇 vs `7-03-concrete-fs` §6 [?] |
| — | 多产品共库: 同一插件树多份产品 manifest 的组织 [?] |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 4 项(**manifest 文件格式定稿** + 校验规则清单)——与 3-05(描述符侧)共同交付; 原归属 3-05 §2 第 1 项, 已迁本篇。

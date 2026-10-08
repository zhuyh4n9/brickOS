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
- **双真值问题的权威已裁**(A-9/BRV-D4): **插件自述在 `plugin.toml`(唯一真值), 产品选择在 `product.toml`** —— 插件的 `dep`/`export`/`privileged` **只许**出现在 `plugin.toml`, 产品 manifest 只做"选谁 + 预算 + 产品参数", 重复插件自述字段 ⇒ `BRV-MF-0007`; 描述符(`BR_PLUGIN`)是**生成物**而非第二真值(见 §2 第 3 项)
- 双层粒度(D13, §7.3): manifest 模块级声明+版本(人读); 符号级碰撞检测以链接器符号表为唯一真值
- 生成物(2-01 §2 第 3 项): manifest.c/h、链接脚本片段、挂载计划表(`br_fs_cfg` 等)——方向: manifest → 生成物
- **表达格式已定(A-4)**: **TOML**(关闭原 3-05 §3 开放问题)。三条纪律: 禁用 TOML 隐式特性(一律显式键); `--json` 输出与 TOML 输入**同源**(同一内部模型的两个序列化); schema 以 **JSON Schema** 单一形式描述并放 `brickie/schema/`
- RAM 预算的输入已定(S-2): **per-plugin `[[res]]{kind = ram|stack, used_kib}`**(插件自述) 求和后在组合期与 `product.toml [budget]{ram_kib,stack_kib}` 上限比对; `[privileged.resources]`(IRQ/DMA/引脚/设备名)只做**独占冲突**检测, 不加总
- **`sched_class` 的声明处已定**(S-2): 每插件在 `plugin.toml` 声明 `[plugin].sched_class`(缺省 `SAFE_PREEMPT`); 产品 manifest 只**引用/选择**调度器插件, 不重复声明各插件的调度兼容类别
- **导出面字段属于插件自述**(S-6 / A-11; `brickie` v0.1 §3.5): `[[export]]` 的 `api_iface`(三值域)/`form`/`reexport_of`(**列表**, `form="skin"` 时非空)只在 `plugin.toml`; 产品 manifest 不得重复

## 2. 大纲(预留, 待细化)

1. **格式选型**: ~~YAML / TOML / 自定义 DSL 取舍~~ → **结论 = TOML**(A-4): 与 Rust 生态一致、标准库级解析器成熟(`tomllib`/`toml`)、数据类型显式; 代价 = 深层嵌套啰嗦、无锚点
2. **schema 定稿**: 字段全集与语义 —— **v0.1 草案已给出**(`brickie` v0.1 §8.1 `plugin.toml` / §8.2 `product.toml`), 本篇据此收敛: 插件自述(`[[dep]]`/`[[res]]`/`[[export]]`/`[privileged]`/`[plugin].phase`·`sched_class`) vs 产品选择(`[select]`/`[budget]`/`[lint]`/`[product].stage`)
3. **真值裁定**: **权威 = 插件自述 `plugin.toml` + 产品选择 `product.toml`; 描述符是生成物**; 不一致时以输入为准并重生成(`brickie gen --check` 逐字节比对), 手改生成物即违规(与 golden 同纪律)
4. **生成物契约**: manifest → manifest.c/h / 链接脚本片段 / 挂载计划表 [TODO: 与 2-01 §2 第 3 项共同细化]
5. **校验挂接**: §6.4 六项组合期校验中 manifest 侧输入与责任划分(与 3-05 的边界)——**v0.1 的覆盖度表**见 `brickie` v0.1 §2(第 3/5 项所需的 `sched_class` 与 `[[res]]` 已入 schema)
6. **版本与演化**: manifest 自身版本字段/前向兼容 [TODO; 与 4-04 版本区间联动——注意产品 manifest 的 `version` 也是四段]

## 3. 开放问题(预留)

| # | 问题 | 状态 |
|---|---|---|
| ~~—~~ | ~~表达格式(YAML? TOML? DSL?)~~ | **✅ 已答(A-4)**: **TOML** |
| ~~—~~ | ~~双真值: 权威方 + CI 同步检查形态~~ | **✅ 已答**: 权威 = `plugin.toml`(自述)+ `product.toml`(选择); 描述符为生成物, CI 用 `brickie gen --check` 逐字节比对 |
| ~~—~~ | ~~分层: 产品 manifest 与插件级声明是否两级(选择 vs 自述)~~ | **✅ 已答**: **两级**(BRV-D4); 产品 manifest **不得**重复插件自述字段(`BRV-MF-0007`) |
| — | 挂载计划 schema 归属: 本篇 vs `7-03-concrete-fs` §6 | **裁定**: **归 `7-03` §6**; 本篇只承载"挂载计划在产品 manifest 中的引用形态"(`brickie` v0.1 §5.3 标注 v0.1 不涉及其语义) |
| — | 多产品共库: 同一插件树多份产品 manifest 的组织 | 待定(v0.1 **不涉及**; 归 `4-03` 与 `2-01` §2 深化, 见 `brickie` v0.1 §5.3) |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 4 项(**manifest 文件格式定稿** + 校验规则清单)——与 3-05(描述符侧)共同交付; 原归属 3-05 §2 第 1 项, 已迁本篇。

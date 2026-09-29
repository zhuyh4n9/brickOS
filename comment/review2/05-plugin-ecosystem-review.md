# 插件生态 + 工具链 + APP + 接口 + 服务 模块评审意见(18/13/14/15/16)

> 评审人: subagent(插件生态模块, 证据经父 agent 抽查核实)
> 评审对象: `tangramOS/docs/3-plugin-dev/18-plugin-dev.md`、`7-toolchain/13-toolchain.md`、`8-app/14-app.md`、`9-interface/15-interface.md`、`10-service/16-service.md`
> 评审维度: 完备性(矛盾、技术点是否清晰、架构是否存在问题)
> 意见分档: P0 = 矛盾/架构缺陷(必须解决); P1 = 完备性缺口/技术不清晰(应当解决); P2 = 改进建议

## 模块总评

五篇骨架的"分工声明 + 决策继承"整体质量较高: D18(svc-posix 双角色/接口严格叶子)在 18/14/15/16 四篇与主文档 §6/§7 完全一致, "八类"计数全文档集无矛盾, 13/14/15/16 的大纲基本兑现 README 的范围承诺, 18 §3/§4 的驱动/FS 契约摘要与 `06-device` §6、`04-vfs` §2、`07-concrete-fs` §6 逐条核对无误。最大风险有三: 其一, **生命周期"相位"模型三处互不一致**(主文档四相 vs 12-plugin-mgr 三相+PRE_ARCH/PIC/ARCH 子相), 且描述符三个 init 回调与相位/启动序列的映射缺失, 直接击穿 18 作为"作者入口"的核心指令"实现 init(选相位)"; 其二, **13-toolchain 把 golden 生成器写成"头文件解析", 与已定决策"构建产物符号表为唯一真值"(CA-10/01 §2.6.4)方向相反**; 其三, 作者体验的关键落点(错误码、日志、sched_class 声明、描述符写法)在作者入口文档 18 中大面积缺失。骨架阶段允许留白, 但上述矛盾会在工具链与 conformance 实现的第一天兑现为返工。

---

## P0 — 矛盾/架构缺陷

### [P0] 生命周期"相位"模型三处矛盾, 描述符回调与相位映射缺失
- **位置**: `18-plugin-dev.md` §1/L11; 交叉: `00-architecture.md` §6.2/L335、§6.1/L323–325、§9/L562–566; `12-plugin-mgr.md` §1/L13
- **问题**: (a) 相位数不一致: 主文档 §6.2 为四相 `EARLY → CORE → LATE → APP`, 12-plugin-mgr 继承时写成"三相"且 EARLY 带子相, APP 相被丢弃; (b) 子相 `PRE_ARCH/PIC/ARCH` 在主文档 §6.2/§9 中不存在(grep 全文档集仅 12 与 11-memory 引用); (c) 描述符三个回调(early_init/init/start)与四相的映射无定义——尤其 `start`("中断可用, 可创建线程")在 §9 启动序列中**没有执行点**(LATE 之后才全局开中断, 紧接 `app.start()`, 中间无各插件 start 回调步骤); (d) 18 让作者"选相位", 但相位清单在两处文档互相矛盾, 作者无从选择, 且描述符中声明"自身 init 相位"的机制也未定义(deps 条目带 phase, 但插件自己的相位字段没有)。
- **证据**: "→ 实现 init(选相位)+ ops 表(类别契约)"(18 L11); "`EARLY → CORE → LATE → APP`, 同阶段内按依赖拓扑序"(00 L335); "**生命周期**(§6.2): EARLY(PRE_ARCH/PIC/ARCH)/CORE/LATE 三相"(12 L13); "int (*start)(void); /* 中断可用, 可创建线程 */"(00 L325) vs §9 ":全局开中断; :app.start() 创建 APP 线程"(00 L564–566)。
- **建议**: 在 12-plugin-mgr 定稿唯一相位模型(建议以主文档四相为准, 或修订主文档并正式定义 PRE_ARCH/PIC/ARCH), 给出"回调 × 相位 × 该时刻可用资源"矩阵(early_init→EARLY; init→CORE/LATE; start→开中断后、app.start 之前补一步"各插件 start"); 18 §1 的"选相位"改为引用该矩阵。

### [P0] golden 生成机制与已定决策矛盾(头文件解析 vs 构建产物符号表)
- **位置**: `13-toolchain.md` §2/L23; 交叉: `01-api-contract-governance.md` §2.6.4/L168–174、§2.6.1/L114; `08-core-api-list.md` §13.4/L345
- **问题**: 13 作为 golden 生成器的**实现归属文档**, 把生成机制写成"头文件解析", 与已定稿的 D12 机械落地(01 §2.6)和 CA-10 相反——已定机制是"扫构建产物符号表(nm)+ abidiff, 真值是二进制", 且 CI 必须"独立重生成比对(防手编 golden 造假)"。头文件解析路线会绕过布局校验与防造假设计。13 继承的是 01 §2.3 中已被 §2.6.4 淘汰的旧备选("或自研头文件解析")。
- **证据**: "5. golden 生成器: 头文件解析 → `api/frozen/*.txt`; abidiff 集成"(13 L23); "构建(libtgcore.a) → 生成器(nm --defined-only + abidiff) → api/frozen/<组>.txt"(01 L168–169); "CI 用**独立重生成**比对(防手编 golden 造假)"(01 L174); "生成器扫构建产物符号表(`nm --defined-only` 过滤)→ `api/frozen/*.txt`——**真值是二进制, 不是文档**"(08 L345)。
- **建议**: 13 §2 第 5 条改为"golden 生成器: 扫构建产物符号表(nm --defined-only)→ api/frozen/*.txt; abidiff 看结构布局; CI 独立重生成比对(01 §2.6.4)"。

### [P0] APP 依赖规则三说不一(直调 native / 必须 Interface / 只有接口被它依赖)
- **位置**: `14-app.md` §1/L8+L11; 交叉: `00-architecture.md` §6.3/L348; `18-plugin-dev.md` §2/L32
- **问题**: 同一条依赖规则有三个版本: 主文档 §6.3 允许"依赖 Interface(或**直调 native**)", 即纯 native 无接口组合合法; 18 路由表与 14-app 的"已有决策"写成必须依赖 Interface; 14-app 自身 L8("只有接口被它依赖")又与 L11("可声明依赖服务")内部矛盾。`tg check` 的接口校验(§6.4 第 4 项"APP 声明的接口在闭包内")无法判定"APP 是否必须声明接口"。
- **证据**: "APP | 唯一业务逻辑 | 1 | 依赖 Interface(或直调 native), 不许被依赖"(00 L348); "| **APP**(恰一) | main | Interface | `14-app` | 启动链演示 |"(18 L32); "但它是终点(没有插件依赖 APP; 只有接口被它依赖)"(14 L8) vs "APP 依赖 **Interface 插件**(严格叶子, §6.3); 可声明依赖服务(sqlite 类经 svc-posix, D18)"(14 L11)。
- **建议**: 在 14-app §1 定稿唯一规则并回改 18: "APP 依赖 = 零或多个 Interface + 零或多个 Service(单向流合法); 直调 native 的零接口 APP 为合法最小形态"; 18 路由表 APP 行依赖列同步改为"Interface(可选)/服务(可选)/native"。

## P1 — 完备性缺口/技术不清晰

### [P1] manifest 与描述符双真值: deps/res 两处声明, 权威与同步机制未定义
- **位置**: `18-plugin-dev.md` §1/L9+L16–17; 交叉: `00-architecture.md` §6.1/L319+L322; `12-plugin-mgr.md` §2/L20; `13-toolchain.md` §2/L21
- **问题**: 依赖与资源在**描述符**(`deps`、`res` 字段)和 **manifest**("依赖声明、资源声明")中各声明一次, 两处都是"组合期校验"的输入; 谁是唯一真值、不一致时以谁为准、是否有 CI 同步检查, 全文档集无一处说明。01 §2.6.1 对 API 治理明确写了"单一事实来源是头文件 + 构建产物", 插件依赖面没有对应条款。13 L21 的"生成物(manifest.c/h…)"暗示某种生成关系, 但方向未定义。
- **证据**: "manifest 声明(依赖 / 资源 / RAM 预算)"(18 L9) vs "const tg_dep_t *deps; /* {name, \">=1.0,<2.0\", phase} 数组 */"、"tg_res_t res; /* RAM/栈/IRQ/DMA 需求, 组合期预算校验 */"(00 L319/L322); "manifest 格式定稿…schema、依赖声明、资源声明、挂载计划、生成物"(12 L20)。
- **建议**: 在 12-plugin-mgr 定稿: 描述符 = 插件自描述真值(deps/res/ver/sched_class), manifest = 产品级选择 + 挂载计划 + 预算总额; `tg check` 增加"manifest 声明 vs 描述符"一致性校验(类比 01 §2.6.1 同步规则); 18 §1 流程图标注两者的读写方向。

### [P1] TG_PLUGIN 宏未定稿且与 CA-10 符号纪律矛盾(非 static / 段名不一致 / 宏体空缺)
- **位置**: `18-plugin-dev.md` §1/L10(作者被路由至此); 交叉: `00-architecture.md` §6.1/L328–330; `08-core-api-list.md` §13.3/L334–338; `12-plugin-mgr.md` §2/L21
- **问题**: 描述符宏是全部插件作者的第一行代码, 但三处机械细节互相矛盾: (a) 00 §6.1 的宏展开为**非 static** 的 `const tg_plugin_t _tg_plugin_##name_`(外部链接, 每插件导出一个符号), 与 08 §13.3/CA-10"static const…**无需导出任何符号**、近零导出面"直接矛盾; (b) 收集段名 00 用 `.tg_plugins`(§4.4/L219 亦然), 08/12 用 `.tg_plugin`——`__start_/__stop_` 边界枚举按哪个实现都会撞错; (c) 宏体 `= {...}` 未定义, ver/api_rev/sched_class/res/回调如何经 `TG_PLUGIN(name_, deps_, ...)` 传入(指定初始化器? 分类别辅助宏?)无可操作规格。
- **证据**: "`#define TG_PLUGIN(name_, deps_, ...) const tg_plugin_t _tg_plugin_##name_ __attribute__((used, section(\".tg_plugins\"), aligned(4))) = {...};`"(00 L328–330) vs "`#define TG_PLUGIN_SECTION __attribute__((used, section(\".tg_plugin\")))` / `static const tg_plugin_t TG_PLUGIN_SECTION my_plugin = { ... };` / 无需导出任何符号"(08 L334–338); "段位置(`.tg_plugin`)+ `__start/__stop` 边界枚举"(12 L21)。
- **建议**: 定稿唯一宏形态: `static const` + 统一段名(建议 `.tg_plugin`, 与 08/12 一致并回改 00 两处) + 指定初始化器传全部字段; 12 §2.2"描述符二进制细节"补作者视角的完整可编译示例, 18 §1 链接之。

### [P1] 18 通用纪律漏掉 sched_class 声明义务
- **位置**: `18-plugin-dev.md` §1/L16–19; 交叉: `00-architecture.md` §5.3/L299、§6.1/L318、§6.4/L354
- **问题**: §5.3 明确"插件**必须声明**"调度兼容类别(SAFE_PREEMPT/COOP_ONLY/TT_SAFE), 描述符含 `sched_class` 字段, 组合期校验第 3 项 + D10 双保险都执法它——但 18 作为"所有插件作者的入口", §1 的纪律清单只有依赖/资源/符号/ISR 四条, sched_class 完全缺席; §6 测试表也未提。作者按 18 走完流程会漏掉这个组合期硬校验字段。
- **证据**: 18 §1 四条纪律为"依赖声明/资源声明/符号纪律/ISR 纪律"(L16–19, 无 sched_class); "调度器插件化 ⇒ **并发契约从"全局固定"变成"每产品组合参数"**。插件必须声明:"(00 L299); "3. 调度类别校验(§5.3) + 双保险静态分析(D10)"(00 L354)。
- **建议**: 18 §1 纪律清单加第五条: "调度类别声明: SAFE_PREEMPT(默认)/COOP_ONLY/TT_SAFE(§5.3); preempt 调度器 × COOP_ONLY = 组合期硬错误; D10 双保险"。

### [P1] sched_class 同名双义: 调度器 ops 字段与插件描述符字段值域不同, §5.1 注释误引 §5.3
- **位置**: `18-plugin-dev.md` §2/L26(把两者并列); 交叉: `00-architecture.md` §5.1/L261、§6.1/L318、§5.3/L301–305
- **问题**: `tg_sched_ops.sched_class` 注释列举"PREEMPT | COOP | TT"(调度器身份), `tg_plugin_t.sched_class` 引用 §5.3(SAFE_PREEMPT/COOP_ONLY/TT_SAFE, 插件兼容类别)——同名字段、两个值域, 而 §5.1 的注释还标着"(§5.3)"却列举了 §5.3 不存在的标识符。18 路由表 Scheduler 行"提供 `tg_sched_ops` + sched_class 声明"进一步混淆。§6.4 第 3 项"调度类别校验"需要比较这两个域, 规则文本无法落地。
- **证据**: "uint32_t sched_class; /* PREEMPT | COOP | TT (§5.3) */"(00 L261) vs §5.3 类别表"SAFE_PREEMPT / COOP_ONLY / TT_SAFE"(00 L303–305) vs "uint32_t sched_class; /* §5.3 */"(00 L318)。
- **建议**: 调度器 ops 字段改名(如 `sched_kind`: COOP/PREEMPT/TT), 插件描述符保留 `sched_class`(SAFE_PREEMPT/COOP_ONLY/TT_SAFE); §6.4-3 写明比较规则(如 sched_kind=PREEMPT ⇒ 全部插件须 SAFE_PREEMPT); 修正 §5.1 注释与 18 L26 措辞。

### [P1] 作者指南缺错误码纪律(两个 SD-10 子集分域未入 18; init 失败语义未入)
- **位置**: `18-plugin-dev.md` 全文(仅 L47 出现一次 -ENOTSUP); 交叉: `08-core-api-list.md` §11/L289; `06-device.md` §4/L186; `12-plugin-mgr.md` §2/L22
- **问题**: "错误码怎么报"是用户视角的关键流程, 18 全文无错误码章节: (a) core 域子集(9 个码)与设备域子集(7 个码, 多 -ENOSPC/-EROFS、少 -EAGAIN/-ENOMEM/-EEXIST/-ETIMEDOUT)分属 08/06 两篇且都叫"SD-10", 服务/框架件作者该用哪个无路由; (b) init/early_init/start 失败返回什么、后果是什么, 只有 12 大纲里一句"错误路径(init 失败 = 启动失败)", 18 未告知作者。
- **证据**: "**错误码**(SD-10 统一): int 返回, 负 errno 子集 `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT`"(08 L289); "**错误模型**(SD-10): int 返回, **负 errno** 子集: `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS`"(06 L186); "错误路径(init 失败 = 启动失败)"(12 L22)。
- **建议**: 18 增"错误码与失败上报"节: 统一负 errno 纪律 + 按类别的子集路由(驱动→06 §4, 其余→08 §11) + "init 返回非 0 ⇒ 启动失败(12 §2.3), 失败码经 init 链打印/trace 上报"。

### [P1] 作者指南缺日志/诊断输出约定
- **位置**: `18-plugin-dev.md` 全文; 交叉: `00-architecture.md` §8/L541; `03-debug.md` §1/L23; `02-roadmap.md` §1/L154
- **问题**: "日志怎么打"在 18 中零落点。可用手段散落三处且分期不同: 早期 console(轮询, init 链打印)、I/O 插件完整 tty、trace 宏(03)、svc-posix stdio、service/log 未排期——插件作者(尤其驱动作者在 early_init/init 阶段)不知道各相位能用什么输出诊断, 这是作者体验的典型断点。
- **证据**: "console 双形态: platform 早期 console(轮询, init 链打印)→ I/O 插件完整 tty(中断驱动)"(00 L541); "API: `TRACE_EVT(id, a, b, c)` 宏"(03 L23); "`service/uds`…`service/ota`、`service/log`(结构化日志)"未排期(roadmap L154); 18 全文无对应条目。
- **建议**: 18 增"日志与诊断"节: 按 init 相位列出可用输出(early_init→platform 早期 console; init 后→trace 宏; 运行期→svc-posix stdio/未来 service/log), 并链接 03 §1 与 00 §8。

### [P1] 18 §5 服务 init 时序术语颠倒("依赖方 publish 在先")
- **位置**: `18-plugin-dev.md` §5/L62; 交叉: `17-service-mgmt.md` §1/L14
- **问题**: 18 写"依赖方 publish 在先、消费方 lookup 在后"——按通行语义"依赖方"=消费方, 该句自相矛盾且把顺序规则写反; 17 的正确表述是"发布时机 = **服务方** init; 查找时机 = **依赖方** init"。作者入口文档的这条摘要会直接误导服务作者。
- **证据**: "- init 时序: 依赖方 publish 在先、消费方 lookup 在后(init-DAG 保证)"(18 L62) vs "- 发布时机 = 服务方 init; 查找时机 = 依赖方 init(init-DAG 保证顺序)"(17 L14)。
- **建议**: 18 L62 改为"被依赖方(提供方)publish 在先、消费方 lookup 在后(init-DAG 保证)", 与 17 对齐。

### [P1] 18 §6 把 19-test 当作插件 conformance 用例目录属过度引用; 插件级用例无落点
- **位置**: `18-plugin-dev.md` §6/L71; 交叉: `19-test.md` L1/L5; `18-plugin-dev.md` §7/L81
- **问题**: 19-test 自我定位是"**core-api 兼容性**用例"目录(§3 全部为 TC-TASK/SYNC/TIME/WORK/MEM/MM/IRQ/SVC 八个 core API 组), 它验证的是 core, 不是插件自身行为。18 §6 把它引为所有插件"语义 conformance"的用例目录, 混淆了"core API conformance"与"插件自有语义 conformance"(如驱动行为、FS 掉电用例——18 §2 FS 行要求"掉电用例", 该用例在 19-test 与 07 中均无目录)。插件级用例编写指南仅是 18 §7 的开放问题, 无大纲归属。
- **证据**: "语义 conformance | 行为测试跑在 host 平台(秒级)+ 目标 | 01 §2.3 层 2 / **用例目录: `docs/5-test/19-test.md`**"(18 L71); "# 19 — 测试框架与 core-api 兼容性用例"(19 L1)、"**本篇 = 层 2 语义一致性套件的用例目录**"(19 L5); "conformance 用例编写指南(断言集形态、host 适配层)"(18 L81, 开放问题)。
- **建议**: 18 §6 该行拆成两行: "core API conformance(用例目录 19-test)"与"插件自有 conformance(指南待成文, 见 §7 开放问题; 各领域用例归领域文档, 如 FS 掉电用例归 07)"; 并给该开放问题指定成文落点。

### [P1] "严格叶子/叶子检查(无被依赖)"未排除 APP, 按字面不可实现
- **位置**: `18-plugin-dev.md` §2/L31; 交叉: `15-interface.md` §1/L8; `00-architecture.md` L162/L68/L347
- **问题**: APP 本身是插件("APP 本身也是一个插件(§6.3)"), 且 Interface"仅被 APP 依赖"——因此"没有任何插件依赖它们"/"叶子检查(无被依赖)"按字面恒为假: 任何被 APP 使用的接口都有 APP 这个依赖方。组合器若照 18 的"叶子检查(无被依赖)"实现, 任何实际被使用的接口插件都会报错。
- **证据**: "叶子检查(无被依赖)"(18 L31); "**严格叶子**: 没有任何插件依赖它们"(15 L8); "系统中没有任何插件依赖它们 ⇒ 可替换性最大化"(00 L162); "仅被 APP 依赖"(00 L347); "APP 本身也是一个插件(§6.3)"(00 L68)。
- **建议**: 三处统一改为"除 APP 外无任何插件依赖(被依赖方 ∌ 非 APP 插件)"; 18 的验证列写明检查的机械形式(依赖图中接口节点的入边只允许来自 APP 类)。

### [P1] CLI 命令面五处文档互不一致, 13 作为权威文档缺 init/test/api-dump/dbg/new
- **位置**: `13-toolchain.md` §1/L11、§2/L20; 交叉: `00-architecture.md` §13/L641–646; `01-api-contract-governance.md` §2.6.4/L174; `03-debug.md` §2/L32; `18-plugin-dev.md` §7/L80
- **问题**: CLI `tg` 的命令集在五处出现且互不相同: 主文档 §13 定义 `init/add/run/test/build`; 13 §1 声称继承 §13 却列成 `add/build/run/check`(`check` 不在 §13, `init`/`test` 被丢); 13 §2 大纲为 `add/remove/build/run/check/flash [?]`; 18 提案 `tg new`; 01 要求 `tg api-dump`; 03/主文档 §11 要求 `tg dbg`。`tg test` 是三层 CI 的入口命令, 在工具链文档中缺席是实质性缺口。
- **证据**: "`tg add <plugin>`(依赖闭包)/ `tg build`(组合+链接)/ `tg run`(QEMU)/ `tg check`(校验)"(13 L11); "tg init myapp…tg run qemu…tg test…tg build --release"(00 L641–646); "本地 `tg api-dump` 出 diff"(01 L174); "`tg dbg` 子命令"(03 L32); "`tg new <kind>` 生成模板"(18 L80)。
- **建议**: 13 §2.2 命令集清单收敛全集: `init/new/add/remove/build/run/test/check/flash/api-dump/dbg`, 每条标注版本归属、参数要点与所属 CI 层(test→层 2 运行器, api-dump→层 1 golden 管线, dbg→bridge); 13 §1 的"已有决策"摘要与主文档 §13 对齐。

### [P1] 组合期校验"全量规则"三处清单互不一致, 13 缺三项且误引 §6.4
- **位置**: `13-toolchain.md` §1/L12; 交叉: `00-architecture.md` §6.4/L350–357; `12-plugin-mgr.md` §2/L24; `06-device.md` §2/L123
- **问题**: 组合校验规则存在三个版本: 主文档 §6.4 共 6 项; 12 §2.5"全量规则"列 5 项(缺"接口在闭包内"与 abi_id); 13 §1 只列 4 项——**缺调度类别校验(第 3 项)、接口校验+符号族碰撞(第 4 项)、abi_id 一致性(第 6 项)**, 而 13 正是 `tg check` 的实现文档。另外 13 把"设备名唯一"归引到 §6.4, 但 §6.4 六项中并无此条(其权威在 06 §2)。
- **证据**: "**组合期校验**(§6.4): init-DAG 环检测(硬错误)、资源冲突、设备名唯一、版本区间"(13 L12) vs §6.4 六项"1 依赖闭包+版本区间…3 调度类别校验…4 接口校验…符号族碰撞检测…6 abi_id 一致性"(00 L350–357); "组合期校验清单(全量规则): 资源冲突 / 符号命名空间 / 设备名唯一 / 版本区间 / sched_class 与调度器组合合法性"(12 L24); "设备名唯一性 = manifest 组合校验主键(§6)"(06 L123)。
- **建议**: 以 00 §6.4 六项为唯一权威清单; 12/13 改为引用而非重抄; 13 §2 大纲加一条"`tg check` 规则清单 = §6.4 全量六项 + 设备名唯一(06 §2)", 并补齐缺失三项。

### [P1] 15-interface 接口清单漏掉 iface-pkcs11(v2.0)
- **位置**: `15-interface.md` §1/L14–15; 交叉: `02-roadmap.md` §1 v2.0/L96; `00-architecture.md` §7.4/L467
- **问题**: 15 的接口清单从 v1.0(iface-posix/iface-min)直接跳到"未排期: iface-autosar-ish", 漏掉路线图与主文档 §7.4 都排定的 **iface-pkcs11(v2.0, 适配 crypto 服务)**; §2 大纲也只有 posix/min 两者的细则条目。接口管理文档的清单不完备。
- **证据**: "v1.0 接口: **iface-posix**…+ **iface-min**…未排期: iface-autosar-ish(车规生态)"(15 L14–15); "`iface-pkcs11` | Interface | 加密 token API(PKCS#11)适配 crypto | service/crypto"(roadmap L96); "`iface-pkcs11` [?] | 域标准: 加密 token API, 适配 crypto Service | v2.0(crypto 之后)"(00 L467)。
- **建议**: 15 §1 清单补"iface-pkcs11(v2.0, 适配 crypto 服务)"; §2 大纲加一条"iface-pkcs11: PKCS#11 子集与 crypto 服务注册表的边界"。

### [P1] "测试/样例/工具属 APP 类"与"APP 恰一"的组合规则未写
- **位置**: `14-app.md` §1/L8; 交叉: `00-architecture.md` §7.2/L395–397、§7.5/L482、§6.3/L348; `18-plugin-dev.md` §2/L32
- **问题**: 主文档把"APP · 测试 · 样例 · 工具"归入同一 APP 类, 同时规定 APP 数量恰一。两条合取的工程含义(产品镜像不能同时带业务 APP 与工具类 APP; conformance APP 只能是专用测试镜像的唯一 APP)没有任何文档写明——14-app 作为 APP 文档对此只字未提, `tg check` 无从判定"app/hello + app/conformance 同镜像"是否合法。设计在"每镜像换一个 APP"的读法下可以自洽, 但规则必须成文, 否则工具/样例作者与产品作者都会踩空。
- **证据**: "package \"APP 类(消费标准)\" as G_APP { [APP · 测试 · 样例 · 工具] as APPN }"(00 L395–397); "谁想"用"标准 API, 谁就是 APP 类插件(测试/样例/工具同此)"(00 L482); "APP | 唯一业务逻辑 | **1**"(00 L348); "恰好一个 APP"(14 L8); "APP(恰一)"(18 L32)。
- **建议**: 14-app §1 增一条组合规则: "每镜像恰一个 APP; 测试/样例/工具 APP 仅作为专用镜像(conformance 镜像/演示镜像)的唯一 APP 存在; 产品镜像需要常驻工具时将其实现为 Service 并经注册表发布"; 18 §2 APP 行加注。

## P2 — 改进建议

### [P2] 18 路由表 Interface 行"依赖 svc-posix 等服务"不覆盖 iface-min
- **位置**: `18-plugin-dev.md` §2/L31; 交叉: `02-roadmap.md` v1.0/L67; `15-interface.md` §1/L14
- **问题**: iface-min 直通 native, 依赖是 core 而非服务; 18 的依赖列写"svc-posix 等服务"对它不成立。
- **证据**: "| **Interface** | 再导出皮肤 | svc-posix 等服务 | …"(18 L31) vs "`iface-min` | Interface | 极简别名层, 直通 native | **core**"(roadmap L67)。
- **建议**: 依赖列改为"svc-posix 等服务 / core(iface-min 直通)"。

### [P2] Platform"链接脚本"归属与组合器"链接脚本片段"未澄清
- **位置**: `18-plugin-dev.md` §2/L25; 交叉: `13-toolchain.md` §2/L21; `00-architecture.md` §6.3/L341
- **问题**: 18 告诉 Platform 作者"你提供…链接脚本", 13 说组合器生成"链接脚本片段", 主文档 §6.3 的 Platform 行没有链接脚本一项——三方归属未澄清, Platform 作者不知道自己写不写链接脚本。
- **证据**: "| **Platform** | PIC ops 表 / 早期 console / region / **链接脚本** | — | …"(18 L25); "生成物(manifest.c/h、**链接脚本片段**、挂载计划表)"(13 L21); §6.3 Platform 行为"reset 汇编、时钟/引脚/RAM、中断控制器实现、console、cache、timer、内存 region 表"(00 L341, 无链接脚本)。
- **建议**: 明确分工: Platform 提供内存布局数据(region 表/段基址), 组合器据此生成最终链接脚本; 18 的 Platform 行把"链接脚本"改为"内存布局数据(供链接脚本生成)"。

### [P2] 13 大纲缺层 3"版本矩阵运行器"条目
- **位置**: `13-toolchain.md` §1/L13、§2/L23–24
- **问题**: 13 §1 继承了"三层 CI 门禁"(含版本矩阵), 但 §2 大纲只有 golden 生成器(层 1)与 conformance 运行器(层 2), 层 3"全部一方插件 × 声称支持的 core 版本各编一遍"的运行器无交付条目。
- **证据**: "**三层 CI 门禁**(01 §2.3): golden diff(abidiff)/ 语义 conformance 矩阵(×3 调度器)/ 版本矩阵"(13 L13) vs §2 大纲第 5/6 条仅覆盖前两层(13 L23–24)。
- **建议**: 13 §2 加第 8 条"版本矩阵运行器: 插件 × core 版本编译矩阵、golden 版本戳比对(01 §2.6.6)、报告与红绿判定"。

### [P2] 13 构建系统倾向(make)与 DoD 第 5 项"CMake 选型论证"表述不一致
- **位置**: `13-toolchain.md` §3/L31; 交叉: `02-roadmap.md` §5/L192
- **问题**: DoD 第 5 项预设"CMake 选型论证", 13 的开放问题倾向"最小依赖(make + 脚本)"——待收敛项在两篇中的预设不同, 收口时容易各改各的。
- **证据**: "构建系统: make/cmake/自研 [?]——倾向最小依赖(make + 脚本)"(13 L31); "构建系统与仓库骨架(**CMake 选型论证**、目录布局、链接脚本、QEMU 脚本)"(roadmap L192)。
- **建议**: 统一为"构建系统选型论证(make/cmake/自研)"并同步修订 roadmap DoD 措辞; 若维持 make 倾向, 在 13 记录偏离 DoD 预设的理由。

### [P2] host 平台插件架构(DoD 第 6 项)无文档认领, 13 大纲无条目
- **位置**: `13-toolchain.md` §1/L14、§2; 交叉: `02-roadmap.md` §5/L193
- **问题**: host 平台插件是三层 CI 的地基(13 §1 引用它支撑"CI 秒级单测 + 完整 ASan"), roadmap DoD 第 6 项要求其架构设计文档, 但 13 §2 大纲没有对应条目, 其他文档也未认领——DoD 交付物悬空。
- **证据**: "**host 平台插件**(02): CI 秒级单测 + 完整 ASan 直通"(13 L14) vs 13 §2 大纲七条无 host 平台条目; "| 6 | host 平台插件架构(core+插件 → Linux 进程的映射规则) | 设计文档 |"(roadmap L193)。
- **建议**: 13 §2 大纲认领(如"host 平台插件架构: core+插件→Linux 进程映射规则、iface-posix host 直通模式(00 §7.3)、ASan 集成"), 或在 README 明确归属到其他文档。

### [P2] 16-service 服务清单缺 ramdump(v2), lwip 本体无大纲条目
- **位置**: `16-service.md` §1/L15、§2/L20; 交叉: `02-roadmap.md` v2.0/L95、未排期/L154; `README.md` L27
- **问题**: (a) 服务清单漏掉 v2.0 的 `service/ramdump`(roadmap 归 Service 类), 也未列未排期的 uds/ota/log; (b) README 承诺本篇覆盖"lwip", 但大纲中 lwip 仅以"socket 路由边界"出现(第 2 条), lwip 作为服务本身的移植契约(netdev 对接、线程模型、RAM 预算)无条目。
- **证据**: "服务清单: trace(M2)/ dbg-bridge(M3)/ svc-posix(M2)/ lwip(v2 ★)/ crypto(v2)/ modload+ed25519(v3)"(16 L15) vs "`service/ramdump` | Service | fault handler 注册 + LZ4 捕获 + host 离线分析"(roadmap L95); "socket 路由(v2): svc-posix ↔ lwip 的服务边界(谁拥有 socket 表 [?])"(16 L20)。
- **建议**: 16 L15 清单补"ramdump(v2)"并加"未排期: uds/ota/log(roadmap §1)"; §2 大纲加一条"lwip 服务本体: netdev(O-S5)对接、线程/内存模型、RAM 预算"。

### [P2] 15-interface 大纲缺接口插件生命周期/相位条目
- **位置**: `15-interface.md` §2/L19–25; 交叉: `00-architecture.md` §6.2/L335
- **问题**: 接口作者需要知道自己的 init 相位与该时刻可用资源(主文档: Interface 在 LATE 初始化、晚于其依赖的 Service), 15 的大纲六条均为符号/叠加/版本议题, 无生命周期条目。
- **证据**: "**Interface 插件在 LATE 初始化**(晚于其依赖的 Service)"(00 L335) vs 15 §2 大纲(再导出机制/iface-posix/iface-min/多接口共存/版本协商/新接口判据, 无生命周期)。
- **建议**: 15 §2 加一条"接口插件生命周期: LATE 相位 init、晚于被再导出服务、init 时可用资源清单(堆/中断状态)"。

### [P2] 18 缺"内存怎么申请"的通用路由(三池)与归属指引
- **位置**: `18-plugin-dev.md` §3/L43、§5/L64; 交叉: `08-core-api-list.md` §6/L129–159; `04-vfs.md` §1/L43
- **问题**: 18 只有两处零散提及(驱动用 tg_dma_alloc、服务 RAM 预算), 没有"何时用 tg_malloc vs tg_mem_alloc_contig vs tg_page_alloc vs tg_dma_alloc"的路由, 也没有跨边界对象所有权约定(如 tg_file_t 由 vfs-core 分配/释放)。
- **证据**: 18 §3 仅"DMA/cache | `tg_dma_alloc` 分配…"(L43)、§5 仅"RAM 预算 manifest 声明(arena 记账, v2)"(L64) vs "内存(tg-mem 组; TLSF 堆 + 连续池 + 页池, 三池由 region 表/manifest 划分)"(08 L129); "`tg_file_t` 由 vfs-core 经 core 堆(`tg_malloc`)分配, close 释放"(04 L43)。
- **建议**: 18 §1 纪律加一条"内存纪律: 默认 `tg_malloc`; DMA 缓冲用 `tg_dma_alloc`(08 §6 三池); 帧缓冲类大固定块走 region 静态预留; 跨边界对象的所有权以提供方文档为准"。

### [P2] 18 缺"版本怎么标"的作者指引(ver/api_rev 语义)
- **位置**: `18-plugin-dev.md` §1/L16、§6/L74; 交叉: `00-architecture.md` §6.1/L316–317; `12-plugin-mgr.md` §3/L33
- **问题**: 18 覆盖了"依赖的版本区间"与"版本矩阵 CI", 但插件**自身**版本怎么标(ver[3] 何时进位、与 api_rev 的关系)无指引; 12 只把"semver 子集"留为开放问题。作者在 §6"声称版本全编"的 CI 要求下无从决定版本号语义。
- **证据**: "const tg_dep_t *deps; /* {name, \">=1.0,<2.0\", phase} 数组 */"(00 L319)、"ver[3]; /* 插件自身语义版本 {maj, min, pat} */"、"api_rev; /* 编码面对的 native API 版本 */"(00 L316–317); "插件版本区间语义(semver 子集?)"(12 L33); 18 无对应条目。
- **建议**: 18 §1 加一行"版本标注: semver 子集(语义待 12 §3 收口), api_rev = 编码面对的 native API 版本(§6.1)"; 并推动 12 的该开放问题进入 DoD 收敛。

---

## 统计

| 严重度 | 数量 |
|---|---|
| P0(矛盾/架构缺陷) | 3 |
| P1(完备性缺口/技术不清晰) | 13 |
| P2(改进建议) | 9 |
| **合计** | **25** |

## 核对无误的方面(供参考)

- D18/svc-posix 定位在 18/14/15/16 四篇与主文档 §6/§7 完全一致; "八类"计数全文档集无矛盾
- 18 §3/§4 的驱动/FS 契约摘要与 `06-device` §6、`04-vfs` §2、`07-concrete-fs` §6 逐条核对无误
- 13 的 golden 文件划分(08 §1/§15)引用正确; 16 的移植双模式与 errno 映射引用正确; 14 的启动序列引用正确

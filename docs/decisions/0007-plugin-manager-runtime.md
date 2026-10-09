# 0007: 插件管理器运行期 —— `.br_plugins` 段 / 相位驱动 / 拓扑与失败策略

> 状态: **已接受(原型已落地)** | 影响面: **插件描述符 / 生命周期 / 启动序列 / 生成物契约**(触及 `1-02` §2.2 阈值表的最高门槛行)
> 格式依据: `1-02` §2.2(决策记录格式与阈值表); 注: `1-02` §2.2 的阈值表把 **"插件描述符 / 注册表语义"** 列为**最高门槛**(需决策记录)——这正是本篇的理由
> 落点: 原型 `brickOS-prototype-v0.x.0` @ `1d60a15`(原型侧落点 = `prototype/docs/decisions/0005-plugin-manager.md`)
> 相关: `1-01` §6.1/§6.2/§6.3/§6.5/§9、`3-05`、`3-06`、`3-01` §9/§10/§13.3、`4-02` §2、`4-04`、`brickie-v0.1.md` §8.1/§8.4

## 1. 动机

1. **阈值表已经把这件事钉在最高门槛上**。`1-02` §2.2 的表格里, "sched_class 契约 / 插件描述符 / 注册表语义"被单列为**最高门槛**(影响所有插件, 需决策记录): `1-02:81`。原型 0.x.0 对描述符做的事(定段、加字段、把生成物变成编译单元)正落在这一行, 设计侧不能只有"骨架 + 大纲"。
2. **`3-05` 仍是骨架, 而它的大纲前三项已被运行期回答**。`3-05:4` 标"骨架", 大纲第 2 项(段位置 + 边界符号)、第 3 项(生命周期状态机 + 错误路径)、第 4 项(init-DAG: 拓扑/环/相内排序)都还"待成文"(`3-05:44-46`)。原型侧已在 QEMU aarch64 上跑通这三项, 并留下可复算的证据(段大小、符号表、门禁日志)——本 ADR 把"设计因此定成什么样"留下来。
3. **生成物契约现在只有路径、没有形态**。`4-02` §2 第 6 项只钉了 `build/gen/<plugin>/plugin_desc.c` 这个落点(`4-02:46`), `brickie-v0.1` §8.4 只写"`BR_PLUGIN(...)` 展开 + `br_plugin_t` 实例"(`brickie-v0.1:1164`), 而它与冻结头的关系、钩子怎么发、数量怎么自证, 都没有契约。原型把描述符变成了**可编译单元**(`product.toml:68` 的 `gen_sources` 非空), 这条契约必须成文。
4. **`1-01` §9 的序列真值只有一张图**(`1-01:614-618`, 图源 `plantUML/1-01-architecture-04.puml`), 图里 `core.init` / `plugin_manager` / 四相是四格流程; 运行期实现对它做了两处必要重排(见 D3/D7), 若不落成裁定, 图与代码会长期互相"看起来都对"。

## 2. 决策

### D1 段机制: `.br_plugins` 是独立输出段, `KEEP` + `ALIGN(8)` + 边界符号用**普通赋值**

* **段名不变** `.br_plugins`(`1-01` §6.1:345 的 `section(".br_plugins")`)、**边界符号名不变** `__br_plugins_start/__br_plugins_stop`(`3-01` §13.3:363)。
* **必须是独立输出段, 不折进 `.rodata`**。理由 = **可验证性**: `objdump -h` 只列输出段, 折进去就"看不见段是否存在、多大"; 而 `3-05` §2.2 把 `.br_plugins` 当成有名有界的段 + 边界符号(`3-05:44`)。实测: `aarch64-linux-gnu-objdump -h build/brick.elf` 同时列出 `.rodata 00005d38` 与独立一行 `.br_plugins 00000300 … 2**3`; 该段挂 `} :rodata`(`platform/qemu-aarch64/src/link.ld:83`), 仍是 R 权限的 PT_LOAD, 不会把镜像退化成 RWX 大段。
* **`KEEP(*(.br_plugins))` 不可省**(`link.ld:81`)。描述符是 `static const` + `used`, **没有任何 C 引用者**, `-Wl,--gc-sections`(`product.toml:96`)会当垃圾裁掉 —— 段必须显式保根。实测: `objdump -t build/brick.elf` 里 8 条 `_br_plugin_*` 全是**局部符号**(`l O .br_plugins … 0x60`), 正是"近零导出面"(CA-10 / `3-01` §13.3:356)的形态。
* **`ALIGN(8)` 在链接脚本一侧**(`link.ld:79`), 与 `BR_PLUGIN_SECTION` 的 `aligned(8)`(`core/include/br/core/br_plugin.h:124`)是两件事: 后者只保证段**内**元素对齐, 段**起点**对齐只能由链接脚本给(`br_plugin_t` 含指针, 设计原写 `aligned(4)` 不够)。
* **边界符号由链接脚本普通赋值给出, 不是 `PROVIDE`**(`link.ld:80,82`)。`PROVIDE` 的"已有定义就不覆盖"语义会**掩盖接线错误** —— 这两个符号必须恒存在且不能被别处定义。
* **与同文件 `.br_extable` 的写法差异只有一处**: extable 折在 `.rodata` 里(`link.ld:45-59`), plugins 给**自己的输出段**(`link.ld:79-83`); 两者的边界符号手法完全相同(都普通赋值、都有 KEEP、都因"段名含 `.`, GNU ld 不自动生成 `__start_/__stop_`"而必须显式给)。

> ⚠ **设计侧口径偏差**: `3-01` §13.3:364 与 `3-05` §2.2(`3-05:44`)都写"链接脚本 **PROVIDE**"。以代码为准 = **普通赋值**, 回灌见 D7(a)。

### D2 描述符扩展: 14 字段(在 `1-01` §6.1 的 11 字段上新增 3 个), 由生成物发射

**实际字段表**(逐字段抄自 `core/include/br/core/br_plugin.h:96-111`; "已有" = `1-01` §6.1:329-341 已列):

| # | 字段 | 类型 | 来源 | 备注 |
|---|---|---|---|---|
| 1 | `name` | `const char *` | 已有 | 全局唯一(`4-02:39`) |
| 2 | **`plugin_type`** | `br_u32` | **0.x.0 新增** | `BR_PLUGIN_TYPE_{APP,INTERFACE,ABILITY,PLATFORM}`(`br_plugin.h:66-69`); 管理器据它判类别 |
| 3 | **`subkind`** | `br_u32` | **0.x.0 新增** | `BR_SUBKIND_{NONE,SCHEDULER,FRAMEWORK,IO,FS,SERVICE}`(`br_plugin.h:71-76`) |
| 4 | **`phase_self`** | `const char *` | **0.x.0 新增** | `[plugin].phase` 自述串, **仅呈现/诊断**(init 相由类别决定, 见 D3) |
| 5 | `ver[4]` | `br_u16[4]` | 已有 | `COMPAT_GEN.MAJOR.MINOR.REVISE`(`1-01` §6.1:331) |
| 6 | `api_rev` | `br_u16` | 已有 | |
| 7 | `sched_class` | `br_u32` | 已有 | |
| 8 | `deps` | `const br_dep_t *` | 已有(形状变) | 见下 |
| 9 | `abi_id` | `const char *` | 已有 | 运行期未使用, 见 D8 |
| 10 | `api_syms` | `const char *const *` | 已有 | |
| 11 | `res` | `br_res_t` | 已有(形状变) | 见下 |
| 12-14 | `early_init` / `init` / `start` | `int (*)(void)` | 已有 | 空钩子 = `BR_PLUGIN_NO_HOOK`(`br_plugin.h:114`) |

* **`deps` 的真实形状变了**: 代码是 `br_dep_t = {name, kind, phase, compat_gen}`(`br_plugin.h:43-48`), 而 `1-01` §6.1:334 写 `{name, range, phase, compat_gen}` —— **`kind` 是 0.x.0 新增的字段, `range` 不在描述符里**(`kind ∈ {init, runtime, type}`, `br_plugin.h:38-40`; 区间是非运行期事实, 不烧进镜像)。deps 以 **`.name == BR_NULL` 结尾**(`br_plugin.h:42`, `core/src/plugin/plugin_mgr.c:313-316`), 不靠长度。
* **`res` 的真实形状**: 代码是 `br_res_t = {ram_kib, stack_kib}`(`br_plugin.h:85-88`), 而 `1-01` §6.1:337 写"RAM/栈/IRQ/DMA 需求" —— IRQ/DMA 目前**不在描述符里**(冲突检测属组合期)。
* **谁写描述符: 机器写, 不是人写**。`br_plugin.h:92-95` 明示"生成物实例化它, 手写插件既不需要也不应该自己写"; 人手写的是 `plugin.toml`(`4-02:37`)。实测 8 份 `build/gen/<plugin>/plugin_desc.c` 都是这种形态, 例如 `build/gen/platform/qemu-aarch64/plugin_desc.c` 里 `static const br_plugin_t BR_PLUGIN_SECTION _br_plugin_qemu_aarch64 = { … }`。
* **生成物用具名初始化器, 不用 `BR_PLUGIN_DEFINE` 宏**(`br_plugin.h:126-139`)。原因: 该宏的形参表**不含** `plugin_type`/`subkind`/`phase_self`, 而宏属冻结面, 不能为原型扩形参；宏保留给手写/测试用。
* **标尺**: 单条描述符实测 **96 B**(`objdump -t` 每条 `0x60`), 8 条 = 段大小 `0x300`, 与 `objdump -h` 的 `.br_plugins 00000300` 一致。

### D3 相位机(运行期): 类别决定 `init` 相; EARLY 第一步显式取 platform; 开中断在 LATE 后; START 走两趟

① **`init` 落在哪一相由插件类别决定**(`plugin_mgr.c:171-177`, 判据函数 `is_service_like()` 在 `:159-165`): **非 Service/Interface ⇒ CORE; Interface 或 `ability.subkind == service` ⇒ LATE**; **没有 `init` 钩子 ⇒ 完成点 = EARLY**(即 `1-01` §6.2:373 的"② 与 ① 重合")。设计与代码口径一致。

② **EARLY 的第一步必须是 platform 插件的 `early_init`, 且靠字段显式取, 不靠拓扑序巧合**(`plugin_mgr.c:380-405`: 扫段找 `plugin_type == BR_PLUGIN_TYPE_PLATFORM`)。此刻 console/PIC/页表都还没起来, "谁排第一"与"能不能观测"无关; 找不到 platform、或它没有 `early_init` ⇒ **直接 panic**(`:388-398`, 理由: 镜像本就不可能成立)。这条设计文档**没有**(见 D7(b))。

③ **全局开中断在 LATE 之后、START 之前**(`plugin_mgr.c:487`), 与 `1-01` §6.2:364 的表行"全局开中断后 → `start`"、§9 图源第 18 行 `:全局开中断` 一致。

④ **START 走两趟: 先全部非 APP, 再 APP**(`plugin_mgr.c:500-533`, 且 `:726-727` 用自检断言"调用第一个 APP `start` 之前非 APP 已全跑完")。**实测理由**: 段序 = 对象路径字典序, `app/hello` 排在**第一个**(`objdump -t build/brick.elf`: `_br_plugin_hello` @ `0x4009cd38` 是段内首条), 零 init 边时 Kahn 的出队序退化成段序 ⇒ APP 会排第一; 靠巧合会把 APP 的 MainLoop 放到 platform 的 timer bring-up 之前。原型为此吃过真红(原型 ADR-0005 §2.2 记的 20 ms 超时用例)。

**相内顺序 = init 拓扑序**(`s_order`, `plugin_mgr.c:447-450`), 与 `1-01` §6.2:355 "同阶段内按依赖拓扑序"一致。

### D4 拓扑与失败策略: 运行期 Kahn; 环报完整路径后 panic; 首个 `init` 失败 = 启动失败

* **运行期做 Kahn 拓扑排序**(`plugin_mgr.c:188-290`), 只取 `kind == BR_DEP_INIT` 的边建图(`:317`; `runtime`/`type` 不参与排序, 与 `4-04` §1 的三类依赖口径一致)。
* **有环 ⇒ 打印完整环路径(闭合)后 `br_panic`**(`plugin_mgr.c:441-445`; 环路径文本 `cycle_text()` 在 `:337-358`, 闭合项在 `:270`)。环仍是**组合期硬错误**(`1-01` §6.5:425), 运行期这一遍是**自证**不是新的执法面。
* **首个钩子失败 ⇒ 启动失败, 不继续后续插件**(`plugin_fail()`: `:294-301`, 打 `[PLUGIN] FAIL <name> phase=<相> rc=<n>` 后 panic; 调用点 `:465`/`:481`/`:530`)。这正是 `3-05` §2 大纲第 3 项"错误路径(init 失败 = 启动失败)"(`3-05:45`)的落地。`br_plugin_init_failures()` 只有 0/1 两态(`br_plugin.h:171-172`)。
* **与 `1-01` §6.5 末句的冲突按"运行期也排"收口**: `1-01:453` 写"运行期不做任何检测(纯静态, 构建期全解)", 但 §9 的行为规格(`1-01:616` 图源 `plugin_manager` 格)与 `1-03` 的环检测用例都要求运行期能自证。**裁定**: "依赖正确性的*责任*在组合期"(§6.5 的原意), 运行期**做**拓扑自证与环 panic —— 静态组合下段内容虽由构建期决定, 但"谁真在段里"是链接产物的事实, 自证一次极便宜, 且报完整环路径远比"启动到一半挂死"可诊断。回灌见 D7(b)。

### D5 启动链归属: `br_core_main` 与 `br_main.h` 已删除; `start.S` 只做 reset/BSS/交给管理器

* **`br_core_main` 与 `core/include/br/core/br_main.h` 已删除**。证据: `ls core/include/br/core/br_main.h` ⇒ No such file; 全仓 `br_core_main` 只剩注释(eg `app/hello/src/main.c:19`)。`start.S` 现在只做 reset/向量表/BSS 清零 → `bl br_irq_cpu_init`(`start.S:90`)→ `bl br_plugin_manager_run`(`start.S:98`), 不再直调 `br_plat_early_init`/`br_core_main`。
* **`br-wa-entry-001` 台账注销**: 它列的四条退出条件(① `brickie check`/`gen` 按布局发现与校验 ② 目录收敛为插件 ③ 顶层 Makefile 不再直编镜像 ④ 启动链调用点改由 `.br_plugins` 段枚举驱动)**全部满足**, 登记表已把它移入"已注销"(`prototype/WORKAROUNDS.md:25`)。
* **`br-wa-boot-001` 的启动链部分还清, 但条目保留**(`WORKAROUNDS.md:26`)—— 仍欠三项, 如实引用其原文: ① **没有独立的 `core.init` 入口**(堆/中断框架/注册表/调度对象仍借 `platform.early_init` 与管理器头部; 设计 `1-01` §9 把它们归 `core.init`); ② **APP 仍直读平台身份**(`br_plat_name/isa/timer_ticks`)⇒ `product.toml:42-44` 还剩一条 `allow_edges = [["app/hello","platform/qemu-aarch64"]]`; ③ **日志/trace 直写 console/RAM 环**, 未经服务注册表。⚠ 该行物理上落在 `## 已注销(还清了)` 标题之下(`WORKAROUNDS.md:19`), 但正文写"仍欠三件"、列结构还是"欠债"形态(列数与表头不匹配) —— **不能据标题读成已注销**; 原型侧排版待主控修。
* 管理器头部代做 `br_clock_init()`/`br_log_init()`(`plugin_mgr.c:366-368`)是**权宜**, 且顺序与 `1-01` §9 图源不同: 图源把 `platform.early_init` 画在 `core.init` **之前**第 5-6 行, 实现把 core.init 的两半分别放在 `start.S`(中断框架)与管理器头部(时钟/日志), 并把 `platform.early_init` 当 **EARLY 第一步**(见 D3②)。

### D6 服务注册表: 语义照设计 + 两个设计未写的错误码; **真实消费者目前为空**

* **实际语义**(`core/src/svc/svc.c:52-84`, 声明 `core/include/br/core/br_svc.h:30-33`): 重复发布 ⇒ `-EEXIST` 且**不覆盖**(`:59`); 表满 ⇒ `-ENOSPC`(`:63`); 空 `name`/空 `ops` ⇒ `-EINVAL`(`:54-56`); 查不到 ⇒ `BR_NULL`(`:83`)且 lookup 记命中数(`:79`)。容量静态上界 `BR_SERVICE_MAX = 32`(`br_svc.h:24-26`)。
* 与 `3-01` §9:281-282 / `3-06` §1:24 相比: `-EEXIST` 与 `NULL` 已写,**`-ENOSPC`/`-EINVAL` 是设计未写的两个码** ⇒ 回灌(见 D7(a)-3)。
* **真实消费者清单 = 空**(从 `[select]` 闭包 + 各插件 `[[dep]]` + 代码调用点三路交叉): `[select]` 是 `platform/qemu-aarch64` / `sched/coop` / `service/dump`(`product.toml:17-21`), dump 经 `[[dep]]` 拉入 `service/{trace,backtrace,hexdump,memleak}`(`service/dump/plugin.toml:47-98`), 加 `[product].app = "app/hello"` 共 **8** 个插件; 但 `grep -rn "br_service_publish\|br_service_lookup"` 排除 `core/src/svc/svc.c` 与 `br_svc.h` 后**无任何命中** —— 唯一引用 `br_svc.h` 的非 core 文件是 `app/hello/src/main.c`, 用途只是调 `br_service_conformance()`(`main.c:151`)。**跨插件协作全部走直接符号调用 + manifest `[[dep]]`(init/runtime 边), 注册表尚无生产用户。**
* **`3-06` §3 的 D7「服务 vs 框架件判据」仍未成文**(`3-06:41` 仍把它列为开放问题): 这是**设计侧未收敛项**, 不是原型欠债 —— 原型的注册表语义已经按设计落地, 判据缺的是"谁该发布"的分类学。

### D7 待回灌清单(本篇不改任何现有文档, 只列要求)

| # | 目标文档 | 回灌内容 | 依据 |
|---|---|---|---|
| a-1 | `1-01` §6.1:329-341 | 描述符字段表补 `plugin_type`/`subkind`/`phase_self`; `aligned(4)` → `aligned(8)`; 宏形态按代码为 `BR_PLUGIN_SECTION`(+`BR_PLUGIN_DEFINE` 不含三新字段) | D2; `br_plugin.h:96-139` |
| a-2 | `1-01` §6.1:334/337、`4-04` §1:25 | `br_dep_t` 补 `kind`(设计写的 `range` 不在描述符里); `br_res_t` 写清实际是 `{ram_kib, stack_kib}` | D2; `br_plugin.h:43-48,85-88` |
| a-3 | `3-01` §13.3:364、`3-05` §2.2:44 | 边界符号写法: **普通赋值**, 不是 `PROVIDE`(并把理由写进去) | D1 |
| a-4 | `3-01` §9:278-286、`3-06` §1:24 | 注册表语义补 `-ENOSPC`(表满)与 `-EINVAL`(空 name/ops); 容量静态上界口径 | D6; `svc.c:52-84` |
| b | `1-01` §6.2:357-364、§9/图源 `1-01-architecture-04.puml` | 写"**START 两趟**: 先全部非 APP, 再 APP"与"**EARLY 第一步 = platform 的 `early_init`, 显式按 `plugin_type` 取, 不靠拓扑序**"; 把 `core.init` 与 `platform.early_init` 的先后口径改成与实现一致(或明确"图源给的是责任格, 运行期实现按 D3 重排"); 把 §6.5:453 的"运行期不做任何检测"改为"责任在组合期, 运行期做拓扑自证与环 panic" | D3, D4, D5 |
| c | `brickie-v0.1` §8.4:1155-1164、`4-02` §2 第 6 项:46 | 生成物 `plugin_desc.c` 的**具体形态与契约**: 注释头 + `#include` 元契约头 + 声明面锚头规则 + 钩子原型自证 + `deps` 逐条带出并以 `.name = BR_NULL` 结尾 + 每个生成物一条同值弱定义 `br_plugin_gen_total` + `aligned(8)`; 明确"生成物用具名初始化器, 不用 `BR_PLUGIN_DEFINE`" | D2; `build/gen/**/plugin_desc.c` |
| d | `6-01` §3 | **补一组 `TC-PLUG` 或明确"自述 id"口径**。核实: `6-01` 的组清单 = §3.1 TC-TASK / §3.2 TC-SYNC / §3.3 TC-TIME / §3.4 TC-WORK / §3.5 TC-MEM / §3.6 TC-MM / §3.7 TC-IRQ / §3.8 TC-SVC / §3.9 TC-HSM(`6-01:61-158`), **没有 TC-PLUG 组**; `Design/` 全库 `grep -rn "TC-PLUG\|PLGCONF"` ⇒ 无命中。而原型自编号并打印 `TC-PLUG-001..003` + `TC-PLUG-002b` + `[PLGCONF] SUMMARY`(`plugin_mgr.c:663-732`; 门禁判据 `tests/gates.toml:207-216`)。该欠债已由原型登记为 **`br-wa-test-001`**(`WORKAROUNDS.md:32` 原文: "三套目标侧一致性用例的 `TC-*` id 是自编号, 与设计 `6-01` 的用例表尚未逐条对齐 …… 日志里 id 后面的自述文字是准确的 …… 但'按 id 检索'会对不上") | `6-01:61-158` 组清单; `plugin_mgr.c:663-732` |

### D8 遗留(与运行期有关, 但不属本刀范围)

* **`abi_id`/`ver[4]` 的运行期使用仍处 `1-01`/`1-02` D14 的"Day1 存在、v2 二进制分发起生效"**: 管理器不读 `->abi_id`/`->ver`/`->api_rev`(对 `plugin_mgr.c` 的字段访问扫描无命中)。同理 `api_syms` 运行期不读; `sched_class` **只记录不执法**(`br_plugin.h:52-53`); `res` **只作诊断**(`br_plugin.h:16-17`)。
* **动态加载(v3, `service/modload`)不涉**: core 无 modload 相关代码(grep 无命中), 描述符的 v3 扩展问题仍挂 `1-01` §6.5:453 末句与 `3-05` §3。
* **运行期资源预算复核未做**: 预算真值在 manifest, 由 `brickie check` 组合期执法(`br_plugin.h:16-17`); 运行期不重算。

## 3. 替代方案与否决

| 方案 | 为什么不取 |
|---|---|
| 运行期**不**排序, 只按段序枚举 | 段序 = 对象路径字典序(实测段内首条是 `app/hello`), 与 init 依赖无关; 直接后果 = dump 排到四个服务之前, "编得过、跑不对" |
| `.br_plugins` 折进 `.rodata` | `objdump -h` 只列输出段, 段存在性/大小不可验证; `3-05` §2.2 把它当有名有界的段 |
| 边界符号用 `PROVIDE` | "已有定义就不覆盖"会掩盖接线错误; 这两个符号必须恒存在且唯一 |
| 用命名约定(名字前缀 `service/`)判类别 | 与 `1-01` §6.3:386 "数量约束与依赖方向只由 `plugin_type` + `api_type` 决定"正面冲突; 且 `plugin.toml` 里本来就有字段 |
| 生成物用 `BR_PLUGIN_DEFINE` 宏实例化 | 宏形参表冻结且不含三新字段; 改形参会破坏冻结面(改宏的值/形参本身就要走解冻硬路径, `1-02` §2.4) |
| EARLY/START 的相内顺序用字典序 / APP 最后靠拓扑序巧合 | `1-01` §6.2:355 要求相内拓扑序; APP 的 MainLoop 会被放到 platform timer bring-up 之前(原型吃过真红) |
| 失败粒度改成"跳过后继续/降级" | EARLY 失败时系统还不可用, 静默继续只会让症状远离根因; `3-05:45` 的"init 失败 = 启动失败"是既有口径 |
| 运行期完全不做检测(纯信组合期) | §9 的行为规格与环检测用例要求运行期自证; 自证成本 = 8 条描述符 × 5 条边, 而收益是"报完整环路径"而不是挂死 |

## 4. 兼容性影响(含"待回灌")

| 面 | 影响 |
|---|---|
| 描述符 ABI | 字段从 11 → **14**, 单条 **96 B**; 段名/边界符号名不变, 但符号定义方式与设计写的不符(普通赋值 vs PROVIDE)。此表属 `1-02` §2.2 最高门槛行 ⇒ 冻结前必须一次改齐 `1-01` §6.1 + `3-01` §13.3 + `3-05` §2.2 + `4-02` §2 |
| 生命周期 / 启动序列 | 官方口径新增两条运行期规则(START 两趟; EARLY 第一步显式 platform)与一条收口(运行期做拓扑自证/环 panic); `1-01` §6.2 表与 §9 图源需同步 |
| 生成物契约 | `plugin_desc.c` 从"路径"变成"有形态的契约"(D7(c)); 影响 `brickie` 生成器实现面 + 作者可见的生成物 |
| 注册表 | 新增两个错误码进设计; `3-06` D7 判据仍缺(设计侧未收敛项, 不阻塞本篇) |
| 测试用例目录 | `6-01` 需补 `TC-PLUG` 组或"自述 id"口径(D7(d)); 另: `6-01:144-146` 把 TC-SVC-001..003 的平台列标 **host**, 而原型实现跑在 **target**(`main.c:151` 的 `br_service_conformance()`), 回灌时须一并定 host/target 口径 |
| WORKAROUND | `br-wa-entry-001` 注销(四条退出条件全满足); `br-wa-boot-001` 保留并改写(启动链部分已还, 三项仍欠); `br-wa-test-001` 新增 |
| 现有产物 | 无向后兼容包袱: 描述符由生成物发射, 改一处契约 = 重跑 `brickie gen`; 但 `1-01` §6.1 的字段序与 `3-05` §2.2 记的"字段序"必须一次对齐, 否则生成器与冻结头会长期错位 |

**"设计文档 vs 代码"口径差异清单**(以代码为准):

1. **边界符号定义方式**: 设计写 `PROVIDE`(`3-01:364`, `3-05:44`), 代码是普通赋值(`link.ld:80,82`)。
2. **描述符字段表**: `1-01`:329-341 缺 3 字段; `aligned(4)` vs 代码 `aligned(8)`(`br_plugin.h:124`); 宏名/形参不同(`BR_PLUGIN(...)` vs `BR_PLUGIN_DEFINE(...)`, `1-01:343-345` vs `br_plugin.h:126-139`)。
3. **`br_dep_t` 形状**: 设计 `{name, range, phase, compat_gen}`(`1-01:334`), 代码 `{name, kind, phase, compat_gen}`(`br_plugin.h:43-48`)—— `kind` 缺、`range` 多。
4. **`br_res_t` 形状**: 设计"RAM/栈/IRQ/DMA"(`1-01:337`), 代码只有 `{ram_kib, stack_kib}`。
5. **注册表错误码**: 设计只写了 `-EEXIST`/`NULL`(`3-01:281-282`), 代码另有 `-ENOSPC`/`-EINVAL`。
6. **运行期检测口径**: `1-01:453`"运行期不做任何检测" vs 实现做拓扑自证 + 环 panic。
7. **START 相**: `1-01:364` 表行"`start`(全部插件) → `app.start()`"字面读会让 APP 的 `start` 被算两次; 实现是两趟(pass 0 非 APP / pass 1 APP), 且 APP 的 `start` **就是** `app.start()`。
8. **启动序列格子先后**: 图源把 `platform.early_init` 排在 `core.init` **之前**(puml:5-8), 实现把 `platform.early_init` 当 **EARLY 第一步**, 而 core.init 的两半分别落在 `start.S` 与管理器头部。
9. **原型侧引用错(不要照抄)**: 原型 ADR-0005 头部把 `TC-PLUG-*` 记在 "`6-01` §3.8/§3.9", 而 §3.9 实为 **TC-HSM**(`6-01:148`), §3.8 是 TC-SVC; `6-01` 没有 TC-PLUG 组。

## 5. 待办与遗留

1. **执行 D7 的 (a-1)…(d) 回灌**; 其中 a-1/a-3 属最高门槛面, 建议与下一次描述符冻结同批(touch 冻结面必须带决策记录 + 明确的"改哪些已冻结条目"清单, `1-02` §2.2)。
2. **`br-wa-boot-001` 三项还债**(原样引用 `WORKAROUNDS.md:26` 的退出条件): ① 把 `core.init` 抽成独立入口(堆/中断框架/注册表/调度对象各自归位); ② 平台身份与心跳发布成服务, 删掉最后一条 `allow_edges`; ③ 日志/trace 经服务注册表(与 `br-wa-debug-002` 同源)。
3. **`br-wa-test-001`**: 三套套件 id 与 `6-01` 对齐, 或接受 D7(d) 的"自述 id"口径并写进 `6-01` §5 用例编写规范。
4. **待核实(不写成断言)**:
   - (i) `descriptor_include` 规则在原型内部有两处措辞冲突: 实现是"声明 `[[export.entries]]` 符号最多的头"( `tools/brickie/rust/src/plan.rs:163-170`), 而 `plan.rs:284-288` 的注释与生成物模板 `tools/brickie/templates/descriptor/native/c/plugin_desc.c.tmpl:16` 写"恰有一个对外头"。**以实现为准**; 生成物注释是否要改, 待原型侧确认(不影响设计契约)。
   - (ii) "管理器的字段访问扫描"只覆盖 `core/src/plugin/plugin_mgr.c`; 未做全仓数据流分析来排除别处对 `ver`/`abi_id` 的运行期使用(低风险)。
   - (iii) 本文的段大小/符号表证据取自 `prototype/build/brick.elf`; 未用构建日志核对该 ELF 即 HEAD `1d60a15` 的产物(可由 `brickie build` 复算: `.br_plugins` 应为 `0x300`、8 条 `0x60`)。
   - (iv) 原型 `README.md:68`/`:448` 仍写"运行期插件管理器未落地 / `br-wa-entry-001` 未注销", 与 `WORKAROUNDS.md:25` 的注销记录冲突 —— 原型侧 stale doc, 需主控同步。

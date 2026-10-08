# brickie-gen — L2 生成器(C++)与 L5 入口 ELF

本目录只放**源码**; 编出来的东西一律出树到
`build/host/<host-arch>/<host-os>/`(见 `../../mk/host.mk`)。本文件记录
**进程边界契约的 C++ 侧落点**、**模板变量契约**与**已知缺口**。

## 1. 职责与边界(contract §1)

| 产物 | 语言 | 职责 |
|---|---|---|
| `brickie-core` | Rust(L0/L1, 在 `../rust/`) | 名字契约 / subkind 推导 / 相位 / 路径布局 / 版本 / IFACE-IR / 求解 —— **一切判定**; 并给出 `plan-*`(路径 + 模板 id + 变量) |
| `brickie-gen` | C++(本目录) | **只做模板渲染**: `templates_root` + `artifacts[{path,template,vars}]` → `files[{path,kind:"rendered",content,mode}]` |
| `brickie` | C++ 启动器(`launcher.cpp`)+ 嵌入载荷 | 解包 `python/**` + `templates/**` + 原生工具, 用系统 `python3` 跑 L5 前端 |

**C++ 里没有任何分类学/求解/版本/IFACE-IR/路径布局判定**。旧的 `rules.*`(名字契约 /
subkind / 相位 / 符号派生 / 路径布局)与 `gen_new.*`(`new` / `plan-new`)已删除,
规则与计划搬去 `brickie-core`。

## 2. 命令面(contract §2 信封, §6 命令)

请求 / 响应都是**一行 JSON over stdio**; 响应字段固定为
`{protocol, status, exit_code, diagnostics, files, data}`。

| `command` | `args` | 响应 |
|---|---|---|
| `render` | `{templates_root, artifacts:[{path, template, vars, mode?}]}` | `files:[{path, kind:"rendered", content, mode}]` |
| `--selftest`(不经信封) | — | JSON 往返 / 渲染 / 路径与模式 / 载荷确定性自检; 退出码 0/1 |
| `--version` / `--help` | — | 文本 |

* 进程退出码: **0** = 协议处理成功(即使响应 `exit_code != 0`); **2** = 工具自身故障
  (stdin 不是 JSON 等)。业务退出码只在响应 `exit_code` 里(BRV-D9)。
* `status`: `ok`(协议处理成功)/ `internal_error`(工具故障)。
* `protocol != 1` 或缺 `protocol` ⇒ 诊断 `BRV-PROTO-0001` + `exit_code = 2` + `status = "ok"`。
* 用法错(未知命令 / 缺 `templates_root` / `artifacts` 形状错 / 路径越界 / 模板不可读 /
  **模板缺变量** / 模式非法 / 输出路径重复)⇒ 无码诊断 + `exit_code = 2` + `files = []`。
* 任一 artifact 出错 ⇒ **不产出任何 files**(避免半生成), 诊断全量回报。
* 计划里带 `kind = "machine"` 的条目(如 `plan-init` 的 `brickie.lock`, 内容归
  `brickie-core`, 见 contract §4)被**跳过**: 不读模板、不算用法错 —— 渲染器只承接
  `human` / `generated` / `rendered`。
* 渲染**没有条件语法**: 差异靠 `templates/<api_type>/<plugin_type>/<lang>/` 目录分化;
  `vars` 只做 `{{key}}` 占位符替换(键 = `[A-Za-z0-9_]`, 大小写敏感)。

示例:

```sh
echo '{"protocol":1,"command":"render","args":{
  "templates_root":"'$PWD'/templates",
  "artifacts":[{"path":"service/crypto/plugin.toml",
                "template":"native/ability/c/plugin.toml.tmpl",
                "vars":{"name":"service/crypto","short":"crypto", "...":"..."}}]}}' \
| build/host/x86-64/linux/bin/brickie-gen
```

## 3. 模板变量契约(与 `brickie-core` 的约定)

`brickie-core` 的 `plan-new` / `plan-init` 返回的 `artifacts[].vars` **必须**包含下表键。
渲染器缺键即报用法错 —— 这不是第二套真值, 而是把"派生规则"留在 Rust 的唯一结果。

### 3.1 骨架模板(`templates/native/<plugin_type>/c/*`)

| 变量 | 含义 | 使用者 |
|---|---|---|
| `name` | 插件全名(`service/crypto`) | 全部 |
| `schema` | `plugin.toml` 的 `schema` 值(`1`) | plugin.toml |
| `plugin_type` | `app`/`interface`/`ability`/`platform`(模板目录已分化, 当前模板按目录字面写死) | (备用) |
| `api_type` | `native`/`runtime_adapter`/`third_party` | plugin.toml / README |
| `subkind` | ability 细分(非 ability 传 `""`) | ability plugin.toml |
| `lang` | `c` | plugin.toml / README |
| `phase` | `early`/`core`/`late`/`app` | plugin.toml / README |
| `sched_class` | `SAFE_PREEMPT`/`COOP_ONLY`/`TT_SAFE` | ability plugin.toml |
| `version` | 四段版本(骨架初值 `0.1.0.0`) | plugin.toml / README / descriptor |
| `summary` / `license` | 人读字段 | plugin.toml |
| `export_name` | `[[export]].name`(当前 = `short`) | plugin.toml |
| `export_api_iface` | `[[export]].api_iface`(必须 = `api_type`) | plugin.toml |
| `export_form` | `[[export]].form`(骨架取 `api`) | plugin.toml |
| `short` | 名字末段(`crypto` / `qemu-aarch64`); 用于**路径**与头文件包含 | src / include |
| `symbol_prefix` | C 标识符前缀(非字母数字 → `_`, 末尾补 `_`: `qemu_aarch64_`) | src / include / smoke / descriptor |
| `header_guard` | 头文件守卫(`PLATFORM_QEMU_AARCH64_H`) | include |
| `descriptor_symbol` | 描述符 `static const` 实例名(`_br_plugin_crypto`) | descriptor |

### 3.2 描述符模板(`templates/descriptor/<api_type>/<lang>/plugin_desc.c.tmpl`)

| 变量 | 含义 |
|---|---|
| `descriptor_include` | 被描述插件自己的对外头文件(`crypto/crypto.h`), 用于 `#include "{{descriptor_include}}"` |

### 3.3 产品模板(`templates/product/<lang>/product.toml.tmpl`, `plan-init`)

| 变量 | 含义 |
|---|---|
| `schema` | `1` |
| `name` | 产品名(`hsm`) |
| `version` | 四段版本 |
| `app` | 唯一 APP 目录(`app/hsm`) |

### 3.4 命名说明与差异

* 任务/契约给的规范名是 `namespace`; `brickie-core` 实际发的是 **`ns`**(同时保留大写
  `NS` 等首刀变量名)。当前模板**两者都不用**, 所以没有缺口; 若将来模板要用 namespace
  语义, 需在 core 补 `namespace` 键或模板改用 `ns`(登记为跨 agent 口径项)。
* `plugin_type` 当前模板按目录字面写死(`native/app/c/` 里写 `"app"`), 因为契约 §6 的口径
  是**目录分化**; core 仍会传 `plugin_type`, 供将来去重。
* 上表是**权威清单**; 任何新模板变量都要先在这里登记, 再让 core 补上。

## 4. 模板目录布局(contract §6)

```
templates/<api_type>/<plugin_type>/<lang>/   # 骨架: plugin.toml / src.c / include.h / smoke.toml / README.md
templates/descriptor/<api_type>/<lang>/      # 描述符(与 plugin_type 正交)
templates/product/<lang>/                    # 产品骨架
```

生成物首行标记 `brickie:generated`(描述符模板已带)。

## 5. 构建、自举种子与降级行为

```sh
cd prototype/tools/brickie
make            # = make cxx: brickie-gen + brickie(条件嵌入 brickie-core)
make core       # cargo build --release -p brickie-core → build/host/<triple>/bin/brickie-core
make selftest   # brickie-gen --selftest
make launcher-smoke
make prebuilt       # 发布**三件**种子 {brickie,brickie-gen,brickie-core}
make prebuilt-check # 三件是否落后(不改盘)
```

* `CARGO_HOME` / `CARGO_TARGET_DIR` 缺省落在
  `prototype/build/{cargo-home,cargo-target}`(受管环境下 `$HOME/.cargo` 可能只读);
  可用环境变量覆盖。
* **降级**: `cxx/Makefile` 用 `CORE_EMBED := $(if $(wildcard $(CORE_BIN)),...)`
  条件嵌入 `brickie-core`。Rust 侧未产出时 **`make` / `make cxx` / `make selftest`
  照常成功**, 只打印提示, 载荷与入口 ELF 不含 core(此时启动器不设 `BRICKIE_CORE`)。
* **门禁不降级**: `make prebuilt` 依赖 `core`, 且把 `brickie-core` 作为必需件 ——
  缺 core ⇒ FAIL(自举种子必须三件齐)。`make prebuilt-check` 同样查三件。
* 启动器解包后: 载荷里有 `bin/brickie-core` 就设 `BRICKIE_CORE`(与 `BRICKIE_GEN`
  一样**不覆盖**显式环境变量); 没有就留空。

## 6. 载荷(`freeze.py`)

`freeze.py` 把 `python/**` + `templates/**` + `--embed` 指定的原生工具打成
**归一化未压缩 ustar tar**(路径排序、模式位显式、mtime/uid/gid 归零)⇒ 逐字节可复现。
`--embed` 可重复(当前 `brickie-gen` 必嵌, `brickie-core` 条件嵌), 同一落点重复嵌入
会在打包前报错。`payload.tar` 落在 `build/host/<triple>/share/brickie/` 供检查。

## 7. 已知缺口 / TODO

1. **Rust 核心是移动目标**: 本文件写作早期 `rust/` 反复编不过(先 3 个、后 11 个 rustc
   错), 那时 `make prebuilt` 在 `core` 目标处 FAIL(口径正确 —— 种子必须三件齐);
   Rust 侧编稳后已用**正规的 `make prebuilt`** 发布三件。若换个 checkout 又遇到 `rust/`
   编不过: `make` / `make cxx` 仍成功(载荷不含 core), `make prebuilt` FAIL —— 这正是
   设计要的降级口径。
   **注意门禁红 ≠ 回归**: 只要载荷源(`python/**`、`templates/**`、`schema/**`)或原生工具
   在最后一次 `make prebuilt` 之后又变过, `prebuilt-check` 与用例的"种子逐字节一致"
   就应当报红, 直到重新发布种子 —— 写作时这份 checkout 正处于这个状态。
2. **删掉 core 后载荷不会自动重打**: make 无法感知"某个嵌入件被删除"; 重新加回
   core(新文件比 `PAYLOAD_C` 新)会正常重打。出现"删 core"这种开发操作时需
   `rm build/.../obj/cxx/brickie_payload.cpp` 强制重打。
3. **缺变量的诊断没有专属码**: contract §6 只说"`BRV-GEN-0002` 同族用法错";
   BRV-D8 的编码表里没有对应码, 故当前用**无码用法错**(`code: null`, `exit_code = 2`),
   与 G-2/G-5 同族。建议补一个 `BRV-GEN-0003`(渲染缺变量)并回灌编码表。
4. **描述符 `.ver` 四段在模板里写死 `{0,1,0,0}`**: 骨架初值 = 声明面初值, 模板字面量
   不算判定; 若将来 `new` 支持非 `0.1.0.0` 的初值, 需要 core 额外发
   `compat_gen/major/minor/revise` 四个变量(或让描述符改读快照)。已在此登记。
5. **`templates/app/c/**` 未新建**: 按 contract 的约定, `plan-init` 的 app 骨架复用
   `templates/native/app/c/`(core 的 `plan.rs` 已如此), 故不需要独立目录。
6. **`build/` 里 `-j` 并发**: `payload` / `prebuilt` 依赖 core 落点, 顶层
   `prebuilt: core` 先串行编 core 再进 cxx 子 make, 保证子 make 解析时 core 已在。

## 8. 实测(本机 x86-64/linux)

* `cd prototype && make tools` — g++ 15.2 全绿, 零 warning。
* `make selftest` — 30 项全 `ok`。
* `render` — 4 类插件 × 5 模板 + 描述符 + product(共 31 件)用 `brickie-core` 实际
  会发的变量集渲染, `exit_code = 0`、`files` 齐、无诊断; 缺变量/越界路径/非法模式/
  协议不匹配等 9 条错误路径均按 §2 返回 `exit_code = 2`。
* 端到端(via 单文件 `brickie` ELF): `brickie new` 四类插件各一套全 `exit 0` 且 6 件
  产物齐(`platform/qemu-aarch64` 的符号前缀实测为 `qemu_aarch64_`); `brickie init hsm`
  `exit 0`, product.toml + app 骨架 + `brickie.lock` 齐(机器文件 `brickie.lock` 由
  render 按 §4 正确跳过, 内容由 L5 落盘)。
* `make launcher-smoke` — `brickie 0.1.0`。
* 种子: 按正规 `core → cxx` 路径发布**三件**; 写此文件时种子里的
  `brickie-core --selftest` = `ok 247 cases`。逐字节一致性只在"最后一次发布之后
  载荷源没再动"时成立, 动过就得 `make prebuilt` 重发(见 §7.1)。只拷 `brickie` 一个
  文件到空目录、`env -i PATH=/usr/bin:/bin` 下 `new platform/qemu-aarch64` 6 件产物齐
  (BRICKIE_GEN / BRICKIE_CORE 均指向解包出来的嵌入件)。
* 再跑一次 `make cxx` 是 no-op(0 条编译) ⇒ 载荷/ELF 可复现; `prebuilt-check` 只在
  种子跟上**最后一次发布**时才全绿。
* 载荷: 嵌入 `bin/brickie-gen` 与 `bin/brickie-core`(均 0755); 启动器实测设
  `BRICKIE_GEN` / `BRICKIE_CORE`、转发退出码、显式环境变量不被覆盖; 条件嵌入与三件
  `prebuilt` / `prebuilt-check` 用一个"假 core"验证通过(验证后已清理, 未把假件写进种子)。

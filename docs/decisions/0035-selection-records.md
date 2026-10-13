# 0035 — 根记录的形态: `{ brick = "<插件名>", requires = {} }`, `[[product.ability]]` 列表, 硬切换

> 状态: **已落地**(工具侧 `model.rs`(记录解析 + `Sel` 模型 + JSON)、`solver.rs`(种子 + 逐记录判定)、
> `selftest.rs`(`product_selection_cases` 重写 + 夹具改写)、`schema/product.schema.json`、
> `templates/product/c/product.toml.tmpl`; 声明面 `product.toml` 与 `tests/fx/**` 三份夹具)。
> 判据: `brickie-core --selftest` **555 例**(+23)、`make tools-test` **644 例**、
> `brickie check` 0 errors(24 插件闭包 / RAM 172 KiB / stack 4 KiB, 与改前**逐字节同结果**:
> `check --json` 无差异)、`brickie gen --check` 25 文件逐字节相同、
> `make tools-prebuilt-check` 3/3 ok(种子已重发)。
> 取代: **ADR-0034 的 D-3**(`[select].plugins` 退役为"可选兼容面")—— 本 ADR 直接**移除**它。
> ADR-0034 的 D-1/D-2(根的三分类、并集推导闭包)继续有效, 只是每个根的值从字符串升级为记录。
> 相关: ADR-0031(`core` 是第一类插件)、ADR-0009/0012/0022(`[select]` 的历史追加)、
> WORKAROUND `br-wa-fs-001`。

## 1. 背景: 根只有"名字", 没有"要求"

ADR-0034 之后选择面收敛成三个键 + 一张列表:

```toml
[product]
app = "app/hello"; platform = "platform/qemu-aarch64"; core = ">=0.1.0"
ability = ["service/dump", "fs/devfs", ...]
```

留下三个问题:

1. **同一个东西有两种写法**: `core` 是**版本区间**(`">=0.1.0"`), 而"选哪个 core"要写进 `ability`
   ——一个字段回答两个问题, 另一个字段只回答一半。`app`/`platform` 则相反: 只有名字, 没有区间。
2. **区间是**装饰**: `[product].core` 从 ADR-0004 起就被解析、进 JSON, 但**从未被任何判据读过**
   ——写 `">=9.9.9"` 与写 `">=0.1.0"` 的 check 结果完全一样。声明面里放一个不执法的字段,
   等于给读者一个假的保证。
3. **`ability` 的每一项都不可标注**: 一张纯字符串列表既不能挂版本要求, 也不能挂"这一项是什么"
   的说明 —— 而"这个能力件必须 >= x"恰恰是产品最该表达的事。

## 2. 决策

### D-1 每个根 = 一条记录 `{ brick, requires }`

```toml
[product]
name     = "brickos-prototype"
version  = "0.2.0.0"
app      = { brick = "app/hello", requires = {} }
platform = { brick = "platform/qemu-aarch64", requires = {} }
core     = { brick = "core/c_core", requires = { version = ">=0.1.0" } }
stage    = "dev"

[[product.ability]]
rr    = { brick = "sched/rr", requires = {} }
devfs = { brick = "fs/devfs", requires = {} }
```

* `brick` = **插件名**(即现有 `name`, 如 `fs/devfs`); 它同时是路径、是身份, 不引入第二种指称。
* `requires` = **该 brick 自己的**版本区间; `{}` = 不要求。键名是 `version`, 取值是 3 段区间
  (`§7.4` 的口径: 与 brick 的 `MAJOR.MINOR.REVISE` 比, 忽略 compat_gen)。空表是常态,
  因为大多数根不钉版本。
* `requires` **不表达**"这个根还依赖谁" —— 依赖由插件自己的 `[[dep]]` 声明, 产品只说"要什么"。

### D-2 `[[product.ability]]`: 数组表 + **别名键**

```toml
[[product.ability]]
rr    = { brick = "sched/rr", requires = {} }
devfs = { brick = "fs/devfs", requires = {} }
```

* 一个 `[[product.ability]]` 块可以放任意多项, 也可以写多个块(累积)。
* **键是别名(label)**: 只用于阅读与诊断定位; **插件身份一律取自 `brick`**。因此别名怎么起
  (甚至起成 `anything-at-all`)都不影响闭包、不影响 `brickie.lock`、不影响生成的描述符 ——
  这是有意的: 若别名能影响结果, 就会长成"第二套插件名"。
* 四个根全部住在 `[product]` 内(裁定: 用户选"全部住 `[product]`"), 于是选择面是一处, 不是两处。

### D-3 `core` 记录取代 `[product].core` 区间, 并**开始执法**

`core = { brick = "core/c_core", requires = { version = ">=0.1.0" } }` 一并回答"哪个 core"
与"兼容哪版"。旧字段 `[product].core`(纯区间)消失; 它不执法的那半条**在本刀补齐**:

* `requires.version` 不被该 brick 的版本满足 ⇒ **`BRV-VER-0002`**(与 `[[dep]].range` 同一个码、
  同一套 message 形状) —— 详见 §4.1: 这条补法会让**携带陈旧区间的夹具变红**, 因此夹具里的
  `core = ">=1.0.0"` 改成了 `{}`(见 §5.3)。
* 记录的**种类**与 `brick` 的 `plugin_type` 不符 ⇒ 新增 **`BRV-TAX-0024`**:
  `app`↔`app`、`platform`↔`platform`、`core`↔`core`; `[[product.ability]]` 接受 `ability` 与
  `interface`(产品显式选一个皮肤仍是合法形状), 但**不接受** app/platform/core(它们有自己的记录)。

### D-4 硬切换: 旧形式是形状错

| 旧形式 | 现在 |
|---|---|
| `app = "app/hello"` | **形状错**(退出码 2), 提示给出新写法 |
| `core = ">=1.0.0"` | **形状错** |
| `platform = "..."` 或 `["..."]` | **形状错** |
| `ability = ["fs/devfs"]`(纯字符串列表) | **形状错** |
| `[select]` 表(即使 `plugins = []`) | **形状错** |

理由: 兼容面会让"两处真值"长期并存(本仓最贵的一类债), 而这次的形状变化是**值**的变化,
不是**语义**的变化 —— 迁移是机械的(每条列一个记录), 夹具与小产品都能一次改完。
"`[select]` 存在即错"也覆盖了"表在但空"这种最容易被忽略的残留。

## 3. 与设计文档的关系

* 设计侧 `brickie-v0.1.md` §7.2 给的是"单记录形状"的最小集合; `brick = <插件名>` 与
  `requires` 是**本仓原型扩展**(同 ADR-0017/0031 的做法: 扩展先落在实现上, 由 ADR 记录口径)。
* 与 ADR-0031 §"被否决的替代方案"不冲突: 那次否决的是"让 `core` 一个字段兼两问";
  本 ADR 让 `core` **只回答它自己那两问**(哪个 core = `brick`, 兼容哪版 = `requires.version`),
  并把"选哪些能力件"交给 `[[product.ability]]`。
* `requires` 的键集被**收紧**(只认 `version`): 未知键 ⇒ 形状错。理由是新表还没有历史包袱,
  而一个拼错的 `verison` 会静默变成"没有要求" —— 静默丢失一条产品要求比报错贵得多。
  同理, 记录内未知键(`range = ...`)也报错。

## 4. 后果与遗留项(诚实清单)

1. **`requires.version` 的比较口径是 `MAJOR.MINOR.REVISE`**(忽略 compat_gen, `§7.4` 既有口径)。
   于是"版本 `1.0.0.0`"的 triple 是 `(0,0,0)`, 区间 `">=1.0.0"` 对它**不成立** ——
   这不是新语义, 而是既有 `[[dep]].range` 一直如此; 本刀只是让它第一次真的被检查。
2. **`app` 的区间暂无意义出口**: `app = { brick = ..., requires = { version = ... } }` 会被
   正常检查(与其它三个根同一条路径), 但"APP 的版本"目前没有任何别的地方会读 —— 保持统一形状,
   比为一个根特例去掉 `requires` 更简单。
3. **夹具携带的陈旧区间被清掉**(`core = ">=1.0.0"` 在 `fx/{build,showcase,budget}` 里从未执法,
   且按 (1) 的口径对 `1.0.0.0` 不成立)⇒ 夹具改写成 `requires = {}`。这一条是"补齐执法"暴露出来的
   **既有声明面瑕疵**, 不是回归。
4. **`platform.brick = ""` 表示"声明了键但没选根"**(`plan-init` 的骨架形态): 三个单例都容忍空
   `brick`, 只有 `[[product.ability]]` 的条目必须给出名字(它本来就没有"空"的含义)。
5. **别名会影响诊断顺序**: TOML 表在解析后按键名字典序排列, 因此 `roots`(诊断/JSON 的顺序)
   按别名排序; 种子集合另行排序 ⇒ **结果与别名无关**, 只有报错行的先后会变。
6. **`BRV-TAX-0024` 只覆盖"根记录 ↔ 插件类型"这一对关系**, 不看依赖图(依赖方向仍由
   `dep_direction_ok` 与 ADR-0018 的包含面执法)。
7. **种子已重发**: `brickie-core` 与入口 `brickie` 变(负载含 schema + 模板), `brickie-gen`
   逐字节未变; `prebuilts/README.md` §5 台账 + §5.1 指纹同一次刷新。

## 5. 判据(本刀怎么算绿)

```bash
brickie-core --selftest                      # 555 例(记录形态 / 种类 / 区间 / 硬切换 的正负例)
make tools-test                              # 644 例(夹具已改写成记录形态, 快照不变)
build/host/x86-64/linux/bin/brickie check     # 0 errors; 24 插件闭包 / RAM 172 KiB / stack 4 KiB
build/host/x86-64/linux/bin/brickie gen --check
make tools-prebuilt-check                    # 3/3 ok
```

负例(必须红, 均已实测):

* `app = "app/m"` / `core = ">=1.0.0"` / `ability = ["service/mem"]` / `[select]` ⇒ 形状错,
  **退出码 2**(`BRV-MF-0001`);
* `core = { brick = "service/mem" }`(能力件)⇒ **`BRV-TAX-0024`**; `platform` 记录写 core 同样红;
* 记录里写 `range = ">=1.0.0"`(未知键)⇒ **`BRV-MF-0001`**;
* `core = { brick = "core/c_core", requires = { version = ">=9.0.0" } }` ⇒ **`BRV-VER-0002`**;
* 从 `core` 记录里去掉 brick(写成 `brick = ""`)⇒ **`BRV-TAX-0020`**(found 0);

正例(必须绿, 均已实测): `[[product.ability]]` 里放一个 **interface 皮肤** ⇒ 不报
`BRV-TAX-0024`(这条列表容纳"不是三种单例"的一切); 别名写成 `anything-at-all` ⇒ 闭包与
`brickie.lock` 一字不变。

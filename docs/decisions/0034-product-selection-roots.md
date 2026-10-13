# 0034 — 产品的**选择面**: `[product]` 增 `platform` / `ability` 两个闭包根, `[select].plugins` 退役为可选兼容面

> ★ **部分被 [ADR-0035](0035-selection-records.md) 取代**: 本 ADR 的 **D-1/D-2 仍然成立**
> (根 = `app` + `platform` + `ability`; 闭包种子由它们并集推导), 但 **D-3 里"`[select].plugins`
> 保留为可选兼容面"与"不设互斥/硬切换"那半条不再成立** —— ADR-0035 把四个根改成**记录形态**
> (`{ brick, requires }`, `ability` 用 `[[product.ability]]`), 并把旧的裸字符串形式与
> `[select]` 表一并**移除**(形状错, 退出码 2)。本 ADR 的顶层键名(`platform`/`ability`)
> 与"直接选中的根"这一语义被 ADR-0035 继承。

> 状态: **已落地**(工具侧 `model.rs`(解析 + JSON)、`solver.rs`(种子)、`selftest.rs`(新增
> `product_selection_cases`)、`schema/product.schema.json`、`templates/product/c/product.toml.tmpl`;
> 声明面 `product.toml` 的 `[select].plugins` 15 项 → 根 12 项)。
> 判据: `brickie-core --selftest` **532 例**、`make tools-test` **644 例**、`brickie check` 0 errors
> (24 插件闭包 / RAM 172 KiB / stack 4 KiB, 与改前**逐字节同结果**: `check --json` 无差异,
> `build --dry-run --json` 仅差一条提示文案)、`brickie gen --check` 25 文件逐字节相同、
> `make tools-prebuilt-check` 3/3 ok(种子已重发)。
> 相关: ADR-0031(`core` 成为第一类插件时**否决**过 `[product].core` 升级为选择键, 理由见 §4.1)、
> ADR-0009(VFS 存储栈的 `[select]` 追加)、ADR-0012(file-table 的"直接选中")、
> ADR-0022(eMMC/FAT/EROFS 的三条选择项)、WORKAROUND `br-wa-fs-001`(挂载计划不生成选择边)。

## 1. 背景: 选择面写着 15 项, 其中 3 项是"根"、6 项是闭包成员

`product.toml` 的 `[select].plugins` 是产品的**唯一**选择面。随着每刀追加, 它长成了 15 项,
而其中真正"产品决定要它"的只有三件: **APP**(`fs` 之外的业务入口)、**platform**(板级事实)、
**scheduler/服务/驱动等能力件**。其余六件 framework(`dev-core` / `cdev-core` / `bdev-core` /
`vfs-core` / `file-table` / `pci-core`)与两个 iface 皮肤(`iface/posix` / `iface/min`)其实**早已**
由前三者的 `[[dep]]` 拉进闭包 —— 它们出现在 `[select]` 里, 是历史追加留下的冗余。

冗余的代价不是行数, 而是**语义**: 一份列表同时混着"根"与"派生", 读的人无法区分
"删掉这行会剪掉能力"与"删掉这行什么都不会变"。ADR-0009/0012/0022 的正文都不得不额外说明
"这条是可选的, 闭包会带进来" —— 说明本身就是这个形状的代价。

## 2. 决策

### D-1 `[product]` 表达**根**, 三个键各回答一个问题

```toml
[product]
app      = "app/hello"              # 唯一的 APP
platform = "platform/qemu-aarch64"  # 唯一的平台
ability  = ["core/c_core", ...]     # 直接选中的能力件(含 core 本身)
```

* `app` / `platform` 是**单数**键(设计 §3.2/§8.3 ④ 的"每镜像恰一个"); `ability` 是列表。
* `core` 仍是**版本区间**(`>=0.1.0`), 不是选择键: "选哪个 core"由 `ability` 里的名字回答,
  "与哪个 core 兼容"由 `[product].core` 回答 —— 两个问题, 两个字段(见 §4.1)。
* `platform` 也接受列表(历史形状兼容), 但推荐单值。

### D-2 闭包种子 = `app + platform + ability + [select].plugins`

`solver.rs` 的种子集合按此顺序并集去重。因此:

* 增量迁移安全 —— 老 `[select].plugins` 照旧生效, 两种写法可以共存(产品可逐条搬);
* 引用完整性沿用既有判据: 种子里的名字不在插件树 ⇒ **`BRV-MF-0001`**;
  闭包内 `plugin_type = "core"` 数量 ≠ 1 ⇒ **`BRV-TAX-0020`**;
* 空值即"没有这个根": `platform = ""` / `ability = []`(模板骨架的默认形态)不报错。

### D-3 `[select].plugins` 退役为**可选兼容面**, 不再是首选写法

schema 里保留(并注明 legacy), `plan-init` 模板不再生成它。**不设"二者只能取一"的互斥判据**:
并集语义下互斥判据只会制造一类新的报错, 而合并规则本身无歧义。

### D-4 本产品的选择面重写为 12 根

```toml
app      = "app/hello"
platform = "platform/qemu-aarch64"
ability  = ["core/c_core", "sched/rr", "service/dump",
            "fs/uboot-fat", "fs/uboot-erofs", "fs/tmpfs", "fs/devfs",
            "driver/uart-pl011", "driver/uboot-mmc"]
```

删掉的 6 项 framework + 2 项 iface 全部由各自的消费者 `[[dep]]` 带出, **闭包内容一字不变**
(24 插件)。

## 3. 为什么 `fs/devfs` / `driver/uart-pl011` / eMMC 栈仍是根

它们**没有**"谁来 `[[dep]]` 我"的答案: `/` 与 `/dev` 的挂载点在 APP 的 `[[mount]]` 计划里,
而**挂载计划不生成选择边**(WORKAROUND `br-wa-fs-001`: 挂载不是依赖, FS 只登记驱动)。
所以删掉这些行不会"剪掉一条依赖", 而是**删掉一个卷/一个 tty** —— 这正是产品该显式决定的事。
把它们标成"可选行"的注释此前散在三条 ADR 里, 现在只有一处: `product.toml` 的根列表。

## 4. 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| **什么都不改**: 只精简注释, `[select]` 保持 15 项 | 语义混杂的问题原样保留: 列表读不出一条"删了会怎样"的规则; 每加一刀就多一行需要 ADR 解释"这行为什么在这" |
| 让 `ability` 成为顶层 `[select].ability`(与 schema 的既有 `[select]` 表同层) | 三个根会分住两个表(`app`/`platform` 在 `[product]`, `ability` 在 `[select]`), 而它们回答的是同一类问题; 直接放进 `[product]` 更短且与 `app` 对称 |
| 自动推导: APP 的 `[[mount]]` 计划补上 `fs/*` 与 block 驱动的选择边 | 那会把"挂载"与"依赖"两个概念合并 —— `br-wa-fs-001` 已裁定挂载不产生依赖(挂载可以指向运行时才出现的设备); 且"要不要 /dev 命名空间"是产品形状(设计 `8-01` §1.2 的 form B), 不是 APP 的实现细节 |
| 只给 `platform` 升级, `ability` 仍写进 `[select].plugins` | 只解决 3 项里的 1 项; `[select]` 剩下的仍然混着根与派生 |

### 4.1 与 ADR-0031 的否决记录更正是**有意的**

ADR-0031 §"被否决的替代方案"里写过: "把 `[product].core` 从版本区间升级为'选择 + 区间' ——
与 `[select].plugins` 的既有语义重叠(两处选择)"。本 ADR 与它不冲突, 因为方向相反:

* 被否决的是**让 `core` 这一个字段同时承担两个问题**(取哪个 core + 与哪版兼容);
* 本 ADR 新增的 `ability` 是"选择"这一问题的**通用**表达, `core` 与 `fs/tmpfs` 在里面是同一种东西
  (一个被直接选中的插件), 而 `[product].core` 保持"只回答兼容区间"。

## 5. 后果与遗留项(诚实清单)

1. **`platform` 的插件类型没有被约束**: `[product].platform` 指向一个 `plugin_type != "platform"`
   的插件不会报错(与 `app` 的现状一致 —— 类型基数由设计 §3.2/§8.3 ④ 的成文约束与
   `BRV-TAX-0020`(core)承担)。若要执法, 是一刀独立的改动(新增/复用一个判据), 不在本刀范围内。
2. **`has_platform` 仍是全树判据**(闭包外的 platform 也算), 与 `[product].platform` 无关 —— 既有
   行为未动, 本刀不改它。
3. **`build.rs` 的 `BRV-BLD-0013` 提示文案**从 "add the plugin to `[select].plugins`" 改成
   "add the plugin to `[product].ability`"; 由于该提示进 `build --dry-run --json`, 基线与新结果
   在这一点上**不同**(其余逐字节相同)。
4. **`tools/brickie/tests/fx/**` 的夹具继续用 `[select].plugins`** —— 它们正是 D-2 并集语义的回归
   保护(老写法必须继续工作)。
5. **种子已重发**: `brickie-core` 与入口 `brickie` 变(负载含模板/schema), `brickie-gen` 逐字节未变;
   `prebuilts/README.md` §5 台账 + §5.1 指纹同一次刷新。
6. **`app + platform` 两个根不足以组成镜像**: 若 `ability = []`, 闭包收敛到 7 件
   (`app/hello` + `platform/qemu-aarch64` + POSIX 链), 此时 `core` 缺失 ⇒ `brickie check` 报
   **`BRV-TAX-0020`**(found 0)。这是正确行为而非缺陷: core 必须由产品显式选中(D-1 的第三问),
   不会被任何 `[[dep]]` 带进来。

## 6. 判据(本刀怎么算绿)

```bash
brickie-core --selftest                      # 532 例(含 platform/ability 种子等价性与负例)
make tools-test                              # 644 例(夹具仍走 [select] 老形状)
build/host/x86-64/linux/bin/brickie check     # 0 errors; 24 插件闭包 / RAM 172 KiB / stack 4 KiB
build/host/x86-64/linux/bin/brickie gen --check
make tools-prebuilt-check                    # 3/3 ok
```

负例(必须红):

* `ability` 里写一个不在插件树里的名字(`service/ghost`)⇒ **`BRV-MF-0001`**;
* `ability` 写成字符串而非列表 ⇒ **`BRV-MF-0001`**(形状);
* 从 `ability` 去掉 `core/c_core` ⇒ **`BRV-TAX-0020`**。

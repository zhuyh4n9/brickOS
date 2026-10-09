# iface/posix

> 设计出处: `1-03` §1 的 v1.0 清单 —— “`iface-posix` | Interface | 薄皮肤:
> 再导出 svc-posix + stdio/errno 接线 | svc-posix | M2”(★ 设计名 `iface-posix`; 原型按
> `1-01` §8.3 的命名空间形态落在 `iface/posix`, 见 ADR-0020);`1-01` §7.4(skin 分类)/
> §7.3(**APP 仅经 Interface**, A-2 的收敛口径)/D18(POSIX 双角色拆分)。

本插件是原型里**第一个 Interface 插件**, 也是第一个 `form = "skin"` 的运行实例。

## 它做什么(以及刻意不做什么)

| 做 | 不做 |
|---|---|
| 把 `runtime/posix#posix` 的 POSIX 头**原样转出**(`include/iface/posix/posix.h`) | 不自己声明/实现任何 POSIX 函数(两份声明 = 同一符号两种真值) |
| 声明级再导出: `form = "skin"` + `reexport_of = ["runtime/posix#posix"]` | 不拥有状态、不碰 fd 表、不做 errno 转换(那些是 runtime/posix 的事) |
| 给 APP 一个**合法**的依赖对象(方向表: `app → interface` ✅) | 不让 APP 直连 `runtime/posix`(那是 `app ✗ ability`, 设计禁则) |

## 依赖与相位

```
app/hello  ──[[dep]] runtime──▶  iface/posix  ──[[dep]] init(late)──▶  runtime/posix
                 (interface)                      (薄皮肤, 无状态)          (唯一实现)
```

* `plugin_type = "interface"` ⇒ ② 完成点在 **LATE**(`1-01` §6.2), 所以 `phase = "late"`;
* `api_type = "runtime_adapter"` 不是自选: 不变量 1(`api_iface == api_type`)与不变量 3
  (`api_iface` 必须等于被再导出单元的分类)把它钉死 —— `runtime/posix#posix` 就是这个分类;
* 单元名取 **`posix-skin`**(不叫 `posix`): 单元 id 在闭包内必须唯一, 而
  `runtime/posix#posix` 已经占用了 `posix`。

## 与 `runtime/posix` 同名(`posix`)的处理

命名空间形态让本件与 `runtime/posix` **共享 `name_short = posix`**。缺省的钩子符号前缀由
short 推导(会撞成同一个 `posix_early_init/init/start` ⇒ 链接期重复定义), 因此本件在
`plugin.toml` 里**显式覆盖**:

```toml
[plugin]
symbol_prefix = "iface_posix_"   # 钩子/描述符用 iface_posix_*, 不是 posix_*
```

## 为什么值得存在(而不是让 APP 直连)

`runtime/posix` 是 **ability**;设计 §7.3 的方向表里 `app ✗ ability` —— APP 只许经 Interface。
没有本件时, APP 想用 `usleep`/`pthread_*` 只有两条路: 直连(非法边, 要 `allow_edges` 豁免)
或不用 POSIX。本件把第二条路变成正解: 一个**零状态**的皮肤, 让"APP 经 Interface 用 POSIX"
既合法又零开销(皮肤里没有一行代码)。

## 目录

```
plugin.toml                       人写   ← 插件级唯一真值(skin + reexport_of + symbol_prefix)
include/iface/posix/posix.h       人写   ← 皮肤面 + 三个钩子原型
src/iface_posix.c                 人写   ← 三个空钩子(皮肤无状态)
tests/smoke.toml                  人写   ← 用例声明面(骨架)
```

接口面快照由 `brickie iface publish iface/posix#posix-skin` 产出到
`api/iface/iface/posix/posix-skin.toml`(机器拥有); `plugin.toml` 里的 `hash` 由人抄回
(§9.1.1: 机器不改人写文件)。

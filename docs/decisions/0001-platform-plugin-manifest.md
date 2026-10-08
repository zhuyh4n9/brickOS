# 0001 — platform 代码收敛为 brickie 管理的插件

- 状态: 已采纳
- 日期: 见仓库首次提交(本仓不引入构建时间戳, 与 `brickie.lock`/`api/iface/**` 的
  "逐字节可复现"纪律同向, 见 `brickie-v0.1` §9.1.1 与工具契约 §4)
- 相关: `WORKAROUNDS.md` 的 `br-wa-entry-001`; 设计 `1-03` §1(插件清单)、
  `4-02`(插件布局)、`4-03`(manifest)、`1-01` §6.3(分类学)、`brickie-v0.1` §8.3/§8.4

## 背景

prototype 的 `platform/` 一直是"被 Makefile 直接编进镜像的一组文件": reset 汇编 /
向量表 / 链接脚本 / PL011 早期 console / arch timer 都没有 manifest、没有描述符,
也不能被组合器发现与校验。这条欠债登记为 **`br-wa-entry-001`**, 其退出条件写明:

> ① `br` 能按布局约定发现并校验插件; ② 本目录收敛为插件 `platform/qemu-aarch64`
> (带描述符 + manifest); ③ `start.S` 的调用点从"Makefile 直编"改为 `.br_plugins`
> 段枚举驱动。

`brickie` v0.1 交付后, ① 已具备(发现 / 校验 / 描述符生成 / 接口发布 / 版本治理)。

## 决策

1. **目录即插件**: `platform/qemu-aarch64/`(顶层 `platform/` = namespace, §8.3 ④),
   内部布局按 `4-02` §2 —— `plugin.toml` 在插件根、对外头文件在 `include/`、
   生成物一律落仓库级 `build/gen/<plugin>/`。
2. **声明面唯一真值**: 平台契约(`br_plat.h` 的 7 个符号)在 `plugin.toml` 的
   `[[export.entries]]` 里显式列出; 描述符 C 代码是生成物, 由 `brickie gen` 重建
   (v0.1 不编译它 —— V-9 的零编译依赖)。
3. **APP 与 M0 引导例外**: 镜像的 APP 是 `app/hello`(`1-03` §1 明写"M0: 直接主循环,
   **不依赖 iface —— M0 引导例外**")。它到 `platform/qemu-aarch64` 的运行期边
   **违反 `§7.3` 的 `app ✗ platform` 通则**, 因此**显式登记**为 `product.toml`
   `[lint].allow_edges` 的一条豁免 —— 例外必须是可评审的, 不许静默放行。
   退出条件: `iface-min` 交付(M2)后改为 `app → iface → platform`。
4. **core 不是插件**: `core/` 是内核本体(被 `[compat].core` 引用), 不进插件树。
   `br_core_main` 的实现随 APP 迁到 `app/hello`(它是 M0 的 app 入口), 声明仍由
   core 头文件 `br/core/br_main.h` 提供。
5. **组合校验接进构建**: `make` 在编译镜像前先跑 `brickie check`(声明面完备性);
   release 镜像用 `brickie check --profile release`。生成物由 `brickie gen` 重建,
   **不参与编译**(编译编排是 v0.3 的能力)。

## 后果

- `br-wa-entry-001` 的 ①② 由本次还清; **③ 仍欠**(`.br_plugins` 段驱动的启动链属 M0
  运行期), 该 workaround 条目改为"部分还债"并保留。
- `br-wa-boot-001` 的范围收窄: MainLoop 现在**明确地**属于 APP 插件, 不再假装是 core
  入口; 但"拆成 `app.start()` + 调度器"仍是 M0/M1 的工作。
- 镜像的依赖闭包、资源预算、特权声明从"没有地方可查"变成"一条命令可查"
  (`brickie check` / `brickie dep closure`)。

## 未采纳的备选

| 备选 | 为什么不取 |
|---|---|
| 保留 `platform/src/aarch64/` 不变, 只加一个旁路 manifest | 违反 §8.3 ④"目录名 = 插件名"; 声明面与源码分居两处, 迟早漂移 |
| 为 M0 造一个 `iface/plat` 皮肤, 让 app 经 interface 依赖平台 | M0 尚无 Interface 插件(设计明说"此时 Interface 插件尚未交付"), 造一个只为规避规则的空皮肤 = 用假声明换绿灯 |
| 把 `core/` 也做成插件 | core 是所有插件的基座与 `[compat].core` 的对象, 不是可选组合件; 插件化会与"每镜像恰一个 platform、恰一个 app"的数量约束混淆 |

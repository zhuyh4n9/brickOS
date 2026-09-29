# 15 — 接口管理(interface management)

> 分类: **9-interface**。状态: **骨架**。
> 来源: 主文档 §6.3(Interface 类别)、§7(POSIX 与接口皮肤); 决策 D11(接口插件化)/D13(叠加规则)/D18(POSIX 双角色)。

## 1. 范围与现状

Interface 插件 = APP 看到的 API 皮肤。**严格叶子**: 除 APP 外没有任何插件依赖它们——这是依赖宪法(§7.2)的单向流保证(APP 亦是插件, 但它是唯一允许的依赖方); **薄皮肤**: 只做再导出/别名 + 接线, 不拥有状态。

已有决策(待深化时继承):
- **D11**: 接口插件化——POSIX 不再是 core 的形状, 而是 APP 可选的皮肤
- **D13**: 叠加规则——模块级声明 + 符号级真值(双层混合; 01 §3)
- **D18**: POSIX 双角色拆分——`svc-posix`(POSIX 运行时, 服务)与 `iface-posix`(薄皮肤, 严格叶子)分离; 接口只做薄皮肤(再导出/别名 + 接线, 如 iface-min 直通 native), 不拥有状态
- v1.0 接口: **iface-posix**(再导出 svc-posix + stdio/errno 接线)+ **iface-min**(直通 native 的极简别名)
- 未排期: iface-autosar-ish(车规生态)

## 2. 大纲(待成文)

1. 再导出机制: 符号表层面如何实现(#define? 弱符号? 链接器脚本 [?])与 D13 的符号级真值对齐
2. iface-posix: POSIX 子集清单(与 svc-posix 的 frozen 面对齐, 01)、stdio/errno 接线细节
3. iface-min: 别名表设计——APP 用最短路径直通 native
4. 多接口共存: 一个 APP 同时依赖多个接口的链接语义(符号冲突 = 组合期错误)
5. 接口版本协商: APP 声明 `requires: iface-posix@>=x.y`
6. 新接口的判据: 什么时候值得加一个 iface(生态驱动, 不是技术驱动)

## 3. 开放问题

| # | 问题 |
|---|---|
| — | 再导出的实现选型: 宏映射(零运行时成本)vs 链接层 [?] |
| — | 接口是否提供 native API 的超集封装(语义糖)——倾向: 不, 保持薄 |

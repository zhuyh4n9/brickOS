# 10-01 — 接口管理(interface management)

> 章节: **10-interface**。状态: **骨架**。
> 来源: 主文档 §6.3(Interface 类别)、§7(POSIX 与接口皮肤); 决策 D11(接口插件化)/D13(叠加规则)/D18(POSIX 双角色)/**D24(iface-pkcs11 前移 v1.x)**; 域标准判例: `docs/9-app/9-02-hsm-sample.md`。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **API** | Application Programming Interface | 应用程序接口 |
| **APP** | Application | 应用(插件类别: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **errno(E*)** | error number | POSIX 错误码; core 以**负值**返回 `-EINVAL`/`-EAGAIN`/`-ETIMEDOUT`/`-ENOTSUP`/`-EBUSY`/`-EEXIST`/`-EIO`/`-ENODEV`/`-ENOMEM`/`-ENOSPC` 等, 域用子集 |
| **EXPERIMENTAL / FROZEN / DEPRECATED** | — | native API 生命周期三态标注(1-02 §2.1) |
| **HSM** | Hardware Security Module | 硬件安全模块(第二产品域; v1.x/M5 样例的 iface-pkcs11 消费者) |
| **PKCS#11** | Public-Key Cryptography Standards #11 | 密码 token 接口标准(iface-pkcs11 的语义来源) |
| **POSIX** | Portable Operating System Interface | 可移植操作系统接口 |
| **stdio** | standard input/output | C 标准输入输出接口(iface-posix 的接线面) |
| **weak symbol** | — | 弱符号: 可被同名强符号覆盖(再导出/别名的候选实现) |

> **编号约定**: `D11`/`D13`/`D18` = 全局决策(接口插件化 / 叠加规则 / POSIX 双角色); `1-02 §3` = 双层粒度。

## 1. 范围与现状

Interface 插件 = APP 看到的 API 皮肤。**严格叶子**: 除 APP 外没有任何插件依赖它们——这是依赖宪法(§7.2)的单向流保证(APP 亦是插件, 但它是唯一允许的依赖方); **薄皮肤**: 只做再导出/别名 + 接线, 不拥有状态。

已有决策(待深化时继承):
- **D11**: 接口插件化——POSIX 不再是 core 的形状, 而是 APP 可选的皮肤
- **D13**: 叠加规则——模块级声明 + 符号级真值(双层混合; 1-02 §3)
- **D18**: POSIX 双角色拆分——`svc-posix`(POSIX 运行时, 服务)与 `iface-posix`(薄皮肤, 严格叶子)分离; 接口只做薄皮肤(再导出/别名 + 接线, 如 iface-min 直通 native), 不拥有状态
- v1.0 接口: **iface-posix**(再导出 svc-posix + stdio/errno 接线)+ **iface-min**(直通 native 的极简别名)
- **v1.x/M5(D24)**: **iface-pkcs11 前移**——加密 token API(PKCS#11 子集)薄皮肤, 适配 `service/crypto` + `service/keyring`(原排 v2.0); 它是 **HSM 完整样例的接口面**, 消费者只有 `app/hsm`(严格叶子的判例)
- 未排期: iface-autosar-ish(车规生态)

## 2. 大纲(待成文)

1. 再导出机制: 符号表层面如何实现(#define? 弱符号? 链接器脚本 [?])与 D13 的符号级真值对齐
2. iface-posix: POSIX 子集清单(与 svc-posix 的 frozen 面对齐, 1-02)、stdio/errno 接线细节
3. iface-min: 别名表设计——APP 用最短路径直通 native
4. **iface-pkcs11(v1.x/M5, D24)**: PKCS#11 子集清单与两个后端的边界(crypto = 运算 / keyring = 密钥生命周期); 拒绝语义的 errno 映射(`-EPERM` 缺口见 `docs/9-app/9-02-hsm-sample.md` §4.2 O-H8); **域标准皮肤的泛化判例**(9-02 §10)
5. 多接口共存: 一个 APP 同时依赖多个接口的链接语义(符号冲突 = 组合期错误)
6. 接口版本协商: APP 声明 `requires: iface-posix@>=x.y`
7. 新接口的判据: 什么时候值得加一个 iface(生态驱动, 不是技术驱动)

## 3. 开放问题

| # | 问题 |
|---|---|
| — | 再导出的实现选型: 宏映射(零运行时成本)vs 链接层 [?] |
| — | 接口是否提供 native API 的超集封装(语义糖)——倾向: 不, 保持薄 |

/*
 * brickOS prototype v0.2.0 — 版本标识
 *
 * 版本号政策(设计文档 1-03 §4, 在 brickOS-Design 分支上): 主版本 = 能力代际,
 * 不承诺时间。原型版本号与 OS 版本号(v1.0)不同轴 —— 原型是"验证设计"的载体。
 *
 * ★ v0.2.0 与 v0.1.0 只差"能力代际"这一轴, 所以是 0.2.0 而不是 1.0.0:
 *   代际内容 = **运行期的那一半**(插件管理器 + 声明面驱动的构建 + 调度/同步/POSIX/
 *   VFS/设备框架 + ADR-0011..0022)。这里**没有任何 ABI 承诺**, 也没有"稳定接口"的意思 ——
 *   连接口单元的冻结都还只在 `iface/posix` 一处有影子(`brickie iface freeze`)。
 */
#ifndef BR_CORE_BR_VERSION_H
#define BR_CORE_BR_VERSION_H

#define BR_PROTOTYPE_NAME     "brickOS-prototype"

#define BR_VERSION_MAJOR      0
#define BR_VERSION_MINOR      2
#define BR_VERSION_PATCH      0
#define BR_VERSION_STRING     "v0.2.0"

#endif /* BR_CORE_BR_VERSION_H */

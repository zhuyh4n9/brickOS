# 14 — 应用模型(APP)

> 分类: **8-app**。状态: **骨架**。
> 来源: 主文档 §2(单 APP 原则)、§6.3(APP 插件类别)、§9(启动序列: APP 最后); 样例: `docs/1-architecture/02-roadmap.md` v1.0 插件清单(app/hello + conformance)。

## 1. 范围与现状

**单 APP 原则**(产品定义): 产品镜像 = OS Core + 选中插件 + **恰好一个 APP**。APP 是插件类别之一——享受同样的组合/校验/版本机制, 但它是终点(没有插件依赖 APP; APP 只依赖 Interface(或直调 native))。

已有决策(待深化时继承):
- APP 依赖 **Interface 插件**(严格叶子, §6.3)或直调 native; 服务经注册表按名取用(§6.3; manifest 级 APP→Service 依赖是否允许 = 开放问题, 见 §3)
- 启动序列(§9): 全部插件 init 完 → 开全局中断 → **进入 APP main**
- conformance APP = 测试 APP(01 §2.3 层 2 的载体); hello = 启动链演示(M0)

## 2. 大纲(待成文)

1. 入口约定: `main` 签名(argc/argv 从 manifest? 环境指针 `tg_app_env` [?])
2. APP 生命周期: main 返回后的语义(idle? restart [?]; 看门狗喂狗约定 [?])
3. APP 的 manifest: 依赖声明(接口/服务)、RAM 预算、栈/堆分配
4. APP 与 trace(03): 事件 id 段位分配(app 段)
5. 多线程约定: APP 建线程(数据路径) vs 服务建线程(拥有状态)——主人规则(§7.2)
6. conformance APP 的结构: 断言集、三调度器矩阵运行方式

## 3. 开放问题

| # | 问题 |
|---|---|
| — | APP main 的 argc/argv 供给: manifest 静态参数 [?] |
| — | APP panic 策略: 与 ramdump/bridge(03)的 panic 独立通道如何配合 |
| — | "单 APP"边界: APP 内**多进程**语义不存在(无 fork/exec)——文档化这个限制; 多线程存在(§2 大纲第 5 条, 主文档 §2.1「调度框架存在的理由」) |
| — | manifest 级 APP→Service 依赖是否允许(如需 init 顺序保证) [?] |

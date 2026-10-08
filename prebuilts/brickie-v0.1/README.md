# brickie **v0.1** 工具在哪

> 这一级目录**只放指针, 不放第二份二进制**。工具的唯一进库落点是设计规定的
> **自举种子**: `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/`(ADR `0004` /
> `brickie-v0.1` BRV-D5)。在这里再放一份拷贝会造出"两个落点、谁是真值"的问题 ——
> 与 `api/iface/**` vs `plugin.toml` 的双真值纪律同一条理由。

## v0.1 的三个可执行(三件齐才叫一个完整种子)

```
prebuilts/seed/brickie/<host-arch>/<host-os>/bin/
├── brickie        ← L5 入口: **单文件自包含 ELF**(Python 前端 + 模板 + schema + 两个原生工具)
├── brickie-gen    ← L2 生成器(C++): 只做模板渲染
└── brickie-core   ← L0/L1 核心(Rust): 模型 / 求解 / 版本 / 接口引擎(唯一判定处)
```

当前仓库里的一份: `x86-64/linux`(见 [`seed/brickie/x86-64/linux/bin/`](seed/brickie/x86-64/linux/bin/))。

## 用法(只拷 `brickie` 一个文件也能跑)

```sh
# 单文件自包含: 不需要 PYTHONPATH / 源码树 / 同目录的 brickie-gen / brickie-core
prebuilts/seed/brickie/x86-64/linux/bin/brickie --version --deps
prebuilts/seed/brickie/x86-64/linux/bin/brickie check --root <插件树根>
prebuilts/seed/brickie/x86-64/linux/bin/brickie new ability service/crypto --subkind service
```

## 重新发布 / 校验

```sh
make tools-prebuilt         # 编三件并发布到 prebuilts/seed/brickie/<triple>/bin/
make tools-prebuilt-check   # 任一缺失或落后于源码 ⇒ 红(不改盘)
```

细节(查找顺序、载荷格式、文件指纹、发布台账)见 [`prebuilts/README.md`](../README.md)
与 [`tools/brickie/README.md`](../../tools/brickie/README.md)。

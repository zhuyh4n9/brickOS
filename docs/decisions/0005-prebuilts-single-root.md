# 0005: `prebuilts/` 收敛为单一顶层目录(`seed/` 与 `toolchain/` 分侧)

> 状态: **已接受** | 影响面: **仓库目录结构 + `.gitignore` + 取件脚本 + `Makefile` + 门禁/前端路径引用**(prototype 分支)
> 格式依据: `1-02` §2.2(动机 / 设计 / 替代方案 / 兼容性影响)
> **取代**: ADR [`0002`](0002-prebuilt-toolchain.md) §2-1(「`prebuilt/`(**单数**)不进版本库」的**目录名**部分)与 ADR [`0004`](0004-brickie-prebuilts-bootstrap.md) §2 的「两者**不合并**、不互为别名」条文
> 关联: `2-02` BR-D5(状态/增量/可复现); ADR-0003(顶层 `Makefile` 退役路径)

## 1. 动机

1. **`prebuilt/`(单数)与 `prebuilts/`(复数)只差一个 `s`, 而语义相反。** ADR-0004 为此专门写了一整节「命名辨析(先读这条)」—— **需要一整节来防止误用, 本身就是名字在主动邀请误用**, 与 `r1/06` §3.1 那条判 `frozen_version` 的纪律同型(命名与语义相反, 属必须改的那类问题)。写作成本也在提示这一点: 0002/0003/0004 三篇 ADR、`docs/README.md`、checklist、`brickie-v0.1`、`2-02` 各自复述了一遍这个区分。
2. **需求方裁定: 所有预打包工具收在 `prebuilts/` 一个顶层目录下。**
3. **「进不进版本库」是派生性属性, 不是身份属性。** 它适合用**子目录**表达, 不适合用两个近名顶层目录表达: 前者一条 `.gitignore` 规则说清, 后者要靠读者记住哪个是哪个。

## 2. 决策

**唯一顶层目录 `prebuilts/`, 内部按"进不进库"分两侧:**

```
prebuilts/
├── README.md                     # 台账(两侧都记)
├── seed/                         # **进版本库**(资产)
│   └── brickie/<host-arch>/<host-os>/bin/{brickie, brickie-gen}
└── toolchain/                    # **派生**(不进库; 一条 .gitignore 规则覆盖)
    ├── .cache/                   #   下载件(含 tar.xz)
    ├── .stamp/                   #   每件一个校验戳(内容 = SHA256)
    ├── toolchain.mk              #   生成物: 把实际路径告诉 Makefile
    ├── arm-gnu-toolchain-15.2.rel1/
    ├── make/
    └── ninja/
└── toolchain.lock.toml           # 锁文件(**进库**; 唯一真值, 见 ADR-0002)
```

| 项 | 变更 |
|---|---|
| 顶层目录 | `prebuilt/`(**不复存在**)→ `prebuilts/toolchain/` |
| 种子 | `prebuilts/brickie/...` → `prebuilts/seed/brickie/...` |
| 锁文件 | `prebuilt.lock.toml` → **`prebuilts/toolchain.lock.toml`** |
| 生成的路径文件 | `prebuilt/toolchain.mk` → `prebuilts/toolchain/toolchain.mk` |
| `.gitignore` | `/prebuilt/` → **`/prebuilts/toolchain/`**(一条规则覆盖整个派生侧) |
| make 目标名 | **保留** `prebuilt` / `prebuilt-check` / `prebuilt-clean`(ADR-0004 §12 已引用根级 `prebuilt` 为取件目标); `prebuilt-clean` 的守卫改为只接受以 `toolchain` 结尾的路径 |

**命名约定沿用**: `prebuilts/seed/<工具族>/<host-arch>/<host-os>/bin/<可执行>`(ADR-0004 §2 的形态不变, 只在前面多了 `seed/`)。

## 3. 替代方案与否决

| 候选 | 评价 |
|---|---|
| **A `prebuilts/{seed,toolchain}/`(采纳)** | 一个顶层目录 ⇒ 不再有近名歧义; 两侧靠**子目录**分开 ⇒ `.gitignore` 一条规则; `toolchain/` 整体是派生的 ⇒ 删了重建语义干净 |
| ❌ B 平铺(`prebuilts/{arm-gnu-toolchain-*,make,ninja,brickie}/`) | 最贴"所有工具都在 `prebuilts/` 下"的字面, 但 `.gitignore` 要**逐项列举大件**, 加一件就漏一次; "派生侧"这个概念在结构上不可见 |
| ❌ C 保留两个顶层目录(现状) | 就是本 ADR 要解决的问题本身 |
| ❌ D 把 `toolchain/` 也进版本库 | 仓库 +558 MB(含 91.5 MB 单文件, 擦 GitHub 100 MB 硬限), 克隆/push 全面变慢; 且会把第三方工具链的**再分发**问题引入源码仓。要真离线, 正解是 Git LFS 或 release 资产, 不是普通提交 |

## 4. 兼容性影响

| 面 | 影响 |
|---|---|
| 引用点 | 19 个文件、约 96 行路径引用; 已按形状分类替换(`prebuilt/` **不会**匹配到 `prebuilts/`, 因为后者是 `prebuilt`+`s/`, 故机械替换安全) |
| `tools/fetch-prebuilt.py` | `PREBUILT` 根常量与 `LOCK` 路径更新; **`toolchain.mk` 里的 `PREBUILT :=` 改为由脚本按实际路径生成**(原先硬编码 `prebuilt`, 正是这次要改的耦合点) |
| `Makefile` | `-include` 路径、`PREBUILT ?=` 兜底值、`prebuilt-clean` 守卫 |
| 门禁 / 前端 | `check-build.sh` 的种子断言、`mk/host.mk` 的 `PREBUILT_BRICKIE_BIN_DIR`、`hostinfo.py` 的查找顺序 —— 均已随路径更新 |
| ADR-0002 / ADR-0004 | 各自的「命名辨析」正文保留但**标注被本 ADR 取代**; 术语统一为「**派生侧** / **进库侧**」, 不再说"单数/复数" |
| 历史存档 `comment/**` | **不动**(按仓库规矩原样保留) |

## 5. 遗留

1. **`seed/` 下是否再分层**(如 `seed/<工具族>/…` vs 将来多工具族时的 `seed/<族>/<arch>/<os>/…`)—— 当前只有一个族(`brickie`), 形态已定, 待第二族出现时复核。
2. **`toolchain/` 是否需要按 `<host-arch>/<host-os>` 分** —— 当前只放了宿主为 `x86-64/linux` 的交叉工具链(它产 `aarch64` 目标码, 但**自身**是宿主程序)。若将来支持在别的宿主上取件, 需要分层(与 `seed/` 的分层语义不同: 那里按**宿主**分, 这里按**取件机**分)。
3. **是否升为缺省硬门禁**: `prebuilt-check` 仍缺省「缺失不算失败」(退回宿主工具链是合法过渡形态), ADR-0002 §7.4-5 的待定项不变。

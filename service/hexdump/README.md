# service/hexdump

> 实现态(不再是骨架)。声明面以 `plugin.toml` 为唯一真值, **本目录不改它**。

十六进制 + ASCII 呈现原语: 把一段内存变成"每 16 字节一行"的可读文本,
给 `service/dump` / debug bridge(`MEMRD`) / panic 报告共用。

## 能力面(对外声明面 = `include/br/debug/br_hexdump.h`)

| 函数 | 语义 |
|---|---|
| `br_hexdump_to(out, cap, base, len)` | 渲染到缓冲;语义照 `snprintf` —— 返回**需要**的字节数(不含 NUL), `cap>0` 时保证 NUL 结尾;`out==NULL 或 cap==0` ⇒ 只回报长度;`base==NULL && len>0` ⇒ 0 |
| `br_hexdump(base, len)` | 逐行渲染到 console(一行 88 B 的栈缓冲, 不缓冲整段);返回渲染字节数 |
| `br_hexdump_line_bytes()` | 一行字节数 = **87**(供调用方预算缓冲) |
| `br_hexdump_init()` | 无状态;返回 0 |
| `br_hexdump_selftest()` | 跑 TC-DBG-020/021, 打 `[DBGCONF] PASS/FAIL`, 返回失败数 |

## 行格式契约(逐字节冻结在头文件抬头, 用例 TC-DBG-020 断言)

    字段:  [地址 16][2 空格][组区 49][1 空格]['|'][ASCII 16]['|']['\n']

- 地址: 16 位**小写** hex(`base + 行内偏移`);
- 组区 49 = 16 组 × (2 位 hex + 1 个分隔空格) + **第 8 组之后额外 1 个空格**;
- 因此最后一个 hex 组与 `|` 之间有 **2 个空格**(组区末分隔空格 + 显式空格) ——
  与头文件样例 `…00  |Hello, world!...|` 一致;
- ASCII 列: `0x20..0x7E` 原样, 其余(含 `\n`/`\0`)一律 `.`;
- **满行 87 字节(含行尾 `\n`)**;`16+2+49+1+1+16+1+1 = 87`。
- 不足 16 字节的尾行: 缺的组用 **3 个空格**(2 占位 + 1 分隔)补齐, ASCII 列缺的用空格,
  所以尾行同样是 87 字节 —— `br_hexdump_line_bytes()` 对 `len <= 16` 恒成立。

实现上不使用 `br_log_info` 的格式化, 而是本地渲染(数字表 + ASCII 判定),
保证逐字节可控;所有字段常量与行宽都有 `_Static_assert` 钉住。

## 设计出处

- `docs/5-debug/5-01-debug.md` §2 —— debug bridge 的 `MEMRD` 与主机侧 `brickie dbg`;
- `docs/5-debug/5-01-debug.md` §3 —— mini ramdump 的输出形态与"捕获路径只用静态缓冲"
  (本实现只用一行的栈缓冲, 不分配、不缓冲整段)。

## 归属边界 —— 为什么这里不做边界判定

`br_hexdump` 是**最底层的呈现原语**: 输入是"一段已经确定可读的内存地址 + 长度"。
"这个地址在不在声明的 region 里 / 该不该拒绝"是 **`service/dump` 的职责**
(它的 `br_dump_memory` 按 region 表判定, 未声明区 `-EINVAL`)。理由: 呈现原语要
给 panic/bridge/dump 三处共用, 把 region 表知识塞进来会让它退化成"又一个 dump"。
本插件因此也没有 privileged 声明(P0)。

## 编译期前提

无额外前提: `-ffreestanding -nostdlib`, 不引 libc、不分配。全部函数 **thread-only**。

## `[[dep]]`(结构依赖)

声明面在 `plugin.toml`, 本目录不改动它。当前:

- 入边: `service/dump → service/hexdump`(kind = runtime, symbol = `br_hexdump_to`),
  声明在 `service/dump/plugin.toml`;
- 出边: **无**(本插件不认识 region 表, 也不认识其它服务)。

`br_hexdump_init` **不跨插件**调 `br_trace_svc_register`: 没有
`service/hexdump → service/trace` 的 `[[dep]]` 边。这一点已与声明面对齐
(`br_hexdump.h` 的 init 注释同样声明"不跨插件注册 trace 事件名"), 实现只返回 0;
要接得先补 `[[dep]]`(声明面变更, 是禁区)。

## 如何被调用

`service/dump` 的 `br_dump_conformance()` **统一驱动** `br_hexdump_selftest()`
(打印 `[DBGCONF]` 行);它的 `br_dump_memory()` / `br_dump_heap()` / `br_dump_all()`
调 `br_hexdump()` 出内容, `br_dump_trace()` 之外的 bridge `MEMRD` 路径用
`br_hexdump_to()` 做两段式(先问长度, 再填缓冲)。

## 用例(见 `tests/smoke.toml`)

| id | 判据 |
|---|---|
| TC-DBG-020 | 16 字节 `"Hello, world!\n\0\0"` 的行与头文件样例逐字节一致(`0x0a` 在 ASCII 列显示为 `.`);3 字节 `{'A',0x1f,0x7f}` 的尾行 padding 与 ASCII 列正确 |
| TC-DBG-021 | `br_hexdump_to(buf,10,base,16) == 87`、`buf[9]=='\0'`、`strlen==9`;`br_hexdump_to(NULL,0,…) == 87`;`br_hexdump_line_bytes() == 87`;`base==NULL && len>0 ⇒ 0` |

★ 测试口径说明(与声明面样例下方的说明一致):
头文件样例的基址(`0x40088000` 一类)只是**格式**示例(地址字段恒为 16 位小写 hex)。
目标镜像的 `.text.boot` 入口在 `0x40080000`(`platform/.../link.ld`), 那里不可写、
内容也不能假定 —— "在样例基址上读 16 字节再比样例"在 target 上无法成立。
所以 TC-DBG-020 用静态负载的**真实地址**构造期望串的地址字段(用例自带独立渲染),
其余 **71 字节与头文件样例后缀逐字节相同**;`87` 的字节账由 `line_bytes()` 与
TC-DBG-021 独立钉住。

## 已知欠账(不在本插件目录可修)

- `build/gen/service/hexdump/plugin_desc.c` 仍引用骨架符号
  `hexdump_early_init`/`hexdump_init`/`hexdump_start`(生成物; v0.1 不编译,
  按 V-9/ADR-0003 记账)。

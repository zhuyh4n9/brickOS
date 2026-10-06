"""brickie — brickOS 组合期工具(host 侧)。

分层(见 Design/docs/2-toolchain/brickie/brickie-v0.1.md §9.1):
    L5 前端  Python3  本包: 子命令 / 参数 / 输出格式 / 退出码 / 文件编排
    L2 生成器 C++     ``build/host/<host-arch>/<host-os>/bin/brickie-gen``:
                      骨架 / 描述符 / 头文件代码生成
    L0/L1     Rust    ``brickie-core``(尚未交付: 见 checklist §5.2 的 P1 批次)

**本包的纪律**: 不实现任何业务规则, 只做编排 / 校验 / 呈现 —— 由 CI 的
"粘合层纯度检查"保证(§9.1)。所有判定与错误码都来自原生侧。
"""

__version__ = "0.1.0"

# L5 ↔ L2 的 JSON over stdio 契约版本(§9.1 / C9)。不兼容变更必须进位。
PROTOCOL = 1

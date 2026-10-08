"""brickie — brickOS 组合期工具(host 侧)。

分层(见 ``Design/docs/2-toolchain/brickie/brickie-v0.1.md`` §9.1 与
``tools/brickie/docs/contract.md``):

    L5 前端  Python3  本包: 参数解析 / 编排 / schema 形状校验 / 落盘 / 呈现 / 退出码
    L2 生成器 C++     ``brickie-gen``: 只做模板渲染(骨架 / 描述符 / 头文件)
    L0/L1     Rust    ``brickie-core``: 全部判定与错误码(模型 / 求解 / 版本 / 接口)

**本包的纪律**(contract §8 / V-18c): 没有业务规则。闭包、区间、拓扑、环、相位、
分类学、特权、预算、四段版本推进、IFACE-IR 与 hash 一律在原生侧; 本包只做
"允许的六件事"—— 参数解析、找二进制、调子进程、读写文件、JSON 序列化、按
``schema/*.schema.json`` 做**单条记录形状**校验。

模块地图:

    cli.py       23 个叶子命令 + 2 个全局开关的 argparse 与编排流水线
    native.py    定位并调用 brickie-core / brickie-gen(JSON over stdio)
    schema.py    自带最小 JSON-Schema 子集(BRV-MF-0001 / 退出码 2)
    writer.py    幂等落盘 / --check 逐字节比对 / 声明面扫描
    present.py   把 data 渲染成人读文本(--json 与文本同源)
    hostinfo.py  宿主三元组(build/host/<arch>/<os> 的坐标, 与 tools/host-detect.sh 同口径)
"""

__version__ = "0.1.0"

# L5 ↔ L0/L1/L2 的 JSON over stdio 契约版本(contract §2)。不兼容变更必须进位。
PROTOCOL = 1

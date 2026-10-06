# brickie — 工具自身的构建入口
#
#   make            构建两个宿主 ELF(出树): brickie-gen(L2) + brickie(L5 入口)
#   make selftest   跑生成器内置自检(JSON 往返 / 规则 / 渲染)
#   make launcher-smoke  跑入口 ELF 的自检(嵌入载荷能否起来)
#   make test       跑全部用例(生成器自检 + 端到端 tests/run.sh)
#   make prebuilt   把两个 ELF 发布为自举种子 prebuilts/seed/brickie/<arch>/<os>/bin/
#   make prebuilt-check  检查种子是否落后于源码(不改盘)
#   make clean
#
# 产物落点由 mk/host.mk 决定(参考 Android): 
#   build/host/<host-arch>/<host-os>/bin/{brickie,brickie-gen}      (派生, 不进库)
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen} (种子, 进库)
#
# `brickie` 是**零依赖入口 ELF**: Python 前端与模板由 freeze.py 嵌进二进制,
# 运行时解包再用系统 python3 解释, 并自定位同目录的 brickie-gen(见 ADR-0004)。
#
# 本文件只做**委派**与选择, 不重复 cxx/ 的编译规则。与镜像(仓库根 Makefile)的
# 分工: 那里用交叉 gcc 编 aarch64 裸机; 这里用**宿主 g++** 编组合期工具 ——
# 两套编译器与选项互不干扰, 所以是两个 Makefile 而不是一个。
#
# v0.1 没有 cargo: L0/L1 的 Rust 核心(brickie-model/solve/ver/iface/core)尚未落地,
# 见 docs/2-toolchain/brickie/checklist.md §5.2 的 P1 批次。

CXX_DIR := cxx

.PHONY: all cxx selftest launcher-smoke test prebuilt prebuilt-check clean \
        print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-launcher-bin

all: cxx

cxx:
	$(MAKE) -C $(CXX_DIR)

selftest: cxx
	$(MAKE) -C $(CXX_DIR) selftest

launcher-smoke: cxx
	$(MAKE) -C $(CXX_DIR) launcher-smoke

test: selftest
	bash tests/run.sh

prebuilt: cxx
	$(MAKE) -C $(CXX_DIR) prebuilt

prebuilt-check: cxx
	$(MAKE) -C $(CXX_DIR) prebuilt-check

clean:
	$(MAKE) -C $(CXX_DIR) clean

# 透传: 宿主三元组 / 宿主 bin 目录 / 种子目录(门禁与用例用, 避免在 shell 里重算映射)
print-host-triple:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-host-triple

print-host-bin-dir:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-host-bin-dir

print-prebuilt-bin-dir:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-prebuilt-bin-dir

print-launcher-bin:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-launcher-bin

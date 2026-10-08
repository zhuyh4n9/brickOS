# brickie — 工具自身的构建入口
#
#   make            构建两个宿主 ELF(出树): brickie-gen(L2) + brickie(L5 入口)
#                   (Rust 核心 brickie-core 若已就位会被条件嵌进入口 ELF; 未就位则跳过)
#   make core       用 cargo 编 L0/L1 的 brickie-core → 拷到 build/host/<triple>/bin/
#                   (需要 rust/ 与 cargo; CARGO_HOME / CARGO_TARGET_DIR 指向可写出树目录)
#   make selftest   跑生成器内置自检(JSON 往返 / 渲染 / 路径与模式 / 载荷确定性)
#   make launcher-smoke  跑入口 ELF 的自检(嵌入载荷能否起来)
#   make test       跑全部用例(生成器自检 + 端到端 tests/run.sh; run.sh 里已含 V-18 purity)
#   make test-purity    只跑 V-18 粘合层纯度三条(selftest 无 Python / 桩替换反证 / 静态检查)
#   make test-v         端到端验收的详细模式
#   make test-snap      重生成 tests/snap/*.json 协议快照(只在明确要更新基线时用)
#   make prebuilt   把**三件** ELF 发布为自举种子 prebuilts/seed/brickie/<arch>/<os>/bin/
#                   {brickie, brickie-gen, brickie-core}; 缺 core ⇒ FAIL(种子必须三件齐)
#   make prebuilt-check  检查三件种子是否落后于源码(不改盘)
#   make clean
#
# 产物落点由 mk/host.mk 决定(参考 Android):
#   build/host/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}          (派生, 不进库)
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core} (种子, 进库)
#
# `brickie` 是**单文件自包含入口 ELF**: Python 前端与模板由 freeze.py 嵌进二进制,
# 运行时解包再用系统 python3 解释, 并把 BRICKIE_GEN / BRICKIE_CORE 指到嵌进去的
# native 工具(见 ADR-0004 §7 与 contract §1)。
#
# 本文件只做**委派**与选择, 不重复 cxx/ 的编译规则。与镜像(仓库根 Makefile)的
# 分工: 那里用交叉 gcc 编 aarch64 裸机; 这里用**宿主 g++/cargo** 编组合期工具 ——
# 两套编译器与选项互不干扰, 所以是两个 Makefile 而不是一个。
#
# 语言分层与"三个宿主可执行"(contract §1):
#   brickie-core  Rust(L0 模型 + L1 求解/版本/接口引擎)  ← 本文件 `core:` 委派 cargo
#   brickie-gen   C++(L2 生成器, 只做模板渲染)            ← cxx/Makefile
#   brickie        C++ 启动器 + 嵌入载荷(L5 入口)          ← cxx/Makefile

include ../../mk/host.mk

CXX_DIR := cxx

INSTALL ?= install
CARGO   ?= cargo
# `$HOME/.cargo` 在受管环境里可能只读 ⇒ 把 CARGO_HOME / target 都放到可写的出树目录
# (build/ 整体不进版本库)。两者都可用环境变量覆盖。
CARGO_HOME       ?= $(abspath ../../build/cargo-home)
CARGO_TARGET_DIR ?= $(abspath ../../build/cargo-target)
CORE_MANIFEST    ?= rust/Cargo.toml
CORE_PACKAGE     ?= brickie-core
CORE_BIN         := $(HOST_BIN_DIR)/brickie-core

.PHONY: all cxx core selftest launcher-smoke test test-purity test-v test-snap prebuilt prebuilt-check clean \
        print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-launcher-bin

all: cxx

cxx:
	$(MAKE) -C $(CXX_DIR)

# L0/L1 的 Rust 核心: 由 Rust 源码产出, 拷贝到与 brickie-gen 同一出树落点。
# rust/ 未就位时: 若已有 brickie-core 产物(别人给的)就跳过, 否则 FAIL —— 因为
# `make prebuilt` 要求三件齐; 单独的 `make` / `make cxx` 不受影响。
#
# 注意: 整段必须是**一个 shell**(行尾 `\` 连写) —— make 的每条 recipe 行各起一个
# shell, `exit 0` 只结束那一行, 后面的 cargo 照跑。
core:
	@if [ ! -f "$(CORE_MANIFEST)" ]; then \
	    if [ -x "$(CORE_BIN)" ]; then \
	        echo "core: 已有 $(CORE_BIN), 跳过(Rust 源 $(CORE_MANIFEST) 不在本 checkout)"; \
	        exit 0; \
	    fi; \
	    echo "FAIL: 缺 $(CORE_MANIFEST)(L0/L1 的 Rust 核心尚未落地)" >&2; \
	    echo "      自举种子要求 {brickie,brickie-gen,brickie-core} 三件齐" >&2; \
	    exit 1; \
	fi; \
	mkdir -p "$(CARGO_HOME)" "$(CARGO_TARGET_DIR)" "$(HOST_BIN_DIR)"; \
	CARGO_HOME="$(CARGO_HOME)" CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" \
	    $(CARGO) build --release --manifest-path "$(CORE_MANIFEST)" -p $(CORE_PACKAGE) || exit $$?; \
	$(INSTALL) -m 0755 "$(CARGO_TARGET_DIR)/release/$(CORE_PACKAGE)" "$(CORE_BIN)" || exit $$?; \
	echo "core: $(CORE_BIN)"

selftest: cxx
	$(MAKE) -C $(CXX_DIR) selftest

launcher-smoke: cxx
	$(MAKE) -C $(CXX_DIR) launcher-smoke

test: selftest
	bash tests/run.sh

# V-18 的三条判据单独可跑(也已被 run.sh 带进 `make test`)
test-purity:
	bash tests/purity.sh

# 端到端验收的详细模式(逐条打印退出码/诊断)
test-v:
	bash tests/run.sh -v

# 只在明确要更新协议快照基线时用: 重生成 tests/snap/*.json
test-snap:
	BRICKIE_SNAP_EMIT=1 bash tests/run.sh
	@echo "tests/snap/ 已重生成 —— 请 review diff 后再提交"

# 先 core 再 cxx: 子 make 重新解析时 $(wildcard $(CORE_BIN)) 已为真 ⇒ 载荷嵌 core。
prebuilt: core
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

# brickie 的 L2 生成器 + L5 入口 ELF。构建: 纯 make + g++ + python3, 无需 CMake。
#
# 产物**出树**(参考 Android 的 out/host/... 布局, 见 mk/host.mk):
#
#   build/host/<host-arch>/<host-os>/bin/brickie-gen      ← L2 生成器(ELF, 只做模板渲染)
#   build/host/<host-arch>/<host-os>/bin/brickie          ← L5 前端入口(ELF, 嵌入 Python 载荷)
#   build/host/<host-arch>/<host-os>/bin/brickie-core     ← L0/L1 Rust 核心(**不在本 Makefile 编**;
#                                                            由 tools/brickie 的 `make core` 产出)
#   build/host/<host-arch>/<host-os>/lib/libbrickie-gen.a ← 静态库
#   build/host/<host-arch>/<host-os>/obj/cxx/*.o          ← 中间产物
#   build/host/<host-arch>/<host-os>/share/brickie/payload.tar ← 嵌进 brickie 的载荷(供检查)
#
# 三个可执行都进自举种子(进版本库, 见 ADR-0004 / contract §1):
#
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}
#
# `brickie` 是**单文件自包含入口 ELF**: Python 包、模板与**原生工具**(brickie-gen;
# 若 brickie-core 已就位则一并嵌入)由 freeze.py 打成未压缩 tar 嵌进去, 运行时解包到
# 临时目录再用系统 python3 解释; 启动器把 BRICKIE_GEN / BRICKIE_CORE 指到解包出来的
# 那几份 ⇒ 种子目录自洽(详见 cxx/launcher.cpp 的头注释)。
#
# **brickie-core 的降级行为**: Rust 侧(`rust/`, 另一个 agent)未产出时,
# `make cxx` / `make` 仍必须成功 —— 本文件用 `CORE_EMBED := $(if $(wildcard ...))`
# 条件嵌入: 文件在就嵌, 不在就跳过并打印提示。但 `make prebuilt` **要求三件齐**
# (种子是"没有编译器也能起步"的那一份), 缺 core 时该目标 FAIL。
#
# 源码树里**不留**任何 .o/.a/可执行文件 —— 生成物纪律(C6)与状态目录纪律(BRV-D5):
# 输出目录整体不进版本库(仓库根 .gitignore 的 build/); 只有 prebuilts/ 进库。
#
# 与 §9.2 的差异(需回灌): 设计写的是 `cxx/ # CMake: libbrickie-gen + brickie-gen 可执行`,
# 但本机无 cmake; 首刀用等价的最小 Makefile, 库/可执行两个产物的划分保持一致,
# 只是**落点**改为宿主出树目录、并新增 L5 入口 ELF(见 README 的 D-1/D-6/D-7/D-8)。

include ../../../mk/host.mk

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
AR       ?= ar
INSTALL  ?= install
PYTHON   ?= python3

# L2 生成器只做模板渲染: json/diag/text 是基础设施, render 是唯一命令。
# (旧的 rules/gen_new —— 名字契约 / 分类学 / 相位 / 路径布局 / `new` —— 已删除:
#  判定归 brickie-core(Rust), 计划由 plan-new / plan-init 给出。)
SRCS := json.cpp diag.cpp text.cpp render.cpp
OBJS := $(addprefix $(HOST_OBJ_DIR)/cxx/,$(SRCS:.cpp=.o))
HDRS := $(wildcard *.h)

MAIN_OBJ := $(HOST_OBJ_DIR)/cxx/main.o
LIB := $(HOST_LIB_DIR)/libbrickie-gen.a
BIN := $(HOST_BIN_DIR)/brickie-gen

# ------------------------------------------------ L5 入口 ELF(自包含载荷)
# 载荷源 = 运行时需要的一切: L5 前端 + 模板 + schema + **原生工具**(见下方 PAYLOAD_EMBEDS)。
# ⚠ 这份清单**必须与 freeze.py 的 PAYLOAD_ROOTS 同步**(python/templates/schema):
#   窄了会导致"改了源文件却不重打载荷"的**静默不一致**(踩过: 只列 python/brickie/*.py
#   + templates 时, 改 python/brickie/README.md 或 schema/*.json 不会触发重打)。
TOOL_ROOT    := ..
FREEZE_PY    := $(TOOL_ROOT)/freeze.py
PAYLOAD_ROOTS := python templates schema
PAYLOAD_SRCS := $(shell find $(addprefix $(TOOL_ROOT)/,$(PAYLOAD_ROOTS)) -type f \
                    ! -path '*/__pycache__/*' ! -name '*.pyc' 2>/dev/null)
PAYLOAD_TAR  := $(HOST_OUT)/share/brickie/payload.tar
PAYLOAD_C    := $(HOST_OBJ_DIR)/cxx/brickie_payload.cpp
PAYLOAD_OBJ  := $(HOST_OBJ_DIR)/cxx/brickie_payload.o
LAUNCHER_OBJ := $(HOST_OBJ_DIR)/cxx/launcher.o
LAUNCHER_BIN := $(HOST_BIN_DIR)/brickie

# L0/L1 Rust 核心: 由 `tools/brickie` 的 `make core`(cargo)拷贝到此落点。
CORE_BIN := $(HOST_BIN_DIR)/brickie-core

# 嵌进 `brickie` 的**原生工具**(需求: "brickie-gen 等工具均需要编译到 brickie elf 中"):
#   * brickie-gen  必嵌(本 Makefile 自己构建);
#   * brickie-core 条件嵌 —— Rust agent 未产出时构建不能失败, 只跳过并提示。
# 左侧是本树的构建产物, 右侧是载荷内落点; 落点带可执行位, 启动器据此设
# BRICKIE_GEN / BRICKIE_CORE。将来加 `brickie-verify` 等, 在这里追加一行即可。
CORE_EMBED    := $(if $(wildcard $(CORE_BIN)),$(CORE_BIN)=bin/brickie-core,)
PAYLOAD_EMBEDS := $(BIN)=bin/brickie-gen $(CORE_EMBED)

# 自举种子(进版本库): **三件** —— brickie(自包含) + brickie-gen + brickie-core。
PREBUILT_BIN          := $(PREBUILT_BRICKIE_BIN_DIR)/brickie-gen
PREBUILT_LAUNCHER_BIN := $(PREBUILT_BRICKIE_BIN_DIR)/brickie
PREBUILT_CORE_BIN     := $(PREBUILT_BRICKIE_BIN_DIR)/brickie-core

.PHONY: all clean selftest launcher-smoke prebuilt prebuilt-check \
        print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-launcher-bin

all: $(BIN) $(LAUNCHER_BIN)

$(BIN): $(MAIN_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(HOST_OBJ_DIR)/cxx/%.o: %.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 入口 ELF: 启动器 + 生成的载荷数组
$(LAUNCHER_BIN): $(LAUNCHER_OBJ) $(PAYLOAD_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(LAUNCHER_OBJ): launcher.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(PAYLOAD_OBJ): $(PAYLOAD_C)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 载荷: 源文件、打包器或**任一嵌入工具**变了就重打(工具在载荷里, 它变 ⇒ ELF 变)。
# freeze.py 保证逐字节可复现(自举种子的前提)。
_EMBED_SRCS := $(foreach pair,$(PAYLOAD_EMBEDS),$(firstword $(subst =, ,$(pair))))
$(PAYLOAD_C): $(PAYLOAD_SRCS) $(FREEZE_PY) $(_EMBED_SRCS)
	@mkdir -p $(dir $@) $(dir $(PAYLOAD_TAR))
	@if [ ! -f "$(CORE_BIN)" ]; then \
	    echo "提示: $(CORE_BIN) 不存在 —— 载荷暂不含 brickie-core"; \
	    echo "      (先 \`make core\` 再回来构建入口 ELF; 现在只嵌 brickie-gen, 构建继续)"; \
	fi
	$(PYTHON) $(FREEZE_PY) --c $@ --tar $(PAYLOAD_TAR) \
	    $(foreach pair,$(PAYLOAD_EMBEDS),--embed $(pair))

selftest: $(BIN)
	$(BIN) --selftest

# 冒烟: 入口 ELF 能不能起来(它自带 Python 载荷, 不依赖源码树)
launcher-smoke: $(LAUNCHER_BIN)
	$(LAUNCHER_BIN) --version

# core 由 `tools/brickie` 的 `make core`(cargo)产出; 本 Makefile 不编 Rust。
# 这条空规则只为在缺 core 时给出**可读的 FAIL**, 而不是 make 的 "No rule to make target"。
$(CORE_BIN):
	@echo "FAIL: 缺 $(CORE_BIN) —— 自举种子必须 {brickie,brickie-gen,brickie-core} 三件齐" >&2
	@echo "      先跑: make -C .. core   (需要 rust/ 与 cargo)" >&2
	@false

# 发布自举种子: 只有"种子 ≠ 本次构建"时才真的写盘(重复发布无 diff)。
prebuilt: $(BIN) $(LAUNCHER_BIN) $(CORE_BIN)
	@mkdir -p $(dir $(PREBUILT_BIN))
	@for pair in "$(BIN):$(PREBUILT_BIN)" "$(LAUNCHER_BIN):$(PREBUILT_LAUNCHER_BIN)" \
	             "$(CORE_BIN):$(PREBUILT_CORE_BIN)"; do \
	    src="$${pair%%:*}"; dst="$${pair##*:}"; \
	    if [ -f "$$dst" ] && cmp -s "$$src" "$$dst"; then \
	        echo "prebuilt 已是最新: $$dst"; \
	    else \
	        $(INSTALL) -m 0755 "$$src" "$$dst"; \
	        echo "prebuilt 已发布: $$dst"; \
	    fi; \
	done

# 门禁: 三件种子是否落后于源码(只看内容, 不改盘)。任一缺失/落后 ⇒ 退出 1。
prebuilt-check: $(BIN) $(LAUNCHER_BIN)
	@status=0; \
	for pair in "$(BIN):$(PREBUILT_BIN)" "$(LAUNCHER_BIN):$(PREBUILT_LAUNCHER_BIN)" \
	             "$(CORE_BIN):$(PREBUILT_CORE_BIN)"; do \
	    src="$${pair%%:*}"; dst="$${pair##*:}"; \
	    if [ ! -f "$$src" ]; then \
	        echo "FAIL: 缺构建产物: $$src(core 请先跑 make -C .. core)" >&2; status=1; \
	    elif [ ! -f "$$dst" ]; then \
	        echo "FAIL: 自举种子缺失: $$dst(跑 make prebuilt)" >&2; status=1; \
	    elif cmp -s "$$src" "$$dst"; then \
	        echo "ok   自举种子与本次构建一致: $$dst"; \
	    else \
	        echo "FAIL: 自举种子落后于源码: $$dst(跑 make prebuilt 重新发布)" >&2; status=1; \
	    fi; \
	done; \
	exit $$status

# 给构建门禁/用例取"当前宿主是谁、产物在哪"(不在 shell 里重算映射)。
print-host-triple:
	@echo $(HOST_TRIPLE)
print-host-bin-dir:
	@echo $(HOST_BIN_DIR)
print-prebuilt-bin-dir:
	@echo $(PREBUILT_BRICKIE_BIN_DIR)
print-launcher-bin:
	@echo $(LAUNCHER_BIN)

clean:
	rm -rf $(HOST_OUT)
	@# 迁移期兜底: 清掉首刀遗留在源码树里的旧产物(现在的规则不再产生它们)
	rm -f $(wildcard *.o) libbrickie-gen.a brickie-gen
	@# 注意: clean **不**动 prebuilts/ —— 自举种子是随源码提交的资产, 不是派生品。

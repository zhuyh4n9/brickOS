# brickie 的 L2 生成器 + L5 入口 ELF。构建: 纯 make + g++ + python3, 无需 CMake。
#
# 产物**出树**(参考 Android 的 out/host/... 布局, 见 mk/host.mk):
#
#   build/host/<host-arch>/<host-os>/bin/brickie-gen      ← L2 生成器(ELF)
#   build/host/<host-arch>/<host-os>/bin/brickie          ← L5 前端入口(ELF, 嵌入 Python 载荷)
#   build/host/<host-arch>/<host-os>/lib/libbrickie-gen.a ← 静态库
#   build/host/<host-arch>/<host-os>/obj/cxx/*.o          ← 中间产物
#   build/host/<host-arch>/<host-os>/share/brickie/payload.tar ← 嵌进 brickie 的载荷(供检查)
#
# 两个可执行都进自举种子(进版本库, 见 ADR-0004):
#
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen}
#
# `brickie` 是**零依赖入口 ELF**: Python 包与模板由 freeze.py 打成未压缩 tar 嵌进去,
# 运行时解包到临时目录再用系统 python3 解释; 它**自定位**同目录的 `brickie-gen`
# ⇒ 种子目录自洽, 拷到任何地方都能跑(详见 cxx/launcher.cpp 的头注释)。
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

SRCS := json.cpp diag.cpp text.cpp rules.cpp gen_new.cpp
OBJS := $(addprefix $(HOST_OBJ_DIR)/cxx/,$(SRCS:.cpp=.o))
HDRS := $(wildcard *.h)

MAIN_OBJ := $(HOST_OBJ_DIR)/cxx/main.o
LIB := $(HOST_LIB_DIR)/libbrickie-gen.a
BIN := $(HOST_BIN_DIR)/brickie-gen

# ------------------------------------------------ L5 入口 ELF(自包含载荷)
# 载荷源 = 运行时需要的一切: L5 前端 + 模板 + **原生工具**(见下方 PAYLOAD_EMBEDS)。
TOOL_ROOT    := ..
FREEZE_PY    := $(TOOL_ROOT)/freeze.py
PAYLOAD_SRCS := $(wildcard $(TOOL_ROOT)/python/brickie/*.py) \
                $(shell find $(TOOL_ROOT)/templates -type f 2>/dev/null)
PAYLOAD_TAR  := $(HOST_OUT)/share/brickie/payload.tar
PAYLOAD_C    := $(HOST_OBJ_DIR)/cxx/brickie_payload.cpp
PAYLOAD_OBJ  := $(HOST_OBJ_DIR)/cxx/brickie_payload.o
LAUNCHER_OBJ := $(HOST_OBJ_DIR)/cxx/launcher.o
LAUNCHER_BIN := $(HOST_BIN_DIR)/brickie

# 嵌进 `brickie` 的**原生工具**(需求: "brickie-gen 等工具均需要编译到 brickie elf 中")。
# 左侧是本树的构建产物, 右侧是载荷内落点; 落点带可执行位, 启动器据此设 BRICKIE_GEN。
# 将来加 `brickie-core` 等, 在这里追加一行即可(launcher 按 bin/<名> 逐个可用)。
PAYLOAD_EMBEDS := $(BIN)=bin/brickie-gen

# 自举种子(进版本库): `brickie` 自包含; `brickie-gen` 仍单独发布(开发态/兼容用)
PREBUILT_BIN         := $(PREBUILT_BRICKIE_BIN_DIR)/brickie-gen
PREBUILT_LAUNCHER_BIN := $(PREBUILT_BRICKIE_BIN_DIR)/brickie

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
	$(PYTHON) $(FREEZE_PY) --c $@ --tar $(PAYLOAD_TAR) \
	    $(foreach pair,$(PAYLOAD_EMBEDS),--embed $(pair))

selftest: $(BIN)
	$(BIN) --selftest

# 冒烟: 入口 ELF 能不能起来(它自带 Python 载荷, 不依赖源码树)
launcher-smoke: $(LAUNCHER_BIN)
	$(LAUNCHER_BIN) --version

# 发布自举种子: 只有"种子 ≠ 本次构建"时才真的写盘(重复发布无 diff)。
prebuilt: $(BIN) $(LAUNCHER_BIN)
	@mkdir -p $(dir $(PREBUILT_BIN))
	@for pair in "$(BIN):$(PREBUILT_BIN)" "$(LAUNCHER_BIN):$(PREBUILT_LAUNCHER_BIN)"; do \
	    src="$${pair%%:*}"; dst="$${pair##*:}"; \
	    if [ -f "$$dst" ] && cmp -s "$$src" "$$dst"; then \
	        echo "prebuilt 已是最新: $$dst"; \
	    else \
	        $(INSTALL) -m 0755 "$$src" "$$dst"; \
	        echo "prebuilt 已发布: $$dst"; \
	    fi; \
	done

# 门禁: 种子是否落后于源码(只看内容, 不改盘)。任一缺失/落后 ⇒ 退出 1。
prebuilt-check: $(BIN) $(LAUNCHER_BIN)
	@status=0; \
	for pair in "$(BIN):$(PREBUILT_BIN)" "$(LAUNCHER_BIN):$(PREBUILT_LAUNCHER_BIN)"; do \
	    src="$${pair%%:*}"; dst="$${pair##*:}"; \
	    if [ ! -f "$$dst" ]; then \
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

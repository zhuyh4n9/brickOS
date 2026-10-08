# brickOS prototype v0.1.0 — 构建入口
#
# 目标: aarch64 裸机 ELF, 跑在 QEMU virt 上。
#
# ---------------------------------------------------------------- 两段式构建
# **工具先于镜像**(见 §工具):
#   ① tools/   brickie(组合期工具): 宿主 g++ 编 C++ 生成器 + Python 前端。
#              它**不碰交叉工具链** —— brickie-v0.1 §0 的边界纪律: v0.1 任何命令
#              不得要求 cc/cargo/nm 在场。所以 `make tools` 在没装交叉编译器的
#              机器上也必须能跑通(CI 的工具作业正是这样用的)。
#   ② brickOS  用外部交叉 gcc 编 aarch64 裸机镜像。
#   顺序由 `$(OBJS): | tools` 这条 order-only 依赖钉死 ⇒ `make -j` 也不会倒过来;
#   order-only 同时保证"工具重新编过"不会触发镜像重链。
#
#   ⚠ 当前镜像是**手工组合**的(br-wa-entry-001): brickie 只在①被编出来,
#     **还没有**参与②(插件发现 / 描述符生成 / 组合期 check 都没接线)。
#     本文件只是先把"工具是镜像的前置"这条纪律立起来;
#     真正的接线是 br-wa-entry-001 的还债动作(见 WORKAROUNDS.md)。
#
# 工具链: **外部 gcc**
#   WORKAROUND(br-wa-toolchain-001): 内部工具链就绪前借用宿主交叉编译器。
#   退出条件 = 只改 CROSS_COMPILE(见 WORKAROUNDS.md), 构建规则本身不动。
#
# 用法:
#   make                    ⓪组合期校验 → ①编工具 → ②构建 build/brick.elf + .bin
#   make tools              只编 L5/L2(不需要交叉编译器, 也不需要 cargo)
#   make tools-core         只编 L0/L1 的 Rust 核心(brickie-core; 需要 cargo)
#   make tools-test         跑工具自身用例(生成器自检 + 端到端)
#   make brickie-check      用 brickie 校验声明面(dev; 不编镜像)
#   make brickie-check-release  发布级门禁(--profile release)
#   make brickie-compose    重建 build/gen/** 生成物(不编译)
#   source setup.sh         配置开发环境(PATH + 环境变量; 见 ADR-0002 §7.5)
#   make env                打印同样的 shell 片段(CI: eval "$(make -s env)")
#   make run                在 QEMU 上跑(Ctrl-A X 退出)
#   make smoke              3 秒冒烟: 自动判定启动与延时是否正常
#   make size / disasm      体积 / 反汇编
#   make check-workarounds  WORKAROUND 登记表与源码标记是否一致
#   make check-build        构建接线门禁: 缺省目标/工具在前/工具段零交叉依赖
#   make tools-prebuilt     把工具发布为自举种子 prebuilts/seed/brickie/<arch>/<os>/bin/
#   make tools-prebuilt-check  检查种子是否落后于源码
#   make print-host-triple  打印宿主三元组(host-arch/host-os)
#   make print-host-bin-dir 打印宿主工具 bin 目录(build/host/<arch>/<os>/bin)
#   make clean              清掉①与②的产物(不动 prebuilts/ 种子)

# ---------------------------------------------------------------- prebuilt(可选)
# prebuilts/toolchain/ 是**派生目录**(不进版本库, 配方见 prebuilts/toolchain.lock.toml), 由 `make prebuilt` 取件。
# 这个 -include 把各产物的实际路径带进来; 文件不在 ⇒ 下面的探测自动退回宿主工具链。
#
# **不信任何"就位标记"**: prebuilts/toolchain/ 允许只取一部分(`fetch-prebuilt.py <id>`), 所以下面
# 逐个用 $(wildcard) 检查二进制**真的存在**。见 ADR-0002。
-include prebuilts/toolchain/toolchain.mk

PREBUILT ?= prebuilts/toolchain

# ---------------------------------------------------------------- 工具链探测
# 优先 prebuilt(自洽、可复现), 否则退回宿主外部交叉工具链(br-wa-toolchain-001 的过渡形态)。
ifeq ($(strip $(PREBUILT_CROSS)),)
  PREBUILT_CROSS_GCC :=
else
  PREBUILT_CROSS_GCC := $(wildcard $(PREBUILT_CROSS)gcc)
endif

ifneq ($(PREBUILT_CROSS_GCC),)
  CROSS_COMPILE ?= $(PREBUILT_CROSS)
  TOOLCHAIN_SOURCE := prebuilts/toolchain/  ($(PREBUILT_CROSS))
else
  CROSS_COMPILE ?= aarch64-linux-gnu-
  TOOLCHAIN_SOURCE := 宿主外部工具链  (br-wa-toolchain-001)
endif

# 递归 make 也用 prebuilt 里那一份(钉住版本); 没有就用调用本文件的那个 make。
ifneq ($(wildcard $(PREBUILT_MAKE)),)
  MAKE := $(PREBUILT_MAKE)
endif

# 无版本号优先; 退化到带版本号的驱动名(Ubuntu 只装 gcc-N 时没有软链)
CC_CANDIDATES := $(CROSS_COMPILE)gcc $(CROSS_COMPILE)gcc-16 $(CROSS_COMPILE)gcc-15 $(CROSS_COMPILE)gcc-14 $(CROSS_COMPILE)gcc-13
CC := $(firstword $(foreach c,$(CC_CANDIDATES),$(if $(shell command -v $(c) 2>/dev/null),$(c))))

binutils_probe = $(firstword $(foreach c,$(CROSS_COMPILE)$(1),$(if $(shell command -v $(c) 2>/dev/null),$(c))))
OBJCOPY := $(call binutils_probe,objcopy)
OBJDUMP := $(call binutils_probe,objdump)
SIZE    := $(call binutils_probe,size)

# 缺失的交叉工具链**不在解析期** $(error) —— 那会让 `make tools` / `make tools-test`
# 在纯宿主环境(以及只要工具的 CI 作业)里也直接死掉, 与上面那条边界纪律相抵。
# 改为在真正用得到它的规则里守卫, 报错信息与退出码保持一致(2 = 环境错)。
# 单行定义: 多行 define 在 recipe 里的续行行为依赖 make 的分行规则, 不值得踩。
define require_cross
@if [ -z "$(strip $(CC))" ]; then echo "FAIL: 找不到交叉编译器 \"$(CROSS_COMPILE)gcc\"。" >&2; echo "      取 prebuilt: make prebuilt   |   只构建工具: make tools(不需要交叉编译器)" >&2; exit 2; fi
endef

define require_binutils
@if [ -z "$(strip $($(1)))" ]; then echo "FAIL: 找不到 $(1)(aarch64 binutils)。取 prebuilt: make prebuilt" >&2; exit 2; fi
endef

define require_qemu
@if ! command -v $(QEMU) >/dev/null 2>&1; then echo "FAIL: 找不到 $(QEMU);装 qemu-system-arm(⚠ 未纳入 prebuilt)" >&2; exit 2; fi
endef

QEMU ?= qemu-system-aarch64

# ------------------------------------------------------------------ 目录/产物
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
ELF       := $(BUILD_DIR)/brick.elf
BIN       := $(BUILD_DIR)/brick.bin
MAP       := $(BUILD_DIR)/brick.map
LDSCRIPT  := platform/qemu-aarch64/src/link.ld

# 宿主工具产物(出树): build/host/<host-arch>/<host-os>/{bin,lib,obj}
# 由 mk/host.mk 经 tools/host-detect.sh 探测宿主; 与镜像产物(build/obj, build/brick.*)
# 分居 build/ 两侧, 互不覆盖 —— 参考 Android 的 out/host 与 out/target 分家。
# HOST_BUILD_ROOT 显式给绝对路径, 避免受调用目录影响(即使从别处 make -f 也一致)。
#
# 另有 `prebuilts/`(进版本库): 自举种子 prebuilts/seed/brickie/<host-arch>/<host-os>/bin/,
# 由 `make tools-prebuilt` 发布 —— 让没有 g++ 的全新 checkout 也能直接把 brickie 跑起来,
# 也是将来用 brickie 自举管理自身编译的起点(见 docs/decisions/0004)。别与下载缓存
# `prebuilts/toolchain/`(派生侧、不进库)混淆。
HOST_BUILD_ROOT := $(abspath $(BUILD_DIR))
include mk/host.mk

# ------------------------------------------------------------------- 源文件
# 插件树就是镜像的源码树(**顶层目录 = namespace**, brickie-v0.1 §8.3 ④):
#   platform/qemu-aarch64/   Platform 插件(每镜像恰 1; 含 start.S/link.ld/console/timer)
#   app/hello/               镜像唯一的 APP(§3.2): M0 的 MainLoop
#   core/                    内核本体(**不是插件**; 被 [compat].core 引用)
CORE_SRCS := $(wildcard core/src/*.c) $(wildcard core/src/*/*.c)
PLAT_SRCS := $(wildcard platform/qemu-aarch64/src/*.c)
APP_SRCS  := $(wildcard app/hello/src/*.c)
ASM_SRCS  := $(wildcard platform/qemu-aarch64/src/*.S)

SRCS := $(CORE_SRCS) $(PLAT_SRCS) $(APP_SRCS) $(ASM_SRCS)
OBJS := $(addprefix $(OBJ_DIR)/,$(patsubst %.c,%.o,$(patsubst %.S,%.o,$(SRCS))))

# --------------------------------------------------------------------- 选项
INCLUDES := -Icore/include -Iplatform/qemu-aarch64/include -Iapp/hello/include

# -ffreestanding: 无宿主运行时假设; -fno-builtin: 不把循环偷偷换成 memcpy
# -mgeneral-regs-only: 内核不碰 FP/SIMD(设计侧 aarch64 目标的纪律)
ARCHFLAGS := -march=armv8-a -mgeneral-regs-only

WARNFLAGS := -Wall -Wextra -Werror -Wshadow -Wundef -Wpointer-arith \
             -Wstrict-prototypes -Wmissing-prototypes

CFLAGS := -std=c11 $(ARCHFLAGS) -O2 -g3 $(WARNFLAGS) \
          -ffreestanding -fno-builtin -fno-common -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
          -fno-unwind-tables $(INCLUDES)

ASFLAGS := $(ARCHFLAGS) -g3

LDFLAGS := -nostdlib -nostartfiles -static -no-pie \
           -Wl,-T,$(LDSCRIPT) \
           -Wl,-Map,$(MAP) \
           -Wl,--build-id=none \
           -Wl,--gc-sections \
           -Wl,-z,max-page-size=4096

# ------------------------------------------------------------------- 目标
# 显式钉住缺省目标: 下面 §工具 那段里的 `tools:` 是文件里**第一条**显式规则,
# 不写这一行, `make` 会去建 `tools` 然后就此结束 —— 镜像根本不编(踩过一次:
# exit 0、build/host/<arch>/<os>/bin/brickie-gen 在, 但 build/ 下没有镜像)。
.DEFAULT_GOAL := all

.PHONY: all prebuilt prebuilt-check prebuilt-clean \
        tools tools-core tools-test tools-clean tools-prebuilt tools-prebuilt-check \
        brickie-check brickie-check-release brickie-compose \
        run smoke size disasm check-workarounds check-build \
        clean clean-brickos help print-host-triple print-host-bin-dir \
        print-prebuilt-bin-dir print-cross-compile env

# ============================================================== ⓪ prebuilt 工具集
# 锁版本 + SHA256 校验的外部工具链**下载缓存**(配方 = prebuilts/toolchain.lock.toml), 见 ADR-0002。
#
# ⚠ 目标名 `prebuilt` 与目录 `prebuilts/toolchain/` **同名** —— 全靠上面 .PHONY 压住。
#   漏了它, make 会认为"目录已存在 ⇒ 无需做任何事", 取件变成**静默空操作**:
#   `make prebuilt` 打印 "对'prebuilt'无需做任何事" 然后 exit 0, 一件都没下(踩过)。
#
# ⚠ 与 prebuilts/(**进库侧**, 自举种子, 进版本库)不是一回事: 那是 `tools-prebuilt` 的目标。
#   两个目录名只差一个 s, 语义相反 —— 见 ADR-0004 开头的命名辨析。
prebuilt:
	@echo "== ⓪ prebuilt: 取件(锁版本 + SHA256 校验) =="
	@python3 tools/fetch-prebuilt.py

# 就位检查。**缺省不算失败**: 未取件时退回宿主工具链是合法过渡形态(br-wa-toolchain-001)。
# 要求硬性自洽的场合(CI 的复现作业)加 PREBUILT_STRICT=1。
prebuilt-check:
	@if [ "$(PREBUILT_STRICT)" = "1" ]; then \
	    python3 tools/fetch-prebuilt.py --check || exit 1; \
	else \
	    python3 tools/fetch-prebuilt.py --check || true; \
	fi
	@echo "   交叉工具链来源: $(TOOLCHAIN_SOURCE)"

# 只删下载缓存。守卫: 只接受以 prebuilt 结尾的路径 —— 免得 PREBUILT 被指到别处时误删。
prebuilt-clean:
	@case "$(PREBUILT)" in prebuilts/toolchain|*/toolchain) ;; \
	  *) echo "拒绝: PREBUILT=$(PREBUILT) 不是 prebuilts/toolchain 目录, 不删"; exit 2;; esac
	rm -rf $(PREBUILT)

# ============================================================== ① 工具段
# brickie 自身(组合期工具)。它有自己的构建入口与用例集 —— 这里只做**委派**,
# 不重复它的规则(mk 的单一真值: tools/brickie/Makefile)。
#
# 为什么是递归 make 而不是把对象文件列进来: 工具与镜像用**不同的编译器与选项**
# (宿主 g++ / -std=c++20 vs 交叉 gcc / -std=c11), 混在一个 Makefile 里必然要
# 分叉变量名, 得不偿失。
TOOLS_DIR := tools/brickie

tools:
	@echo "== ① tools: $(TOOLS_DIR) (宿主 g++, 不碰交叉工具链) =="
	@echo "   产物: $(HOST_BIN_DIR)/{brickie,brickie-gen}  [$(HOST_TRIPLE)]"
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) cxx

tools-test:
	@echo "== ① tools 用例 =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) test

tools-clean:
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) clean

# L0/L1 的 Rust 核心(单独目标: `make tools` 只编 L5/L2, 这样没有 cargo 的 checkout
# 也能起步; 自举种子要求三件齐 ⇒ `make tools-prebuilt` 会连它一起要)。
tools-core:
	@echo "== ① tools: brickie-core(L0/L1 Rust; cargo + 出树到 $(HOST_BIN_DIR))=="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) core

# 自举种子(进版本库): 发布到 prebuilts/seed/brickie/<host-arch>/<host-os>/bin/。
# 将来 brickie 自举管理自身编译时, 这个种子就是"第一块砖"。
tools-prebuilt: tools
	@echo "== ① tools 自举种子 → prebuilts/seed/brickie/$(HOST_TRIPLE)/bin/ =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) prebuilt

# 门禁(按需): 种子落后于源码即报红 —— 提醒重新发布, 不自动改盘。
tools-prebuilt-check: tools
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) prebuilt-check

# ------------------------------------------------ 组合期声明面(用 brickie 管)
# 声明面(schema/依赖/导出面/特权/预算)由 brickie 校验与治理; 生成物由 brickie gen
# 重建(`build/gen/**`), **不参与编译** —— 编译编排是 v0.3 的能力(brickie-v0.1 §0)。
#
# 入口用**入口 ELF**: 它自带 Python 前端与两个原生工具(brickie-core/brickie-gen),
# 所以这里不需要 PYTHONPATH/源码树; 找不到 core 时会退回自举种子(见 native.py 的
# 查找顺序)。`make check-build` 的第 6 条不变量会核对种子三件齐。
BRICKIE      := $(HOST_BIN_DIR)/brickie
COMPOSE_ROOT := $(abspath .)

.PHONY: brickie-check brickie-check-release brickie-compose
brickie-check: tools
	@echo "== 组合期校验: brickie check(声明面完备性, dev)=="
	@$(BRICKIE) check --root $(COMPOSE_ROOT)

brickie-check-release: tools
	@echo "== 组合期校验: brickie check --profile release(发布级门禁)=="
	@$(BRICKIE) check --root $(COMPOSE_ROOT) --profile release

brickie-compose: tools
	@echo "== 生成物重建: brickie gen → build/gen/**(不编译)=="
	@$(BRICKIE) gen --root $(COMPOSE_ROOT)

# 顺序铁律: 镜像的每个目标文件都排在工具**与组合期校验**之后
# (order-only ⇒ 工具/声明面变新不触发重链, 但每次都会先校验)。
# 挂在 $(OBJS) 上而不是 $(ELF) 上, 是为了让 `make -j` 也不会编译与建工具并行。
$(OBJS): | tools brickie-check

# ============================================================== ② 镜像段
all: $(ELF) $(BIN)
	@echo "OK: $(ELF)  ($(CC))"

$(ELF): $(OBJS) $(LDSCRIPT)
	$(require_cross)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BIN): $(ELF)
	$(call require_binutils,OBJCOPY)
	$(OBJCOPY) -O binary $< $@

$(OBJ_DIR)/%.o: %.c
	$(require_cross)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	$(require_cross)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# ---------------------------------------------------------------- 运行/验证
QEMUFLAGS ?= -M virt -cpu cortex-a53 -m 128M -nographic

run: all
	$(require_qemu)
	$(QEMU) $(QEMUFLAGS) -kernel $(ELF)

# 冒烟: 限时跑, 然后把"启动成功"与"延时判据"当断言查 —— 让人眼看的日志
# 变成 CI 能判的红绿(grep 的判据就是 main.c 里自己打的那两行)。
SMOKE_SECONDS ?= 3
smoke: all
	$(require_qemu)
	@set +e; \
	timeout $(SMOKE_SECONDS) $(QEMU) $(QEMUFLAGS) -kernel $(ELF) > $(BUILD_DIR)/smoke.log 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 124 ]; then echo "FAIL: QEMU 未按期运行(rc=$$rc)"; cat $(BUILD_DIR)/smoke.log; exit 1; fi; \
	if ! grep -q "core MainLoop" $(BUILD_DIR)/smoke.log; then echo "FAIL: 未见 MainLoop 启动横幅"; cat $(BUILD_DIR)/smoke.log; exit 1; fi; \
	if ! grep -q "tick=2 " $(BUILD_DIR)/smoke.log; then echo "FAIL: MainLoop 未跑到第 2 拍"; cat $(BUILD_DIR)/smoke.log; exit 1; fi; \
	if grep -q "EARLY" $(BUILD_DIR)/smoke.log; then echo "FAIL: 出现早醒(delay < 请求值)"; cat $(BUILD_DIR)/smoke.log; exit 1; fi; \
	if grep -q "FATAL" $(BUILD_DIR)/smoke.log; then echo "FAIL: 触发未处理异常"; cat $(BUILD_DIR)/smoke.log; exit 1; fi; \
	echo "PASS: 启动 + MainLoop + 延时判据"; \
	grep -c "^\[" $(BUILD_DIR)/smoke.log | sed 's/^/日志行数: /'

size: all
	$(call require_binutils,SIZE)
	$(SIZE) -A -x $(ELF)
	@$(SIZE) $(ELF)

disasm: all
	$(call require_binutils,OBJDUMP)
	$(OBJDUMP) -d $(ELF) | head -120

# ------------------------------------------------------- WORKAROUND / 构建门禁
check-workarounds:
	@bash tools/check-workarounds.sh

# 构建接线门禁: 缺省目标是不是镜像 / 工具是不是排在镜像前(含 -j) /
# 工具段有没有被交叉工具链污染。理由见脚本头部 —— 顺序纪律坏了通常是**静默**的。
check-build:
	@bash tools/check-build.sh

# ------------------------------------------------------------------- 查询
# 宿主三元组与产物落点: 让门禁/CI/用例从这里取, 而不是各自重算映射
# (单一真值 = tools/host-detect.sh → mk/host.mk)。
print-host-triple:
	@echo $(HOST_TRIPLE)

print-host-bin-dir:
	@echo $(HOST_BIN_DIR)

print-prebuilt-bin-dir:
	@echo $(PREBUILT_BRICKIE_BIN_DIR)

print-cross-compile:
	@echo $(CROSS_COMPILE)

# 开发环境(shell 片段): 工具链 bin 目录 + CROSS_COMPILE + brickie 的根路径。
#   eval "$(make -s env)"   —— 直接进当前 shell(等价于 source setup.sh)
#   source setup.sh         —— 常规用法; 另带 --unset / --with-make / 缺失提示
# 这里只做**打印**: Makefile 不该去改调用者的环境(它做不到, 也不该假装能做到)。
env:
	@bash setup.sh --print

# ------------------------------------------------------------------- 清理
# `clean` 清掉本文件构建的**全部**产物(①工具 + ②镜像); 只清工具用 tools-clean。
#
# 两者分开清: 宿主工具在 build/host/(出树输出), 镜像在 build/{obj,brick.*}。
# 注意 clean-brickos **不**动 build/host —— 只清镜像侧, 免得"清镜像"顺手把
# 工具删了(那会让下一次 make 又重编一遍宿主工具)。
clean: clean-brickos tools-clean

clean-brickos:
	rm -rf $(OBJ_DIR) $(ELF) $(BIN) $(MAP) $(BUILD_DIR)/smoke.log

help:
	@echo "目标: all(缺省) prebuilt prebuilt-check prebuilt-clean"
	@echo "      tools tools-test tools-prebuilt tools-prebuilt-check run smoke size disasm"
	@echo "      check-workarounds check-build clean tools-clean"
	@echo "      print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-cross-compile env"
	@echo "变量: CROSS_COMPILE=$(CROSS_COMPILE)  CC=$(CC)"
	@echo "      QEMU=$(QEMU)  QEMUFLAGS=$(QEMUFLAGS)"
	@echo "      HOST_TRIPLE=$(HOST_TRIPLE)  HOST_BIN_DIR=$(HOST_BIN_DIR)"
	@echo "      PREBUILT_BRICKIE_BIN_DIR=$(PREBUILT_BRICKIE_BIN_DIR)"
	@echo "顺序: all = ①tools(宿主 g++) → ②brickOS(交叉 gcc); 只做①用 make tools"
	@echo "布局: 工具 → build/host/<host-arch>/<host-os>/bin(参考 Android out/host/...); 镜像 → build/"
	@echo "种子: tools-prebuilt → prebuilts/seed/brickie/<host-arch>/<host-os>/bin(进版本库, 自举用)"
	@echo "工具链来源: $(TOOLCHAIN_SOURCE)"
	@echo "prebuilt(取件目标 → prebuilts/toolchain/): $(if $(filter 1,$(PREBUILT_READY)),已就位,未取件 —— make prebuilt 取件(ADR-0002))"

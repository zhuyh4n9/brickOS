# brickOS prototype v0.1.0 — 顶层构建入口(**只编工具** + 镜像侧的过渡别名)
#
# ================================================================ 构建归属(ADR-0003 的 S1/S4)
# **镜像的入口是 `brickie build`**, 不是这个文件。
#
#   ① 工具段(本文件唯一的真身): `make` / `make tools` 用**宿主 g++/cargo** 编组合期工具
#      `{brickie, brickie-gen, brickie-core}`, 出树到 build/host/<host-arch>/<host-os>/bin。
#      它**不碰交叉工具链** —— 组合期命令的零编译依赖纪律(contract §9 R-13)。
#   ② 镜像段: 源码集合 / 编译标志 / 链接规则**全部**搬进了声明面 ——
#      product.toml [build] + 各插件 plugin.toml [build] + platform 的 [build.target]
#      + tests/gates.toml 的门禁; 由 `brickie build` 消费。
#
# 本文件里的 `all` / `run` / `smoke` / `irq-test` / `mem-test` / `string-test` / `dbg-test` /
# `check-string` / `check-headers` / `size` / `disasm` / `clean-brickos` 因此都降级成
# **薄委派别名**(调 $(BRICKIE) 的对应命令)—— 这是设计 ADR-0003 缩减期的**过渡形态**,
# 真身在 `brickie build` 与 `tests/gates.toml`; 别名只保证"老的肌肉记忆还能用"。
# `make check-build` 的第 ② 条不变量会抓任何"字面源码/标志回潮"。
#
# 工具链: **外部 gcc**(WORKAROUND br-wa-toolchain-001)。它的选型(交叉前缀 / 变体 / 链接
# 脚本 / QEMU 型号)现在由 product.toml + platform 插件声明面表达, 工具**候选序与解析在
# core**(contract §9 R-14 / R-17)。本文件只保留 `print-cross-compile` 这一个查询口。
#
# 用法:
#   make                    只编工具(= make tools; 不编镜像)
#   make all                编工具 → 委派 `brickie build`(镜像 + [build].post 门禁)
#   make tools              只编 L5/L2(不需要交叉编译器, 也不需要 cargo)
#   make tools-core         只编 L0/L1 的 Rust 核心(brickie-core; 需要 cargo)
#   make tools-test         跑工具自身用例(生成器自检 + 端到端)
#   make brickie-check      用 brickie 校验声明面(dev; 不编镜像)
#   make brickie-check-release  发布级门禁(--profile release)
#   make brickie-compose    重建 build/gen/** 生成物(不编译)
#   source setup.sh         配置开发环境(PATH + 环境变量; 见 ADR-0002 §7.5)
#   make env                打印同样的 shell 片段(CI: eval "$(make -s env)")
#   make run                在 QEMU 上跑(Ctrl-A X 退出; 委派 brickie run)
#   make smoke              3 秒冒烟(brickie test smoke)
#   make irq-test           中断子系统逐用例门禁(brickie test irq-test)
#   make mem-test           宿主侧内存语义门禁(brickie test mem-test --no-build)
#   make string-test        宿主侧编译器支持例程用例(brickie test string-test --no-build)
#   make dbg-test           内存映射 + 调试插件门禁(brickie test dbg-test)
#   make check-string       编译器支持例程自递归门禁(brickie test check-string)
#   make check-headers      对外头文件自洽门禁(brickie test check-headers --no-build)
#   make size / disasm      体积 / 反汇编(brickie size / disasm)
#   make check-workarounds  WORKAROUND 登记表与源码标记是否一致
#   make check-build        构建接线门禁(缺省目标=工具 / 无字面源码 / 委派 / 出树 / 种子)
#   make tools-prebuilt     把工具发布为自举种子 prebuilts/seed/brickie/<arch>/<os>/bin/
#   make tools-prebuilt-check  检查种子是否落后于源码
#   make print-host-triple  打印宿主三元组(host-arch/host-os)
#   make print-host-bin-dir 打印宿主工具 bin 目录(build/host/<arch>/<os>/bin)
#   make print-cross-compile 打印交叉前缀(仅供查询 / 门禁; 构建不再读它)
#   make clean              清掉①与②的产物(只清工具: make tools-clean; 不动 prebuilts/)

# ---------------------------------------------------------------- prebuilt(可选)
# prebuilts/toolchain/ 是**派生目录**(不进版本库, 配方见 prebuilts/toolchain.lock.toml), 由 `make prebuilt` 取件。
# 这个 -include 把各产物的实际路径带进来; 文件不在 ⇒ 下面的探测自动退回宿主工具链。
#
# **不信任何"就位标记"**: prebuilts/toolchain/ 允许只取一部分(`fetch-prebuilt.py <id>`), 所以下面
# 逐个用 $(wildcard) 检查二进制**真的存在**。见 ADR-0002。
-include prebuilts/toolchain/toolchain.mk

PREBUILT ?= prebuilts/toolchain

# ---------------------------------------------------------------- 工具链探测(仅查询)
# 优先 prebuilt(自洽、可复现), 否则退回宿主外部交叉工具链(br-wa-toolchain-001 的过渡形态)。
ifneq ($(strip $(PREBUILT_CROSS)),)
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

# ------------------------------------------------------------------ 目录/产物
BUILD_DIR := build

# 宿主工具产物(出树): build/host/<host-arch>/<host-os>/{bin,lib,obj}
# 由 mk/host.mk 经 tools/host-detect.sh 探测宿主; 与镜像产物(build/obj, build/brick.*)
# 分居 build/ 两侧, 互不覆盖 —— 参考 Android 的 out/host 与 out/target 分家。
#
# 另有 `prebuilts/`(进版本库): 自举种子 prebuilts/seed/brickie/<host-arch>/<host-os>/bin/,
# 由 `make tools-prebuilt` 发布 —— 让没有 g++ 的全新 checkout 也能直接把 brickie 跑起来,
# 也是将来用 brickie 自举管理自身编译的起点(见 docs/decisions/0004)。别与下载缓存
# `prebuilts/toolchain/`(派生侧、不进库)混淆。
HOST_BUILD_ROOT := $(abspath $(BUILD_DIR))
include mk/host.mk

# ------------------------------------------------------------------- 目标
# ⚠ 缺省目标 = **工具**(不是镜像): 迁移期 `make` 不再顺带编镜像, 免得"只想编工具"
#   的环境被交叉工具链卡住;"编镜像"显式走 `make all`(委派 `brickie build`)。
.DEFAULT_GOAL := tools

.PHONY: all prebuilt prebuilt-check prebuilt-clean \
        tools tools-core tools-test tools-clean tools-prebuilt tools-prebuilt-check \
        brickie-check brickie-check-release brickie-compose \
        run smoke irq-test mem-test string-test dbg-test check-string check-headers \
        size disasm check-workarounds check-build \
        clean clean-brickos help print-host-triple print-host-bin-dir \
        print-prebuilt-bin-dir print-cross-compile env

# ============================================================== ⓪ prebuilt 工具集
# 锁版本 + SHA256 校验的外部工具链**下载缓存**(配方 = prebuilts/toolchain.lock.toml), 见 ADR-0002。
#
# ⚠ 目标名 `prebuilt` 与目录 `prebuilts/toolchain/` **同名** —— 全靠上面 .PHONY 压住。
#   漏了它, make 会认为"目录已存在 ⇒ 无需做任何事", 取件变成**静默空操作**(踩过)。
#
# ⚠ 与 prebuilts/(**进库侧**, 自举种子)不是一回事: 那是 `tools-prebuilt` 的目标。
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

# ============================================================== ① 工具段(本文件唯一的真身)
# brickie 自身(组合期工具)。它有自己的构建入口与用例集 —— 这里只做**委派**,
# 不重复它的规则(mk 的单一真值: tools/brickie/Makefile)。
#
# 为什么是递归 make 而不是把对象文件列进来: 工具与镜像用**不同的编译器与选项**
# (宿主 g++ vs 交叉 gcc), 而且镜像已经搬去 brickie 那边, 混在一个 Makefile 里没有意义。
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
# 重建(`build/gen/**`)。
#
# 入口用**入口 ELF**: 它自带 Python 前端与两个原生工具(brickie-core/brickie-gen),
# 所以这里不需要 PYTHONPATH/源码树; 找不到 core 时会退回自举种子(见 native.py 的
# 查找顺序)。`make check-build` 的第 ⑥ 条不变量会核对种子三件齐。
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

# ============================================================== ② 镜像段(过渡别名)
# 下面是 ADR-0003 **缩减期的过渡别名**: 真身在 brickie(build.rs)+ tests/gates.toml。
# 每个别名只做三件事: 先确保工具在(order-only) → 调对应命令 → 不自己写任何规则。
# 一旦 CI/习惯都改直调 `brickie`, 这些别名可以整段删掉(设计 ADR-0003 的 S4 终点)。
#
# 为什么 order-only(`| tools`): 工具变新不该触发"重建镜像"的错觉 —— 判据在 brickie
# 的 argv 指纹与时间戳(contract §9 R-15), 不在这里。
all: | tools
	@echo "== ② 镜像: 委派 brickie build(ADR-0003 缩减期的过渡别名) =="
	@$(BRICKIE) build --root $(COMPOSE_ROOT)

run: | tools
	@echo "== ② QEMU 运行: 委派 brickie run(Ctrl-A X 退出) =="
	@$(BRICKIE) run --root $(COMPOSE_ROOT)

smoke: | tools
	@echo "== ② 冒烟门禁: 委派 brickie test smoke =="
	@$(BRICKIE) test smoke --root $(COMPOSE_ROOT)

irq-test: | tools
	@echo "== ② 中断逐用例门禁: 委派 brickie test irq-test =="
	@$(BRICKIE) test irq-test --root $(COMPOSE_ROOT)

# 宿主侧门禁用 --no-build: 它们只需要宿主 C 编译器, 与交叉工具链/QEMU 无关。
mem-test: | tools
	@echo "== ② 宿主内存语义门禁: 委派 brickie test mem-test --no-build =="
	@$(BRICKIE) test mem-test --no-build --root $(COMPOSE_ROOT)

string-test: | tools
	@echo "== ② 宿主支持例程用例: 委派 brickie test string-test --no-build =="
	@$(BRICKIE) test string-test --no-build --root $(COMPOSE_ROOT)

dbg-test: | tools
	@echo "== ② 内存映射 + 调试插件门禁: 委派 brickie test dbg-test =="
	@$(BRICKIE) test dbg-test --root $(COMPOSE_ROOT)

# check-string 判的是**编译产物**里的自递归(需要镜像已编) ⇒ 不带 --no-build。
check-string: | tools
	@echo "== ② 支持例程自递归门禁: 委派 brickie test check-string =="
	@$(BRICKIE) test check-string --root $(COMPOSE_ROOT)

# check-headers 只做 `-fsyntax-only`(脚本自己找交叉或宿主 C 编译器) ⇒ --no-build。
check-headers: | tools
	@echo "== ② 头文件自洽门禁: 委派 brickie test check-headers --no-build =="
	@$(BRICKIE) test check-headers --no-build --root $(COMPOSE_ROOT)

size: | tools
	@echo "== ② 体积: 委派 brickie size =="
	@$(BRICKIE) size --root $(COMPOSE_ROOT)

disasm: | tools
	@echo "== ② 反汇编: 委派 brickie disasm =="
	@$(BRICKIE) disasm --root $(COMPOSE_ROOT)

# ------------------------------------------------------- WORKAROUND / 构建门禁
check-workarounds:
	@bash tools/check-workarounds.sh

# 构建接线门禁: 缺省目标是不是工具 / Makefile 里还有没有字面源码与标志 /
# 镜像目标是不是委派 / 工具段有没有被交叉工具链污染 / 出树与种子。
# 理由见脚本头部 —— 接线坏了通常是**静默**的。
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

# 交叉前缀的**只读**查询口(门禁与 setup.sh 用)。镜像怎么编已经不读它 ——
# 目标事实在 platform 插件的 [build.target](contract §7.1)。
print-cross-compile:
	@echo $(CROSS_COMPILE)

# 开发环境(shell 片段): 工具链 bin 目录 + CROSS_COMPILE + brickie 的根路径。
#   eval "$(make -s env)"   —— 直接进当前 shell(等价于 source setup.sh)
#   source setup.sh         —— 常规用法; 另带 --unset / --with-make / 缺失提示
# 这里只做**打印**: Makefile 不该去改调用者的环境(它做不到, 也不该假装能做到)。
env:
	@bash setup.sh --print

# ------------------------------------------------------------------- 清理
# `clean` 清掉工具与镜像**两侧**的派生物; 只清工具用 tools-clean。
#
# 两者分开清: 宿主工具在 build/host/(出树输出), 镜像侧由 `brickie clean` 按声明面
# 清(它**不**动 build/host —— 免得"清镜像"顺手把工具删了)。
clean: clean-brickos tools-clean

clean-brickos: | tools
	@echo "== ② 清镜像派生物: 委派 brickie clean =="
	@$(BRICKIE) clean --root $(COMPOSE_ROOT)

help:
	@echo "目标: tools(缺省) all(委派 brickie build) prebuilt prebuilt-check prebuilt-clean"
	@echo "      tools-core tools-test tools-prebuilt tools-prebuilt-check tools-clean"
	@echo "      brickie-check brickie-check-release brickie-compose"
	@echo "      run smoke irq-test mem-test string-test dbg-test check-string check-headers"
	@echo "      size disasm check-workarounds check-build clean"
	@echo "      print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-cross-compile env"
	@echo "变量: CROSS_COMPILE=$(CROSS_COMPILE)(仅查询口)"
	@echo "      HOST_TRIPLE=$(HOST_TRIPLE)  HOST_BIN_DIR=$(HOST_BIN_DIR)"
	@echo "      PREBUILT_BRICKIE_BIN_DIR=$(PREBUILT_BRICKIE_BIN_DIR)"
	@echo "归属: 镜像入口 = brickie build(声明面: product.toml [build] + 插件 [build] + [build.target]);"
	@echo "      本文件只编工具, 镜像目标都是**过渡别名**(ADR-0003 的 S1/S4)"
	@echo "布局: 工具 → build/host/<host-arch>/<host-os>/bin(参考 Android out/host/...); 镜像 → build/"
	@echo "种子: tools-prebuilt → prebuilts/seed/brickie/<host-arch>/<host-os>/bin(进版本库, 自举用)"
	@echo "工具链来源: $(TOOLCHAIN_SOURCE)"
	@echo "prebuilt(取件目标 → prebuilts/toolchain/): $(if $(filter 1,$(PREBUILT_READY)),已就位,未取件 —— make prebuilt 取件(ADR-0002))"

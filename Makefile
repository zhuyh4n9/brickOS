# brickOS prototype v0.1.0 — 构建入口
#
# 目标: aarch64 裸机 ELF, 跑在 QEMU virt 上。
# 工具链: **外部 gcc**
#   WORKAROUND(br-wa-toolchain-001): 内部工具链就绪前借用宿主交叉编译器。
#   退出条件 = 只改 CROSS_COMPILE(见 WORKAROUNDS.md), 构建规则本身不动。
#
# 用法:
#   make                    构建 build/brick.elf + .bin
#   make run                在 QEMU 上跑(Ctrl-A X 退出)
#   make smoke              3 秒冒烟: 自动判定启动与延时是否正常
#   make size / disasm      体积 / 反汇编
#   make check-workarounds  WORKAROUND 登记表与源码标记是否一致
#   make clean

# ---------------------------------------------------------------- 工具链探测
CROSS_COMPILE ?= aarch64-linux-gnu-

# 无版本号优先; 退化到带版本号的驱动名(Ubuntu 只装 gcc-N 时没有软链)
CC_CANDIDATES := $(CROSS_COMPILE)gcc $(CROSS_COMPILE)gcc-15 $(CROSS_COMPILE)gcc-14 $(CROSS_COMPILE)gcc-13
CC := $(firstword $(foreach c,$(CC_CANDIDATES),$(if $(shell command -v $(c) 2>/dev/null),$(c))))

binutils_probe = $(firstword $(foreach c,$(CROSS_COMPILE)$(1),$(if $(shell command -v $(c) 2>/dev/null),$(c))))
OBJCOPY := $(call binutils_probe,objcopy)
OBJDUMP := $(call binutils_probe,objdump)
SIZE    := $(call binutils_probe,size)

ifeq ($(strip $(CC)),)
$(error 找不到交叉编译器 "$(CROSS_COMPILE)gcc"。装一个, 或指定 CROSS_COMPILE=<前缀->)
endif

QEMU ?= qemu-system-aarch64

# ------------------------------------------------------------------ 目录/产物
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
ELF       := $(BUILD_DIR)/brick.elf
BIN       := $(BUILD_DIR)/brick.bin
MAP       := $(BUILD_DIR)/brick.map
LDSCRIPT  := platform/src/aarch64/link.ld

# ------------------------------------------------------------------- 源文件
CORE_SRCS := $(wildcard core/src/*.c) $(wildcard core/src/*/*.c)
PLAT_SRCS := $(wildcard platform/src/*/*.c)
ASM_SRCS  := $(wildcard platform/src/*/*.S)

SRCS := $(CORE_SRCS) $(PLAT_SRCS) $(ASM_SRCS)
OBJS := $(addprefix $(OBJ_DIR)/,$(patsubst %.c,%.o,$(patsubst %.S,%.o,$(SRCS))))

# --------------------------------------------------------------------- 选项
INCLUDES := -Icore/include -Iplatform/include

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
.PHONY: all run smoke size disasm check-workarounds clean help

all: $(ELF) $(BIN)
	@echo "OK: $(ELF)  ($(CC))"

$(ELF): $(OBJS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# ---------------------------------------------------------------- 运行/验证
QEMUFLAGS ?= -M virt -cpu cortex-a53 -m 128M -nographic

run: all
	$(QEMU) $(QEMUFLAGS) -kernel $(ELF)

# 冒烟: 限时跑, 然后把"启动成功"与"延时判据"当断言查 —— 让人眼看的日志
# 变成 CI 能判的红绿(grep 的判据就是 main.c 里自己打的那两行)。
SMOKE_SECONDS ?= 3
smoke: all
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
	$(SIZE) -A -x $(ELF)
	@$(SIZE) $(ELF)

disasm: all
	$(OBJDUMP) -d $(ELF) | head -120

# ------------------------------------------------------------- WORKAROUND 门禁
check-workarounds:
	@bash tools/check-workarounds.sh

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "目标: all(缺省) run smoke size disasm check-workarounds clean"
	@echo "变量: CROSS_COMPILE=$(CROSS_COMPILE)  CC=$(CC)"
	@echo "      QEMU=$(QEMU)  QEMUFLAGS=$(QEMUFLAGS)"

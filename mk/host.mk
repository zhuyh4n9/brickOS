# mk/host.mk — host 平台三元组与 host 工具产物目录
#
# 布局(参考 Android 的 out/host/...):
#
#   build/host/<host-arch>/<host-os>/bin/   ← 宿主可执行(brickie-core + brickie-gen + 入口 ELF brickie)
#   build/host/<host-arch>/<host-os>/lib/   ← 宿主静态库(如 libbrickie-gen.a)
#   build/host/<host-arch>/<host-os>/obj/   ← 宿主中间产物(.o)
#
#   host-arch: x86-64 / aarch64 / ...   (处理器架构)
#   host-os:   linux / darwin / win     (操作系统)
#
# 另有一份**进版本库**的宿主产物(自举种子, 参考 Android 的 prebuilts/):
#
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie       ← L5 入口 ELF(嵌入 Python 载荷)
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie-gen   ← L2 渲染器
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie-core  ← L0/L1 Rust 核心
#
#   `build/host/**` 是**本次/本机**构建产物(派生, 不进库); `prebuilts/**` 是
#   随源码提交的**种子**, 供没有 g++/cargo 的全新 checkout 直接把 brickie 跑起来 ——
#   也即"用 brickie 自举管理 brickie 自身编译"的起点(见 ADR 0004)。三件成组,
#   入口 ELF 的载荷里已嵌两个原生工具(种子目录只是冗余副本, 供开发态直用)。
#
# 单一真值: arch/os 的映射只在 tools/host-detect.sh; 本文件只把它变成 make 变量。
# 用法: 在 Makefile 里 `include <相对路径>/mk/host.mk` —— 路径按**本文件自身位置**
# 推导仓库根, 所以从哪个目录调用 make 都得到同一组目录(出树构建的关键)。
#
# 可用变量覆盖:
#   HOST_BUILD_ROOT  默认 <repo>/build(集中放 host/ 与镜像产物)
#   PREBUILTS_ROOT   默认 <repo>/prebuilts(自举种子; 与下载缓存 prebuilts/toolchain/ 不同)
#   BRICKIE_HOST_ARCH / BRICKIE_HOST_OS  透传给 host-detect.sh(CI 构造异宿主)
#
# 目录名辨析: `prebuilts/toolchain/`(**单数**, 下载缓存 + 解压出的外部工具链; 派生, 体积大,
# 不进库)与 `prebuilts/`(**复数**, 随源码提交的自举种子)是两件事, 不要合并。

MK_HOST_MK := $(lastword $(MAKEFILE_LIST))
REPO_ROOT  := $(abspath $(dir $(abspath $(MK_HOST_MK)))/..)

HOST_DETECT ?= $(REPO_ROOT)/tools/host-detect.sh

# 一次探测, 拆成两段。两道防御: 脚本不在 / 脚本跑了但没输出 —— 都当场报"环境错",
# 不要静默退化成空段路径(那会把产物写到 build/host//bin 这种地方)。
ifeq ($(wildcard $(HOST_DETECT)),)
$(error 找不到 host 探测脚本 $(HOST_DETECT)(mk/host.mk 依赖它; 检查仓库是否完整))
endif
_HOST_DETECTED := $(shell bash $(HOST_DETECT))

HOST_ARCH   := $(word 1,$(_HOST_DETECTED))
HOST_OS     := $(word 2,$(_HOST_DETECTED))

ifeq ($(strip $(HOST_ARCH))$(strip $(HOST_OS)),)
$(error host 探测脚本没有输出 ($(HOST_DETECT));先单独跑 `bash $(HOST_DETECT)` 看报错)
endif

HOST_TRIPLE := $(HOST_ARCH)/$(HOST_OS)

HOST_BUILD_ROOT ?= $(REPO_ROOT)/build
HOST_OUT     := $(HOST_BUILD_ROOT)/host/$(HOST_TRIPLE)
HOST_BIN_DIR := $(HOST_OUT)/bin
HOST_LIB_DIR := $(HOST_OUT)/lib
HOST_OBJ_DIR := $(HOST_OUT)/obj

# 自举种子(进版本库): prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}
PREBUILTS_ROOT           ?= $(REPO_ROOT)/prebuilts
PREBUILT_BRICKIE_DIR     := $(PREBUILTS_ROOT)/seed/brickie/$(HOST_TRIPLE)
PREBUILT_BRICKIE_BIN_DIR := $(PREBUILT_BRICKIE_DIR)/bin

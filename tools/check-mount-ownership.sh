#!/usr/bin/env bash
#
# brickOS prototype — **挂载归属**门禁(ADR-0030)
#
# 为什么需要它:
#   ADR-0030 把"谁挂载"从 **FS 插件自己** 改成 **APP**:
#     * FS 插件只向 vfs-core 登记**驱动**(`br_fs_driver_register`, fstype 名 → ops);
#     * APP 用 POSIX `mount(source, target, fstype, …)` 按名挂载, 计划写在它自己的
#       `plugin.toml` 的 `[[mount]]` 里。
#
#   这两条都属于"**改了也不报错、只是行为悄悄变了**":
#     - 谁再往 FS 插件里写一行 `br_mount_register`, 挂载就又成了 FS 自己的决定 ——
#       而功能测试**照旧能挂上**, 于是没人会发现;
#     - 谁再往 FS 插件的 plugin.toml 里写 `[[mount]]`, "挂载计划"就又有两个真值
#       (码里/清单里各一份, 见已还债的 br-wa-fs-001)。
#   ⇒ 判据必须是机械的。
#
# 两条判据:
#   ① **FS 不许自挂载**: `fs/**` 与 `adapter/**` 的 .c 里不出现 `br_mount_register`
#      (唯一豁免: vfs-core 自己的实现与一致性套件 —— 它测的正是这张表);
#   ② **`[[mount]]` 只归 APP**: 除 `app/**` 外的任何 `plugin.toml` 里没有 `[[mount]]`。
#
# 用法: bash tools/check-mount-ownership.sh
# 退出码: 0 = 全过, 1 = 有违规, 2 = 用法/环境错
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

status=0
fail() { echo "FAIL: $*" >&2; status=1; }
pass() { echo "ok   $*"; }

# ---- ① FS 插件(与适配层)不许自挂载 -------------------------------------------
# 豁免: framework/vfs-core —— `br_mount_register` 是它自己的实现, 且它的一致性套件
# 用它构造挂载表(测的正是挂载语义)。除此之外没人该直接调它。
hits="$(grep -rn --include='*.c' -E '\bbr_mount_register[[:space:]]*\(' fs adapter 2>/dev/null || true)"
if [ -n "$hits" ]; then
    fail "FS 插件/适配层仍在自挂载(应改为 br_fs_driver_register, 由 APP mount):"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
else
    pass "fs/ 与 adapter/ 里无人自挂载(都改为登记驱动)"
fi

# ---- ② [[mount]] 只归 APP -----------------------------------------------------
hits2="$(grep -rln --include='plugin.toml' '^\[\[mount\]\]' . 2>/dev/null \
         | grep -v '^\./app/' | grep -v '^\./build/' || true)"
if [ -n "$hits2" ]; then
    fail "非 APP 的 plugin.toml 里声明了 [[mount]](挂载计划归消费者):"
    printf '%s\n' "$hits2" | sed 's/^/      /' >&2
else
    pass "[[mount]] 只出现在 app/ 的声明面里"
fi

# ---- ③ APP 侧要有执行点(正面判据: 计划不能只声明不执行) ----------------------
if ! grep -rqn --include='plugin.toml' '^\[\[mount\]\]' app 2>/dev/null; then
    echo "note: app/ 下没有 [[mount]] 声明(合法: 不需要挂任何 FS 的产品)" >&2
else
    # 生成了"应用函数"才算真的能执行(brickie 的发散点见 plan.rs)
    if grep -rqn 'mount_plan_apply' build/gen 2>/dev/null; then
        pass "APP 的挂载计划已被 brickie 生成成可执行入口(mount_plan_apply)"
    else
        fail "app/ 声明了 [[mount]] 但没有生成 mount_plan_apply —— 计划执行不了(先跑 brickie gen)"
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: 挂载归属正确(FS 只登记驱动 / [[mount]] 归 APP / 计划有执行入口)"
fi
exit "$status"

// brickie-gen — C++ 生成器(L2), **只做模板渲染**, 不做任何判定
//
// 进程边界(contract §1/§2/§6): 输入输出 = **JSON over stdio**。
//   stdin  = 请求对象;  stdout = 响应对象(始终是合法 JSON)
// 退出码: 本进程**只**用 0(协议处理成功, 诊断与业务退出码在响应里)与 2(自身故障:
// 入参不是 JSON 等)。业务退出码由响应字段 exit_code 承载, 由 Python L5 前端转成
// 进程退出码 —— 这样"工具故障"与"校验红"不会被混为一谈。
//
// 命令面(contract §6): 只有 `render`(模板渲染)。
//   * 名字契约 / subkind 推导 / 相位 / 路径布局 / 版本分段等**判定**已全部搬去
//     `brickie-core`(Rust, L0/L1); 本工具只承接 "计划 + 变量 → 文件内容"。
//   * 旧的 `plan-new` / `new` 已删除(规则与计划归 Rust 的 `plan-new` / `plan-init`)。
//
// `--selftest`: 不读 stdin, 跑内置自检(JSON 往返 / 渲染 / 路径与模式 / 载荷相关
// 确定性), 供 CI 的"粘合层纯度检查"使用(§9.1: brickie-core --selftest 的生成器侧
// 对应物)。
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "diag.h"
#include "json.h"
#include "render.h"
#include "text.h"

namespace {

using brickie::J;

// 响应信封(contract §2)。status 只有 ok / internal_error 两种。
J makeResponse(int exitCode, J diagnostics, J files, J data) {
    J o = J::obj();
    o.set("protocol", J::num(1));
    o.set("status", J::str("ok"));
    o.set("exit_code", J::num(exitCode));
    o.set("diagnostics", std::move(diagnostics));
    o.set("files", std::move(files));
    o.set("data", std::move(data));
    return o;
}

int failInternal(const std::string &msg, const std::string &hint = "") {
    brickie::Diags diags;
    diags.usage(msg, hint);
    J o = makeResponse(2, diags.toJson(), J::arr(), J::obj());
    o.set("status", J::str("internal_error"));
    std::cout << brickie::dumpJson(o, 2) << "\n";
    std::cerr << "brickie-gen: " << msg << (hint.empty() ? "" : (" (" + hint + ")")) << "\n";
    return 2;
}

std::string readStdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- selftest

int selftest() {
    int failures = 0;
    auto check = [&](bool ok, const std::string &what) {
        std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
        if (!ok) ++failures;
    };

    // JSON 往返(含转义、嵌套、非 ASCII、代理对)
    {
        const std::string src =
            R"({"a":[1,true,null,"x\n\"\u4e2d\u6587\uD83D\uDE00"],"b":{"c":-1.5e3}})";
        J v;
        std::string err;
        const bool parsed = brickie::parseJson(src, v, err);
        check(parsed, "parseJson 接受嵌套/转义/非 ASCII: " + err);
        if (parsed) {
            const std::string round = brickie::dumpJson(v, -1);
            J v2;
            std::string err2;
            check(brickie::parseJson(round, v2, err2), "dumpJson 输出可被重新解析");
            check(brickie::dumpJson(v2, -1) == round, "JSON 往返逐字节稳定");
        }
    }
    // 非法 JSON 必须被拒
    {
        J v;
        std::string err;
        check(!brickie::parseJson("{\"a\":}", v, err), "非法 JSON 被拒");
    }

    // 渲染: 新变量契约取小写键; 缺键必须失败(不静默成空串); 无分支语法
    {
        brickie::Vars vars;
        vars["name"] = "service/crypto";
        vars["short"] = "crypto";
        vars["Version"] = "0.1.0.0";  // 大小写敏感: 与 version 不是同一个键
        std::string out, err;
        check(brickie::render("x{{name}}y", vars, "t", out, err) && out == "xservice/cryptoy",
              "render: 基本替换(小写键)");
        check(brickie::render("{{ name }}", vars, "t", out, err) && out == "service/crypto",
              "render: 占位符两侧空白容忍");
        check(!brickie::render("{{missing}}", vars, "t", out, err), "render: 缺键失败");
        check(!brickie::render("{{#if x}}a{{/if}}", vars, "t", out, err),
              "render: 无条件/分支语法(非法键被拒)");
        check(!brickie::render("{{short} }", vars, "t", out, err), "render: 未闭合占位符被拒");
        check(brickie::render("{{Version}}", vars, "t", out, err) && out == "0.1.0.0",
              "render: 键名大小写敏感");
    }

    // 路径安全: 生成物必须落仓库内
    {
        std::string err;
        check(brickie::safeRelPath("service/crypto/plugin.toml", err),
              "safeRelPath: 正常相对路径");
        check(!brickie::safeRelPath("/etc/passwd", err), "safeRelPath: 拒绝绝对路径");
        check(!brickie::safeRelPath("../escape", err), "safeRelPath: 拒绝 ..");
        check(!brickie::safeRelPath("a/../b", err), "safeRelPath: 拒绝中段 ..");
        check(!brickie::safeRelPath("a//b", err), "safeRelPath: 拒绝空路径段");
        check(!brickie::safeRelPath("a/", err), "safeRelPath: 拒绝结尾 /");
        check(!brickie::safeRelPath("", err), "safeRelPath: 拒绝空串");
    }

    // 模式位(载荷条目模式与落盘模式都走它)
    {
        std::string mode;
        check(brickie::parseMode("0644", mode) && mode == "0644", "parseMode: 0644");
        check(brickie::parseMode("755", mode) && mode == "0755", "parseMode: 755 → 0755 规范化");
        check(!brickie::parseMode("", mode), "parseMode: 拒绝空串");
        check(!brickie::parseMode("999", mode), "parseMode: 拒绝非八进制");
        check(!brickie::parseMode("06442", mode), "parseMode: 拒绝过长");
        check(!brickie::parseMode("0x1", mode), "parseMode: 拒绝十六进制");
    }

    // 生成物标记(contract §6: 首行带 `brickie:generated`)
    {
        const std::string marker = brickie::kGeneratedMarker;
        check(marker == "brickie:generated", "生成物标记 = brickie:generated");
        brickie::Vars vars;
        vars["name"] = "service/crypto";
        std::string out, err;
        check(brickie::render("/* brickie:generated */ {{name}}", vars, "t", out, err) &&
                  out.find("brickie:generated") != std::string::npos,
              "生成物首行可含标记");
    }

    // 载荷相关自检: 同一输入两次渲染逐字节一致(嵌入载荷逐字节可复现的前提);
    // 响应信封(含 files[].mode)可稳定往返。
    {
        brickie::Vars vars;
        vars["name"] = "service/crypto";
        vars["short"] = "crypto";
        std::string a, b, err;
        const bool ok1 = brickie::render("{{name}}:{{short}}\n", vars, "t", a, err);
        const bool ok2 = brickie::render("{{name}}:{{short}}\n", vars, "t", b, err);
        check(ok1 && ok2 && a == b, "载荷: 渲染逐字节可复现(两次调用相同)");

        J files = J::arr();
        J f = J::obj();
        f.set("path", J::str("service/crypto/plugin.toml"));
        f.set("kind", J::str("rendered"));
        f.set("content", J::str(a));
        f.set("mode", J::str("0644"));
        files.push(f);
        const std::string wire = brickie::dumpJson(makeResponse(0, J::arr(), files, J::obj()), 2);
        J back;
        std::string perr;
        const bool parsedWire = brickie::parseJson(wire, back, perr);
        check(parsedWire, "载荷: 响应信封是合法 JSON");
        std::string wire2;
        J back2;
        std::string perr2;
        if (parsedWire) wire2 = brickie::dumpJson(back, -1);
        check(parsedWire && brickie::parseJson(wire2, back2, perr2) &&
                  brickie::dumpJson(back2, -1) == wire2,
              "载荷: 响应信封往返稳定");
        const J *backFiles = back.find("files");
        check(backFiles != nullptr && backFiles->isArr() && backFiles->a.size() == 1 &&
                  backFiles->a[0].find("mode") != nullptr &&
                  backFiles->a[0].find("mode")->asStr() == "0644" &&
                  backFiles->a[0].find("kind")->asStr() == "rendered",
              "载荷: files[] 带 kind/mode 且形状稳定");
    }

    // 文件归属(contract §4): `kind=machine` 的内容由 brickie-core 拥有, render 跳过
    // (不读模板、不算错) —— plan-init 的 `brickie.lock` 就带这个 kind。
    {
        J args = J::obj();
        J arts = J::arr();
        J machine = J::obj();
        machine.set("path", J::str("brickie.lock"));
        machine.set("kind", J::str("machine"));
        machine.set("template", J::str(""));
        arts.push(machine);
        args.set("artifacts", arts);
        const brickie::RenderResult rr = brickie::runRender(args, "/nonexistent-templates");
        check(rr.diags.exitCode() == 0 && rr.files.empty(),
              "render: kind=machine 跳过(不读模板, 不算用法错)");
    }

    if (failures == 0) {
        std::cout << "brickie-gen selftest: 全部通过\n";
        return 0;
    }
    std::cout << "brickie-gen selftest: " << failures << " 项失败\n";
    return 1;
}

// ---------------------------------------------------------------- 协议分派

// 协议握手的失败响应: BRV-PROTO-0001, exit_code 2, status 保持 "ok"
// (协议处理本身成功, 业务退出码在响应里; contract §2)。
int protoMismatch(const std::string &detail) {
    brickie::Diags diags;
    diags.coded("BRV-PROTO-0001", brickie::Severity::Error, "", "",
                "子进程 JSON 协议不匹配: " + detail,
                "本工具 protocol = 1; 请求必须带 {\"protocol\": 1}");
    std::cout << brickie::dumpJson(makeResponse(2, diags.toJson(), J::arr(), J::obj()), 2) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") return selftest();
        if (a == "--version") {
            std::cout << "brickie-gen 0.1.0 (protocol 1)\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::cout << "用法: brickie-gen [--selftest|--version]\n"
                         "  无参数时从 stdin 读 JSON 请求, 向 stdout 写 JSON 响应。\n"
                         "  命令: render — 按 brickie-core 给的计划渲染模板(contract §6)。\n";
            return 0;
        }
        std::cerr << "brickie-gen: 未知参数 `" << a << "`\n";
        return 2;
    }

    const std::string input = readStdin();
    if (input.empty()) return failInternal("stdin 为空: 需要一个 JSON 请求对象");

    J req;
    std::string err;
    if (!brickie::parseJson(input, req, err)) return failInternal("请求不是合法 JSON", err);
    if (!req.isObj()) return failInternal("请求必须是 JSON 对象");

    // ---- 协议握手(contract §2)----
    const J *proto = req.find("protocol");
    if (proto == nullptr) return protoMismatch("请求缺 `protocol` 字段");
    if (!(proto->t == brickie::JType::Num && proto->n == 1.0)) {
        return protoMismatch("收到 protocol = " + brickie::dumpJson(*proto, -1));
    }

    const J *cmdJ = req.find("command");
    const std::string command = (cmdJ != nullptr && cmdJ->isStr()) ? cmdJ->s : std::string();
    if (command.empty()) {
        brickie::Diags diags;
        diags.usage("请求缺 `command` 字段(字符串)", "v0.1 生成器只承接 `render`(contract §6)");
        std::cout << brickie::dumpJson(makeResponse(2, diags.toJson(), J::arr(), J::obj()), 2)
                  << "\n";
        return 0;
    }

    J args = J::obj();
    if (const J *a = req.find("args"); a != nullptr && a->isObj()) args = *a;

    if (command != "render") {
        brickie::Diags diags;
        diags.usage("brickie-gen 不认识命令 `" + command + "`",
                    "v0.1 的生成器只承接 `render`(模板渲染); 判定与计划由 brickie-core 承接");
        std::cout << brickie::dumpJson(makeResponse(2, diags.toJson(), J::arr(), J::obj()), 2)
                  << "\n";
        return 0;
    }

    // templates_root: 契约 §6 放在 args 里; 兼容顶层字段(旧前端形态)。
    std::string templatesRoot;
    if (const J *t = args.find("templates_root"); t != nullptr && t->isStr()) {
        templatesRoot = t->s;
    } else if (const J *t = req.find("templates_root"); t != nullptr && t->isStr()) {
        templatesRoot = t->s;
    }

    const brickie::RenderResult r = brickie::runRender(args, templatesRoot);

    J files = J::arr();
    for (const auto &f : r.files) {
        J o = J::obj();
        o.set("path", J::str(f.path));
        o.set("kind", J::str(f.kind));
        o.set("content", J::str(f.content));
        o.set("mode", J::str(f.mode));
        files.push(std::move(o));
    }

    std::cout << brickie::dumpJson(makeResponse(r.diags.exitCode(), r.diags.toJson(),
                                               std::move(files), J::obj()),
                                  2)
              << "\n";
    return 0;
}

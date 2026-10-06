// brickie-gen — C++ 生成器(L2), 骨架/描述符/头文件代码生成
//
// 进程边界(§9.1 / C9): 输入输出 = **JSON over stdio**。
//   stdin  = 请求对象;  stdout = 响应对象(始终是合法 JSON)
// 退出码: 本进程**只**用 0(协议处理成功, 诊断与退出码在响应里)与 2(自身故障:
// 入参不是 JSON / 模板目录缺失等)。业务退出码由响应字段 exit_code 承载, 由
// Python L5 前端转成进程退出码 —— 这样"工具故障"与"校验红"不会被混为一谈。
//
// `--selftest`: 不读 stdin, 跑内置自检(JSON 往返 + 规则 + 模板渲染), 供 CI
// 的"粘合层纯度检查"使用(§9.1: brickie-core --selftest 的生成器侧对应物)。
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "diag.h"
#include "gen_new.h"
#include "json.h"
#include "rules.h"
#include "text.h"

namespace {

int failInternal(const std::string &msg, const std::string &hint = "") {
    brickie::J o = brickie::J::obj();
    o.set("protocol", brickie::J::num(1));
    o.set("status", brickie::J::str("internal_error"));
    o.set("exit_code", brickie::J::num(2));
    brickie::J diags = brickie::J::arr();
    brickie::Diag d;
    d.severity = brickie::Severity::Error;
    d.message = msg;
    d.hint = hint;
    diags.push(d.toJson());
    o.set("diagnostics", diags);
    o.set("artifacts", brickie::J::arr());
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
        brickie::J v;
        std::string err;
        const bool parsed = brickie::parseJson(src, v, err);
        check(parsed, "parseJson 接受嵌套/转义/非 ASCII: " + err);
        if (parsed) {
            const std::string round = brickie::dumpJson(v, -1);
            brickie::J v2;
            std::string err2;
            check(brickie::parseJson(round, v2, err2), "dumpJson 输出可被重新解析");
            check(brickie::dumpJson(v2, -1) == round, "JSON 往返逐字节稳定");
        }
    }
    // 非法 JSON 必须被拒
    {
        brickie::J v;
        std::string err;
        check(!brickie::parseJson("{\"a\":}", v, err), "非法 JSON 被拒");
    }

    // 名字契约
    check(brickie::validPluginName("service/crypto"), "名字契约: service/crypto");
    check(brickie::validPluginName("sched-coop"), "名字契约: sched-coop");
    check(!brickie::validPluginName("Service/Crypto"), "名字契约: 大写非法");
    check(!brickie::validPluginName("service//crypto"), "名字契约: 双斜杠非法");
    check(!brickie::validPluginName("service/"), "名字契约: 尾分隔符非法");
    check(!brickie::validPluginName("1crypto"), "名字契约: 数字开头非法");

    // 分类学推导
    {
        std::string sk;
        check(brickie::deriveSubkind("service/crypto", sk) && sk == "service", "subkind: service/→service");
        check(brickie::deriveSubkind("sched/coop", sk) && sk == "scheduler", "subkind: sched/→scheduler");
        check(!brickie::deriveSubkind("sched-coop", sk), "subkind: 裸名不可推导");
    }

    // 相位
    check(brickie::phaseFor("platform", "") == "early", "phase: platform→early");
    check(brickie::phaseFor("ability", "scheduler") == "early", "phase: scheduler→early");
    check(brickie::phaseFor("ability", "service") == "late", "phase: service→late");
    check(brickie::phaseFor("ability", "fs") == "core", "phase: fs→core");
    check(brickie::phaseFor("interface", "") == "late", "phase: interface→late");
    check(brickie::phaseFor("app", "") == "app", "phase: app→app");

    // 渲染: 缺键必须失败(不静默成空串)
    {
        brickie::Vars vars;
        vars["A"] = "1";
        std::string out, err;
        check(brickie::render("x{{A}}y", vars, "t", out, err) && out == "x1y", "render: 基本替换");
        check(!brickie::render("{{B}}", vars, "t", out, err), "render: 缺键失败");
    }

    // 路径布局
    {
        const brickie::PluginPaths p = brickie::layoutOf("service/crypto");
        check(p.descriptor == "build/gen/service/crypto/plugin_desc.c", "layout: 生成物落仓库级 build/gen/");
        check(p.header == "service/crypto/include/crypto/crypto.h", "layout: 头文件在 include/<short>/");
        check(p.source == "service/crypto/src/crypto.c", "layout: 源在 src/<short>.c");
    }

    if (failures == 0) {
        std::cout << "brickie-gen selftest: 全部通过\n";
        return 0;
    }
    std::cout << "brickie-gen selftest: " << failures << " 项失败\n";
    return 1;
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
                         "  无参数时从 stdin 读 JSON 请求, 向 stdout 写 JSON 响应。\n";
            return 0;
        }
        std::cerr << "brickie-gen: 未知参数 `" << a << "`\n";
        return 2;
    }

    const std::string input = readStdin();
    if (input.empty()) return failInternal("stdin 为空: 需要一个 JSON 请求对象");

    brickie::J req;
    std::string err;
    if (!brickie::parseJson(input, req, err))
        return failInternal("请求不是合法 JSON", err);
    if (!req.isObj()) return failInternal("请求必须是 JSON 对象");

    const brickie::J *cmdJ = req.find("command");
    const std::string command = cmdJ ? cmdJ->asStr() : "";
    if (command.empty()) return failInternal("请求缺 `command` 字段");

    const brickie::J *rootJ = req.find("templates_root");
    const std::string templatesRoot = rootJ ? rootJ->asStr() : "";
    if (templatesRoot.empty()) return failInternal("请求缺 `templates_root` 字段");

    brickie::J args = brickie::J::obj();
    if (const brickie::J *a = req.find("args"); a && a->isObj()) args = *a;
    brickie::J context = brickie::J::obj();
    if (const brickie::J *c = req.find("context"); c && c->isObj()) context = *c;

    brickie::J response = brickie::J::obj();
    response.set("protocol", brickie::J::num(1));

    if (command == "new" || command == "plan-new") {
        const bool planOnly = (command == "plan-new");
        brickie::NewResult r = brickie::runNew(args, context, templatesRoot, planOnly);
        response.set("status", brickie::J::str(r.diags.exitCode() == 0 ? "ok" : "diagnostics"));
        response.set("exit_code", brickie::J::num(r.diags.exitCode()));
        response.set("diagnostics", r.diags.toJson());
        brickie::J planned = brickie::J::arr();
        for (const auto &p : r.planned) {
            brickie::J o = brickie::J::obj();
            o.set("path", brickie::J::str(p.path));
            o.set("kind", brickie::J::str(p.kind));
            planned.push(std::move(o));
        }
        response.set("planned", std::move(planned));
        brickie::J arr = brickie::J::arr();
        for (const auto &a : r.artifacts) {
            brickie::J o = brickie::J::obj();
            o.set("path", brickie::J::str(a.path));
            o.set("kind", brickie::J::str(a.kind));
            o.set("content", brickie::J::str(a.content));
            arr.push(std::move(o));
        }
        response.set("artifacts", std::move(arr));
    } else {
        response.set("status", brickie::J::str("unknown_command"));
        response.set("exit_code", brickie::J::num(2));
        brickie::Diag d;
        d.severity = brickie::Severity::Error;
        d.message = "brickie-gen 不认识命令 `" + command + "`";
        d.hint = "v0.1 的生成器只承接 `new` / `plan-new`; 其余命令由 brickie-core 承接";
        brickie::J diags = brickie::J::arr();
        diags.push(d.toJson());
        response.set("diagnostics", diags);
        response.set("planned", brickie::J::arr());
        response.set("artifacts", brickie::J::arr());
    }

    std::cout << brickie::dumpJson(response, 2) << "\n";
    return 0;
}

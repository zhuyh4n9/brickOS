#include "render.h"

#include <set>

#include "text.h"

namespace brickie {
namespace {

// vars 的值必须是可替换的文本。契约里都是字符串, 但为了不把"数字/bool"
// 这种明显无歧义的 JSON 标量当用法错, 这里就地串化; null ⇒ 空串(可选变量)。
bool scalarToText(const J &v, std::string &out) {
    switch (v.t) {
        case JType::Str:
            out = v.s;
            return true;
        case JType::Num:
        case JType::Bool:
            out = dumpJson(v, -1);
            return true;
        case JType::Null:
            out.clear();
            return true;
        default:
            return false;
    }
}

bool collectVars(const J *vars, Vars &out, std::string &err) {
    if (vars == nullptr) return true;
    if (!vars->isObj()) {
        err = "`vars` 必须是 JSON 对象(`{键: 字符串}`)";
        return false;
    }
    for (const auto &kv : vars->o) {
        std::string text;
        if (!scalarToText(kv.second, text)) {
            err = "`vars." + kv.first + "` 不是字符串/数字/bool/null";
            return false;
        }
        out[kv.first] = std::move(text);
    }
    return true;
}

}  // namespace

RenderResult runRender(const J &args, const std::string &templatesRoot) {
    RenderResult res;

    if (templatesRoot.empty()) {
        res.diags.usage("`render` 需要 `templates_root`(模板根目录的绝对路径)",
                        "见 contract §6: args = {templates_root, artifacts:[{path,template,vars}]}");
        return res;
    }

    const J *artifacts = args.find("artifacts");
    if (artifacts == nullptr || !artifacts->isArr()) {
        res.diags.usage("`render` 需要 `args.artifacts` 数组",
                        "每条 = {path, template, vars}; 计划由 brickie-core 的 plan-* 给出");
        return res;
    }

    std::vector<File> files;
    std::set<std::string> seenPaths;
    bool anyError = false;

    for (std::size_t i = 0; i < artifacts->a.size(); ++i) {
        const J &item = artifacts->a[i];
        const std::string where = "artifacts[" + std::to_string(i) + "]";
        if (!item.isObj()) {
            res.diags.usage(where + " 不是 JSON 对象", "每条 = {path, template, vars}");
            anyError = true;
            continue;
        }

        const std::string path =
            item.find("path") != nullptr ? item.find("path")->asStr() : std::string();
        const std::string tpl =
            item.find("template") != nullptr ? item.find("template")->asStr() : std::string();
        const std::string kind =
            item.find("kind") != nullptr ? item.find("kind")->asStr() : std::string();

        // contract §4 的文件归属: `kind = "machine"`(如 brickie.lock / 快照)的内容由
        // `brickie-core` 拥有, 不走模板渲染 ⇒ 计划里带着它时**跳过**, 不是用法错
        // (L5 按 §4 用 core 给的内容落盘)。渲染器只承接 human/generated/rendered。
        if (kind == "machine") continue;

        std::string why;
        if (!safeRelPath(path, why)) {
            res.diags.usageAt(path, where + ".path 非法: " + why,
                              "生成物路径必须是仓库内相对路径(如 service/crypto/plugin.toml)");
            anyError = true;
            continue;
        }
        if (!seenPaths.insert(path).second) {
            res.diags.usageAt(path, where + ": 同一请求里输出路径重复: `" + path + "`",
                              "计划应由 brickie-core 去重");
            anyError = true;
            continue;
        }
        if (tpl.empty()) {
            res.diags.usageAt(path, where + " 缺 `template`",
                              "模板 id 相对 templates_root, 如 native/ability/c/plugin.toml.tmpl;"
                              " machine 文件(kind=\"machine\")由 brickie-core 产出, 不走渲染");
            anyError = true;
            continue;
        }
        if (!safeRelPath(tpl, why)) {
            res.diags.usageAt(tpl, where + ".template 非法: " + why,
                              "模板 id 必须是 templates_root 下的相对路径");
            anyError = true;
            continue;
        }

        Vars vars;
        std::string varsErr;
        if (!collectVars(item.find("vars"), vars, varsErr)) {
            res.diags.usageAt(path, where + ".vars 非法: " + varsErr, "");
            anyError = true;
            continue;
        }

        std::string mode = "0644";
        if (const J *m = item.find("mode"); m != nullptr && !m->isNull()) {
            if (!parseMode(m->asStr(), mode)) {
                res.diags.usageAt(path,
                                  where + ".mode 非法: `" + m->asStr() + "`(只接受 3–4 位八进制)",
                                  "缺省 0644; 载荷/落盘按该模式位");
                anyError = true;
                continue;
            }
        }

        std::string tplText;
        std::string readErr;
        const std::string tplPath = joinPath(templatesRoot, tpl);
        if (!readFile(tplPath, tplText, readErr)) {
            res.diags.usageAt(tpl, where + ": 模板不可读: " + tplPath,
                              "检查 templates_root 与 template id(模板缺失 = 环境错, exit 2)");
            anyError = true;
            continue;
        }

        std::string content;
        std::string renderErr;
        if (!render(tplText, vars, tpl, content, renderErr)) {
            res.diags.usageAt(tpl, where + ": 渲染失败: " + renderErr,
                              "模板用到未提供的变量就是用法错(exit 2); 变量契约见 cxx/README.md");
            anyError = true;
            continue;
        }

        File f;
        f.path = path;
        f.kind = "rendered";
        f.content = std::move(content);
        f.mode = mode;
        files.push_back(std::move(f));
    }

    // 一条错都不留才出 files —— 半套生成物比不生成更糟(§8.4 的既有口径)。
    if (anyError || res.diags.hasError()) return res;
    res.files = std::move(files);
    return res;
}

}  // namespace brickie

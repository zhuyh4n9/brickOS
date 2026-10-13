#include "render.h"

#include <set>

#include "text.h"

namespace brickie {
namespace {

// vars values must be substitutable text. The contract uses only strings, but stringify
// unambiguous JSON scalars here instead of flagging them as usage errors; null ⇒ empty string.
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
        err = "`vars` must be a JSON object (`{key: string}`)";
        return false;
    }
    for (const auto &kv : vars->o) {
        std::string text;
        if (!scalarToText(kv.second, text)) {
            err = "`vars." + kv.first + "` is not a string/number/bool/null";
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
        res.diags.usage("`render` requires `templates_root` (absolute path to the template root)",
                        "see contract §6: args = {templates_root, artifacts:[{path,template,vars}]}");
        return res;
    }

    const J *artifacts = args.find("artifacts");
    if (artifacts == nullptr || !artifacts->isArr()) {
        res.diags.usage("`render` requires a `args.artifacts` array",
                        "each = {path, template, vars}; the plan comes from brickie-core's plan-*");
        return res;
    }

    std::vector<File> files;
    std::set<std::string> seenPaths;
    bool anyError = false;

    for (std::size_t i = 0; i < artifacts->a.size(); ++i) {
        const J &item = artifacts->a[i];
        const std::string where = "artifacts[" + std::to_string(i) + "]";
        if (!item.isObj()) {
            res.diags.usage(where + " is not a JSON object", "each = {path, template, vars}");
            anyError = true;
            continue;
        }

        const std::string path =
            item.find("path") != nullptr ? item.find("path")->asStr() : std::string();
        const std::string tpl =
            item.find("template") != nullptr ? item.find("template")->asStr() : std::string();
        const std::string kind =
            item.find("kind") != nullptr ? item.find("kind")->asStr() : std::string();

        // `kind = "machine"` content (brickie.lock / snapshots) is owned by brickie-core and does
        // not go through rendering ⇒ skip it when the plan carries one; not a usage error (L5
        // writes core's content). The renderer only handles human/generated/rendered.
        if (kind == "machine") continue;

        std::string why;
        if (!safeRelPath(path, why)) {
            res.diags.usageAt(path, where + ".path is invalid: " + why,
                              "generated paths must be repo-relative (e.g. service/crypto/brick.toml)");
            anyError = true;
            continue;
        }
        if (!seenPaths.insert(path).second) {
            res.diags.usageAt(path, where + ": duplicate output path in the same request: `" + path + "`",
                              "the plan should be deduplicated by brickie-core");
            anyError = true;
            continue;
        }
        if (tpl.empty()) {
            res.diags.usageAt(path, where + " is missing `template`",
                              "the template id is relative to templates_root, e.g. native/ability/c/brick.toml.tmpl;"
                              " machine files (kind=\"machine\") are produced by brickie-core and are not rendered");
            anyError = true;
            continue;
        }
        if (!safeRelPath(tpl, why)) {
            res.diags.usageAt(tpl, where + ".template is invalid: " + why,
                              "the template id must be a relative path under templates_root");
            anyError = true;
            continue;
        }

        Vars vars;
        std::string varsErr;
        if (!collectVars(item.find("vars"), vars, varsErr)) {
            res.diags.usageAt(path, where + ".vars is invalid: " + varsErr, "");
            anyError = true;
            continue;
        }

        std::string mode = "0644";
        if (const J *m = item.find("mode"); m != nullptr && !m->isNull()) {
            if (!parseMode(m->asStr(), mode)) {
                res.diags.usageAt(path,
                                  where + ".mode is invalid: `" + m->asStr() + "` (only 3–4 octal digits)",
                                  "defaults to 0644; payload/write uses these permission bits");
                anyError = true;
                continue;
            }
        }

        std::string tplText;
        std::string readErr;
        const std::string tplPath = joinPath(templatesRoot, tpl);
        if (!readFile(tplPath, tplText, readErr)) {
            res.diags.usageAt(tpl, where + ": template not readable: " + tplPath,
                              "check templates_root and the template id (missing template = environment error, exit 2)");
            anyError = true;
            continue;
        }

        std::string content;
        std::string renderErr;
        if (!render(tplText, vars, tpl, content, renderErr)) {
            res.diags.usageAt(tpl, where + ": rendering failed: " + renderErr,
                              "a template using an unprovided variable is a usage error (exit 2); see cxx/README.md for the variable contract");
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

    // files are emitted only when not a single error remains -- a half-generated set is worse
    // than none.
    if (anyError || res.diags.hasError()) return res;
    res.files = std::move(files);
    return res;
}

}  // namespace brickie

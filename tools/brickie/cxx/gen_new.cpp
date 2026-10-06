#include "gen_new.h"

#include <set>

#include "rules.h"
#include "text.h"

namespace brickie {
namespace {

const char *const kDefaultSummary = "TODO: 一句话描述本插件的能力面(见 4-03 manifest 语义)";
const char *const kDefaultLicense = "WTFPL";
const char *const kVersion = "0.1.0.0";
const char *const kCoreRange = ">=1.0.0";
const char *const kApiRev = "1";

// 已存在的路径 → 首行(用于判"这个路径上的是不是我们的生成物")
std::map<std::string, std::string> existingIndex(const J &context) {
    std::map<std::string, std::string> out;
    const J *arr = context.find("existing");
    if (!arr || !arr->isArr()) return out;
    for (const auto &e : arr->a) {
        if (!e.isObj()) continue;
        const J *p = e.find("path");
        if (!p || !p->isStr()) continue;
        const J *h = e.find("first_line");
        out[p->s] = (h && h->isStr()) ? h->s : std::string();
    }
    return out;
}

bool looksGenerated(const std::string &firstLine) {
    return firstLine.find(kGeneratedMarker) != std::string::npos;
}

bool loadTemplate(const std::string &root, const std::string &rel, const Vars &vars,
                  const std::string &what, std::string &content, std::string &err) {
    const std::string path = joinPath(root, rel);
    std::string tpl;
    if (!readFile(path, tpl, err)) {
        err = "模板缺失(" + what + "): " + path + " —— " + err;
        return false;
    }
    std::string rerr;
    if (!render(tpl, vars, rel, content, rerr)) {
        err = rerr;
        return false;
    }
    return true;
}

}  // namespace

NewResult runNew(const J &args, const J &context, const std::string &templatesRoot, bool planOnly) {
    NewResult res;

    const std::string pluginType = args.find("plugin_type") ? args.find("plugin_type")->asStr() : "";
    const std::string name = args.find("name") ? args.find("name")->asStr() : "";
    const std::string apiType = args.find("api_type") ? args.find("api_type")->asStr() : "";
    const std::string lang = args.find("lang") ? args.find("lang")->asStr() : "c";
    const J *subkindJ = args.find("subkind");
    const bool force = args.find("force") ? args.find("force")->asBool() : false;
    const bool hasExplicitSubkind = subkindJ && subkindJ->isStr() && !subkindJ->s.empty();
    const std::string explicitSubkind = hasExplicitSubkind ? subkindJ->s : std::string();

    // ---- 1. 用法层校验(无码 ⇒ 退出码 2)----
    if (!isPluginType(pluginType)) {
        res.diags.usage("未知插件类别 `" + pluginType + "`",
                        "合法值: " + listOf(kPluginTypes) + "(§8.1)");
        return res;
    }
    if (!isApiType(apiType)) {
        res.diags.usage("未知 api_type `" + apiType + "`",
                        "合法值: " + listOf(kApiTypes) + "(§3.1)");
        return res;
    }
    if (!validPluginName(name)) {
        res.diags.usage("插件名 `" + name + "` 不符名字契约",
                        "契约: ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$ (§8.3)");
        return res;
    }
    if (lang != "c") {
        res.diags.usage("v0.1 只交付 `c` 模板, 收到 `" + lang + "`",
                        "`rust` 排 v2.0(`1-03`); `cxx` 待 BRV-Q8 拍板");
        return res;
    }

    // ---- 2. api_type 模板交付状态(§8.4: v0.1 只交付 native)----
    // BRV-TAX-0014 的严重度是 info, 但命令**无法完成** ⇒ 显式置退出码 2。
    // 这条口径记在 tools/brickie/README.md 与 checklist(§5.3 发现的缺口)。
    if (apiType != "native") {
        res.diags.coded("BRV-TAX-0014", Severity::Info, name, "",
                        "`" + apiType + "` 模板在 v0.1 未交付(目录已预留)", "v0.x 交付");
        res.diags.setExitOverride(2);
        return res;
    }

    // ---- 3. subkind(§8.4)----
    std::string subkind;
    if (pluginType == "ability") {
        std::string derived;
        const bool derivable = deriveSubkind(name, derived);
        if (derivable) {
            if (hasExplicitSubkind && explicitSubkind != derived) {
                res.diags.coded("BRV-TAX-0015", Severity::Error, name, "",
                                "subkind 推导与显式声明冲突: 由名字推导 = `" + derived +
                                    "`, 显式 = `" + explicitSubkind + "`",
                                "去掉 --subkind, 或改用与 namespace 相符的名字");
                return res;
            }
            subkind = derived;
        } else {
            if (!hasExplicitSubkind) {
                res.diags.usage("ability 插件 `" + name + "` 必须显式 --subkind",
                                "合法值: " + listOf(kSubkinds) +
                                    "; 或改用可推导的名字(service/ sched/ framework/ io/ fs/) (§8.4)");
                return res;
            }
            if (!isSubkind(explicitSubkind)) {
                res.diags.usage("未知 subkind `" + explicitSubkind + "`",
                                "合法值: " + listOf(kSubkinds));
                return res;
            }
            subkind = explicitSubkind;
        }
    } else if (hasExplicitSubkind) {
        res.diags.usage("`--subkind` 只对 ability 插件有意义, `" + pluginType + "` 不接受",
                        "§8.1: subkind 仅 ability 细分");
        return res;
    }

    // ---- 4. 推荐形态 lint(§8.3: 只对新增插件给 warning, 不阻断)----
    if (!isRecommendedForm(name)) {
        res.diags.coded("BRV-TAX-0013", Severity::Warning, name, "",
                        "插件名不符推荐形态 `<namespace>/<short>`",
                        "推荐 namespace: " + listOf(kRecommendedNamespaces) + "(§8.3)");
    }

    // ---- 5. 占位符取值 ----
    const std::string ns = nameNamespace(name);
    const std::string shortName = nameShort(name);
    const std::string phase = phaseFor(pluginType, subkind);
    const PluginPaths paths = layoutOf(name);

    Vars vars;
    vars["PLUGIN_NAME"] = name;
    vars["NS"] = ns;
    vars["SHORT"] = shortName;
    vars["PLUGIN_TYPE"] = pluginType;
    vars["API_TYPE"] = apiType;
    vars["SUBKIND"] = subkind;
    vars["LANG"] = lang;
    vars["PHASE"] = phase;
    vars["VERSION"] = kVersion;
    vars["COMPAT_GEN"] = "0";
    vars["MAJOR"] = "1";
    vars["MINOR"] = "0";
    vars["REVISE"] = "0";
    vars["SUMMARY"] = kDefaultSummary;
    vars["LICENSE"] = kDefaultLicense;
    vars["CORE_RANGE"] = kCoreRange;
    vars["API_REV"] = kApiRev;
    vars["SYMPREFIX"] = symbolPrefix(shortName);
    vars["GUARD"] = includeGuard(ns, shortName);
    vars["DESC_TAG"] = descriptorTag(shortName);
    vars["DESC_SYMBOL"] = descriptorSymbol(shortName);
    vars["EXPORT_UNIT"] = shortName;

    // ---- 6. 生成计划 + 渲染(模板缺失 = 环境错 ⇒ 退出码 2)----
    struct Spec {
        std::string rel;
        std::string path;
        std::string kind;
    };
    const std::string skel = apiType + "/" + pluginType + "/" + lang + "/";
    const std::string desc = "descriptor/" + apiType + "/" + lang + "/";
    std::vector<Spec> specs = {
        {skel + "plugin.toml.tmpl", paths.manifest, "human"},
        {skel + "src.c.tmpl", paths.source, "human"},
        {skel + "include.h.tmpl", paths.header, "human"},
        {skel + "smoke.toml.tmpl", paths.smoke, "human"},
        {skel + "README.md.tmpl", paths.readme, "human"},
        {desc + "plugin_desc.c.tmpl", paths.descriptor, "generated"},
    };
    for (const Spec &s : specs) {
        PlannedFile p;
        p.path = s.path;
        p.kind = s.kind;
        res.planned.push_back(std::move(p));
    }

    // plan 阶段: 只确认模板齐备, 让 L5 能先把"哪些路径存在"扫出来。
    if (planOnly) {
        for (const Spec &s : specs) {
            std::string probe, err;
            if (!readFile(joinPath(templatesRoot, s.rel), probe, err)) {
                res.diags.env("模板缺失: " + joinPath(templatesRoot, s.rel), err);
                return res;
            }
        }
        return res;
    }

    std::vector<Artifact> artifacts;
    for (const Spec &s : specs) {
        std::string content, err;
        if (!loadTemplate(templatesRoot, s.rel, vars, s.path, content, err)) {
            res.diags.env(err, "检查 tools/brickie/templates/ 是否完整");
            return res;
        }
        Artifact a;
        a.path = s.path;
        a.kind = s.kind;
        a.content = std::move(content);
        artifacts.push_back(std::move(a));
    }

    // ---- 7. 已存在文件的冲突判定(§8.4)----
    // 人写文件: **一律不覆盖**, --force 不适用 ⇒ BRV-GEN-0002
    // 生成物:   仅在"该路径上不是我们的生成物"时冲突; --force 可越过
    const std::map<std::string, std::string> existing = existingIndex(context);
    std::vector<Artifact> keep;
    for (const Artifact &a : artifacts) {
        auto it = existing.find(a.path);
        if (it == existing.end()) {
            keep.push_back(a);
            continue;
        }
        if (std::string(a.kind) == "human") {
            res.diags.coded("BRV-GEN-0002", Severity::Error, name, a.path,
                            "生成目标已存在且为人写文件, 不覆盖",
                            "人写文件不由工具改写; 需要新骨架请换插件名或先移走该文件(§8.4)");
            continue;
        }
        if (!looksGenerated(it->second)) {
            if (force) {
                keep.push_back(a);  // --force 只对生成物目录生效
                continue;
            }
            res.diags.coded("BRV-GEN-0002", Severity::Error, name, a.path,
                            "生成目标已存在且不是本工具的生成物",
                            "确认可覆盖后加 --force(--force 只对生成物目录生效) (§8.4)");
            continue;
        }
        keep.push_back(a);  // 已是生成物 ⇒ 正常重建
    }

    // ---- 8. 有 error 就不产出任何 artifacts ----
    if (res.diags.hasError()) return res;

    res.artifacts = std::move(keep);
    return res;
}

}  // namespace brickie

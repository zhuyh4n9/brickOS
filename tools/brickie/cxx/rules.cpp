#include "rules.h"

namespace brickie {

const char *const kPluginTypes[] = {"app", "interface", "ability", "platform", nullptr};
const char *const kApiTypes[] = {"native", "runtime_adapter", "third_party", nullptr};
const char *const kSubkinds[] = {"scheduler", "framework", "io", "fs", "service", nullptr};
const char *const kRecommendedNamespaces[] = {"app",  "iface",     "platform", "sched",
                                              "framework", "io",   "fs",       "service",
                                              nullptr};

namespace {

bool inList(const char *const *items, const std::string &v) {
    for (const char *const *p = items; *p; ++p)
        if (v == *p) return true;
    return false;
}

bool alnumLower(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool isSeparator(char c) { return c == '-' || c == '_' || c == '/'; }

}  // namespace

bool isPluginType(const std::string &v) { return inList(kPluginTypes, v); }
bool isApiType(const std::string &v) { return inList(kApiTypes, v); }
bool isSubkind(const std::string &v) { return inList(kSubkinds, v); }
bool isRecommendedNamespace(const std::string &v) { return inList(kRecommendedNamespaces, v); }

std::string listOf(const char *const *items) {
    std::string out;
    for (const char *const *p = items; *p; ++p) {
        if (!out.empty()) out += " | ";
        out += *p;
    }
    return out;
}

// ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$
bool validPluginName(const std::string &name) {
    if (name.empty()) return false;
    if (!(name[0] >= 'a' && name[0] <= 'z')) return false;
    size_t i = 1;
    while (i < name.size() && alnumLower(name[i])) ++i;
    while (i < name.size()) {
        if (!isSeparator(name[i])) return false;
        ++i;
        size_t seg = i;
        while (i < name.size() && alnumLower(name[i])) ++i;
        if (i == seg) return false;  // 分隔符后必须有 >=1 个 [a-z0-9]
    }
    return true;
}

std::string nameNamespace(const std::string &name) {
    size_t p = name.find('/');
    return p == std::string::npos ? std::string() : name.substr(0, p);
}

std::string nameShort(const std::string &name) {
    size_t p = name.rfind('/');
    return p == std::string::npos ? name : name.substr(p + 1);
}

bool isRecommendedForm(const std::string &name) {
    std::string ns = nameNamespace(name);
    return !ns.empty() && isRecommendedNamespace(ns);
}

bool deriveSubkind(const std::string &name, std::string &out) {
    const std::string ns = nameNamespace(name);
    if (ns == "service") {
        out = "service";
    } else if (ns == "sched") {
        out = "scheduler";
    } else if (ns == "framework") {
        out = "framework";
    } else if (ns == "io") {
        out = "io";
    } else if (ns == "fs") {
        out = "fs";
    } else if (ns == "app" || ns == "platform" || ns.empty() || ns == "iface") {
        return false;  // 裸名或非 ability 语义的 namespace ⇒ 必须显式 --subkind
    } else {
        return false;
    }
    return true;
}

std::string phaseFor(const std::string &pluginType, const std::string &subkind) {
    if (pluginType == "platform") return "early";
    if (pluginType == "app") return "app";
    if (pluginType == "interface") return "late";  // `1-01` §6.2: Interface 在 LATE 初始化
    if (pluginType == "ability") {
        if (subkind == "scheduler") return "early";  // `1-01` §6.2: 调度插件必须在 EARLY 完成注册
        if (subkind == "service") return "late";    // CORE = init(非 Service/Interface)
        return "core";                              // framework / io / fs
    }
    return "core";
}

std::string symbolPrefix(const std::string &shortName) {
    std::string out;
    out.reserve(shortName.size() + 1);
    for (char c : shortName) {
        if (c >= 'a' && c <= 'z')
            out.push_back(c);
        else if (c >= '0' && c <= '9')
            out.push_back(c);
        else if (c >= 'A' && c <= 'Z')
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        else
            out.push_back('_');
    }
    out.push_back('_');
    return out;
}

std::string includeGuard(const std::string &ns, const std::string &shortName) {
    std::string base = ns.empty() ? shortName : ns + "_" + shortName;
    // upperSnake 在 text.h; 这里就地实现以免 rules 依赖 text
    std::string up;
    for (char c : base) {
        if (c >= 'a' && c <= 'z')
            up.push_back(static_cast<char>(c - 'a' + 'A'));
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            up.push_back(c);
        else
            up.push_back('_');
    }
    return up + "_H";
}

std::string descriptorTag(const std::string &shortName) {
    std::string out = symbolPrefix(shortName);
    out.pop_back();  // 宏实参不要尾下划线
    return out;
}

std::string descriptorSymbol(const std::string &shortName) {
    return "_br_plugin_" + descriptorTag(shortName);
}

PluginPaths layoutOf(const std::string &name) {
    const std::string shortName = nameShort(name);
    PluginPaths p;
    p.pluginDir = name;
    p.manifest = name + "/plugin.toml";
    p.source = name + "/src/" + shortName + ".c";
    p.header = name + "/include/" + shortName + "/" + shortName + ".h";
    p.smoke = name + "/tests/smoke.toml";
    p.readme = name + "/README.md";
    p.descriptor = "build/gen/" + name + "/plugin_desc.c";
    return p;
}

}  // namespace brickie

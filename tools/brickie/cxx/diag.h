// brickie — 诊断模型(BRV-D8: 编号化 + 位置化 + 可机读)
//
// 每条诊断 = {code, severity, target, file, span, message, hint}
// 编号 BRV-<域>-NNNN; 域: MF manifest / DEP 依赖 / VER 版本 / IFACE 接口 /
// TAX 分类学 / PRIV 特权 / GEN 生成。
//
// **无码诊断**: code 为空 = 用法/环境错(退出码 2), 不是声明面诊断。
// 见 tools/brickie/README.md「诊断与退出码」小节记录的口径与已知缺口。
#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace brickie {

enum class Severity { Info, Warning, Error };

const char *severityName(Severity s);

struct Diag {
    std::string code;      // "" = 无码(用法/环境错)
    Severity severity = Severity::Error;
    std::string target;    // 插件名 / 条目 id
    std::string file;      // 仓库相对路径
    std::string span;      // 位置(可空)
    std::string message;
    std::string hint;

    J toJson() const;
};

// 诊断集合: 统计最高严重度, 并按 BRV-D9 给出退出码
class Diags {
public:
    void add(Diag d);
    void usage(std::string message, std::string hint = "");      // 无码 ⇒ 退出码 2
    void env(std::string message, std::string hint = "");        // 无码 ⇒ 退出码 2
    // 带位置的用法错(file = 仓库相对路径或模板相对路径); 仍是无码诊断 ⇒ 退出码 2。
    void usageAt(std::string file, std::string message, std::string hint = "");
    void coded(std::string code, Severity sev, std::string target, std::string file,
               std::string message, std::string hint = "");

    bool hasError() const;
    bool hasUsage() const;
    size_t size() const { return items_.size(); }
    const std::vector<Diag> &items() const { return items_; }

    J toJson() const;

    // 命令无法完成但已给出 info/warning 级诊断时, 显式置退出码。
    // 当前唯一用点: BRV-TAX-0014(api_type 模板 v0.1 未交付) —— 严重度是 info,
    // 但"什么都没生成"必须让 shell 看见 ⇒ 退出码 2。口径见 tools/brickie/README.md。
    void setExitOverride(int code) { exitOverride_ = code; }

    // BRV-D9: 0 成功 / 1 校验红 / 2 用法或环境错
    int exitCode() const;

private:
    std::vector<Diag> items_;
    int exitOverride_ = 0;
};

}  // namespace brickie

#include "diag.h"

namespace brickie {

const char *severityName(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "error";
}

J Diag::toJson() const {
    J o = J::obj();
    o.set("code", code.empty() ? J() : J::str(code));
    o.set("severity", J::str(severityName(severity)));
    o.set("target", J::str(target));
    o.set("file", J::str(file));
    o.set("span", J::str(span));
    o.set("message", J::str(message));
    o.set("hint", J::str(hint));
    return o;
}

void Diags::add(Diag d) { items_.push_back(std::move(d)); }

void Diags::usage(std::string message, std::string hint) {
    Diag d;
    d.severity = Severity::Error;
    d.message = std::move(message);
    d.hint = std::move(hint);
    d.code.clear();
    items_.push_back(std::move(d));
}

void Diags::env(std::string message, std::string hint) {
    Diag d;
    d.severity = Severity::Error;
    d.message = std::move(message);
    d.hint = std::move(hint);
    d.code.clear();
    items_.push_back(std::move(d));
}

void Diags::coded(std::string code, Severity sev, std::string target, std::string file,
                  std::string message, std::string hint) {
    Diag d;
    d.code = std::move(code);
    d.severity = sev;
    d.target = std::move(target);
    d.file = std::move(file);
    d.message = std::move(message);
    d.hint = std::move(hint);
    items_.push_back(std::move(d));
}

bool Diags::hasError() const {
    for (const auto &d : items_)
        if (d.severity == Severity::Error) return true;
    return false;
}

// 用法/环境错 = 无码诊断(见 README「诊断与退出码」)
bool Diags::hasUsage() const {
    for (const auto &d : items_)
        if (d.code.empty()) return true;
    return false;
}

J Diags::toJson() const {
    J arr = J::arr();
    for (const auto &d : items_) arr.push(d.toJson());
    return arr;
}

int Diags::exitCode() const {
    if (exitOverride_ != 0) return exitOverride_;
    if (hasUsage()) return 2;   // BRV-D9: 用法或环境错
    if (hasError()) return 1;   // BRV-D9: 校验红
    return 0;
}

}  // namespace brickie

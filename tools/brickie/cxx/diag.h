// brickie — diagnostic model (BRV-D8: numbered + positioned + machine-readable)
//
// Each diagnostic = {code, severity, target, file, span, message, hint}
// Codes are BRV-<domain>-NNNN; domains: MF manifest / DEP dependencies / VER version /
// IFACE interface / TAX taxonomy / PRIV privilege / GEN generation.
//
// **Codeless diagnostic**: empty code = usage/environment error (exit code 2), not a
// declaration-surface diagnostic. See the "diagnostics and exit codes" section of
// tools/brickie/README.md for the recorded rules and known gaps.
#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace brickie {

enum class Severity { Info, Warning, Error };

const char *severityName(Severity s);

struct Diag {
    std::string code;      // "" = codeless (usage/environment error)
    Severity severity = Severity::Error;
    std::string target;    // plugin name / entry id
    std::string file;      // repo-relative path
    std::string span;      // position (may be empty)
    std::string message;
    std::string hint;

    J toJson() const;
};

// Diagnostic set: tracks the highest severity and derives the exit code per BRV-D9
class Diags {
public:
    void add(Diag d);
    void usage(std::string message, std::string hint = "");      // codeless ⇒ exit code 2
    void env(std::string message, std::string hint = "");        // codeless ⇒ exit code 2
    // Positioned usage error (file = repo-relative or template-relative path); still codeless ⇒ exit 2.
    void usageAt(std::string file, std::string message, std::string hint = "");
    void coded(std::string code, Severity sev, std::string target, std::string file,
               std::string message, std::string hint = "");

    bool hasError() const;
    bool hasUsage() const;
    size_t size() const { return items_.size(); }
    const std::vector<Diag> &items() const { return items_; }

    J toJson() const;

    // Sets the exit code explicitly when the command cannot complete but only info/warning
    // diagnostics exist. Current sole use: BRV-TAX-0014 (api_type templates v0.1 undelivered) —
    // severity is info, but "nothing was generated" must be visible to the shell ⇒ exit code 2.
    // See tools/brickie/README.md.
    void setExitOverride(int code) { exitOverride_ = code; }

    // BRV-D9: 0 success / 1 validation failure / 2 usage or environment error
    int exitCode() const;

private:
    std::vector<Diag> items_;
    int exitOverride_ = 0;
};

}  // namespace brickie

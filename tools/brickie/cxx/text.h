// brickie — text layer: file reading + template rendering + path/mode helpers
//
// There is only one placeholder syntax: `{{key}}` (key = [A-Za-z0-9_]), no logic, no branches,
// no conditionals. Splitting by api_type / plugin_type / lang already externalizes the
// differences into the template files themselves (BRV-D3 / §9.2
// templates/<api_type>/<plugin_type>/<lang>/), so conditionals are unnecessary.
//
// Key names are case-sensitive and all valid: the brickie-core variable contract uses
// lowercase (`{{name}}`); legacy templates using uppercase keys still render (compatibility,
// not a second source of truth).
//
// Missing key ⇒ **failure** (never a silent empty string): templates are our own assets, a
// missing key means the caller omitted a variable — a usage error (exit 2) that must fail fast.
#pragma once

#include <map>
#include <string>

namespace brickie {

using Vars = std::map<std::string, std::string>;

// Generated-file marker: identifies whether a path holds our generated output
// (§8.4 --force semantics depend on it; contract §6 requires the first line to carry
// `brickie:generated`)
extern const char *kGeneratedMarker;

std::string joinPath(const std::string &a, const std::string &b);
std::string dirName(const std::string &path);
bool readFile(const std::string &path, std::string &out, std::string &err);
bool fileExists(const std::string &path);

// Render; missing key / syntax error ⇒ false + err
bool render(const std::string &tpl, const Vars &vars, const std::string &tplName,
            std::string &out, std::string &err);

std::string upperSnake(const std::string &s);

// File mode: accepts only 3–4 octal digits ("644"/"0644"); on success canonical is "0644".
bool parseMode(const std::string &text, std::string &canonical);

// Relative repo-path safety check: rejects absolute paths / backslashes / empty segments
// (`//`, trailing `/`) / `.` and `..` segments. All generated files stay inside the repo;
// this is a usage check on inputs at the **process boundary**, not a domain decision.
bool safeRelPath(const std::string &path, std::string &err);

}  // namespace brickie

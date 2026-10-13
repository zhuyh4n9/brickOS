// brickie — text layer: file reading + template rendering + path/mode helpers
// One placeholder syntax only: `{{key}}` (key = [A-Za-z0-9_]), no logic or conditionals --
// per-api_type/plugin_type/lang differences live in the template tree layout instead. Key
// names are case-sensitive. Missing key ⇒ failure, never a silent empty string (usage error).
#pragma once

#include <map>
#include <string>

namespace brickie {

using Vars = std::map<std::string, std::string>;

// Generated-file marker; --force semantics and the contract's required first line depend on it.
extern const char *kGeneratedMarker;

std::string joinPath(const std::string &a, const std::string &b);
std::string dirName(const std::string &path);
bool readFile(const std::string &path, std::string &out, std::string &err);
bool fileExists(const std::string &path);

// false + err on a missing key or syntax error
bool render(const std::string &tpl, const Vars &vars, const std::string &tplName,
            std::string &out, std::string &err);

std::string upperSnake(const std::string &s);

// Accepts only 3–4 octal digits ("644"/"0644"); canonical is "0644" on success.
bool parseMode(const std::string &text, std::string &canonical);

// Repo-relative path check: rejects absolute paths / backslashes / empty segments (`//`,
// trailing `/`) / `.` and `..`. Input-boundary usage check, not a domain decision.
bool safeRelPath(const std::string &path, std::string &err);

}  // namespace brickie

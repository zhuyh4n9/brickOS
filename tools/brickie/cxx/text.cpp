#include "text.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace brickie {

const char *kGeneratedMarker = "brickie:generated";

std::string joinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string dirName(const std::string &path) {
    size_t p = path.rfind('/');
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

bool readFile(const std::string &path, std::string &out, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "无法读取文件: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool fileExists(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

std::string upperSnake(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'a' && c <= 'z')
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out;
}

bool render(const std::string &tpl, const Vars &vars, const std::string &tplName,
            std::string &out, std::string &err) {
    out.clear();
    out.reserve(tpl.size() + 256);
    size_t i = 0;
    while (i < tpl.size()) {
        size_t open = tpl.find("{{", i);
        if (open == std::string::npos) {
            out.append(tpl, i, std::string::npos);
            return true;
        }
        out.append(tpl, i, open - i);
        size_t close = tpl.find("}}", open + 2);
        if (close == std::string::npos) {
            err = tplName + ": 占位符未闭合(offset " + std::to_string(open) + ")";
            return false;
        }
        std::string key = tpl.substr(open + 2, close - open - 2);
        // 两边空白容忍: {{ KEY }}
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key.empty()) {
            err = tplName + ": 空占位符(offset " + std::to_string(open) + ")";
            return false;
        }
        for (char c : key) {
            bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                err = tplName + ": 占位符键非法 `" + key + "`(只允许 [A-Z0-9_])";
                return false;
            }
        }
        auto it = vars.find(key);
        if (it == vars.end()) {
            err = tplName + ": 模板用到未提供的占位符 `{{" + key + "}}`";
            return false;
        }
        out += it->second;
        i = close + 2;
    }
    return true;
}

}  // namespace brickie

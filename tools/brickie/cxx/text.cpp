#include "text.h"

#include <cctype>
#include <cstdio>
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
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                err = tplName + ": 占位符键非法 `" + key + "`(只允许 [A-Za-z0-9_])";
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

bool parseMode(const std::string &text, std::string &canonical) {
    // 只接受八进制文本: 3 位("755")或 4 位("0644")。空串 / 非八进制 / 过长一律拒。
    if (text.size() < 3 || text.size() > 4) return false;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '7') return false;
        value = value * 8 + static_cast<unsigned>(c - '0');
    }
    if (value > 07777u) return false;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04o", value);
    canonical = buf;
    return true;
}

bool safeRelPath(const std::string &path, std::string &err) {
    if (path.empty()) {
        err = "路径为空";
        return false;
    }
    if (path[0] == '/') {
        err = "不允许绝对路径(`" + path + "`)";
        return false;
    }
    if (path.find('\\') != std::string::npos) {
        err = "不允许反斜杠路径分隔符(`" + path + "`)";
        return false;
    }
    std::size_t i = 0;
    while (true) {
        std::size_t end = path.find('/', i);
        std::string seg =
            path.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (seg.empty()) {
            err = "不允许空路径段(`//` 或结尾 `/`): `" + path + "`";
            return false;
        }
        if (seg == "." || seg == "..") {
            err = "不允许 `.`/`..` 路径段: `" + path + "`";
            return false;
        }
        if (end == std::string::npos) break;
        i = end + 1;
    }
    return true;
}

}  // namespace brickie

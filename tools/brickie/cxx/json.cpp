#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace brickie {

J J::obj() {
    J v;
    v.t = JType::Obj;
    return v;
}
J J::arr() {
    J v;
    v.t = JType::Arr;
    return v;
}
J J::str(std::string v) {
    J j;
    j.t = JType::Str;
    j.s = std::move(v);
    return j;
}
J J::num(double v) {
    J j;
    j.t = JType::Num;
    j.n = v;
    return j;
}
J J::boolean(bool v) {
    J j;
    j.t = JType::Bool;
    j.b = v;
    return j;
}

const J *J::find(const std::string &k) const {
    if (t != JType::Obj) return nullptr;
    for (const auto &kv : o)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

J &J::set(const std::string &k, J v) {
    if (t != JType::Obj) {
        t = JType::Obj;
        o.clear();
    }
    for (auto &kv : o) {
        if (kv.first == k) {
            kv.second = std::move(v);
            return kv.second;
        }
    }
    o.emplace_back(k, std::move(v));
    return o.back().second;
}

void J::push(J v) {
    if (t != JType::Arr) {
        t = JType::Arr;
        a.clear();
    }
    a.push_back(std::move(v));
}

std::string J::asStr(const std::string &dflt) const {
    return t == JType::Str ? s : dflt;
}
bool J::asBool(bool dflt) const {
    return t == JType::Bool ? b : dflt;
}
double J::asNum(double dflt) const {
    return t == JType::Num ? n : dflt;
}

namespace {

void appendUtf8(std::string &out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(const std::string &src) : s_(src) {}

    bool run(J &out) {
        skipWs();
        if (!value(out)) return false;
        skipWs();
        if (i_ != s_.size()) return fail("末尾有多余内容");
        return true;
    }

    std::string err() const { return err_; }

private:
    const std::string &s_;
    size_t i_ = 0;
    std::string err_;

    bool fail(const std::string &m) {
        if (err_.empty()) err_ = m + " @offset " + std::to_string(i_);
        return false;
    }
    void skipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++i_;
            else
                break;
        }
    }
    bool lit(const char *t) {
        size_t n = std::strlen(t);
        if (s_.compare(i_, n, t) != 0) return fail(std::string("期望 `") + t + "`");
        i_ += n;
        return true;
    }
    bool hex4(unsigned &out) {
        if (i_ + 4 > s_.size()) return fail("\\u 转义不完整");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_ + k];
            unsigned d;
            if (c >= '0' && c <= '9')
                d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = static_cast<unsigned>(c - 'A' + 10);
            else
                return fail("\\u 转义含非十六进制字符");
            out = (out << 4) | d;
        }
        i_ += 4;
        return true;
    }

    bool value(J &out) {
        if (i_ >= s_.size()) return fail("值缺失");
        char c = s_[i_];
        switch (c) {
            case 'n':
                if (!lit("null")) return false;
                out = J();
                return true;
            case 't':
                if (!lit("true")) return false;
                out = J::boolean(true);
                return true;
            case 'f':
                if (!lit("false")) return false;
                out = J::boolean(false);
                return true;
            case '"': {
                std::string v;
                if (!string(v)) return false;
                out = J::str(std::move(v));
                return true;
            }
            case '[':
                return array(out);
            case '{':
                return object(out);
            default:
                return number(out);
        }
    }

    bool string(std::string &out) {
        if (s_[i_] != '"') return fail("期望字符串");
        ++i_;
        out.clear();
        while (true) {
            if (i_ >= s_.size()) return fail("字符串未闭合");
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) return fail("转义未闭合");
                char e = s_[i_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 代理对高半区 ⇒ 必须跟低半区
                            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                                i_ += 2;
                                unsigned lo = 0;
                                if (!hex4(lo)) return false;
                                if (lo < 0xDC00 || lo > 0xDFFF) return fail("代理对低半区非法");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                return fail("代理对缺少低半区");
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        return fail("未知转义");
                }
                continue;
            }
            if (c < 0x20) return fail("字符串含未转义控制字符");
            out.push_back(static_cast<char>(c));
            ++i_;
        }
    }

    bool number(J &out) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool digits = false;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
            ++i_;
            digits = true;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
                ++i_;
                digits = true;
            }
        }
        if (digits && i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (!digits) return fail("非法数字");
        out = J::num(std::strtod(s_.substr(start, i_ - start).c_str(), nullptr));
        return true;
    }

    bool array(J &out) {
        ++i_;  // '['
        out = J::arr();
        skipWs();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            J v;
            skipWs();
            if (!value(v)) return false;
            out.push(std::move(v));
            skipWs();
            if (i_ >= s_.size()) return fail("数组未闭合");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("数组元素后期望 `,` 或 `]`");
        }
    }

    bool object(J &out) {
        ++i_;  // '{'
        out = J::obj();
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            skipWs();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"') return fail("对象键必须是字符串");
            if (!string(key)) return false;
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("键后期望 `:`");
            ++i_;
            skipWs();
            J v;
            if (!value(v)) return false;
            out.set(key, std::move(v));
            skipWs();
            if (i_ >= s_.size()) return fail("对象未闭合");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("对象成员后期望 `,` 或 `}`");
        }
    }
};

void escapeTo(std::string &out, const std::string &v) {
    out.push_back('"');
    for (unsigned char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void numTo(std::string &out, double n) {
    long long ll = static_cast<long long>(n);
    if (static_cast<double>(ll) == n) {
        out += std::to_string(ll);
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", n);
    out += buf;
}

void dumpTo(std::string &out, const J &v, int indent, int depth) {
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent) * (depth + 1), ' ') : std::string();
    const std::string padEnd = pretty ? std::string(static_cast<size_t>(indent) * depth, ' ') : std::string();
    switch (v.t) {
        case JType::Null: out += "null"; break;
        case JType::Bool: out += v.b ? "true" : "false"; break;
        case JType::Num: numTo(out, v.n); break;
        case JType::Str: escapeTo(out, v.s); break;
        case JType::Arr: {
            if (v.a.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t k = 0; k < v.a.size(); ++k) {
                if (k) out.push_back(',');
                if (pretty) {
                    out.push_back('\n');
                    out += pad;
                }
                dumpTo(out, v.a[k], indent, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                out += padEnd;
            }
            out.push_back(']');
            break;
        }
        case JType::Obj: {
            if (v.o.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t k = 0; k < v.o.size(); ++k) {
                if (k) out.push_back(',');
                if (pretty) {
                    out.push_back('\n');
                    out += pad;
                }
                escapeTo(out, v.o[k].first);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                dumpTo(out, v.o[k].second, indent, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                out += padEnd;
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

bool parseJson(const std::string &text, J &out, std::string &err) {
    Parser p(text);
    if (!p.run(out)) {
        err = p.err();
        return false;
    }
    err.clear();
    return true;
}

std::string dumpJson(const J &v, int indent) {
    std::string out;
    dumpTo(out, v, indent, 0);
    return out;
}

}  // namespace brickie

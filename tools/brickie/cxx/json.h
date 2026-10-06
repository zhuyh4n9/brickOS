// brickie — 极小 JSON 实现(自研, 无第三方依赖)
//
// 为什么自研: v0.1 依赖纪律(Design/docs/2-toolchain/brickie/brickie-v0.1.md §9.3)
// 规定 C++ 侧「标准库 + 轻量模板引擎(或自研文本渲染)」, 且 C9 要求
// 「稳定的进程间 JSON 契约」——契约的编解码本身不能引入外部件。
//
// 支持: null / bool / number / string(含 \u 转义与代理对)/ array / object。
// object 保留插入顺序, 以便输出 diff 稳定(V-2 的"逐字节一致"依赖确定性序列化)。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace brickie {

enum class JType { Null, Bool, Num, Str, Arr, Obj };

class J {
public:
    JType t = JType::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    static J obj();
    static J arr();
    static J str(std::string v);
    static J num(double v);
    static J boolean(bool v);

    bool isObj() const { return t == JType::Obj; }
    bool isArr() const { return t == JType::Arr; }
    bool isStr() const { return t == JType::Str; }
    bool isNull() const { return t == JType::Null; }

    // object 取值; 不存在或类型不符返回 nullptr
    const J *find(const std::string &k) const;
    J &set(const std::string &k, J v);
    void push(J v);

    std::string asStr(const std::string &dflt = "") const;
    bool asBool(bool dflt = false) const;
    double asNum(double dflt = 0.0) const;
};

// 解析失败时返回 false, err 填人类可读原因(含偏移)
bool parseJson(const std::string &text, J &out, std::string &err);

// indent < 0 ⇒ 紧凑输出(单行)。字符串按 JSON 规则转义, 非 ASCII 原样输出(合法 UTF-8)。
std::string dumpJson(const J &v, int indent = 2);

}  // namespace brickie

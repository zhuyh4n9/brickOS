// brickie — minimal JSON implementation (own code, no third-party deps)
//
// Why own code: v0.1 dependency discipline (Design/docs/2-toolchain/brickie/brickie-v0.1.md §9.3)
// allows only "std library + lightweight template engine (or own text rendering)" for C++, and
// C9 requires a "stable inter-process JSON contract" — its codec cannot add external components.
//
// Supports: null / bool / number / string (with \u escapes and surrogate pairs) / array / object.
// object keeps insertion order for stable output diffs (V-2 "byte-identical" needs it).
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

    // object lookup; returns nullptr if missing or wrong type
    const J *find(const std::string &k) const;
    J &set(const std::string &k, J v);
    void push(J v);

    std::string asStr(const std::string &dflt = "") const;
    bool asBool(bool dflt = false) const;
    double asNum(double dflt = 0.0) const;
};

// on parse failure returns false and fills err with a human-readable reason (incl. offset)
bool parseJson(const std::string &text, J &out, std::string &err);

// indent < 0 ⇒ compact output (single line). Strings escaped per JSON rules; non-ASCII emitted as-is (valid UTF-8).
std::string dumpJson(const J &v, int indent = 2);

}  // namespace brickie

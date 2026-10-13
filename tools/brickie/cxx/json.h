// brickie — minimal JSON implementation (own code, no third-party deps).
// Own code: allowed C++ deps are the std library plus lightweight template/text rendering only,
// and the stable inter-process JSON contract's codec must add no external components.
// Supports null / bool / number / string (\u escapes, surrogate pairs) / array / object; object
// keeps insertion order for byte-identical output diffs.
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

    // nullptr if missing or wrong type
    const J *find(const std::string &k) const;
    J &set(const std::string &k, J v);
    void push(J v);

    std::string asStr(const std::string &dflt = "") const;
    bool asBool(bool dflt = false) const;
    double asNum(double dflt = 0.0) const;
};

// false on parse failure; err gets a human-readable reason (incl. offset)
bool parseJson(const std::string &text, J &out, std::string &err);

// indent < 0 ⇒ compact output (single line). Non-ASCII emitted as-is (valid UTF-8).
std::string dumpJson(const J &v, int indent = 2);

}  // namespace brickie

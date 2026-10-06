// brickie — 插件声明面规则(名字契约 / 分类学推导 / 相位 / 路径布局)
//
// 出处: Design/docs/2-toolchain/brickie/brickie-v0.1.md
//   §8.3 名字契约与物理目录   §8.4 骨架生成(subkind 推导 / api_type 模板)
//   §8.1 plugin.toml schema   `1-01` §6.2 生命周期相位 / §6.3 插件分类
//
// 这里是**业务规则**, 因此必须在原生侧(C9: Python 粘合层禁止实现业务规则)。
#pragma once

#include <string>
#include <vector>

namespace brickie {

// ---- 插件类别(§8.1)与 API 类别(§3.1)----

extern const char *const kPluginTypes[];   // app | interface | ability | platform
extern const char *const kApiTypes[];      // native | runtime_adapter | third_party
extern const char *const kSubkinds[];      // scheduler | framework | io | fs | service
extern const char *const kRecommendedNamespaces[];  // §8.3, 用于 TAX-0013

bool isPluginType(const std::string &v);
bool isApiType(const std::string &v);
bool isSubkind(const std::string &v);
bool isRecommendedNamespace(const std::string &v);
std::string listOf(const char *const *items);  // "a | b | c"

// ---- 名字契约(§8.3)----
// ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$
bool validPluginName(const std::string &name);

// 名字的 namespace 部分(`service/crypto` → `service`); 裸名 → 空串
std::string nameNamespace(const std::string &name);
// 名字的 short 部分(`service/crypto` → `crypto`)
std::string nameShort(const std::string &name);
// 是否是"推荐形态" `<namespace>/<short>`(§8.3)
bool isRecommendedForm(const std::string &name);

// ---- ability 的 subkind 推导(§8.4)----
// 由 namespace 推导: service→service / sched→scheduler / framework→framework
//                    io→io / fs→fs
bool deriveSubkind(const std::string &name, std::string &out);

// ---- 相位(§8.1 的 phase; 语义见 `1-01` §6.2)----
// platform→early; ability.scheduler→early; ability.{framework,io,fs}→core;
// ability.service→late; interface→late; app→app
std::string phaseFor(const std::string &pluginType, const std::string &subkind);

// ---- 符号与标识符派生(4-02 §1 符号纪律: `br_*` 为 core 独占)----
// 插件自有符号前缀: short 的非字母数字换成 `_`, 并补尾 `_`(crypto → crypto_)
std::string symbolPrefix(const std::string &shortName);
// 头文件守卫: <NAMESPACE>_<SHORT>_H
std::string includeGuard(const std::string &ns, const std::string &shortName);
// BR_PLUGIN(...) 宏的第一个实参(C 标识符): 非字母数字 → `_`
std::string descriptorTag(const std::string &shortName);
// static 描述符实例名(3-01 §13.3 形态)
std::string descriptorSymbol(const std::string &shortName);

// ---- 路径布局(§8.3 顶层即 namespace; §8.4 生成物落仓库级 build/gen/)----

struct PluginPaths {
    std::string pluginDir;    // service/crypto
    std::string manifest;     // service/crypto/plugin.toml
    std::string source;       // service/crypto/src/crypto.c
    std::string header;       // service/crypto/include/crypto/crypto.h
    std::string smoke;        // service/crypto/tests/smoke.toml
    std::string readme;       // service/crypto/README.md
    std::string descriptor;   // build/gen/service/crypto/plugin_desc.c
};

PluginPaths layoutOf(const std::string &name);

}  // namespace brickie

// brickie — 文本层: 文件读取 + 模板渲染
//
// 模板占位符语法只有一种: `{{KEY}}`(KEY = [A-Z0-9_]+), 无逻辑、无分支。
// 「按 plugin_type / api_type 分目录」已把差异外化到模板文件本身
// (BRV-D3 / §9.2 的 templates/<api_type>/<plugin_type>/<lang>/), 因此不需要条件语法。
//
// 缺键 ⇒ **失败**(不静默成空串): 模板是我们自己的资产, 缺键是编程错误, 必须早爆。
#pragma once

#include <map>
#include <string>

namespace brickie {

using Vars = std::map<std::string, std::string>;

// 生成物头部标记: 判"这个路径上的是不是我们的生成物"(§8.4 的 --force 语义依赖它)
extern const char *kGeneratedMarker;

std::string joinPath(const std::string &a, const std::string &b);
std::string dirName(const std::string &path);
bool readFile(const std::string &path, std::string &out, std::string &err);
bool fileExists(const std::string &path);

// 渲染; 缺键 / 语法错 ⇒ false + err
bool render(const std::string &tpl, const Vars &vars, const std::string &tplName,
            std::string &out, std::string &err);

std::string upperSnake(const std::string &s);

}  // namespace brickie

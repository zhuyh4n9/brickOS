// brickie — 文本层: 文件读取 + 模板渲染 + 路径/模式工具
//
// 模板占位符语法只有一种: `{{key}}`(key = [A-Za-z0-9_]), 无逻辑、无分支、无条件语法。
// 「按 api_type / plugin_type / lang 分目录」已把差异外化到模板文件本身
// (BRV-D3 / §9.2 的 templates/<api_type>/<plugin_type>/<lang>/), 因此不需要条件语法。
//
// 键名大小写敏感、均合法: 与 brickie-core 的新变量契约取小写(`{{name}}`),
// 迁移期的旧模板若仍用大写键也能渲染(兼容, 不是第二套真值)。
//
// 缺键 ⇒ **失败**(不静默成空串): 模板是我们自己的资产, 缺键是调用方少了变量,
// 属用法错(exit 2), 必须早爆。
#pragma once

#include <map>
#include <string>

namespace brickie {

using Vars = std::map<std::string, std::string>;

// 生成物头部标记: 识别"这个路径上的是不是我们的生成物"(§8.4 的 --force 语义依赖它;
// 契约 §6 要求生成物首行带 `brickie:generated`)
extern const char *kGeneratedMarker;

std::string joinPath(const std::string &a, const std::string &b);
std::string dirName(const std::string &path);
bool readFile(const std::string &path, std::string &out, std::string &err);
bool fileExists(const std::string &path);

// 渲染; 缺键 / 语法错 ⇒ false + err
bool render(const std::string &tpl, const Vars &vars, const std::string &tplName,
            std::string &out, std::string &err);

std::string upperSnake(const std::string &s);

// 文件模式: 只接受 3–4 位八进制文本("644"/"0644"); 成功后 canonical 形如 "0644"。
bool parseMode(const std::string &text, std::string &canonical);

// 相对的仓库路径安全检查: 拒绝绝对路径 / 反斜杠 / 空段(`//`、结尾 `/`)/ `.`、`..` 段。
// 生成物一律落仓库内, 这是**进程边界**上对入参的用法校验, 不是领域判定。
bool safeRelPath(const std::string &path, std::string &err);

}  // namespace brickie

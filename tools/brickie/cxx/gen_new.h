// brickie — `new`: 插件骨架生成(能力 1 / §8.4)
//
// 契约(与 Python L5 前端的 JSON over stdio 边界):
//   请求 args:
//     {"plugin_type": "ability", "name": "service/crypto", "api_type": "native",
//      "lang": "c", "subkind": null|"service", "force": false}
//   请求 context:
//     {"existing": [{"path": "service/crypto/plugin.toml", "first_line": "..."}]}
//   请求 templates_root: 模板根目录(绝对路径; 由 Python 侧解析后传入)
//
// 产物: 6 件(5 件人写 + 1 件生成物)。**只有零 error 时才产出 artifacts** ——
// 部分写盘会让"项目处于半生成状态", 比不生成更糟。
#pragma once

#include <string>

#include "diag.h"
#include "json.h"

namespace brickie {

struct Artifact {
    std::string path;  // 仓库相对路径
    std::string kind;  // "human" | "generated"
    std::string content;
};

// 生成计划里的一条(不带内容)。L5 前端据此扫盘, 再把存在的路径回传给 `new` ——
// 这样"哪些路径属于本次生成"这条规则留在原生侧, 而文件 IO 留在 Python 侧(§9.1)。
struct PlannedFile {
    std::string path;
    std::string kind;
};

struct NewResult {
    Diags diags;
    std::vector<PlannedFile> planned;
    std::vector<Artifact> artifacts;
};

// planOnly = true: 只校验入参 + 探模板是否齐备, 返回 planned(不渲染内容、不判冲突)。
NewResult runNew(const J &args, const J &context, const std::string &templatesRoot,
                 bool planOnly = false);

}  // namespace brickie

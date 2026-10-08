// brickie — `render`: 模板渲染(契约 §2 信封 / §6 命令面)
//
// **本命令不做任何判定**: 名字契约 / subkind 推导 / 相位 / 路径布局 / 版本分段
// 一律由 `brickie-core`(Rust, L0/L1)算好后, 通过 `args.artifacts[].vars` 传进来;
// 这里只做 "读模板 → 替换 {{key}} → 回 files[]"。
//
// 请求(信封见 contract §2; 本命令的 args 见 §6):
//   {
//     "protocol": 1, "command": "render",
//     "args": {
//       "templates_root": "/abs/path/tools/brickie/templates",
//       "artifacts": [
//         {"path": "service/crypto/plugin.toml",
//          "template": "native/ability/c/plugin.toml.tmpl",
//          "vars": {"name": "service/crypto", "short": "crypto", …}}
//       ]
//     }
//   }
//
// 响应 files[](契约 §4):
//   [{"path": "service/crypto/plugin.toml", "kind": "rendered",
//     "content": "…", "mode": "0644"}]
//
// 规则:
//   * `kind` 恒为 `"rendered"`(§4: 生成物只由 brickie-gen 产出, L5 只落盘);
//   * `mode` 缺省 `"0644"`, 可显式给 3–4 位八进制; 落盘由 L5 负责;
//   * `path` / `template` 必须是安全的仓库相对路径(拒绝绝对路径与 `..`);
//   * **没有条件语法**: 该分化的地方用 `templates/<api_type>/<plugin_type>/<lang>/`
//     目录分化; 缺变量 ⇒ 用法错(exit 2, 无码诊断, 见 cxx/README.md);
//   * 任一 artifact 出错 ⇒ **不产出任何 files**(避免半生成状态), 诊断全量回报。
#pragma once

#include <string>
#include <vector>

#include "diag.h"
#include "json.h"

namespace brickie {

struct File {
    std::string path;
    std::string kind = "rendered";
    std::string content;
    std::string mode = "0644";
};

struct RenderResult {
    Diags diags;
    std::vector<File> files;
};

// `templatesRoot` 可来自 `args.templates_root`, 也可来自顶层 `templates_root`
// (main.cpp 统一解析后传入; 两者都空 ⇒ 用法错)。
RenderResult runRender(const J &args, const std::string &templatesRoot);

}  // namespace brickie

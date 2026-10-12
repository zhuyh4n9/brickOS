// brickie — `render`: template rendering (contract §2 envelope / §6 command surface)
//
// **This command makes no decisions**: the name contract / subkind derivation / phase / path
// layout / version segmentation are all computed by `brickie-core` (Rust, L0/L1) and passed in
// via `args.artifacts[].vars`; here we only "read template → substitute {{key}} → return files[]".
//
// Request (envelope: contract §2; this command's args: §6):
//   {
//     "protocol": 1, "command": "render",
//     "args": {
//       "templates_root": "/abs/path/tools/brickie/templates",
//       "artifacts": [
//         {"path": "service/crypto/brick.toml",
//          "template": "native/ability/c/brick.toml.tmpl",
//          "vars": {"name": "service/crypto", "short": "crypto", …}}
//       ]
//     }
//   }
//
// Response files[] (contract §4):
//   [{"path": "service/crypto/brick.toml", "kind": "rendered",
//     "content": "…", "mode": "0644"}]
//
// Rules:
//   * `kind` is always `"rendered"` (§4: generated files come only from brickie-gen; L5 only writes);
//   * `mode` defaults to `"0644"`, may be given explicitly as 3–4 octal digits; L5 does the writing;
//   * `path` / `template` must be safe repo-relative paths (absolute paths and `..` are rejected);
//   * **no conditionals**: where differentiation is needed, use
//     `templates/<api_type>/<plugin_type>/<lang>/` directories; a missing variable ⇒ usage error
//     (exit 2, codeless diagnostic, see cxx/README.md);
//   * any artifact failing ⇒ **no files produced** (avoid partial generation), all diags reported.
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

// `templatesRoot` may come from `args.templates_root` or from top-level `templates_root`
// (main.cpp resolves both and passes it in; both empty ⇒ usage error).
RenderResult runRender(const J &args, const std::string &templatesRoot);

}  // namespace brickie

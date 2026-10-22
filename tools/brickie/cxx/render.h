// brickie — `render`: template rendering
// Makes no decisions: name contract / subkind / phase / path layout / version segmentation all
// come from brickie-core via args.artifacts[].vars; here we only read template → substitute
// {{key}} → return files[].
// Request: {"protocol":1,"command":"render","args":{"templates_root":"<abs path>","artifacts":
//           [{"path":"provider/crypto/brick.toml","template":"native/ability/c/brick.toml.tmpl",
//             "vars":{...}}]}}
// Response files[]: [{"path":..., "kind":"rendered", "content":..., "mode":"0644"}]
// Rules: kind is always "rendered" (generated files come only from brickie-gen; L5 only
// writes); mode defaults to "0644", else explicit 3–4 octal digits; path/template must be
// safe repo-relative paths (absolute paths and `..` rejected); no conditionals --
// differentiation lives in templates/<api_type>/<plugin_type>/<lang>/, and a missing variable
// is a usage error (exit 2, codeless diagnostic); any artifact failing ⇒ no files produced
// (no partial generation), all diags reported.
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

// `templatesRoot` comes from `args.templates_root` or top-level `templates_root` (main.cpp
// resolves both); both empty ⇒ usage error.
RenderResult runRender(const J &args, const std::string &templatesRoot);

}  // namespace brickie

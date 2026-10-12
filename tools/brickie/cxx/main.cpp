// brickie-gen — C++ generator (L2), **template rendering only**, makes no decisions
//
// Process boundary (contract §1/§2/§6): input and output are **JSON over stdio**.
//   stdin  = request object;  stdout = response object (always valid JSON)
// Exit codes: this process uses **only** 0 (protocol handling succeeded; diagnostics and the business
// exit code live in the response) and 2 (its own failure: input is not JSON, etc.). The business exit
// code travels in the response field exit_code and is turned into a process exit code by the Python L5
// front-end — so "tool failure" and "validation failure" are never conflated.
//
// Command surface (contract §6): only `render` (template rendering).
//   * Name contract / subkind derivation / phase / path layout / version segmentation — all such
//     **decisions** have moved to `brickie-core` (Rust, L0/L1); this tool only handles
//     "plan + variables → file contents".
//   * The old `plan-new` / `new` are gone (rules and plans belong to Rust's `plan-new` / `plan-init`).
//
// `--selftest`: does not read stdin; runs the built-in self-check (JSON round-trip / rendering / paths
// and modes / payload determinism) for CI's "glue-layer purity check" (§9.1: the generator-side
// counterpart of brickie-core --selftest).
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "diag.h"
#include "json.h"
#include "render.h"
#include "text.h"

namespace {

using brickie::J;

// Response envelope (contract §2). status is only ever ok / internal_error.
J makeResponse(int exitCode, J diagnostics, J files, J data) {
    J o = J::obj();
    o.set("protocol", J::num(1));
    o.set("status", J::str("ok"));
    o.set("exit_code", J::num(exitCode));
    o.set("diagnostics", std::move(diagnostics));
    o.set("files", std::move(files));
    o.set("data", std::move(data));
    return o;
}

int failInternal(const std::string &msg, const std::string &hint = "") {
    brickie::Diags diags;
    diags.usage(msg, hint);
    J o = makeResponse(2, diags.toJson(), J::arr(), J::obj());
    o.set("status", J::str("internal_error"));
    std::cout << brickie::dumpJson(o, 2) << "\n";
    std::cerr << "brickie-gen: " << msg << (hint.empty() ? "" : (" (" + hint + ")")) << "\n";
    return 2;
}

std::string readStdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- selftest

int selftest() {
    int failures = 0;
    auto check = [&](bool ok, const std::string &what) {
        std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
        if (!ok) ++failures;
    };

    // JSON round-trip (escapes, nesting, non-ASCII, surrogate pairs)
    {
        const std::string src =
            R"({"a":[1,true,null,"x\n\"\u00e9\u00fc\uD83D\uDE00"],"b":{"c":-1.5e3}})";
        J v;
        std::string err;
        const bool parsed = brickie::parseJson(src, v, err);
        check(parsed, "parseJson accepts nesting/escapes/non-ASCII: " + err);
        if (parsed) {
            const std::string round = brickie::dumpJson(v, -1);
            J v2;
            std::string err2;
            check(brickie::parseJson(round, v2, err2), "dumpJson output can be re-parsed");
            check(brickie::dumpJson(v2, -1) == round, "JSON round-trip is byte-stable");
        }
    }
    // Invalid JSON must be rejected
    {
        J v;
        std::string err;
        check(!brickie::parseJson("{\"a\":}", v, err), "invalid JSON is rejected");
    }

    // Rendering: the new variable contract uses lowercase keys; missing keys must fail (never a silent
    // empty string); no branch syntax
    {
        brickie::Vars vars;
        vars["name"] = "service/crypto";
        vars["short"] = "crypto";
        vars["Version"] = "0.1.0.0";  // case-sensitive: not the same key as version
        std::string out, err;
        check(brickie::render("x{{name}}y", vars, "t", out, err) && out == "xservice/cryptoy",
              "render: basic substitution (lowercase key)");
        check(brickie::render("{{ name }}", vars, "t", out, err) && out == "service/crypto",
              "render: whitespace around the placeholder is tolerated");
        check(!brickie::render("{{missing}}", vars, "t", out, err), "render: missing key fails");
        check(!brickie::render("{{#if x}}a{{/if}}", vars, "t", out, err),
              "render: no conditionals/branches (invalid key rejected)");
        check(!brickie::render("{{short} }", vars, "t", out, err), "render: unclosed placeholder is rejected");
        check(brickie::render("{{Version}}", vars, "t", out, err) && out == "0.1.0.0",
              "render: key names are case-sensitive");
    }

    // Path safety: generated files must stay inside the repo
    {
        std::string err;
        check(brickie::safeRelPath("service/crypto/brick.toml", err),
              "safeRelPath: normal relative path");
        check(!brickie::safeRelPath("/etc/passwd", err), "safeRelPath: rejects absolute paths");
        check(!brickie::safeRelPath("../escape", err), "safeRelPath: rejects ..");
        check(!brickie::safeRelPath("a/../b", err), "safeRelPath: rejects a mid-path ..");
        check(!brickie::safeRelPath("a//b", err), "safeRelPath: rejects empty path segments");
        check(!brickie::safeRelPath("a/", err), "safeRelPath: rejects a trailing /");
        check(!brickie::safeRelPath("", err), "safeRelPath: rejects the empty string");
    }

    // Mode bits (used for both payload entry modes and on-disk modes)
    {
        std::string mode;
        check(brickie::parseMode("0644", mode) && mode == "0644", "parseMode: 0644");
        check(brickie::parseMode("755", mode) && mode == "0755", "parseMode: 755 -> 0755 normalization");
        check(!brickie::parseMode("", mode), "parseMode: rejects the empty string");
        check(!brickie::parseMode("999", mode), "parseMode: rejects non-octal");
        check(!brickie::parseMode("06442", mode), "parseMode: rejects overlong input");
        check(!brickie::parseMode("0x1", mode), "parseMode: rejects hexadecimal");
    }

    // Generated-file marker (contract §6: first line carries `brickie:generated`)
    {
        const std::string marker = brickie::kGeneratedMarker;
        check(marker == "brickie:generated", "generated marker = brickie:generated");
        brickie::Vars vars;
        vars["name"] = "service/crypto";
        std::string out, err;
        check(brickie::render("/* brickie:generated */ {{name}}", vars, "t", out, err) &&
                  out.find("brickie:generated") != std::string::npos,
              "generated first line may carry the marker");
    }

    // Payload checks: rendering the same input twice is byte-identical (a prerequisite for a
    // byte-reproducible embedded payload); the response envelope (including files[].mode) round-trips stably.
    {
        brickie::Vars vars;
        vars["name"] = "service/crypto";
        vars["short"] = "crypto";
        std::string a, b, err;
        const bool ok1 = brickie::render("{{name}}:{{short}}\n", vars, "t", a, err);
        const bool ok2 = brickie::render("{{name}}:{{short}}\n", vars, "t", b, err);
        check(ok1 && ok2 && a == b, "payload: rendering is byte-reproducible (two calls agree)");

        J files = J::arr();
        J f = J::obj();
        f.set("path", J::str("service/crypto/brick.toml"));
        f.set("kind", J::str("rendered"));
        f.set("content", J::str(a));
        f.set("mode", J::str("0644"));
        files.push(f);
        const std::string wire = brickie::dumpJson(makeResponse(0, J::arr(), files, J::obj()), 2);
        J back;
        std::string perr;
        const bool parsedWire = brickie::parseJson(wire, back, perr);
        check(parsedWire, "payload: the response envelope is valid JSON");
        std::string wire2;
        J back2;
        std::string perr2;
        if (parsedWire) wire2 = brickie::dumpJson(back, -1);
        check(parsedWire && brickie::parseJson(wire2, back2, perr2) &&
                  brickie::dumpJson(back2, -1) == wire2,
              "payload: the response envelope round-trips stably");
        const J *backFiles = back.find("files");
        check(backFiles != nullptr && backFiles->isArr() && backFiles->a.size() == 1 &&
                  backFiles->a[0].find("mode") != nullptr &&
                  backFiles->a[0].find("mode")->asStr() == "0644" &&
                  backFiles->a[0].find("kind")->asStr() == "rendered",
              "payload: files[] carries kind/mode with a stable shape");
    }

    // File ownership (contract §4): `kind=machine` content is owned by brickie-core, so render skips it
    // (no template read, not an error) — plan-init's `brickie.lock` carries this kind.
    {
        J args = J::obj();
        J arts = J::arr();
        J machine = J::obj();
        machine.set("path", J::str("brickie.lock"));
        machine.set("kind", J::str("machine"));
        machine.set("template", J::str(""));
        arts.push(machine);
        args.set("artifacts", arts);
        const brickie::RenderResult rr = brickie::runRender(args, "/nonexistent-templates");
        check(rr.diags.exitCode() == 0 && rr.files.empty(),
              "render: kind=machine is skipped (no template read, not a usage error)");
    }

    if (failures == 0) {
        std::cout << "brickie-gen selftest: all passed\n";
        return 0;
    }
    std::cout << "brickie-gen selftest: " << failures << " failed\n";
    return 1;
}

// ---------------------------------------------------------------- protocol dispatch

// Handshake failure response: BRV-PROTO-0001, exit_code 2, status stays "ok"
// (protocol handling itself succeeded; the business exit code is in the response; contract §2).
int protoMismatch(const std::string &detail) {
    brickie::Diags diags;
    diags.coded("BRV-PROTO-0001", brickie::Severity::Error, "", "",
                "subprocess JSON protocol mismatch: " + detail,
                "this tool uses protocol = 1; requests must carry {\"protocol\": 1}");
    std::cout << brickie::dumpJson(makeResponse(2, diags.toJson(), J::arr(), J::obj()), 2) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") return selftest();
        if (a == "--version") {
            std::cout << "brickie-gen 0.1.0 (protocol 1)\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::cout << "usage: brickie-gen [--selftest|--version]\n"
                         "  with no arguments, reads a JSON request from stdin and writes a JSON response to stdout.\n"
                         "  command: render -- render templates from the plan given by brickie-core (contract §6).\n";
            return 0;
        }
        std::cerr << "brickie-gen: unknown argument `" << a << "`\n";
        return 2;
    }

    const std::string input = readStdin();
    if (input.empty()) return failInternal("stdin is empty: a JSON request object is required");

    J req;
    std::string err;
    if (!brickie::parseJson(input, req, err)) return failInternal("request is not valid JSON", err);
    if (!req.isObj()) return failInternal("request must be a JSON object");

    // ---- protocol handshake (contract §2) ----
    const J *proto = req.find("protocol");
    if (proto == nullptr) return protoMismatch("request is missing the `protocol` field");
    if (!(proto->t == brickie::JType::Num && proto->n == 1.0)) {
        return protoMismatch("received protocol = " + brickie::dumpJson(*proto, -1));
    }

    const J *cmdJ = req.find("command");
    const std::string command = (cmdJ != nullptr && cmdJ->isStr()) ? cmdJ->s : std::string();
    if (command.empty()) {
        brickie::Diags diags;
        diags.usage("request is missing the `command` field (string)",
                    "the v0.1 generator handles only `render` (contract §6)");
        std::cout << brickie::dumpJson(makeResponse(2, diags.toJson(), J::arr(), J::obj()), 2)
                  << "\n";
        return 0;
    }

    J args = J::obj();
    if (const J *a = req.find("args"); a != nullptr && a->isObj()) args = *a;

    if (command != "render") {
        brickie::Diags diags;
        diags.usage("brickie-gen does not recognize command `" + command + "`",
                    "the v0.1 generator handles only `render` (template rendering); decisions and plans belong to brickie-core");
        std::cout << brickie::dumpJson(makeResponse(2, diags.toJson(), J::arr(), J::obj()), 2)
                  << "\n";
        return 0;
    }

    // templates_root: contract §6 puts it in args; top-level field accepted too (old front-end shape).
    std::string templatesRoot;
    if (const J *t = args.find("templates_root"); t != nullptr && t->isStr()) {
        templatesRoot = t->s;
    } else if (const J *t = req.find("templates_root"); t != nullptr && t->isStr()) {
        templatesRoot = t->s;
    }

    const brickie::RenderResult r = brickie::runRender(args, templatesRoot);

    J files = J::arr();
    for (const auto &f : r.files) {
        J o = J::obj();
        o.set("path", J::str(f.path));
        o.set("kind", J::str(f.kind));
        o.set("content", J::str(f.content));
        o.set("mode", J::str(f.mode));
        files.push(std::move(o));
    }

    std::cout << brickie::dumpJson(makeResponse(r.diags.exitCode(), r.diags.toJson(),
                                               std::move(files), J::obj()),
                                  2)
              << "\n";
    return 0;
}

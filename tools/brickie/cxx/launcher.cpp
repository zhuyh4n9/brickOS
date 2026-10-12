// brickie — **single-file self-contained** host entry ELF (zero third-party dependencies).
//
// Shape (user decision, see ADR-0004 §7): everything below is embedded as data into this executable
// (freeze.py generates `brickie_payload.cpp`), unpacked at runtime into a temp dir, then interpreted
// by the system `python3`, which also runs the embedded native tools:
//   * the Python package (python/brickie/**)
//   * skeleton templates (templates/**)
//   * **native tools** (bin/**, currently `brickie-gen`, later `brickie-core` etc.)
//
//   ┌─ brickie (this ELF) ───────────────────────────────────────────────────┐
//   │  1. locate own path (/proc/self/exe, fallback to argv[0])              │
//   │  2. unpack payload -> $TMPDIR/brickie-XXXXXX/{python,templates,bin}    │
//   │  3. set env: PYTHONPATH / BRICKIE_TOOL_ROOT / BRICKIE_GEN /            │
//   │             BRICKIE_CORE (all point at the embedded files)             │
//   │  4. fork+exec `python3 -m brickie <args>`; wait for child tree         │
//   │  5. remove temp dir, forward the child's exit code                     │
//   └────────────────────────────────────────────────────────────────────────┘
//
// Why not PyInstaller/Nuitka: the v0.1 design discipline is "zero third-party Python dependencies"
// (BR-D3 revised wording); the bootstrap seed must be reproducible from source. A hand-written launcher
// plus an uncompressed tar payload needs only g++ and the Python standard library, and the payload is
// byte-for-byte reproducible.
//
// **Single-file self-contained**: native tools are embedded under `bin/` in the payload (with the
// executable bit) — `brickie-gen` (L2) is always embedded, and `brickie-core` (L0/L1 Rust) too once it
// exists. The launcher points `BRICKIE_GEN` / `BRICKIE_CORE` at the unpacked copies ⇒ copying **just the
// single `brickie` file** anywhere works, with no sibling `brickie-gen`, no source tree and no
// `PYTHONPATH` needed. (Transitional fallback: if the payload has no `brickie-gen` but a standalone
// `brickie-gen` sits next to us, that is accepted too.)
//
// Exit code: the child's exit code is forwarded as-is; the launcher's own environment errors (payload
// missing/undecodable, cannot create temp dir, cannot start python3) return **2** per BRV-D9's
// "environment error" class.
//
// Platform: POSIX (/proc/self/exe on Linux; other platforms fall back to argv[0]/PATH lookup).
// The Windows `_spawnvp` variant comes later (see ADR-0004 §7.4).

#define _GNU_SOURCE 1

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
extern const unsigned char brickie_payload[];
extern const unsigned long brickie_payload_size;
}

namespace {

constexpr int kEnvError = 2;      // BRV-D9: usage/environment error
constexpr unsigned long kBlock = 512;

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "brickie: %s\n", message.c_str());
    std::exit(kEnvError);
}

// ------------------------------------------------------------------ own path

std::string path_lookup(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return {};
    std::string dirs(path);
    std::size_t pos = 0;
    while (pos <= dirs.size()) {
        std::size_t end = dirs.find(':', pos);
        std::string dir = dirs.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

std::string self_path(const char* argv0) {
#if defined(__linux__)
    {
        char buffer[4096];
        ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len > 0) {
            buffer[len] = '\0';
            return buffer;
        }
    }
#endif
    if (argv0 != nullptr && std::strchr(argv0, '/') != nullptr) {
        char resolved[4096];
        if (::realpath(argv0, resolved) != nullptr) return resolved;
        return argv0;
    }
    if (argv0 != nullptr) {
        std::string found = path_lookup(argv0);
        if (!found.empty()) return found;
    }
    return {};
}

std::string dir_of(const std::string& path) {
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// ------------------------------------------------------------------ dirs/files

void make_dirs(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    std::size_t i = 0;
    if (path[0] == '/') {
        current = "/";
        i = 1;
    }
    while (i < path.size()) {
        std::size_t slash = path.find('/', i);
        std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (!part.empty()) {
            if (current.empty() || current.back() != '/') current += '/';
            current += part;
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                die("cannot create directory " + current + ": " + std::strerror(errno));
            }
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
}

void remove_tree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
        while (struct dirent* entry = ::readdir(dir)) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string child = path + "/" + name;
            struct stat st {};
            if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                remove_tree(child);
            } else {
                ::unlink(child.c_str());
            }
        }
        ::closedir(dir);
    }
    ::rmdir(path.c_str());
}

// ---------------------------------------------------------------------- tar

unsigned long long parse_octal(const unsigned char* field, std::size_t width) {
    unsigned long long value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        unsigned char c = field[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') die("invalid tar header field in embedded payload");
        value = value * 8 + static_cast<unsigned long long>(c - '0');
    }
    return value;
}

bool block_is_zero(const unsigned char* block) {
    for (unsigned long i = 0; i < kBlock; ++i) {
        if (block[i] != 0) return false;
    }
    return true;
}

void write_file(const std::string& path, const unsigned char* data, unsigned long long size,
                unsigned mode) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (fd < 0) die("cannot write " + path + ": " + std::strerror(errno));
    // umask may strip the executable bit ⇒ set the mode explicitly (embedded native tools must be execable).
    if (::fchmod(fd, mode & 0777) != 0) {
        ::close(fd);
        die("cannot set permissions on " + path + ": " + std::strerror(errno));
    }
    unsigned long long written = 0;
    while (written < size) {
        ssize_t n = ::write(fd, data + written, static_cast<std::size_t>(size - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            die("failed to write " + path + ": " + std::strerror(errno));
        }
        written += static_cast<unsigned long long>(n);
    }
    ::close(fd);
}

void extract_payload(const std::string& dest) {
    if (brickie_payload_size == 0) die("embedded payload is empty (build error)");

    const unsigned char* cursor = brickie_payload;
    const unsigned char* end = brickie_payload + brickie_payload_size;
    unsigned long files = 0;

    while (cursor + kBlock <= end && !block_is_zero(cursor)) {
        const unsigned char* header = cursor;

        char name[101];
        std::memcpy(name, header, 100);
        name[100] = '\0';

        unsigned long long size = parse_octal(header + 124, 12);
        unsigned mode = static_cast<unsigned>(parse_octal(header + 100, 8));
        unsigned char type = header[156];

        if (type == '0' || type == '\0') {
            std::string rel(name);
            if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
                die("embedded payload contains an illegal path: " + rel);
            }
            std::string full = dest + "/" + rel;
            std::string parent = dir_of(full);
            make_dirs(parent);

            const unsigned char* data = header + kBlock;
            if (data + size > end) die("embedded payload is truncated");
            write_file(full, data, size, mode);
            ++files;
        } else if (type == '5') {
            make_dirs(dest + "/" + std::string(name));
        } else {
            die("embedded payload contains an unsupported entry type");
        }

        unsigned long long padded = ((size + kBlock - 1) / kBlock) * kBlock;
        cursor = header + kBlock + padded;
    }

    if (files == 0) die("embedded payload contains no files (build error)");
}

// -------------------------------------------------------------------- environment

void set_env_if_absent(const char* key, const std::string& value) {
    if (::getenv(key) == nullptr) ::setenv(key, value.c_str(), 1);
}

void set_env(const char* key, const std::string& value) {
    ::setenv(key, value.c_str(), 1);
}

void prepend_pythonpath(const std::string& dir) {
    const char* old = ::getenv("PYTHONPATH");
    std::string value = dir;
    if (old != nullptr && *old != '\0') value += std::string(":") + old;
    set_env("PYTHONPATH", value);
}

bool is_executable(const std::string& path) {
    return ::access(path.c_str(), X_OK) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string self = self_path(argc > 0 ? argv[0] : nullptr);
    const std::string self_dir = self.empty() ? std::string() : dir_of(self);

    // Temp dir (TMPDIR preferred; per-run, removed before exit)
    const char* tmpdir = std::getenv("TMPDIR");
    std::string pattern = (tmpdir != nullptr && *tmpdir != '\0') ? tmpdir : "/tmp";
    if (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
    pattern += "/brickie-XXXXXX";

    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* made = ::mkdtemp(buffer.data());
    if (made == nullptr) die(std::string("cannot create temp directory: ") + std::strerror(errno));
    const std::string work(made);

    extract_payload(work);

    // Make the front-end use the **embedded** code and templates
    prepend_pythonpath(work + "/python");
    set_env_if_absent("BRICKIE_TOOL_ROOT", work);
    set_env_if_absent("PYTHONDONTWRITEBYTECODE", "1");
    set_env_if_absent("PYTHONNOUSERSITE", "1");

    // Native tools are already **embedded in the payload** (bin/brickie-gen): prefer that one ⇒ brickie is
    // a single self-contained file needing no sibling brickie-gen and no source tree. An explicit
    // BRICKIE_GEN is never overridden.
    if (::getenv("BRICKIE_GEN") == nullptr) {
        std::string embedded = work + "/bin/brickie-gen";
        if (is_executable(embedded)) {
            set_env("BRICKIE_GEN", embedded);
        } else if (!self_dir.empty()) {
            // Fallback (transitional shape): accept a standalone brickie-gen in the same directory.
            std::string sibling = self_dir + "/brickie-gen";
            if (is_executable(sibling)) set_env("BRICKIE_GEN", sibling);
        }
    }

    // L0/L1 Rust core: point at it if the payload has it (absence is a permitted degradation — core may
    // not be built yet, see CORE_EMBED in cxx/Makefile); like BRICKIE_GEN, never override an explicit value.
    if (::getenv("BRICKIE_CORE") == nullptr) {
        std::string embedded = work + "/bin/brickie-core";
        if (is_executable(embedded)) set_env("BRICKIE_CORE", embedded);
    }

    const char* python_env = std::getenv("BRICKIE_PYTHON");
    const std::string python = (python_env != nullptr && *python_env != '\0') ? python_env : "python3";

    std::vector<char*> child_argv;
    child_argv.push_back(const_cast<char*>(python.c_str()));
    child_argv.push_back(const_cast<char*>("-m"));
    child_argv.push_back(const_cast<char*>("brickie"));
    for (int i = 1; i < argc; ++i) child_argv.push_back(argv[i]);
    child_argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) die(std::string("fork failed: ") + std::strerror(errno));
    if (pid == 0) {
        ::execvp(python.c_str(), child_argv.data());
        std::fprintf(stderr, "brickie: cannot execute %s: %s\n", python.c_str(), std::strerror(errno));
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) die(std::string("waitpid failed: ") + std::strerror(errno));
    }

    int exit_code = 1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);   // same convention as the shell
    }

    remove_tree(work);
    return exit_code;
}

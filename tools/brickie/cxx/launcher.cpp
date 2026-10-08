// brickie — **单文件自包含**的宿主入口 ELF(零第三方依赖)。
//
// 形态(用户裁定, 见 ADR-0004 §7): 把
//   * Python 包(python/brickie/**)
//   * 骨架模板(templates/**)
//   * **原生工具**(bin/**, 现在 `brickie-gen`, 将来 `brickie-core` 等)
// 全部作为数据嵌进本可执行文件(由 freeze.py 生成 `brickie_payload.cpp`),
// 运行时解包到临时目录, 再用系统 `python3` 解释, 并执行嵌入的原生工具。
//
//   ┌─ brickie(本 ELF) ─────────────────────────────────────────────────┐
//   │  1. 定位自身路径(/proc/self/exe, 退化到 argv[0])                   │
//   │  2. 解包嵌入载荷 → $TMPDIR/brickie-XXXXXX/{python,templates,bin}   │
//   │  3. 布环境: PYTHONPATH / BRICKIE_TOOL_ROOT / BRICKIE_GEN /         │
//   │             BRICKIE_CORE(均指嵌入件)                                │
//   │  4. fork+exec `python3 -m brickie <原样参数>`; 等子进程(含其孙进程)  │
//   │  5. 清理临时目录, 转发子进程退出码                                 │
//   └────────────────────────────────────────────────────────────────────┘
//
// 为什么不是 PyInstaller/Nuitka: 设计侧 v0.1 的纪律是"Python 零第三方依赖"
// (BR-D3 修订口径); 自举种子要能随源码复现。自写启动器 + 未压缩 tar 载荷
// 只需要 g++ 与 Python 标准库, 且载荷逐字节可复现。
//
// **单文件自包含**: 原生工具嵌在载荷的 `bin/` 下(带可执行位) —— 现在必嵌
// `brickie-gen`(L2), 若 `brickie-core`(L0/L1 Rust)已就位则一并嵌。启动器把
// `BRICKIE_GEN` / `BRICKIE_CORE` 指到解包出来的那几份 ⇒ **只拷 `brickie` 一个文件**
// 到任何地方都能跑, 不需要同目录的 `brickie-gen`、不需要源码树、不需要 `PYTHONPATH`。
// (过渡兜底: 若载荷里没有 `brickie-gen` 而自身同目录有独立的 `brickie-gen`, 也认。)
//
// 退出码: 子进程的退出码原样转发; 启动器自身的环境错(找不到/解不开载荷、
// 建不了临时目录、起不了 python3)按 BRV-D9 的"环境错"档返回 **2**。
//
// 平台: POSIX(Linux 用 /proc/self/exe; 其他平台退化到 argv[0]/PATH 查找)。
// Windows 的 `_spawnvp` 变体属后续(见 ADR-0004 §7.4)。

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

constexpr int kEnvError = 2;      // BRV-D9: 用法/环境错
constexpr unsigned long kBlock = 512;

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "brickie: %s\n", message.c_str());
    std::exit(kEnvError);
}

// ------------------------------------------------------------------ 自身路径

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

// ------------------------------------------------------------------ 目录/文件

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
                die("无法创建目录 " + current + ": " + std::strerror(errno));
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
        if (c < '0' || c > '7') die("嵌入载荷的 tar 头字段非法");
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
    if (fd < 0) die("无法写入 " + path + ": " + std::strerror(errno));
    // umask 可能削掉可执行位 ⇒ 显式设一次模式(嵌入的原生工具要能 exec)。
    if (::fchmod(fd, mode & 0777) != 0) {
        ::close(fd);
        die("无法设置 " + path + " 的权限: " + std::strerror(errno));
    }
    unsigned long long written = 0;
    while (written < size) {
        ssize_t n = ::write(fd, data + written, static_cast<std::size_t>(size - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            die("写入 " + path + " 失败: " + std::strerror(errno));
        }
        written += static_cast<unsigned long long>(n);
    }
    ::close(fd);
}

void extract_payload(const std::string& dest) {
    if (brickie_payload_size == 0) die("嵌入载荷为空(构建异常)");

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
                die("嵌入载荷含非法路径: " + rel);
            }
            std::string full = dest + "/" + rel;
            std::string parent = dir_of(full);
            make_dirs(parent);

            const unsigned char* data = header + kBlock;
            if (data + size > end) die("嵌入载荷被截断");
            write_file(full, data, size, mode);
            ++files;
        } else if (type == '5') {
            make_dirs(dest + "/" + std::string(name));
        } else {
            die("嵌入载荷含不支持的条目类型");
        }

        unsigned long long padded = ((size + kBlock - 1) / kBlock) * kBlock;
        cursor = header + kBlock + padded;
    }

    if (files == 0) die("嵌入载荷没有任何文件(构建异常)");
}

// -------------------------------------------------------------------- 环境

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

    // 临时目录(TMPDIR 优先; 每次运行独立, 退出前清掉)
    const char* tmpdir = std::getenv("TMPDIR");
    std::string pattern = (tmpdir != nullptr && *tmpdir != '\0') ? tmpdir : "/tmp";
    if (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
    pattern += "/brickie-XXXXXX";

    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* made = ::mkdtemp(buffer.data());
    if (made == nullptr) die(std::string("无法创建临时目录: ") + std::strerror(errno));
    const std::string work(made);

    extract_payload(work);

    // 让前端用**嵌入的**代码与模板
    prepend_pythonpath(work + "/python");
    set_env_if_absent("BRICKIE_TOOL_ROOT", work);
    set_env_if_absent("PYTHONDONTWRITEBYTECODE", "1");
    set_env_if_absent("PYTHONNOUSERSITE", "1");

    // 原生工具已**嵌在载荷里**(bin/brickie-gen): 优先用它 ⇒ brickie 单文件自包含,
    // 不需要同目录的 brickie-gen、也不需要源码树。显式设了 BRICKIE_GEN 则不覆盖。
    if (::getenv("BRICKIE_GEN") == nullptr) {
        std::string embedded = work + "/bin/brickie-gen";
        if (is_executable(embedded)) {
            set_env("BRICKIE_GEN", embedded);
        } else if (!self_dir.empty()) {
            // 兜底(过渡形态): 同目录若还有独立的 brickie-gen, 也认。
            std::string sibling = self_dir + "/brickie-gen";
            if (is_executable(sibling)) set_env("BRICKIE_GEN", sibling);
        }
    }

    // L0/L1 的 Rust 核心: 载荷里有就指过去(缺失是允许的降级 —— 构建期 core 可能
    // 还没产出, 见 cxx/Makefile 的 CORE_EMBED); 与 BRICKIE_GEN 一样不覆盖显式值。
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
    if (pid < 0) die(std::string("fork 失败: ") + std::strerror(errno));
    if (pid == 0) {
        ::execvp(python.c_str(), child_argv.data());
        std::fprintf(stderr, "brickie: 无法执行 %s: %s\n", python.c_str(), std::strerror(errno));
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) die(std::string("waitpid 失败: ") + std::strerror(errno));
    }

    int exit_code = 1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);   // 与 shell 的口径一致
    }

    remove_tree(work);
    return exit_code;
}

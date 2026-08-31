// SPDX-FileCopyrightText: © 2025 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "jit_build_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <chrono>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <process.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <tt-logger/tt-logger.hpp>

#include "tt_metal/tools/profiler/tracy_debug_zones.hpp"

namespace tt::jit_build::utils {

bool run_command(const std::string& cmd, const std::string& log_file, bool verbose) {
    TTZoneScopedD(JIT);
    TTZoneTextD(JIT, cmd.c_str(), cmd.length());
    int ret;
    static std::mutex io_mutex;

    if (verbose) {
        {
            std::lock_guard<std::mutex> lk(io_mutex);
            std::cout << "===== RUNNING SYSTEM COMMAND:\n";
            std::cout << cmd << "\n" << std::endl;
        }
        ret = system(cmd.c_str());
    } else {
        std::string redirected_cmd = cmd + " >> " + log_file + " 2>&1";
        ret = system(redirected_cmd.c_str());
    }

    return (ret == 0);
}

std::vector<std::string> tokenize_flags(const std::string& flags) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < flags.size()) {
        while (i < flags.size() && std::isspace(static_cast<unsigned char>(flags[i]))) {
            ++i;
        }
        if (i >= flags.size()) {
            break;
        }
        std::size_t start = i;
        while (i < flags.size() && !std::isspace(static_cast<unsigned char>(flags[i]))) {
            ++i;
        }
        tokens.emplace_back(flags, start, i - start);
    }
    return tokens;
}

std::vector<std::string> build_gpp_argv(
    const std::string& gpp,
    const std::string& opt_level,
    const std::string& cflags,
    const std::string& includes,
    const std::vector<std::string>& defines,
    const std::string& src,
    GppAction action,
    const std::string& out_path,
    const std::string& dep_path) {
    std::vector<std::string> args = tokenize_flags(gpp);
    args.push_back("-" + opt_level);
    auto append = [&args](const std::string& flags) {
        auto toks = tokenize_flags(flags);
        args.insert(args.end(), std::make_move_iterator(toks.begin()), std::make_move_iterator(toks.end()));
    };
    append(cflags);
    append(includes);
    // Each define is one argv element, passed verbatim (no shell) — this is what makes defines
    // carrying shell metacharacters, like -DFULL_KERNEL_NAME="<name>", survive unescaped.
    args.insert(args.end(), defines.begin(), defines.end());
    switch (action) {
        case GppAction::Compile:
            args.push_back("-c");
            args.push_back("-o");
            args.push_back(out_path);
            args.push_back(src);
            args.push_back("-MF");
            args.push_back(dep_path);
            break;
        case GppAction::Preprocess:
            // Keep line markers: the .ii is later compiled with -fpreprocessed, which uses them to
            // keep -Werror suppressed inside system headers and fatal only on kernel code.
            args.push_back("-E");
            args.push_back("-o");
            args.push_back(out_path);
            args.push_back(src);
            break;
    }
    return args;
}

#ifdef _WIN32

namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

// Append |arg| to |cmdline| quoted per the Microsoft CRT command-line parsing rules
// (the inverse of the argv splitting done by parse_cmdline/CommandLineToArgvW), so the
// child's argv matches |arg| byte-for-byte:
//   - N backslashes before a '"' become 2N backslashes + escaped quote,
//   - N backslashes at the end of a quoted arg become 2N backslashes,
//   - backslashes anywhere else are literal.
void append_quoted_arg(std::string& cmdline, const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        cmdline += arg;  // No quoting needed; keep the command line readable in logs.
        return;
    }
    cmdline += '"';
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            cmdline.append(backslashes * 2 + 1, '\\');
        } else {
            cmdline.append(backslashes, '\\');
        }
        backslashes = 0;
        cmdline += c;
    }
    cmdline.append(backslashes * 2, '\\');  // Trailing backslashes must not escape the closing quote.
    cmdline += '"';
}

// Quote |arg| for a GCC @response-file: gcc splits response files on whitespace and honors
// double quotes with backslash escapes, so wrapping every arg in quotes and escaping '\' and '"'
// round-trips arbitrary content (including the -DFULL_KERNEL_NAME="<name>" defines).
void append_response_file_arg(std::string& out, const std::string& arg) {
    out += '"';
    for (char c : arg) {
        if (c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += "\"\n";
}

}  // namespace

bool exec_command(const std::vector<std::string>& args, const std::string& working_dir, const std::string& log_file) {
    if (args.empty()) {
        return false;
    }

    // CreateProcessW caps the command line at 32767 chars. g++ (the only program spawned through
    // here) accepts @file response files, so overflow the tail of the argv into one when the
    // assembled line approaches the ceiling. Kernel compiles with large define sets get here.
    std::string cmdline;
    append_quoted_arg(cmdline, args[0]);
    std::string tail;
    for (std::size_t i = 1; i < args.size(); ++i) {
        tail += ' ';
        append_quoted_arg(tail, args[i]);
    }

    constexpr std::size_t kMaxCmdline = 30000;  // Headroom below the 32767-char hard limit.
    std::string response_path;
    if (cmdline.size() + tail.size() > kMaxCmdline) {
        static std::atomic<std::uint64_t> response_counter{0};
        std::string contents;
        for (std::size_t i = 1; i < args.size(); ++i) {
            append_response_file_arg(contents, args[i]);
        }
        // Unique per process and per call: concurrent kernel compiles share log/out dirs.
        std::string base = !log_file.empty() ? log_file : (std::filesystem::temp_directory_path() / "tt_jit").string();
        response_path =
            fmt::format("{}.{}_{}.rsp", base, ::_getpid(), response_counter.fetch_add(1, std::memory_order_relaxed));
        std::ofstream rsp(response_path, std::ios::binary);
        rsp << contents;
        rsp.close();
        if (rsp.fail()) {
            log_error(tt::LogBuildKernels, "Failed to write response file '{}'", response_path);
            return false;
        }
        cmdline += " @";
        cmdline += response_path;
    } else {
        cmdline += tail;
    }

    HANDLE log_handle = INVALID_HANDLE_VALUE;
    if (!log_file.empty()) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;  // The child writes stdout/stderr through this handle.
        log_handle = CreateFileW(
            to_wide(log_file).c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (log_handle == INVALID_HANDLE_VALUE) {
            log_error(
                tt::LogBuildKernels, "Failed to open log file '{}': error {}", log_file, static_cast<unsigned>(GetLastError()));
            return false;
        }
    }

    // Restrict handle inheritance to exactly this child's log handle. With a bare
    // bInheritHandles=TRUE, EVERY inheritable handle in the process leaks into the child --
    // under parallel firmware builds, compiler child A would inherit (and hold open until it
    // exits) child B's log handle, making B's post-build rename/delete of its own log fail
    // with sharing violations. PROC_THREAD_ATTRIBUTE_HANDLE_LIST scopes inheritance to the
    // one listed handle.
    STARTUPINFOEXW six{};
    six.StartupInfo.cb = sizeof(six);
    std::vector<unsigned char> attr_buf;
    if (log_handle != INVALID_HANDLE_VALUE) {
        six.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        // No stdin: the compiler never reads it, and the console handle is not in the
        // inherit list anyway.
        six.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        six.StartupInfo.hStdOutput = log_handle;
        six.StartupInfo.hStdError = log_handle;

        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attr_buf.resize(attr_size);
        auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) ||
            !UpdateProcThreadAttribute(
                attr_list,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                &log_handle,
                sizeof(HANDLE),
                nullptr,
                nullptr)) {
            log_error(
                tt::LogBuildKernels,
                "Failed to set up handle-inheritance list: error {}",
                static_cast<unsigned>(GetLastError()));
            CloseHandle(log_handle);
            return false;
        }
        six.lpAttributeList = attr_list;
    }

    // CreateProcessW may scribble on the command-line buffer, so it must be mutable.
    std::wstring wcmdline = to_wide(cmdline);
    std::wstring wcwd = to_wide(working_dir);
    PROCESS_INFORMATION pi{};
    // lpApplicationName stays null so the exe name resolves through PATH (with an implied
    // .exe extension), matching posix_spawnp's PATH search on Linux.
    BOOL ok = CreateProcessW(
        nullptr,
        wcmdline.data(),
        nullptr,
        nullptr,
        /*bInheritHandles=*/log_handle != INVALID_HANDLE_VALUE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        working_dir.empty() ? nullptr : wcwd.c_str(),
        &six.StartupInfo,
        &pi);
    DWORD create_error = GetLastError();

    // Drop the parent's copy of the log handle immediately -- before waiting on the child --
    // so the only remaining reference lives in the child and dies with it.
    if (six.lpAttributeList != nullptr) {
        DeleteProcThreadAttributeList(six.lpAttributeList);
    }
    if (log_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(log_handle);
    }
    SetLastError(create_error);

    bool success = false;
    if (!ok) {
        log_error(
            tt::LogBuildKernels,
            "CreateProcessW failed for '{}': error {}",
            args[0],
            static_cast<unsigned>(GetLastError()));
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 1;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            success = (exit_code == 0);
        } else {
            log_error(
                tt::LogBuildKernels,
                "GetExitCodeProcess failed for '{}': error {}",
                args[0],
                static_cast<unsigned>(GetLastError()));
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (!response_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(response_path, ec);  // Best-effort cleanup; the compile already ran.
    }
    return success;
}

#else  // !_WIN32

bool exec_command(const std::vector<std::string>& args, const std::string& working_dir, const std::string& log_file) {
    if (args.empty()) {
        return false;
    }

    // Build a null-terminated argv array for posix_spawn.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    int log_fd = -1;
    if (!log_file.empty()) {
        log_fd = open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (log_fd < 0) {
            posix_spawn_file_actions_destroy(&file_actions);
            return false;
        }
        posix_spawn_file_actions_adddup2(&file_actions, log_fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&file_actions, log_fd, STDERR_FILENO);
    }

    if (!working_dir.empty()) {
        posix_spawn_file_actions_addchdir_np(&file_actions, working_dir.c_str());
    }

    pid_t pid = 0;
    int spawn_ret =
        posix_spawnp(&pid, argv[0], &file_actions, nullptr, const_cast<char* const*>(argv.data()), ::environ);

    if (log_fd >= 0) {
        close(log_fd);
    }
    posix_spawn_file_actions_destroy(&file_actions);

    if (spawn_ret != 0) {
        log_error(tt::LogBuildKernels, "posix_spawnp failed for '{}': {}", argv[0], std::strerror(spawn_ret));
        return false;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            log_error(tt::LogBuildKernels, "waitpid failed for '{}': {}", argv[0], std::strerror(errno));
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif  // _WIN32

bool grep_lines_to_file(
    const std::string& dir, const std::string& suffix, const std::string& needle, const std::string& out_file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::ofstream out(out_file, std::ios::app);
    if (!out.is_open()) {
        return false;
    }
    bool matched = false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string name = entry.path().filename().string();
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(needle) != std::string::npos) {
                // Match multi-file grep output: each hit prefixed with its file path.
                out << entry.path().string() << ':' << line << '\n';
                matched = true;
            }
        }
    }
    return matched && !out.fail();
}

std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot read file: " + path);
    }
    std::streampos pos = file.tellg();
    if (pos == std::streampos(-1)) {
        throw std::runtime_error("Cannot determine size of file: " + path);
    }
    auto byte_count = static_cast<std::streamsize>(pos);
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(byte_count));
    file.read(reinterpret_cast<char*>(data.data()), byte_count);
    if (file.gcount() != byte_count || (!file && !file.eof())) {
        throw std::runtime_error(
            fmt::format("Failed to read file '{}' fully (expected {} bytes, got {})", path, byte_count, file.gcount()));
    }
    return data;
}

std::vector<tt::jit_build::GeneratedFile> read_directory_files(
    const std::string& dir, std::span<const std::string> extensions) {
    namespace fs = std::filesystem;
    std::vector<tt::jit_build::GeneratedFile> files;
    if (!fs::is_directory(dir)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (!extensions.empty() &&
            std::find(extensions.begin(), extensions.end(), entry.path().extension().string()) == extensions.end()) {
            continue;
        }
        // Tolerate concurrent writers: another process compiling the same kernel into this
        // shared cache dir may rename a FileRenamer temp file away between enumeration and read.
        // Skip files that vanish or fail to read rather than aborting the whole upload.
        try {
            files.push_back({entry.path().filename().string(), read_file_bytes(entry.path().string())});
        } catch (const std::runtime_error& e) {
            log_debug(
                tt::LogBuildKernels,
                "Skipping file that could not be read during directory scan of {}: {}",
                dir,
                e.what());
        }
    }
    return files;
}

std::string format_named_ct_arg_map(const std::unordered_map<std::string, std::uint32_t>& named_args) {
    std::vector<const std::pair<const std::string, std::uint32_t>*> sorted;
    sorted.reserve(named_args.size());
    for (const auto& entry : named_args) {
        sorted.push_back(&entry);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->first < b->first; });

    // Whole-model kernels reach 100 KB+ here; size it up front rather than growing ~1750 times.
    std::size_t reserved = 0;
    for (const auto* entry : sorted) {
        reserved += entry->first.size() + 16;
    }
    std::string out;
    out.reserve(reserved);

    for (const auto* entry : sorted) {
        if (!out.empty()) {
            out += ',';
        }
        out += "{\"";
        out += entry->first;
        out += "\",";
        out += std::to_string(entry->second);
        out += '}';
    }
    return out;
}

std::string format_named_ct_arg_map_header(const std::unordered_map<std::string, std::uint32_t>& named_args) {
    return "// AUTO-GENERATED -- do not edit.\n#pragma once\n\n#define KERNEL_COMPILE_TIME_ARG_MAP " +
           format_named_ct_arg_map(named_args) + "\n";
}

void create_file(const std::string& file_path_str) {
    namespace fs = std::filesystem;

    fs::path file_path(file_path_str);
    fs::create_directories(file_path.parent_path());

    std::ofstream ofs(file_path);
    ofs.close();
}

void remove_file_with_retry(const std::string& path) {
#ifdef _WIN32
    // As with FileRenamer above: sharing violations on freshly written files are usually
    // transient on Windows. A leftover file is harmless, so degrade to a warning.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (int attempt = 0; ec && attempt < 50; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::filesystem::remove(path, ec);
    }
    if (ec) {
        log_warning(tt::LogBuildKernels, "Failed to remove temporary file {}: {}", path, ec.message());
    }
#else
    std::filesystem::remove(path);
#endif
}

uint64_t FileRenamer::unique_id_ = []() {
    std::random_device rd;
    std::uniform_int_distribution<uint64_t> distr;
    return distr(rd);
}();

std::string FileRenamer::generate_temp_path(const std::filesystem::path& target_path) {
    // unique_id_ is initialized once per process, so a fork()ed child inherits the
    // parent's value and would otherwise generate byte-identical temp paths. Mix in
    // the live pid so forked siblings -- e.g. pytest --forked test processes sharing
    // one kernel cache -- never collide on the same temp file.
    //
    // Formatted in one call rather than through an intermediate tag string: this runs
    // once per source file during JIT setup, and the extra allocation measured more
    // expensive than the getpid() syscall it accompanies.
#ifdef _WIN32
    const auto pid = ::_getpid();
#else
    const auto pid = ::getpid();
#endif
    std::filesystem::path path(target_path);
    if (path.has_extension()) {
        path.replace_extension(fmt::format("{}_{}{}", unique_id_, pid, path.extension().string()));
        return path.string();
    }
    return fmt::format("{}.{}_{}", target_path.string(), unique_id_, pid);
}

FileRenamer::FileRenamer(const std::string& target_path) :
    temp_path_(generate_temp_path(target_path)), target_path_(target_path) {}

FileRenamer::~FileRenamer() {
    std::error_code ec;
    if (target_path_.empty()) {
        return;
    }
    std::filesystem::rename(temp_path_, target_path_, ec);
#ifdef _WIN32
    // Windows sharing violations on a freshly written file are usually transient (an
    // antivirus scan, or a just-exited child process tree whose handles are still being
    // reclaimed). Retry briefly before reporting.
    for (int attempt = 0; ec && attempt < 50; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::filesystem::rename(temp_path_, target_path_, ec);
    }
#endif
    if (ec) {
        log_error(
            tt::LogBuildKernels,
            "Failed to rename temporary file {} to target file {}: {}",
            temp_path_,
            target_path_,
            ec.message());
    }
}

}  // namespace tt::jit_build::utils

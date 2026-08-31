// SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "tt_metal/distributed/shm_resource_tracker.hpp"

#include <mutex>
#include <tt-logger/tt-logger.hpp>
#include <fmt/format.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <process.h>
#else
#include <csignal>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tt::tt_metal::distributed {

namespace {

pid_t extract_pid_from_manifest_name(const std::string& filename) {
    // Expected format: tt_socket_manifest_<pid>
    const std::string prefix = "tt_socket_manifest_";
    if (!filename.starts_with(prefix)) {
        return 0;
    }
    try {
        return static_cast<pid_t>(std::stol(filename.substr(prefix.size())));
    } catch (...) {
        return 0;
    }
}

pid_t extract_pid_from_shm_name(const std::string& filename) {
    // Expected format: tt_{prefix}_{pid}_{counter}
    if (!filename.starts_with("tt_")) {
        return 0;
    }
    auto first = filename.find('_', 3);
    if (first == std::string::npos) {
        return 0;
    }
    auto second = filename.find('_', first + 1);
    if (second == std::string::npos) {
        return 0;
    }
    try {
        return static_cast<pid_t>(std::stol(filename.substr(first + 1, second - first - 1)));
    } catch (...) {
        return 0;
    }
}

#ifndef _WIN32
struct sigaction prev_sigint, prev_sigterm;

void invoke_previous_handler(int sig, const struct sigaction& prev) {
    if (prev.sa_handler == SIG_IGN) {
        return;
    }
    if (prev.sa_flags & SA_SIGINFO) {
        // Restore the original SA_SIGINFO handler and re-raise so the kernel
        // delivers the signal with a real siginfo_t and ucontext_t*.
        struct sigaction restore = prev;
        sigaction(sig, &restore, nullptr);
        raise(sig);
        return;
    }
    if (prev.sa_handler != SIG_DFL) {
        prev.sa_handler(sig);
        return;
    }
    // Previous handler was SIG_DFL or SIG_IGN (or null): restore default and re-raise
    // so the process terminates with the correct signal exit status.
    signal(sig, SIG_DFL);
    raise(sig);
}

void signal_handler(int sig) {
    // Use try_lock to avoid deadlock if the signal interrupted a thread
    // holding mutex_. If we can't acquire the lock, the manifest file
    // ensures the next process will clean up via stale-PID scan.
    ShmResourceTracker::instance().cleanup_from_signal();

    const struct sigaction& prev = (sig == SIGINT) ? prev_sigint : prev_sigterm;
    invoke_previous_handler(sig, prev);
}
#endif  // !_WIN32

#ifdef _WIN32
// Windows shm objects (named file mappings) vanish with their last handle, so only descriptor
// FILES need crash cleanup here; there is no shm_unlink equivalent to run from a handler.
// Manifests live in the temp directory instead of /dev/shm.
std::string manifest_dir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    return ec ? std::string{} : dir.generic_string();
}
#endif

}  // namespace

std::string ShmResourceTracker::manifest_path_for_pid(pid_t pid) {
#ifdef _WIN32
    return fmt::format("{}/tt_socket_manifest_{}", manifest_dir(), pid);
#else
    return fmt::format("/dev/shm/tt_socket_manifest_{}", pid);
#endif
}

bool ShmResourceTracker::is_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        // Access denied still proves the pid exists (mirrors the EPERM case below).
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0;
    bool alive = GetExitCodeProcess(h, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(pid, 0) == 0 || errno == EPERM;
#endif
}

#ifdef _WIN32
ShmResourceTracker::ShmResourceTracker() : manifest_path_(manifest_path_for_pid(::_getpid())) {
    cleanup_stale_resources();
    // No signal handlers: shm segments are named file mappings that the kernel destroys with
    // the process, so a crash leaks nothing; descriptor files left behind by a hard kill are
    // reclaimed by the stale-PID scan above on the next start. Normal exits clean up through
    // the singleton's destructor.
}
#else
ShmResourceTracker::ShmResourceTracker() : manifest_path_(manifest_path_for_pid(getpid())) {
    cleanup_stale_resources();

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &prev_sigint);
    sigaction(SIGTERM, &sa, &prev_sigterm);
}
#endif

ShmResourceTracker::~ShmResourceTracker() {
    try {
        cleanup_all();
    } catch (const std::exception& e) {
        log_warning(LogMetal, "ShmResourceTracker cleanup failed: {}", e.what());
    } catch (...) {
        log_warning(LogMetal, "ShmResourceTracker cleanup failed with unknown exception");
    }
}

ShmResourceTracker& ShmResourceTracker::instance() {
    static ShmResourceTracker tracker;
    return tracker;
}

void ShmResourceTracker::track_shm(const std::string& shm_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    shm_names_.insert(shm_name);
    flush_manifest();
}

void ShmResourceTracker::track_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_paths_.insert(file_path);
    flush_manifest();
}

void ShmResourceTracker::untrack_shm(const std::string& shm_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    shm_names_.erase(shm_name);
    flush_manifest();
}

void ShmResourceTracker::untrack_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_paths_.erase(file_path);
    flush_manifest();
}

void ShmResourceTracker::flush_manifest() {
    if (shm_names_.empty() && file_paths_.empty()) {
        std::remove(manifest_path_.c_str());
        return;
    }
    // Write to a temp file and atomically rename to avoid leaving a
    // truncated manifest if the process is killed mid-write.
    const std::string tmp_path = manifest_path_ + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::trunc);
    if (!ofs) {
        return;
    }
    for (const auto& name : shm_names_) {
        ofs << "shm " << name << "\n";
    }
    for (const auto& path : file_paths_) {
        ofs << "file " << path << "\n";
    }
    ofs.flush();
    if (!ofs) {
        std::remove(tmp_path.c_str());
        return;
    }
    if (::rename(tmp_path.c_str(), manifest_path_.c_str()) != 0) {
        std::remove(tmp_path.c_str());
    }
}

void ShmResourceTracker::cleanup_all() {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
    // Nothing to unlink: named mappings die with their last handle.
    shm_names_.clear();
#else
    for (const auto& name : shm_names_) {
        if (shm_unlink(name.c_str()) == 0) {
            log_debug(LogMetal, "ShmResourceTracker: cleaned up shm '{}'", name);
        }
    }
    shm_names_.clear();
#endif

    for (const auto& path : file_paths_) {
        if (std::remove(path.c_str()) == 0) {
            log_debug(LogMetal, "ShmResourceTracker: cleaned up file '{}'", path);
        }
    }
    file_paths_.clear();

    std::remove(manifest_path_.c_str());
}

void ShmResourceTracker::cleanup_from_signal() {
    // try_lock avoids deadlock if the signal interrupted a thread holding mutex_.
    // If we can't lock, leave the manifest intact so the next process can
    // discover and clean up all resources via stale-PID scan.
    if (!mutex_.try_lock()) {
        return;
    }

#ifdef _WIN32
    // Not reachable from an asynchronous handler on Windows (none is installed); regular
    // library calls are fine and shm entries need no unlinking.
    shm_names_.clear();
    for (const auto& path : file_paths_) {
        std::remove(path.c_str());
    }
    file_paths_.clear();
    std::remove(manifest_path_.c_str());
    mutex_.unlock();
#else
    // Lock acquired. shm_unlink and unlink are async-signal-safe.
    // Avoid logging here (not async-signal-safe).
    for (const auto& name : shm_names_) {
        shm_unlink(name.c_str());
    }
    shm_names_.clear();

    for (const auto& path : file_paths_) {
        ::unlink(path.c_str());
    }
    file_paths_.clear();

    ::unlink(manifest_path_.c_str());
    mutex_.unlock();
#endif
}

void ShmResourceTracker::cleanup_stale_resources() {
#ifdef _WIN32
    // Shm objects self-destruct with their owner on Windows, so only stale manifests (and the
    // descriptor files they list) need reclaiming; scan the temp directory where manifests live.
    std::vector<std::string> stale_manifests;
    std::vector<std::string> stale_shm_names;  // stays empty: nothing to unlink on Windows

    const std::string dir_path = manifest_dir();
    if (dir_path.empty()) {
        return;
    }
    pid_t my_pid = ::_getpid();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
        std::string name = entry.path().filename().string();
        pid_t manifest_pid = extract_pid_from_manifest_name(name);
        if (manifest_pid > 0 && manifest_pid != my_pid && !is_pid_alive(manifest_pid)) {
            stale_manifests.push_back(dir_path + "/" + name);
        }
    }
#else
    DIR* dir = opendir("/dev/shm");
    if (!dir) {
        return;
    }

    std::vector<std::string> stale_manifests;
    std::vector<std::string> stale_shm_names;

    pid_t my_pid = getpid();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);

        // Check for manifest files from dead processes
        pid_t manifest_pid = extract_pid_from_manifest_name(name);
        if (manifest_pid > 0 && manifest_pid != my_pid && !is_pid_alive(manifest_pid)) {
            stale_manifests.push_back("/dev/shm/" + name);
            continue;
        }

        // Check for orphaned shm objects from dead processes
        // Pattern: tt_{h2d|d2h}_{pid}_{counter}
        pid_t shm_pid = extract_pid_from_shm_name(name);
        if (shm_pid > 0 && shm_pid != my_pid && !is_pid_alive(shm_pid)) {
            stale_shm_names.push_back(name);
        }
    }
    closedir(dir);
#endif

    // Clean up resources listed in stale manifests
    for (const auto& manifest : stale_manifests) {
        std::ifstream ifs(manifest);
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.starts_with("shm ")) {
#ifndef _WIN32
                std::string shm_name = line.substr(4);
                if (shm_unlink(shm_name.c_str()) == 0) {
                    log_info(LogMetal, "ShmResourceTracker: removed stale shm '{}'", shm_name);
                }
#endif
            } else if (line.starts_with("file ")) {
                std::string file_path = line.substr(5);
                if (std::remove(file_path.c_str()) == 0) {
                    log_info(LogMetal, "ShmResourceTracker: removed stale file '{}'", file_path);
                }
            }
        }
        std::remove(manifest.c_str());
        log_info(LogMetal, "ShmResourceTracker: removed stale manifest '{}'", manifest);
    }

    // Clean up orphaned shm objects not covered by any manifest
#ifndef _WIN32
    for (const auto& name : stale_shm_names) {
        std::string shm_name = "/" + name;
        if (shm_unlink(shm_name.c_str()) == 0) {
            log_info(LogMetal, "ShmResourceTracker: removed orphaned shm '{}'", shm_name);
        }
    }
#else
    (void)stale_shm_names;
#endif
}

}  // namespace tt::tt_metal::distributed

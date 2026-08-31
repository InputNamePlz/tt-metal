// SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "tt_metal/distributed/named_shm.hpp"
#include "tt_metal/distributed/shm_resource_tracker.hpp"

#include <tt_stl/assert.hpp>
#include <fmt/format.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <random>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <process.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tt::tt_metal::distributed {

#ifdef _WIN32
namespace {
// POSIX shm names ("/tt_...") map onto session-local named file mappings ("Local\tt_...").
// Serialized descriptors keep the POSIX spelling, so cross-process opens agree on the name.
std::string windows_mapping_name(const std::string& posix_name) { return "Local\\" + posix_name.substr(1); }
}  // namespace
#endif

NamedShm::NamedShm(const std::string& name, void* ptr, size_t size) : name_(name), ptr_(ptr), size_(size) {}

NamedShm::~NamedShm() noexcept { close(); }

NamedShm::NamedShm(NamedShm&& other) noexcept : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
#ifdef _WIN32
    mapping_ = other.mapping_;
    other.mapping_ = nullptr;
#endif
}

NamedShm& NamedShm::operator=(NamedShm&& other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        ptr_ = other.ptr_;
        size_ = other.size_;
        other.ptr_ = nullptr;
        other.size_ = 0;
#ifdef _WIN32
        mapping_ = other.mapping_;
        other.mapping_ = nullptr;
#endif
    }
    return *this;
}

NamedShm NamedShm::create(const std::string& name, size_t size) {
    TT_FATAL(!name.empty() && name[0] == '/', "POSIX shm name must start with '/': {}", name);
    TT_FATAL(size > 0, "Shared memory size must be > 0");

#ifdef _WIN32
    // Pagefile-backed named mapping: create-exclusive (ERROR_ALREADY_EXISTS mirrors the POSIX
    // O_CREAT|O_EXCL stale-object failure), zero-initialized by the kernel like a freshly
    // ftruncated shm object. The mapping handle is retained so the name stays valid for
    // openers for the NamedShm's lifetime; the object dies with its last handle/view, so
    // there is nothing to unlink and no crash-cleanup tracker entry to record.
    const std::string win_name = windows_mapping_name(name);
    HANDLE mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(static_cast<uint64_t>(size) >> 32),
        static_cast<DWORD>(size & 0xFFFFFFFFull),
        win_name.c_str());
    TT_FATAL(
        mapping != nullptr,
        "CreateFileMapping(create) failed for '{}': error {}",
        name,
        static_cast<unsigned>(GetLastError()));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        TT_THROW(
            "Shared memory '{}' already exists. Another process in this session still holds it open "
            "(Windows named mappings vanish with their last handle).",
            name);
    }
    void* ptr = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (ptr == nullptr) {
        DWORD map_error = GetLastError();
        CloseHandle(mapping);
        TT_THROW("MapViewOfFile failed for '{}': error {}", name, static_cast<unsigned>(map_error));
    }
    std::memset(ptr, 0, size);
    NamedShm shm(name, ptr, size);
    shm.mapping_ = mapping;
    return shm;
#else
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    TT_FATAL(
        fd != -1,
        "shm_open(create) failed for '{}': {}. If a stale shm object exists, remove it with shm_unlink or delete "
        "/dev/shm{}.",
        name,
        std::strerror(errno),
        name);

    int rc = ftruncate(fd, static_cast<off_t>(size));
    if (rc == -1) {
        int saved_errno = errno;
        ::close(fd);
        shm_unlink(name.c_str());
        TT_THROW("ftruncate failed for '{}': {}", name, std::strerror(saved_errno));
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int mmap_errno = errno;
    ::close(fd);
    if (ptr == MAP_FAILED) {
        shm_unlink(name.c_str());
        TT_THROW("mmap failed for '{}': {}", name, std::strerror(mmap_errno));
    }

    std::memset(ptr, 0, size);
    ShmResourceTracker::instance().track_shm(name);
    return NamedShm(name, ptr, size);
#endif
}

NamedShm NamedShm::open(const std::string& name, size_t size) {
    TT_FATAL(!name.empty() && name[0] == '/', "POSIX shm name must start with '/': {}", name);
    TT_FATAL(size > 0, "Shared memory size must be > 0");

#ifdef _WIN32
    const std::string win_name = windows_mapping_name(name);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, win_name.c_str());
    TT_FATAL(
        mapping != nullptr,
        "OpenFileMapping failed for '{}': error {}",
        name,
        static_cast<unsigned>(GetLastError()));
    // Mapping a view of `size` bytes fails if the backing section is smaller, standing in for
    // the POSIX fstat size check.
    void* ptr = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (ptr == nullptr) {
        DWORD map_error = GetLastError();
        CloseHandle(mapping);
        TT_THROW(
            "MapViewOfFile failed for '{}' ({} B; the backing section may be smaller): error {}",
            name,
            size,
            static_cast<unsigned>(map_error));
    }
    NamedShm shm(name, ptr, size);
    shm.mapping_ = mapping;
    return shm;
#else
    int fd = shm_open(name.c_str(), O_RDWR, 0600);
    TT_FATAL(fd != -1, "shm_open(open) failed for '{}': {}", name, std::strerror(errno));

    struct stat st;
    TT_FATAL(fstat(fd, &st) == 0, "fstat failed for '{}': {}", name, std::strerror(errno));
    TT_FATAL(
        static_cast<size_t>(st.st_size) >= size,
        "Shared memory '{}' backing size ({}) is smaller than requested size ({})",
        name,
        st.st_size,
        size);

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int mmap_errno = errno;
    ::close(fd);
    if (ptr == MAP_FAILED) {
        TT_THROW("mmap failed for '{}': {}", name, std::strerror(mmap_errno));
    }

    return NamedShm(name, ptr, size);
#endif
}

void NamedShm::close() {
#ifdef _WIN32
    if (ptr_ != nullptr) {
        UnmapViewOfFile(ptr_);
        ptr_ = nullptr;
        size_ = 0;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
#else
    if (ptr_ != nullptr) {
        munmap(ptr_, size_);
        ptr_ = nullptr;
        size_ = 0;
    }
#endif
}

void NamedShm::unlink() {
#ifdef _WIN32
    // No unlink on Windows: the kernel object (and its name) disappears when the last handle
    // or view is released, which close() just did for this process's references.
    close();
    name_.clear();
#else
    close();
    if (!name_.empty()) {
        int rc = shm_unlink(name_.c_str());
        if (rc == 0 || errno == ENOENT) {
            ShmResourceTracker::instance().untrack_shm(name_);
            name_.clear();
        }
    }
#endif
}

std::string generate_shm_name(const std::string& prefix) {
    static std::atomic<uint32_t> counter{0};
    static const uint32_t random_number = []() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist;
        return dist(gen);
    }();
#ifdef _WIN32
    return fmt::format("/tt_{}_{}_{}_{}", prefix, ::_getpid(), random_number, counter.fetch_add(1));
#else
    return fmt::format("/tt_{}_{}_{}_{}", prefix, getpid(), random_number, counter.fetch_add(1));
#endif
}

}  // namespace tt::tt_metal::distributed

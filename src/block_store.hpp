#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using u64 = std::uint64_t;
constexpr u64 MIB = 1024ULL * 1024;
constexpr u64 CHUNK = 16 * MIB;
constexpr u64 CAPACITY = 1024 * MIB;
constexpr u64 VOLUME = 64 * 1024 * MIB;
constexpr std::size_t BUFFER = 1024 * 1024;

static void check(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}
static u64 parse(const std::string& s) {
    std::size_t used = 0;
    auto n = std::stoull(s, &used, 0);
    check(used == s.size(), "invalid number: " + s);
    return n;
}
static void range(u64 off, u64 len) {
    check(off <= VOLUME && len <= VOLUME - off, "I/O outside virtual volume");
}
class Lock {
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
public:
    explicit Lock(const fs::path& p) {
#ifdef _WIN32
        h_ = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(h_ != INVALID_HANDLE_VALUE, "volume locked or lock file unavailable");
#else
        fd_ = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        check(fd_ >= 0, "cannot open volume lock");
        if (::flock(fd_, LOCK_EX | LOCK_NB)) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("volume locked by another process");
        }
#endif
    }
    ~Lock() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
static void fsync_file(const fs::path& p) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE, "cannot open for FlushFileBuffers: " + p.string());
    BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    check(ok, "FlushFileBuffers failed: " + p.string());
#else
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "cannot open for fsync: " + p.string());
    int ok = ::fsync(fd);
    ::close(fd);
    check(ok == 0, "fsync failed: " + p.string());
#endif
}
static void fsync_directory(const fs::path& p) {
#ifndef _WIN32
    int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(fd >= 0, "directory fsync open failed");
    int ok = ::fsync(fd);
    ::close(fd);
    check(ok == 0, "directory fsync failed");
#else
    (void)p; // crash durability of Windows directory renames not proven here
#endif
}
static void restore_backup(const fs::path& p) {
    auto bak = p;
    bak += ".bak";
    if (!fs::exists(bak)) return;
    if (fs::exists(p)) fs::remove(bak);
    else fs::rename(bak, p);
}
static void replace(const fs::path& tmp, const fs::path& dst) {
#ifdef _WIN32
    auto bak = dst; bak += ".bak";
    check(!fs::exists(bak), "unexpected backup file");
    if (fs::exists(dst)) fs::rename(dst, bak);
    fs::rename(tmp, dst);
    if (fs::exists(bak)) fs::remove(bak);
#else
    fs::rename(tmp, dst);
#endif
    fsync_directory(dst.parent_path());
}
static void copy_chunk(const fs::path& src, const fs::path& dst) {
    std::ifstream in;
    if (fs::exists(src)) {
        check(fs::file_size(src) == CHUNK, "invalid chunk size: " + src.string());
        in.open(src, std::ios::binary);
        check(bool(in), "cannot open backing chunk: " + src.string());
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    check(bool(out), "cannot create chunk: " + dst.string());
    std::vector<char> buf(BUFFER, 0);
    for (u64 pos = 0; pos < CHUNK; pos += BUFFER) {
        if (in.is_open()) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            check(static_cast<std::size_t>(in.gcount()) == buf.size(),
                  "short read from chunk: " + src.string());
        }
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        check(bool(out), "cannot write chunk: " + dst.string());
    }
    out.flush();
    check(bool(out), "failed flushing chunk: " + dst.string());
    out.close();
    fsync_file(dst);
}
static void read_part(const fs::path& p, u64 offset, char* dst, std::size_t n) {
    std::ifstream in(p, std::ios::binary);
    check(bool(in), "cannot open cached chunk");
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(dst, static_cast<std::streamsize>(n));
    check(static_cast<std::size_t>(in.gcount()) == n, "short cache read");
}
static void write_part(const fs::path& p, u64 offset, const char* src, std::size_t n) {
    std::fstream io(p, std::ios::binary | std::ios::in | std::ios::out);
    check(bool(io), "cannot open chunk for write");
    io.seekp(static_cast<std::streamoff>(offset));
    io.write(src, static_cast<std::streamsize>(n));
    io.flush();
    check(bool(io), "cache write failed");
    io.close();
    fsync_file(p);
}

struct Entry { bool dirty = false; u64 last = 0; };
class Store {
    fs::path disk_, cache_;
    u64 cap_, tick_ = 0;
    Lock lock_;
    std::map<u64, Entry> state_;
    fs::path filename(const fs::path& dir, u64 id) const {
        return dir / (std::to_string(id) + ".bin");
    }
    void recover_dir(const fs::path& folder) {
        for (const auto& item : fs::directory_iterator(folder)) {
            if (!item.is_regular_file()) continue;
            if (item.path().extension() == ".tmp") {
                fs::remove(item.path());  // only stale temp files, exclusive volume lock held
            } else if (item.path().extension() == ".bak") {
                auto dest = item.path();
                dest.replace_extension("");
                restore_backup(dest);
            }
        }
    }
    void reload() {
        recover_dir(disk_);
        recover_dir(cache_);
        for (const auto& item : fs::directory_iterator(cache_)) {
            if (!item.is_regular_file() || item.path().extension() != ".bin") continue;
            auto s = item.path().stem().string();
            std::size_t p = 0;
            u64 id;
            try { id = std::stoull(s, &p); }
            catch (const std::exception&) { continue; }
            if (p != s.size() || id >= VOLUME / CHUNK) continue;
            check(fs::file_size(item.path()) == CHUNK, "truncated cache chunk " + s);
            state_.emplace(id, Entry{true, ++tick_});
        }
        check(state_.size() <= cap_ / CHUNK, "cache over capacity");
    }
    void sync(u64 id) {
        auto& e = state_.at(id);
        if (!e.dirty) return;
        auto target = filename(disk_, id), tmp = target;
        tmp += ".tmp";
        copy_chunk(filename(cache_, id), tmp);
        replace(tmp, target);
        e.dirty = false;
    }
    void evict() {
        if (state_.size() < cap_ / CHUNK) return;
        auto it = std::min_element(state_.begin(), state_.end(),
              [](const auto& a, const auto& b) { return a.second.last < b.second.last; });
        check(it != state_.end(), "empty cache at capacity");
        u64 id = it->first;
        sync(id); // dirty never discarded
        fs::remove(filename(cache_, id));
        fsync_directory(cache_);
        state_.erase(id);
    }
    fs::path ensure(u64 id) {
        auto it = state_.find(id);
        if (it != state_.end()) {
            it->second.last = ++tick_;
            return filename(cache_, id);
        }
        evict();
        auto dest = filename(cache_, id), tmp = dest;
        tmp += ".tmp";
        copy_chunk(filename(disk_, id), tmp);
        replace(tmp, dest);
        state_[id] = Entry{false, ++tick_};
        return dest;
    }
public:
    Store(const fs::path& root, u64 cap)
        : disk_(root / "disk"), cache_(root / "cache"), cap_(cap),
          lock_([&]() -> fs::path {
              check(cap_ >= CHUNK && cap_ % CHUNK == 0,
                    "cache must be >=16MiB and multiple of 16MiB");
              fs::create_directories(disk_);
              fs::create_directories(cache_);
              return root / ".lock";
          }()) {
        reload();
    }
    void read(u64 pos, char* dst, std::size_t size) {
        range(pos, size);
        while (size) {
            u64 id = pos / CHUNK, within = pos % CHUNK;
            auto n = static_cast<std::size_t>(std::min<u64>(size, CHUNK - within));
            if (!state_.contains(id) && !fs::exists(filename(disk_, id))) {
                std::fill_n(dst, n, char(0)); // sparse holes do not allocate cache
            } else read_part(ensure(id), within, dst, n);
            pos += n; dst += n; size -= n;
        }
    }
    void write(u64 pos, const char* src, std::size_t size) {
        range(pos, size);
        while (size) {
            u64 id = pos / CHUNK, within = pos % CHUNK;
            auto n = static_cast<std::size_t>(std::min<u64>(size, CHUNK - within));
            auto dest = ensure(id);
            write_part(dest, within, src, n);
            state_.at(id).dirty = true;
            pos += n; src += n; size -= n;
        }
    }
    void flush() {
        for (auto& [id, entry] : state_) { (void)entry; sync(id); }
    }
    void status() const {
        std::size_t dirty = 0, disk_files = 0;
        for (auto& [id, e] : state_) { (void)id; dirty += e.dirty; }
        for (auto& file : fs::directory_iterator(disk_))
            if (file.is_regular_file() && file.path().extension() == ".bin")
                ++disk_files;
        std::cout << "logical_bytes=" << VOLUME
            << " chunk_bytes=" << CHUNK
            << " cache_limit_bytes=" << cap_
            << " cached_chunks=" << state_.size()
            << " dirty_chunks=" << dirty
            << " disk_chunks=" << disk_files << '\n';
    }
};
